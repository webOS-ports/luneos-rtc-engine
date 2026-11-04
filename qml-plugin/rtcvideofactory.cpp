// Copyright (c) 2026 LuneOS project
// SPDX-License-Identifier: Apache-2.0

#include "rtcvideofactory.h"

#include <QDebug>

#if __has_include(<QtMultimedia/spi/qgstreamervideosource.h>)
#include <QtMultimedia/spi/qgstreamervideosource.h>
#define HAVE_QGSTREAMER_VIDEO_SOURCE 1
#include "rtcshmreader.h"
#endif

RtcVideoFactory::RtcVideoFactory(QObject *parent) : QObject(parent) {}

bool RtcVideoFactory::available() const
{
#ifdef HAVE_QGSTREAMER_VIDEO_SOURCE
    return true;
#else
    return false;
#endif
}

QObject *RtcVideoFactory::createShmSource(const QString &socketPath, int width,
                                          int height)
{
#ifdef HAVE_QGSTREAMER_VIDEO_SOURCE
    /* Frames are copied out of the engine's shared memory as they come
     * (rtcshmreader.h): otherwise frames waiting for the GUI thread hold
     * the engine's shm area and stall its output. */
    const QString desc = QString::fromStdString(
        rtcShmReaderDescription(socketPath.toStdString(), width, height));
    qInfo() << "RtcVideoFactory: creating shm video source:" << desc;
    auto *source = new QGStreamerVideoSource(desc, this);
    if (!rtcShmReaderDetach(source->gstElement()))
        qWarning() << "RtcVideoFactory: frames stay in shared memory:"
                   << "no" << RTC_SHM_READER_SRC << "in" << desc;
    return source;
#else
    qWarning() << "RtcVideoFactory: QtMultimedia GStreamer SPI not available";
    return nullptr;
#endif
}

void RtcVideoFactory::stopShmSource(QObject *source)
{
#ifdef HAVE_QGSTREAMER_VIDEO_SOURCE
    auto *video = qobject_cast<QGStreamerVideoSource *>(source);
    if (!video || !rtcShmReaderStop(video->gstElement()))
        qWarning() << "RtcVideoFactory: stopShmSource: not a shm video source";
#else
    Q_UNUSED(source);
#endif
}
