// Copyright (c) 2026 LuneOS project
// SPDX-License-Identifier: Apache-2.0

#ifndef RTCVIDEOFACTORY_H
#define RTCVIDEOFACTORY_H

#include <QObject>

/**
 * Creates QtMultimedia native video sources for the decoded-frame
 * streams luneos-rtc-engine publishes over gst-shm, so an app renders
 * call video in a plain VideoOutput:
 *
 *   captureSession.nativeVideoSource =
 *       RtcVideoFactory.createShmSource("/tmp/rtc-video", 720, 1280);
 *   captureSession.nativeVideoSource.start();
 *
 * The width/height must match what the engine was started with (after
 * rotation): gst-shm transports raw bytes, both sides must agree on the
 * frame layout.
 */
class RtcVideoFactory : public QObject
{
    Q_OBJECT

public:
    explicit RtcVideoFactory(QObject *parent = nullptr);

    Q_INVOKABLE bool available() const;
    Q_INVOKABLE QObject *createShmSource(const QString &socketPath, int width,
                                         int height);
};

#endif // RTCVIDEOFACTORY_H
