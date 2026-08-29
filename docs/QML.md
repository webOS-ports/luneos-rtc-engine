# LuneOS.Foreign QML module

Installed by this package to `/usr/lib/qml/LuneOS/Foreign`. Two types,
registered at version 1.0:

```qml
import LuneOS.Foreign 1.0
```

## RtcVideoFactory (singleton)

Bridges the engine's gst-shm frame output into Qt Multimedia, so an app
renders received call video as a regular `VideoOutput` — normal card
behavior in the shell, no punch-through, no extra surfaces.

| Member | Meaning |
|---|---|
| `bool available()` | `true` when Qt Multimedia was built with the GStreamer SPI (`FEATURE_gstreamer_qt_api`); `false` means `createShmSource` returns null |
| `QObject *createShmSource(socketPath, width, height)` | A native video source reading I420 frames from the engine's `shmPath` socket. Assign to `CaptureSession.nativeVideoSource`, then call `start()` on it |

`width`/`height` must match the engine session's frame size *after* its
`rotation` — swap them for 90/270 (a 1280×720 session with
`rotation: 90` delivers 720×1280 frames). gst-shm carries raw bytes
with no negotiation, so the caps are stamped from these values; a
mismatch stalls the source with a caps error.

Usage, as the Phone app's `VideoCallOverlay.qml` does it:

```qml
CaptureSession { id: captureSession }
VideoOutput {
    id: videoOutput
    fillMode: VideoOutput.PreserveAspectFit
}

function startVideo(shmPath, frameWidth, frameHeight) {
    // pair with: luna://org.webosports.rtcengine/start
    //   '{"camera":0,"shmPath":"<shmPath>","rotation":90}'
    var source = RtcVideoFactory.createShmSource(shmPath, frameWidth, frameHeight);
    if (source) {
        captureSession.nativeVideoSource = source;
        source.start();
    }
}

function stopVideo() {
    if (captureSession.nativeVideoSource) {
        captureSession.nativeVideoSource.stop();
        captureSession.nativeVideoSource = null;
    }
    // then: luna://org.webosports.rtcengine/stop '{}'
}
```

## ForeignExportedRegion (item)

Exports the item's on-screen rectangle through `wl_webos_foreign` so an
*external* process (the engine's `windowId` render path, or any OSE-style
video pipeline) can punch video through the compositor into it. For
video rendered *by the app itself*, prefer `RtcVideoFactory` — under the
LuneOS shell, punch-through composites behind the app card; the foreign
path exists for OSE-style clients.

All wayland traffic runs on a dedicated event queue, so Qt's own
wayland dispatch is never disturbed.

| Member | Meaning |
|---|---|
| `windowId` (string, read-only) | Compositor-assigned id once exported (`""` before). Pass to the engine's `start` as `windowId` |
| `exported` (bool, read-only) | Convenience: `windowId !== ""` |
| `updateRegion()` | Re-export after the item's geometry changed (a resize/move the item does not pick up itself) |

```qml
ForeignExportedRegion {
    id: videoRegion
    anchors.fill: parent
    onWindowIdChanged: if (exported) startEngineWith(windowId)
}
```
