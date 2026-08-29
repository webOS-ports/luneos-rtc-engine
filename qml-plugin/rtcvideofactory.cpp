// Copyright (c) 2026 LuneOS project
// SPDX-License-Identifier: Apache-2.0

#include "rtcvideofactory.h"

#include <QDebug>

#if __has_include(<QtMultimedia/spi/qgstreamervideosource.h>)
#include <QtMultimedia/spi/qgstreamervideosource.h>
#define HAVE_QGSTREAMER_VIDEO_SOURCE 1
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
    /* The caps must state the exact frame layout: gst-shm carries no
     * negotiation, just bytes. The tail videoconvert is the bin's only
     * unlinked pad, which QGStreamerVideoSource ghosts as its source. */
    const QString desc =
        QStringLiteral("shmsrc socket-path=%1 is-live=true do-timestamp=true ! "
                       "video/x-raw,format=I420,width=%2,height=%3 ! "
                       "queue ! videoconvert")
            .arg(socketPath)
            .arg(width)
            .arg(height);
    qInfo() << "RtcVideoFactory: creating shm video source:" << desc;
    return new QGStreamerVideoSource(desc, this);
#else
    qWarning() << "RtcVideoFactory: QtMultimedia GStreamer SPI not available";
    return nullptr;
#endif
}
