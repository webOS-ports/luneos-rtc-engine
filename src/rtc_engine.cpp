// Copyright (c) 2026 LuneOS project
// SPDX-License-Identifier: Apache-2.0
//
// luneos-rtc-engine: real-time video media engine for calls on Halium
// devices. The network side of a call (e.g. meowcaller's WhatsApp
// Call.SendVideo/ReceiveVideo) exchanges H.264 Annex-B access units with
// this engine over SOCK_SEQPACKET unix sockets - one access unit per
// datagram, boundaries preserved by the socket type.
//
//   TX: droidcamsrc (recorder mode, hardware H264, viewfinder-independent)
//       -> h264parse (AU aligned) -> appsink -> tx socket
//   RX: rx socket -> appsrc -> h264parse -> droidvdec -> waylandsink
//       rendered into a wl_webos_foreign imported window (the call UI
//       exports a region and passes its window ID), or a fakesink when
//       no window is given.
//
// Control planes:
//   argv:  --tx= --rx= --camera= --bitrate= --window-id=   (standalone)
//   LS2:   --service registers org.webosports.rtcengine with start/stop/
//          status; start acquires VENC/VDEC units from uMediaServer and a
//          policy action from the resource manager stops the session.

#include <gst/gst.h>
#include <gst/video/videooverlay.h>
#include <glib-unix.h>

#include <luna-service2/lunaservice.h>
#include <pbnjson.hpp>
#include <ResourceManagerClient.h>

#include <camera_window_manager.h>

#include <cstdio>
#include <cstring>
#include <memory>
#include <string>

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

struct Options
{
    std::string txSocket;
    std::string rxSocket;
    std::string windowId;
    std::string shmPath; /* publish decoded frames via gst-shm for in-app rendering */
    int camera   = 0;
    int width    = 1280;
    int height   = 720;
    int bitrate  = 2000000;
    int fps      = 30;
    int rotation  = 0; /* degrees clockwise applied at render: 0/90/180/270 */
    bool loopback = false; /* feed TX access units straight into RX (self-view) */
};

static GMainLoop *loop;
static guint64 txAUs, rxAUs, rxFrames;
static bool sessionRunning;

/* ---------------- uMediaServer resources ---------------- */

static std::shared_ptr<uMediaServer::ResourceManagerClient> rmc;
static std::string acquiredResources;

static void stopSession(void);

static bool policyActionCb(const char *action, const char *resources,
                           const char *, const char *, const char *)
{
    g_printerr("uMS policy action '%s' on %s: stopping session\n",
               action ? action : "?", resources ? resources : "?");
    g_idle_add(
        +[](gpointer) -> gboolean {
            stopSession();
            return G_SOURCE_REMOVE;
        },
        nullptr);
    return true;
}

static const char *acquireCodecs(bool wantEnc, bool wantDec)
{
    try
    {
        rmc = std::make_shared<uMediaServer::ResourceManagerClient>();
        rmc->registerPipeline("media", "org.webosports.rtcengine");
        rmc->registerPolicyActionHandler(
            std::bind(policyActionCb, std::placeholders::_1, std::placeholders::_2,
                      std::placeholders::_3, std::placeholders::_4,
                      std::placeholders::_5));

        std::string request = "[";
        if (wantEnc)
            request += "{\"resource\":\"VENC\",\"qty\":1}";
        if (wantEnc && wantDec)
            request += ",";
        if (wantDec)
            request += "{\"resource\":\"VDEC\",\"qty\":1}";
        request += "]";

        std::string response;
        if (!rmc->acquire(request, response))
        {
            g_printerr("uMS acquire failed: %s\n", response.c_str());
            rmc->unregisterPipeline();
            rmc.reset();
            return "denied";
        }
        /* release() takes the resources array, not the acquire response */
        acquiredResources = response;
        pbnjson::JDomParser parser;
        if (parser.parse(response, pbnjson::JSchema::AllSchema()))
        {
            pbnjson::JValue resources = parser.getDom()["resources"];
            std::string serialized;
            pbnjson::JGenerator generator(nullptr);
            if (resources.isArray() &&
                generator.toString(resources, pbnjson::JSchema::AllSchema(),
                                   serialized))
                acquiredResources = serialized;
        }
        g_print("uMS acquired: %s\n", acquiredResources.c_str());
        return "acquired";
    }
    catch (const std::exception &e)
    {
        g_printerr("uMS unavailable (%s), continuing without resource "
                   "management\n",
                   e.what());
        rmc.reset();
        return "unavailable";
    }
}

