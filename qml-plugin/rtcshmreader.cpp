// Copyright (c) 2026 LuneOS project
// SPDX-License-Identifier: Apache-2.0

#include "rtcshmreader.h"

std::string rtcShmReaderDescription(const std::string &socketPath, int width,
                                    int height)
{
    /* The caps must fully specify the frame layout - gst-shm carries no
     * negotiation, just bytes, and capsfilter refuses to stamp buffers
     * with incomplete caps (framerate included). The queue holds copies
     * (see rtcShmReaderDetach) and drops the oldest past two. The tail
     * videoconvert is the bin's only unlinked pad, which
     * QGStreamerVideoSource ghosts as its source. */
    return "shmsrc name=" RTC_SHM_READER_SRC " socket-path=" + socketPath +
           " is-live=true do-timestamp=true ! "
           "video/x-raw,format=I420,width=" +
           std::to_string(width) + ",height=" + std::to_string(height) +
           ",framerate=30/1,pixel-aspect-ratio=1/1,"
           "interlace-mode=progressive,colorimetry=bt601 ! "
           "queue name=" RTC_SHM_READER_QUEUE " leaky=downstream "
           "max-size-buffers=2 max-size-bytes=0 max-size-time=0 ! "
           "videoconvert";
}

namespace {

/* Streaming thread of shmsrc: hand on a copy in ordinary memory and let go
 * of the shared one, which tells the writer the block is free. */
GstPadProbeReturn copyOutOfShm(GstPad * /*pad*/, GstPadProbeInfo *info,
                               gpointer /*data*/)
{
    GstBuffer *shared = GST_PAD_PROBE_INFO_BUFFER(info);
    if (!shared)
        return GST_PAD_PROBE_OK;
    GstBuffer *copy = gst_buffer_copy_deep(shared);
    gst_buffer_unref(shared);
    if (!copy)
    {
        /* Out of memory: the frame is lost, the writer not held up. */
        GST_PAD_PROBE_INFO_DATA(info) = nullptr;
        return GST_PAD_PROBE_HANDLED;
    }
    GST_PAD_PROBE_INFO_DATA(info) = copy;
    return GST_PAD_PROBE_OK;
}

} // namespace

bool rtcShmReaderDetach(GstElement *bin)
{
    if (!bin || !GST_IS_BIN(bin))
        return false;
    GstElement *src = gst_bin_get_by_name(GST_BIN(bin), RTC_SHM_READER_SRC);
    if (!src)
        return false;
    GstPad *pad = gst_element_get_static_pad(src, "src");
    gst_object_unref(src);
    if (!pad)
        return false;
    const gulong id = gst_pad_add_probe(pad, GST_PAD_PROBE_TYPE_BUFFER,
                                  copyOutOfShm, nullptr, nullptr);
    gst_object_unref(pad);
    return id != 0;
}
