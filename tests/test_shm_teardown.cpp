// Copyright (c) 2026 LuneOS project
// SPDX-License-Identifier: Apache-2.0
//
// Tearing down a live gst-shm reader the way Qt Multimedia 6.12 does
// (QGstreamerMediaCaptureSession::setCameraActive(false)), with and without
// rtcShmReaderStop() first.
//
// Qt unlinks the source in an IDLE pad probe (QGstPad::doInIdleProbe,
// qgst_p.h) whose state - a once_flag, a semaphore and the work, which
// refers to the caller's locals - lives on the caller's stack. When the pad is
// idle, gst_pad_add_probe() runs the probe at once and returns 0, and Qt
// returns. But a non-serialized upstream event (the sink's QoS after each
// frame it renders) does not wait for a running idle probe: the streaming
// thread that pushed it through the same pad runs the idle probes again when
// it leaves the pad, and can reach Qt's probe after Qt has returned. It then
// runs on a dead stack frame: the SIGSEGV in "videoQueue:src" on sargo,
// 2026-11-04 19:12:24, inside std::call_once of that probe.
//
// The model here keeps the probe data on the stack as Qt does, but every call
// first checks a registry of live frames, so a late call is counted instead
// of crashing.

#include "rtcshmreader.h"

#include <gtest/gtest.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <mutex>
#include <semaphore>
#include <set>
#include <string>
#include <unistd.h>