static void releaseCodecs(void)
{
    if (!rmc)
        return;
    if (!acquiredResources.empty())
        rmc->release(acquiredResources);
    rmc->unregisterPipeline();
    rmc.reset();
    acquiredResources.clear();
}

/* ---------------- pipeline state ---------------- */

struct RxState
{
    GstElement *pipeline = nullptr;
    GstElement *appsrc   = nullptr;
    int listenFd         = -1;
    int peerFd           = -1;
    guint listenWatch    = 0;
    guint peerWatch      = 0;
    std::string socketPath;
    bool windowAttached = false;
    int renderWidth     = 0;
    int renderHeight    = 0;
} rx;

/* ---------------- TX ---------------- */

struct TxState
{
    GstElement *pipeline = nullptr;
    int listenFd         = -1;
    int peerFd           = -1;
    guint listenWatch    = 0;
    guint modeTimeout    = 0;
    guint captureTimeout = 0;
    bool loopback        = false;
    std::string socketPath;
} tx;

static gboolean txAcceptCb(gint fd, GIOCondition, gpointer)
{
    int peer = accept(fd, nullptr, nullptr);
    if (peer >= 0)
    {
        if (tx.peerFd >= 0)
            close(tx.peerFd);
        tx.peerFd = peer;
        int sz    = 1 << 20;
        setsockopt(peer, SOL_SOCKET, SO_SNDBUF, &sz, sizeof(sz));
        g_print("tx: peer connected\n");
    }
    return G_SOURCE_CONTINUE;
}

static GstFlowReturn txSampleCb(GstElement *sink, gpointer)
{
    GstSample *sample = nullptr;
    g_signal_emit_by_name(sink, "pull-sample", &sample);
    if (!sample)
        return GST_FLOW_OK;

    GstBuffer *buf = gst_sample_get_buffer(sample);
    GstMapInfo map;
    if (buf && gst_buffer_map(buf, &map, GST_MAP_READ))
    {
        if (tx.loopback && rx.appsrc)
        {
            GstBuffer *copy = gst_buffer_new_allocate(nullptr, map.size, nullptr);
            gst_buffer_fill(copy, 0, map.data, map.size);
            GstFlowReturn fret;
            g_signal_emit_by_name(rx.appsrc, "push-buffer", copy, &fret);
            gst_buffer_unref(copy);
            txAUs++;
            rxAUs++;
        }
        else if (tx.peerFd >= 0)
        {
            if (send(tx.peerFd, map.data, map.size, MSG_NOSIGNAL) < 0)
            {
                g_printerr("tx: peer gone (%s)\n", strerror(errno));
                close(tx.peerFd);
                tx.peerFd = -1;
            }
            else
            {
                txAUs++;
            }
        }
        gst_buffer_unmap(buf, &map);
    }
    gst_sample_unref(sample);
    return GST_FLOW_OK;
}

