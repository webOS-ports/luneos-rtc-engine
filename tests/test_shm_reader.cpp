// Copyright (c) 2026 LuneOS project
// SPDX-License-Identifier: Apache-2.0
//
// The app's gst-shm reader (qml-plugin/rtcshmreader.h) against a writer set
// up as the engine sets up its own (src/rtc_engine.cpp: a leaky queue into
// shmsink, wait-for-connection=false, sync=false), with an app that does not
// let go of what it is given: what Qt Multimedia's video sink does while the
// GUI thread is busy.

#include "rtcshmreader.h"

#include <gtest/gtest.h>

#include <atomic>
#include <cstdio>
#include <string>
#include <unistd.h>

namespace {

constexpr int W = 320, H = 240;
constexpr guint FRAME = W * H * 3 / 2;

std::string socketPath()
{
    const char *dir = g_getenv("TEST_SHM_DIR");
    return std::string(dir ? dir : ".") + "/rtc-shm-test-" +
           std::to_string(getpid());
}

std::atomic<int> written{0};

GstPadProbeReturn countWritten(GstPad *, GstPadProbeInfo *, gpointer)
{
    written++;
    return GST_PAD_PROBE_OK;
}

GstElement *startWriter(const std::string &path, guint frames)
{
    unlink(path.c_str());
    gchar *desc = g_strdup_printf(
        "videotestsrc is-live=true ! "
        "video/x-raw,format=I420,width=%d,height=%d,framerate=30/1 ! "
        "queue leaky=downstream max-size-buffers=4 ! "
        "shmsink name=w socket-path=%s shm-size=%u enable-last-sample=false "
        "wait-for-connection=false sync=false",
        W, H, path.c_str(), frames * FRAME + 4096);
    GError *err = nullptr;
    GstElement *p = gst_parse_launch(desc, &err);
    g_free(desc);
    EXPECT_TRUE(p) << (err ? err->message : "?");
    g_clear_error(&err);
    GstElement *w = gst_bin_get_by_name(GST_BIN(p), "w");
    GstPad *pad = gst_element_get_static_pad(w, "sink");
    gst_pad_add_probe(pad, GST_PAD_PROBE_TYPE_BUFFER, countWritten, nullptr,
                      nullptr);
    gst_object_unref(pad);
    gst_object_unref(w);
    gst_element_set_state(p, GST_STATE_PLAYING);
    return p;
}

/* What the app holds on to. */
struct App
{
    GMutex lock;
    GPtrArray *held = g_ptr_array_new_with_free_func(
        reinterpret_cast<GDestroyNotify>(gst_buffer_unref));
    std::atomic<int> got{0};
    /* stalls the sink's streaming thread while set */
    std::atomic<bool> stall{false};
    bool keep = true;
};

void onHandoff(GstElement *, GstBuffer *buf, GstPad *, gpointer data)
{
    auto *app = static_cast<App *>(data);
    app->got++;
    if (app->keep)
    {
        g_mutex_lock(&app->lock);
        g_ptr_array_add(app->held, gst_buffer_ref(buf));
        g_mutex_unlock(&app->lock);
    }
    while (app->stall)
        g_usleep(5000);
}

GstElement *startReader(const std::string &path, App *app, bool detach)
{
    std::string desc = rtcShmReaderDescription(path, W, H);
    GError *err = nullptr;
    GstElement *bin = gst_parse_bin_from_description(desc.c_str(), TRUE, &err);
    EXPECT_TRUE(bin) << (err ? err->message : "?");
    g_clear_error(&err);
    if (detach)
    {
        EXPECT_TRUE(rtcShmReaderDetach(bin));
    }
    GstElement *p = gst_pipeline_new(nullptr);
    GstElement *sink = gst_element_factory_make("fakesink", "appsink");
    g_object_set(sink, "sync", FALSE, "signal-handoffs", TRUE, nullptr);
    g_signal_connect(sink, "handoff", G_CALLBACK(onHandoff), app);
    gst_bin_add_many(GST_BIN(p), bin, sink, nullptr);
    EXPECT_TRUE(gst_element_link(bin, sink));
    gst_element_set_state(p, GST_STATE_PLAYING);
    return p;
}

void runFor(int ms)
{
    gint64 end = g_get_monotonic_time() + ms * 1000;
    while (g_get_monotonic_time() < end)
        g_main_context_iteration(nullptr, FALSE), g_usleep(2000);
}

void stop(GstElement *p)
{
    gst_element_set_state(p, GST_STATE_NULL);
    gst_object_unref(p);
}

class ShmReader : public ::testing::Test
{
protected:
    std::string path = socketPath();
    App app;
    void SetUp() override { written = 0; g_mutex_init(&app.lock); }
    void TearDown() override
    {
        app.stall = false;
        g_ptr_array_unref(app.held);
        g_mutex_clear(&app.lock);
        unlink(path.c_str());
    }
};

} // namespace