namespace {

constexpr int W = 64, H = 48;

std::string socketPath()
{
    const char *dir = g_getenv("TEST_SHM_DIR");
    return std::string(dir ? dir : ".") + "/rtc-teardown-" + std::to_string(getpid());
}

GstElement *startWriter(const std::string &path, int fps)
{
    unlink(path.c_str());
    gchar *desc = g_strdup_printf(
        "videotestsrc is-live=true ! "
        "video/x-raw,format=I420,width=%d,height=%d,framerate=%d/1 ! "
        "queue leaky=downstream max-size-buffers=4 ! "
        "shmsink socket-path=%s shm-size=200000 enable-last-sample=false "
        "wait-for-connection=false sync=false",
        W, H, fps, path.c_str());
    GstElement *p = gst_parse_launch(desc, nullptr);
    g_free(desc);
    gst_element_set_state(p, GST_STATE_PLAYING);
    return p;
}

void runFor(int ms)
{
    gint64 end = g_get_monotonic_time() + ms * 1000;
    while (g_get_monotonic_time() < end)
        g_main_context_iteration(nullptr, FALSE), g_usleep(1000);
}

// ---- the model of qgst_p.h doInIdleProbe / executeWhilePadsAreIdle

std::recursive_mutex liveLock;
std::set<const void *> liveFrames;
std::atomic<int> lateCalls{0};
std::atomic<int> calls{0};

struct CallbackData
{
    std::counting_semaphore<64> waitDone{0};
    std::once_flag onceFlag;
    bool called = false;
    std::function<void()> work;
    void run() { std::call_once(onceFlag, [&] { work(); }); }
};

GstPadProbeReturn idleCallback(GstPad *, GstPadProbeInfo *, gpointer p)
{
    calls++;
    /* Held while the work runs, so the frame cannot go away under it; the
     * work takes it again for its own nested probe. */
    std::lock_guard<std::recursive_mutex> g(liveLock);
    auto *cd = static_cast<CallbackData *>(p);
    if (!liveFrames.count(p) || cd->called)
    {
        /* A second call. Qt runs call_once and releases the semaphore here,
         * on a frame that may already be gone (it is, if this call came
         * after doInIdleProbe returned). */
        lateCalls++;
        return GST_PAD_PROBE_REMOVE;
    }
    cd->called = true;
    cd->run();
    cd->waitDone.release();
    return GST_PAD_PROBE_REMOVE;
}

void doInIdleProbe(GstPad *pad, const std::function<void()> &work)
{
    CallbackData cd;
    cd.work = work;
    {
        std::lock_guard<std::recursive_mutex> g(liveLock);
        liveFrames.insert(&cd);
    }
    gulong probe = gst_pad_add_probe(pad, GST_PAD_PROBE_TYPE_IDLE, idleCallback, &cd, nullptr);
    if (probe != 0 && !cd.waitDone.try_acquire_for(std::chrono::milliseconds(1250)))
    {
        gst_pad_remove_probe(pad, probe);
        std::lock_guard<std::recursive_mutex> g(liveLock);
        if (!cd.called)
        {
            cd.called = true;
            cd.run();
        }
    }
    std::lock_guard<std::recursive_mutex> g(liveLock);
    liveFrames.erase(&cd);
}

/* Qt: modifyPipelineInIdleProbe, nested over the pads (tee src, tee sink). */
void unlinkWhileIdle(GstPad *teeSrc, GstPad *teeSink)
{
    doInIdleProbe(teeSrc, [&] {
        doInIdleProbe(teeSink, [&] {
            /* the GUI thread is not alone on the CPU (on sargo it shares
             * it with two decoders) */
            g_usleep(3000);
            for (GstPad *pad : { teeSrc, teeSink })
            {
                GstPad *peer = gst_pad_get_peer(pad);
                if (!peer)
                    continue;
                if (GST_PAD_IS_SRC(pad))
                    gst_pad_unlink(pad, peer);
                else
                    gst_pad_unlink(peer, pad);
                gst_object_unref(peer);
            }
        });
    });
}

std::atomic<int> upstreamAtTee{0}, buffersAtTee{0};
GstPadProbeReturn countUp(GstPad *, GstPadProbeInfo *info, gpointer)
{
    if (GST_PAD_PROBE_INFO_TYPE(info) & GST_PAD_PROBE_TYPE_BUFFER)
        buffersAtTee++;
    else if (GST_EVENT_TYPE(GST_PAD_PROBE_INFO_EVENT(info)) == GST_EVENT_QOS)
        upstreamAtTee++;
    return GST_PAD_PROBE_OK;
}

struct Session
{
    GstElement *pipeline, *bin, *tee, *queue, *sink;
    GstPad *teeSrc, *teeSink;
};

/* The capture session as Qt builds it for a native video source: the
 * source bin, a tee, the output's leaky queue (setIsPreview) and a sink
 * that syncs (and so sends QoS upstream). */
Session start(const std::string &path)
{
    Session s{};
    s.pipeline = gst_pipeline_new(nullptr);
    std::string desc = rtcShmReaderDescription(path, W, H);
    s.bin = gst_parse_bin_from_description(desc.c_str(), TRUE, nullptr);
    rtcShmReaderDetach(s.bin);
    s.tee = gst_element_factory_make("tee", nullptr);
    s.queue = gst_element_factory_make("queue", "videoQueue");
    g_object_set(s.queue, "leaky", 2, "max-size-buffers", 1, "max-size-bytes", 0,
                 "max-size-time", (guint64)0, nullptr);
    s.sink = gst_element_factory_make("fakesink", nullptr);
    g_object_set(s.sink, "sync", TRUE, "qos", TRUE, nullptr);
    gst_bin_add_many(GST_BIN(s.pipeline), s.bin, s.tee, s.queue, s.sink, nullptr);
    gst_element_link(s.bin, s.tee);
    s.teeSink = gst_element_get_static_pad(s.tee, "sink");
    if (g_getenv("TEARDOWN_DEBUG"))
        gst_pad_add_probe(s.teeSink, (GstPadProbeType)(GST_PAD_PROBE_TYPE_EVENT_UPSTREAM | GST_PAD_PROBE_TYPE_BUFFER), countUp, nullptr, nullptr);
    s.teeSrc = gst_element_request_pad_simple(s.tee, "src_%u");
    GstPad *qsink = gst_element_get_static_pad(s.queue, "sink");
    gst_pad_link(s.teeSrc, qsink);
    gst_object_unref(qsink);
    gst_element_link(s.queue, s.sink);
    if (g_getenv("TEARDOWN_DEBUG"))
    {
        GstPad *sp = gst_element_get_static_pad(s.sink, "sink");
        gst_pad_add_probe(sp, (GstPadProbeType)(GST_PAD_PROBE_TYPE_EVENT_UPSTREAM | GST_PAD_PROBE_TYPE_BUFFER), +[](GstPad *, GstPadProbeInfo *i, gpointer) {
            if (GST_PAD_PROBE_INFO_TYPE(i) & GST_PAD_PROBE_TYPE_BUFFER)
                std::printf("buffer at sink pts %" GST_TIME_FORMAT "\n", GST_TIME_ARGS(GST_BUFFER_PTS(GST_PAD_PROBE_INFO_BUFFER(i))));
            else
                std::printf("up at sink: %s\n", GST_EVENT_TYPE_NAME(GST_PAD_PROBE_INFO_EVENT(i)));
            return GST_PAD_PROBE_OK; }, nullptr, nullptr);
        gst_object_unref(sp);
    }
    gst_element_set_state(s.pipeline, GST_STATE_PLAYING);
    return s;
}

/* Qt: setCameraActive(false), then stopAndRemoveElements. */
void stop(Session &s, bool ourStopFirst)
{
    if (ourStopFirst)
        rtcShmReaderStop(s.bin);
    unlinkWhileIdle(s.teeSrc, s.teeSink);
    gst_element_set_state(s.pipeline, GST_STATE_NULL);
    gst_element_release_request_pad(s.tee, s.teeSrc);
    gst_object_unref(s.teeSrc);
    gst_object_unref(s.teeSink);
    gst_object_unref(s.pipeline);
}

int settle()
{
    const char *n = g_getenv("TEARDOWN_SETTLE_MS");
    return n ? atoi(n) : 150;
}

int cycles()
{
    const char *n = g_getenv("TEARDOWN_CYCLES");
    return n ? atoi(n) : 200;
}

class Teardown : public ::testing::Test
{
protected:
    std::string path = socketPath();
    void SetUp() override { lateCalls = 0; calls = 0; }
    void TearDown() override { unlink(path.c_str()); }
};

} // namespace