static bool startTx(const Options &o)
{
    tx.loopback = o.loopback;
    if (!o.txSocket.empty())
    {
        unlink(o.txSocket.c_str());
        tx.listenFd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
        sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        g_strlcpy(addr.sun_path, o.txSocket.c_str(), sizeof(addr.sun_path));
        if (bind(tx.listenFd, (sockaddr *)&addr, sizeof(addr)) < 0 ||
            listen(tx.listenFd, 1) < 0)
        {
            g_printerr("tx: bind/listen %s: %s\n", o.txSocket.c_str(),
                       strerror(errno));
            return false;
        }
        tx.socketPath  = o.txSocket;
        tx.listenWatch = g_unix_fd_add(tx.listenFd, G_IO_IN, txAcceptCb, nullptr);
    }

    gchar *desc = g_strdup_printf(
        "droidcamsrc name=cam camera-device=%d target-bitrate=%d "
        "cam.imgsrc ! fakesink async=false "
        "cam.vfsrc ! capsfilter caps=video/x-raw,format=NV21 ! "
        "queue leaky=downstream max-size-buffers=4 ! fakesink sync=false "
        "cam.vidsrc ! video/x-h264,width=%d,height=%d ! "
        "h264parse config-interval=-1 ! "
        "video/x-h264,stream-format=byte-stream,alignment=au ! "
        "appsink name=txsink emit-signals=true sync=false max-buffers=8 drop=false",
        o.camera, o.bitrate, o.width, o.height);
    GError *err  = nullptr;
    tx.pipeline  = gst_parse_launch(desc, &err);
    g_free(desc);
    if (!tx.pipeline)
    {
        g_printerr("tx pipeline: %s\n", err ? err->message : "?");
        g_clear_error(&err);
        return false;
    }

    GstElement *sink = gst_bin_get_by_name(GST_BIN(tx.pipeline), "txsink");
    g_signal_connect(sink, "new-sample", G_CALLBACK(txSampleCb), nullptr);
    gst_object_unref(sink);

    gst_element_set_state(tx.pipeline, GST_STATE_PLAYING);

    /* video mode + start-capture begins the hardware-encoded stream on
     * vidsrc while the raw viewfinder branch keeps the preview path
     * available (needs gst-droid's recorder-in-raw-preview patch). The
     * camera takes a few seconds to open; switching modes or starting the
     * capture before the device is running is silently ignored. */
    tx.modeTimeout = g_timeout_add_seconds(4, +[](gpointer) -> gboolean {
        tx.modeTimeout = 0;
        if (!tx.pipeline)
            return G_SOURCE_REMOVE;
        GstElement *cam = gst_bin_get_by_name(GST_BIN(tx.pipeline), "cam");
        g_object_set(cam, "mode", 2, nullptr);
        gst_object_unref(cam);
        tx.captureTimeout = g_timeout_add_seconds(1, +[](gpointer) -> gboolean {
            tx.captureTimeout = 0;
            if (!tx.pipeline)
                return G_SOURCE_REMOVE;
            GstElement *cam2 = gst_bin_get_by_name(GST_BIN(tx.pipeline), "cam");
            g_signal_emit_by_name(cam2, "start-capture");
            gst_object_unref(cam2);
            g_print("tx: capture started\n");
            return G_SOURCE_REMOVE;
        }, nullptr);
        return G_SOURCE_REMOVE;
    }, nullptr);

    g_print("tx: streaming camera %d h264 %dx%d @%dbps to %s\n", o.camera,
            o.width, o.height, o.bitrate, o.txSocket.c_str());
    return true;
}

static void stopTx(void)
{
    if (tx.modeTimeout)
        g_source_remove(tx.modeTimeout);
    if (tx.captureTimeout)
        g_source_remove(tx.captureTimeout);
    tx.modeTimeout = tx.captureTimeout = 0;
    if (tx.pipeline)
    {
        GstElement *cam = gst_bin_get_by_name(GST_BIN(tx.pipeline), "cam");
        if (cam)
        {
            g_signal_emit_by_name(cam, "stop-capture");
            gst_object_unref(cam);
        }
        gst_element_set_state(tx.pipeline, GST_STATE_NULL);
        gst_object_unref(tx.pipeline);
        tx.pipeline = nullptr;
    }
    if (tx.listenWatch)
        g_source_remove(tx.listenWatch);
    tx.listenWatch = 0;
    if (tx.peerFd >= 0)
        close(tx.peerFd);
    if (tx.listenFd >= 0)
        close(tx.listenFd);
    tx.peerFd = tx.listenFd = -1;
    if (!tx.socketPath.empty())
        unlink(tx.socketPath.c_str());
    tx.socketPath.clear();
}