TEST_F(ShmReader, DescriptionPinsTheFrameLayoutAndBoundsTheQueue)
{
    std::string d = rtcShmReaderDescription("/tmp/x", 640, 480);
    EXPECT_NE(d.find("shmsrc name=rtcshmsrc socket-path=/tmp/x "), std::string::npos) << d;
    EXPECT_NE(d.find("format=I420,width=640,height=480,framerate=30/1"), std::string::npos) << d;
    EXPECT_NE(d.find("colorimetry=bt601"), std::string::npos) << d;
    EXPECT_NE(d.find("leaky=downstream max-size-buffers=2 max-size-bytes=0 max-size-time=0"),
              std::string::npos) << d;
    EXPECT_EQ(d.rfind("videoconvert"), d.size() - 12) << "ends unlinked: " << d;
}

TEST_F(ShmReader, DetachNeedsTheReader)
{
    EXPECT_FALSE(rtcShmReaderDetach(nullptr));
    GstElement *other = gst_parse_bin_from_description("videotestsrc ! videoconvert", TRUE, nullptr);
    ASSERT_TRUE(other);
    gst_object_ref_sink(other);
    EXPECT_FALSE(rtcShmReaderDetach(other));
    gst_object_unref(other);
}

/* The app keeps every frame (a GUI thread that never catches up): the
 * writer, with room for six frames, has to keep going all the same. */
TEST_F(ShmReader, AnAppHoldingFramesDoesNotStallTheWriter)
{
    GstElement *w = startWriter(path, 6);
    runFor(200);
    GstElement *r = startReader(path, &app, true);
    runFor(2000);
    int sent = written, got = app.got;
    stop(r);
    stop(w);
    std::printf("held: written %d, got %d\n", sent, got);
    EXPECT_GE(got, 30) << "the app got " << got << " frames in 2 s";
    EXPECT_GE(sent, 40) << "the writer stalled after " << sent << " frames";
    EXPECT_GE(app.held->len, 30u);
}

/* The same app without the copy: the writer stops once six frames are held.
 * Shows the test above can see a stall at all. */
TEST_F(ShmReader, WithoutTheCopyTheWriterStalls)
{
    GstElement *w = startWriter(path, 6);
    runFor(200);
    GstElement *r = startReader(path, &app, false);
    runFor(2000);
    int got = app.got, sent = written;
    stop(r);
    stop(w);
    std::printf("no copy: written %d, got %d\n", sent, got);
    EXPECT_LE(got, 12) << "frames kept in shm still let " << got << " through";
}

/* A sink that does not take frames for a second: the reader queues two and
 * drops the rest, the writer keeps going, and the app then gets fresh ones. */
TEST_F(ShmReader, ASlowSinkCostsFramesNotTheWriter)
{
    app.keep = false;
    GstElement *w = startWriter(path, 6);
    runFor(200);
    GstElement *r = startReader(path, &app, true);
    runFor(300);
    ASSERT_GT(app.got, 0);
    app.stall = true;
    runFor(300);
    int before = written;
    runFor(1000);
    int during = written - before;
    GstElement *q = gst_bin_get_by_name(GST_BIN(r), RTC_SHM_READER_QUEUE);
    ASSERT_TRUE(q);
    guint level = 0;
    g_object_get(q, "current-level-buffers", &level, nullptr);
    gst_object_unref(q);
    int gotBefore = app.got;
    app.stall = false;
    runFor(500);
    int gotAfter = app.got - gotBefore;
    stop(r);
    stop(w);
    std::printf("slow sink: written %d while stalled, queue %u, got %d after\n",
                during, level, gotAfter);
    EXPECT_LE(level, 2u);
    EXPECT_GE(during, 20) << "the writer stalled with the sink";
    EXPECT_GE(gotAfter, 8) << "the app got nothing after the stall";
}

int main(int argc, char **argv)
{
    gst_init(&argc, &argv);
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