/* Qt's teardown of a reader that is still getting frames: the probe is
 * called again after Qt returned. Shows the model sees the race at all. */
TEST_F(Teardown, QtsTeardownOfALiveReaderCallsTheProbeLate)
{
    GstElement *w = startWriter(path, 200);
    runFor(100);
    for (int i = 0; i < cycles() / 4; i++)
    {
        Session s = start(path);
        runFor(settle());
        stop(s, false);
    }
    gst_element_set_state(w, GST_STATE_NULL);
    gst_object_unref(w);
    std::printf("without rtcShmReaderStop: %d late of %d calls\n", lateCalls.load(), calls.load());
    if (g_getenv("TEARDOWN_DEBUG"))
        std::printf("  %d QoS events and %d frames at the tee\n", upstreamAtTee.load(),
                    buffersAtTee.load());
    EXPECT_GT(lateCalls, 0) << "the race did not show in " << cycles() / 4 << " cycles";
}

/* The camera turned off: the reader stopped first, then Qt's teardown. */
TEST_F(Teardown, StoppingTheReaderFirstLeavesNoLateCall)
{
    GstElement *w = startWriter(path, 200);
    runFor(100);
    for (int i = 0; i < cycles(); i++)
    {
        Session s = start(path);
        runFor(settle());
        stop(s, true);
    }
    gst_element_set_state(w, GST_STATE_NULL);
    gst_object_unref(w);
    std::printf("with rtcShmReaderStop: %d late of %d calls\n", lateCalls.load(), calls.load());
    EXPECT_EQ(lateCalls, 0);
}

/* The engine closes the socket first (its preview stops with the camera),
 * then the app tears the reader down. */
TEST_F(Teardown, TheWriterGoingAwayFirst)
{
    for (int i = 0; i < cycles() / 4; i++)
    {
        GstElement *w = startWriter(path, 200);
        runFor(50);
        Session s = start(path);
        runFor(settle());
        gst_element_set_state(w, GST_STATE_NULL);
        gst_object_unref(w);
        unlink(path.c_str());
        runFor(i % 3 * 10);
        stop(s, true);
    }
    EXPECT_EQ(lateCalls, 0);
}

/* After a stop the reader reads nothing more, also when its pipeline
 * changes state afterwards. */
TEST_F(Teardown, AStoppedReaderStaysStopped)
{
    GstElement *w = startWriter(path, 200);
    runFor(100);
    Session s = start(path);
    runFor(settle());
    ASSERT_TRUE(rtcShmReaderStop(s.bin));
    for (const char *name : { RTC_SHM_READER_SRC, RTC_SHM_READER_QUEUE })
    {
        GstElement *e = gst_bin_get_by_name(GST_BIN(s.bin), name);
        GstState state = GST_STATE_VOID_PENDING;
        gst_element_get_state(e, &state, nullptr, 0);
        EXPECT_EQ(state, GST_STATE_NULL) << name;
        EXPECT_TRUE(gst_element_is_locked_state(e)) << name;
        gst_object_unref(e);
    }
    gst_element_set_state(s.pipeline, GST_STATE_PAUSED);
    gst_element_set_state(s.pipeline, GST_STATE_PLAYING);
    GstElement *e = gst_bin_get_by_name(GST_BIN(s.bin), RTC_SHM_READER_SRC);
    GstState state = GST_STATE_VOID_PENDING;
    gst_element_get_state(e, &state, nullptr, 0);
    EXPECT_EQ(state, GST_STATE_NULL) << "the pipeline started the reader again";
    gst_object_unref(e);
    stop(s, false);
    gst_element_set_state(w, GST_STATE_NULL);
    gst_object_unref(w);
}

TEST_F(Teardown, StopNeedsTheReader)
{
    EXPECT_FALSE(rtcShmReaderStop(nullptr));
    GstElement *other = gst_parse_bin_from_description("videotestsrc ! videoconvert", TRUE, nullptr);
    gst_object_ref_sink(other);
    EXPECT_FALSE(rtcShmReaderStop(other));
    gst_object_unref(other);
}

int main(int argc, char **argv)
{
    gst_init(&argc, &argv);
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