/* ---------------- RX ---------------- */

static LSM::CameraWindowManager windowManager;

static gboolean rxDataCb(gint fd, GIOCondition cond, gpointer)
{
    static char buf[1 << 20];
    ssize_t n = recv(fd, buf, sizeof(buf), 0);
    if (n <= 0)
    {
        g_print("rx: peer disconnected\n");
        close(fd);
        rx.peerFd    = -1;
        rx.peerWatch = 0;
        return G_SOURCE_REMOVE;
    }

    GstBuffer *gbuf = gst_buffer_new_allocate(nullptr, n, nullptr);
    gst_buffer_fill(gbuf, 0, buf, n);
    GstFlowReturn fret;
    g_signal_emit_by_name(rx.appsrc, "push-buffer", gbuf, &fret);
    gst_buffer_unref(gbuf);
    rxAUs++;
    return G_SOURCE_CONTINUE;
}

static gboolean rxAcceptCb(gint fd, GIOCondition, gpointer)
{
    int peer = accept(fd, nullptr, nullptr);
    if (peer >= 0)
    {
        if (rx.peerFd >= 0)
            close(rx.peerFd);
        rx.peerFd    = peer;
        rx.peerWatch = g_unix_fd_add(peer, G_IO_IN, rxDataCb, nullptr);
        g_print("rx: peer connected\n");
    }
    return G_SOURCE_CONTINUE;
}

static GstPadProbeReturn rxFrameProbe(GstPad *, GstPadProbeInfo *info, gpointer)
{
    if (GST_PAD_PROBE_INFO_TYPE(info) & GST_PAD_PROBE_TYPE_BUFFER)
        rxFrames++;
    return GST_PAD_PROBE_OK;
}

/* The webOS OSE camera player render contract: hand waylandsink the
 * foreign display through a GstContext when it asks, and the imported
 * surface when it prepares its window. */
static GstBusSyncReply rxBusSyncCb(GstBus *, GstMessage *msg, gpointer)
{
    if (GST_MESSAGE_TYPE(msg) == GST_MESSAGE_NEED_CONTEXT)
    {
        const gchar *type = nullptr;
        gst_message_parse_context_type(msg, &type);
        if (g_strcmp0(type, "GstWaylandDisplayHandleContextType") != 0 ||
            !windowManager.getDisplay())
            return GST_BUS_PASS;
        GstContext *context =
            gst_context_new("GstWaylandDisplayHandleContextType", TRUE);
        gst_structure_set(gst_context_writable_structure(context), "handle",
                          G_TYPE_POINTER, windowManager.getDisplay(), nullptr);
        gst_element_set_context(GST_ELEMENT(GST_MESSAGE_SRC(msg)), context);
        gst_context_unref(context);
        gst_message_unref(msg);
        return GST_BUS_DROP;
    }
    if (gst_is_video_overlay_prepare_window_handle_message(msg))
    {
        if (windowManager.getSurface())
        {
            GstVideoOverlay *overlay = GST_VIDEO_OVERLAY(GST_MESSAGE_SRC(msg));
            gst_video_overlay_set_window_handle(
                overlay, (guintptr)windowManager.getSurface());
            gst_video_overlay_set_render_rectangle(overlay, 0, 0, rx.renderWidth,
                                                   rx.renderHeight);
            gst_video_overlay_expose(overlay);
            g_print("rx: waylandsink attached to imported surface\n");
        }
        gst_message_unref(msg);
        return GST_BUS_DROP;
    }
    return GST_BUS_PASS;
}

