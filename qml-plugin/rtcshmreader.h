// Copyright (c) 2026 LuneOS project
// SPDX-License-Identifier: Apache-2.0

#ifndef RTCSHMREADER_H
#define RTCSHMREADER_H

#include <string>

#include <gst/gst.h>

/*
 * The reading end of an engine gst-shm stream, without Qt, so it can be
 * tested on its own.
 *
 * A frame read from gst-shm stays in the writer's shared memory until the
 * reader drops its last reference to it, and the writer (shmsink) stops
 * until there is room again. Qt Multimedia's video sink keeps the frames it
 * has been given in an unbounded queue until the GUI thread draws them, so a
 * busy GUI thread - two pictures on screen in a video call - could hold the
 * whole area and stall the engine's output. The reader therefore copies each
 * frame out of the shared memory as it arrives (rtcShmReaderDetach) and keeps
 * at most two copies queued, dropping the oldest, so what the app is slow
 * to draw costs it frames, never the engine.
 */

/* Element names in the description, for rtcShmReaderDetach and tests. */
#define RTC_SHM_READER_SRC   "rtcshmsrc"
#define RTC_SHM_READER_QUEUE "rtcshmqueue"

/* A gst-launch description of the reader: I420 frames of width x height at
 * socketPath, ending in an unlinked videoconvert (the bin's source pad). */
std::string rtcShmReaderDescription(const std::string &socketPath, int width,
                                    int height);

/* Makes the reader in bin (built from the description above) copy every
 * frame out of the shared memory as it arrives. False when bin has no such
 * reader. */
bool rtcShmReaderDetach(GstElement *bin);

/* Stops the reader in bin and clears out what it already passed on, before
 * Qt Multimedia tears the source down (QGStreamerVideoSource::stop()).
 *
 * Qt unlinks the source in an IDLE pad probe whose data lives on its stack.
 * A streaming thread pushing a non-serialized upstream event (the video
 * sink's QoS after every frame) through that pad runs the probe a second
 * time, possibly after Qt has returned: a crash in that thread (the Phone
 * app on sargo, 2026-11-04, camera turned off during a video call). With
 * the reader stopped and the frames already downstream flushed, the sink
 * renders nothing more and sends no QoS, so no thread but Qt's is at the
 * pad. The bin cannot be started again afterwards. False when bin has no
 * such reader. */
bool rtcShmReaderStop(GstElement *bin);

#endif // RTCSHMREADER_H