static bool startRx(const Options &o)
{
    if (!o.rxSocket.empty())
    {
        unlink(o.rxSocket.c_str());
        rx.listenFd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
        sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        g_strlcpy(addr.sun_path, o.rxSocket.c_str(), sizeof(addr.sun_path));
        if (bind(rx.listenFd, (sockaddr *)&addr, sizeof(addr)) < 0 ||
            listen(rx.listenFd, 1) < 0)
        {
            g_printerr("rx: bind/listen %s: %s\n", o.rxSocket.c_str(),
                       strerror(errno));
            return false;
        }
        rx.socketPath  = o.rxSocket;
        rx.listenWatch = g_unix_fd_add(rx.listenFd, G_IO_IN, rxAcceptCb, nullptr);
    }

    /* Rendering: gst-shm hands decoded frames to the app to composite as
     * a regular QML item (the LuneOS-native path - correct card stacking,
     * no compositor involvement); wl_webos_foreign punch-through remains
     * for OSE-style clients. */
    bool toShm    = !o.shmPath.empty();
    bool onScreen = !toShm && !o.windowId.empty();
    if (onScreen)
    {
        if (!windowManager.registerID(o.windowId.c_str(), nullptr) ||
            !windowManager.attachSurface())
        {
            g_printerr("rx: failed to import window '%s'\n", o.windowId.c_str());
            return false;
        }
        rx.windowAttached = true;
        bool swapped      = o.rotation == 90 || o.rotation == 270;
        rx.renderWidth    = swapped ? o.height : o.width;
        rx.renderHeight   = swapped ? o.width : o.height;
        windowManager.setVideoSize(rx.renderWidth, rx.renderHeight);
        g_print("rx: imported window %s\n", o.windowId.c_str());
    }

    /* The encoded stream carries unrotated sensor frames; phone camera
     * sensors are landscape-mounted, so the caller tells us how far to
     * rotate at render. */
    const char *flip = "";
    switch (((o.rotation % 360) + 360) % 360)
    {
    case 90:
        flip = "videoflip method=clockwise ! ";
        break;
    case 180:
        flip = "videoflip method=rotate-180 ! ";
        break;
    case 270:
        flip = "videoflip method=counterclockwise ! ";
        break;
    }

    GError *err = nullptr;
    gchar *sinks;
    if (toShm)
    {
        bool swapped = o.rotation == 90 || o.rotation == 270;
        int outW     = swapped ? o.height : o.width;
        int outH     = swapped ? o.width : o.height;
        unlink(o.shmPath.c_str());
        /* The leaky queue keeps the decoder running while no reader is
         * draining the shm pool (shmsink blocks once it fills). */
        sinks = g_strdup_printf(
            "videoconvert ! %svideoscale ! "
            "video/x-raw,format=I420,width=%d,height=%d ! "
            "queue leaky=downstream max-size-buffers=4 ! "
            "shmsink name=rxsink socket-path=%s shm-size=8388608 "
            "wait-for-connection=false sync=false",
            flip, outW, outH, o.shmPath.c_str());
    }
    else if (onScreen)
        sinks = g_strdup_printf(
            "videoconvert ! %swaylandsink name=rxsink sync=false", flip);
    else
        sinks = g_strdup("fakesink name=rxsink sync=false");
    gchar *desc = g_strdup_printf(
        "appsrc name=rxsrc is-live=true format=time do-timestamp=true "
        "caps=video/x-h264,stream-format=byte-stream,alignment=au ! "
        "h264parse ! droidvdec ! %s",
        sinks);
    g_free(sinks);
    rx.pipeline = gst_parse_launch(desc, &err);
    g_free(desc);
    if (!rx.pipeline)
    {
        g_printerr("rx pipeline: %s\n", err ? err->message : "?");
        g_clear_error(&err);
        return false;
    }

    rx.appsrc = gst_bin_get_by_name(GST_BIN(rx.pipeline), "rxsrc");

    if (onScreen)
    {
        GstBus *bus = gst_element_get_bus(rx.pipeline);
        gst_bus_set_sync_handler(bus, rxBusSyncCb, nullptr, nullptr);
        gst_object_unref(bus);
    }

    GstElement *sink = gst_bin_get_by_name(GST_BIN(rx.pipeline), "rxsink");
    GstPad *pad      = gst_element_get_static_pad(sink, "sink");
    gst_pad_add_probe(pad, GST_PAD_PROBE_TYPE_BUFFER, rxFrameProbe, nullptr,
                      nullptr);
    gst_object_unref(pad);
    gst_object_unref(sink);

    gst_element_set_state(rx.pipeline, GST_STATE_PLAYING);
    g_print("rx: decoding from %s%s\n", o.rxSocket.c_str(),
            onScreen ? " to screen" : "");
    return true;
}

static void stopRx(void)
{
    if (rx.pipeline)
    {
        gst_element_set_state(rx.pipeline, GST_STATE_NULL);
        if (rx.appsrc)
            gst_object_unref(rx.appsrc);
        gst_object_unref(rx.pipeline);
        rx.pipeline = nullptr;
        rx.appsrc   = nullptr;
    }
    if (rx.windowAttached)
    {
        windowManager.detachSurface();
        windowManager.unregisterID();
        rx.windowAttached = false;
    }
    if (rx.peerWatch)
        g_source_remove(rx.peerWatch);
    if (rx.listenWatch)
        g_source_remove(rx.listenWatch);
    rx.peerWatch = rx.listenWatch = 0;
    if (rx.peerFd >= 0)
        close(rx.peerFd);
    if (rx.listenFd >= 0)
        close(rx.listenFd);
    rx.peerFd = rx.listenFd = -1;
    if (!rx.socketPath.empty())
        unlink(rx.socketPath.c_str());
    rx.socketPath.clear();
}

/* ---------------- session ---------------- */

static const char *startSession(const Options &o, bool withResources)
{
    if (sessionRunning)
        return "already running";

    bool wantTx = !o.txSocket.empty() || o.loopback;
    bool wantRx = !o.rxSocket.empty() || o.loopback;

    const char *resourceState = "skipped";
    if (withResources)
        resourceState = acquireCodecs(wantTx, wantRx);
    if (g_strcmp0(resourceState, "denied") == 0)
        return "resource acquisition denied";

    txAUs = rxAUs = rxFrames = 0;
    /* rx first so a loopback tx has the appsrc to push into */
    if (wantRx && !startRx(o))
    {
        stopRx();
        releaseCodecs();
        return "rx start failed";
    }
    if (wantTx && !startTx(o))
    {
        stopTx();
        stopRx();
        releaseCodecs();
        return "tx start failed";
    }
    sessionRunning = true;
    return nullptr;
}

static void stopSession(void)
{
    if (!sessionRunning)
        return;
    stopTx();
    stopRx();
    releaseCodecs();
    sessionRunning = false;
    g_print("session stopped\n");
}

/* ---------------- LS2 service ---------------- */

static LSHandle *lsHandle;

static void lsReply(LSHandle *sh, LSMessage *msg, const char *payload)
{
    LSError e;
    LSErrorInit(&e);
    if (!LSMessageReply(sh, msg, payload, &e))
    {
        g_printerr("ls reply: %s\n", e.message);
        LSErrorFree(&e);
    }
}

static bool svcStart(LSHandle *sh, LSMessage *msg, void *)
{
    pbnjson::JDomParser parser;
    pbnjson::JValue v = pbnjson::Object();
    const char *raw   = LSMessageGetPayload(msg);
    if (raw && parser.parse(raw, pbnjson::JSchema::AllSchema()))
        v = parser.getDom();

    Options o;
    o.txSocket = v["txSocket"].isString() ? v["txSocket"].asString()
                                          : std::string("/tmp/rtc-tx.sock");
    o.rxSocket = v["rxSocket"].isString() ? v["rxSocket"].asString()
                                          : std::string("/tmp/rtc-rx.sock");
    if (v["windowId"].isString())
        o.windowId = v["windowId"].asString();
    if (v["shmPath"].isString())
        o.shmPath = v["shmPath"].asString();
    if (v["camera"].isNumber())
        o.camera = v["camera"].asNumber<int32_t>();
    if (v["bitrate"].isNumber())
        o.bitrate = v["bitrate"].asNumber<int32_t>();
    if (v["width"].isNumber())
        o.width = v["width"].asNumber<int32_t>();
    if (v["height"].isNumber())
        o.height = v["height"].asNumber<int32_t>();
    if (v["rotation"].isNumber())
        o.rotation = v["rotation"].asNumber<int32_t>();
    if (v["loopback"].isBoolean() && v["loopback"].asBool())
    {
        /* self-view demo: encoded AUs feed straight back into the
         * decoder, no sockets involved unless explicitly requested */
        o.loopback = true;
        if (!v["txSocket"].isString())
            o.txSocket.clear();
        if (!v["rxSocket"].isString())
            o.rxSocket.clear();
    }
    if (v["txEnabled"].isBoolean() && !v["txEnabled"].asBool())
        o.txSocket.clear();
    if (v["rxEnabled"].isBoolean() && !v["rxEnabled"].asBool())
        o.rxSocket.clear();

    const char *error = startSession(o, true);
    gchar *reply;
    if (error)
        reply = g_strdup_printf(
            "{\"returnValue\":false,\"errorText\":\"%s\"}", error);
    else
        reply = g_strdup_printf(
            "{\"returnValue\":true,\"txSocket\":\"%s\",\"rxSocket\":\"%s\"}",
            o.txSocket.c_str(), o.rxSocket.c_str());
    lsReply(sh, msg, reply);
    g_free(reply);
    return true;
}

static bool svcStop(LSHandle *sh, LSMessage *msg, void *)
{
    bool wasRunning = sessionRunning;
    stopSession();
    gchar *reply = g_strdup_printf(
        "{\"returnValue\":true,\"wasRunning\":%s}", wasRunning ? "true" : "false");
    lsReply(sh, msg, reply);
    g_free(reply);
    return true;
}

/* Camera presence, probed from droidcamsrc's camera-device property
 * range at READY (the gst-droid notifier pattern): the platform may
 * ship the whole video-call stack yet have no physical camera. */
static int probeCameraCount(void)
{
    static int cached = -1;
    if (cached >= 0)
        return cached;
    GstElement *cam = gst_element_factory_make("droidcamsrc", nullptr);
    if (!cam)
        return cached = 0;
    int count = 0;
    if (gst_element_set_state(cam, GST_STATE_READY) != GST_STATE_CHANGE_FAILURE)
    {
        GParamSpec *spec = g_object_class_find_property(
            G_OBJECT_GET_CLASS(cam), "camera-device");
        if (spec && G_IS_PARAM_SPEC_INT(spec))
            count = G_PARAM_SPEC_INT(spec)->maximum + 1;
        gst_element_set_state(cam, GST_STATE_NULL);
    }
    gst_object_unref(cam);
    return cached = count;
}

static bool svcCapabilities(LSHandle *sh, LSMessage *msg, void *)
{
    int cameras  = probeCameraCount();
    gchar *reply = g_strdup_printf(
        "{\"returnValue\":true,\"cameraCount\":%d,\"videoCallCapable\":%s}",
        cameras, cameras > 0 ? "true" : "false");
    lsReply(sh, msg, reply);
    g_free(reply);
    return true;
}

static bool svcStatus(LSHandle *sh, LSMessage *msg, void *)
{
    gchar *reply = g_strdup_printf(
        "{\"returnValue\":true,\"running\":%s,\"txAus\":%" G_GUINT64_FORMAT
        ",\"rxAus\":%" G_GUINT64_FORMAT ",\"rxFrames\":%" G_GUINT64_FORMAT "}",
        sessionRunning ? "true" : "false", txAUs, rxAUs, rxFrames);
    lsReply(sh, msg, reply);
    g_free(reply);
    return true;
}

static LSMethod serviceMethods[] = {
    {"start", svcStart, LUNA_METHOD_FLAGS_NONE},
    {"stop", svcStop, LUNA_METHOD_FLAGS_NONE},
    {"status", svcStatus, LUNA_METHOD_FLAGS_NONE},
    {"capabilities", svcCapabilities, LUNA_METHOD_FLAGS_NONE},
    {nullptr, nullptr, LUNA_METHOD_FLAGS_NONE},
};

static bool startService(void)
{
    LSError e;
    LSErrorInit(&e);
    if (!LSRegister("org.webosports.rtcengine", &lsHandle, &e) ||
        !LSRegisterCategory(lsHandle, "/", serviceMethods, nullptr, nullptr, &e) ||
        !LSGmainAttach(lsHandle, loop, &e))
    {
        g_printerr("LS2 registration failed: %s\n", e.message);
        LSErrorFree(&e);
        return false;
    }
    g_print("registered org.webosports.rtcengine\n");
    return true;
}

/* ---------------- main ---------------- */

static gboolean statsCb(gpointer)
{
    if (sessionRunning)
        g_print("stats: tx_aus=%" G_GUINT64_FORMAT " rx_aus=%" G_GUINT64_FORMAT
                " rx_frames=%" G_GUINT64_FORMAT "\n",
                txAUs, rxAUs, rxFrames);
    return G_SOURCE_CONTINUE;
}

static gboolean quitCb(gpointer)
{
    g_main_loop_quit(loop);
    return G_SOURCE_REMOVE;
}

int main(int argc, char **argv)
{
    /* ls-hubd launched services have a minimal environment; the compositor
     * socket lives in the LuneOS runtime dir. */
    setenv("XDG_RUNTIME_DIR", "/tmp/xdg", 0);

    gst_init(&argc, &argv);

    Options o;
    bool serviceMode = false;
    for (int i = 1; i < argc; i++)
    {
        std::string a = argv[i];
        auto val      = [&a](const char *k) -> const char * {
            size_t l = strlen(k);
            return a.compare(0, l, k) == 0 ? a.c_str() + l : nullptr;
        };
        if (const char *v = val("--tx="))
            o.txSocket = v;
        else if (const char *v = val("--rx="))
            o.rxSocket = v;
        else if (const char *v = val("--camera="))
            o.camera = atoi(v);
        else if (const char *v = val("--bitrate="))
            o.bitrate = atoi(v);
        else if (const char *v = val("--window-id="))
            o.windowId = v;
        else if (const char *v = val("--rotation="))
            o.rotation = atoi(v);
        else if (const char *v = val("--shm="))
            o.shmPath = v;
        else if (a == "--loopback")
            o.loopback = true;
        else if (a == "--service")
            serviceMode = true;
    }

    if (!serviceMode && o.txSocket.empty() && o.rxSocket.empty() && !o.loopback)
    {
        g_printerr("usage: %s [--service] [--tx=SOCK] [--rx=SOCK] [--camera=N] "
                   "[--bitrate=BPS] [--window-id=ID] [--rotation=DEG] "
                   "[--loopback]\n",
                   argv[0]);
        return 1;
    }

    loop = g_main_loop_new(nullptr, FALSE);

    if (serviceMode && !startService())
        return 1;

    if (!o.txSocket.empty() || !o.rxSocket.empty() || o.loopback)
    {
        const char *error = startSession(o, false);
        if (error)
        {
            g_printerr("start: %s\n", error);
            if (!serviceMode)
                return 1;
        }
    }

    g_timeout_add_seconds(2, statsCb, nullptr);
    g_unix_signal_add(SIGINT, quitCb, nullptr);
    g_unix_signal_add(SIGTERM, quitCb, nullptr);

    g_main_loop_run(loop);

    stopSession();
    return 0;
}
