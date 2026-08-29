# luneos-rtc-engine

Real-time video media engine for calls on LuneOS Halium devices.

It bridges the device cameras and hardware codecs to a call network
stack. Hardware-encoded H.264 access units stream out of (and decode
back in through) `SOCK_SEQPACKET` unix sockets, one access unit per
datagram — the interface shape messaging call engines (for example
meowcaller's `Call.SendVideo`/`Call.ReceiveVideo` for WhatsApp)
exchange video in.

```
 TX: camera → H.264 encode → h264parse (AU aligned) → appsink → tx socket

 RX: rx socket → appsrc → h264parse → H.264 decode
     → gst-shm for in-app rendering (RtcVideoFactory + VideoOutput),
       a wl_webos_foreign imported window, or a fakesink
```

Two capture/codec backends, chosen at runtime and reported by
`capabilities`:

- **droid** (Halium devices): droidcamsrc recorder mode — the hardware
  encoder attached to the camera session, viewfinder-independent — and
  droidvdec hardware decode.
- **v4l2** (mainline kernels — PineTab2, PinePhone, PinePhone Pro):
  v4l2src on the capture-capable video node (`videoDevice` overrides the
  `camera` index mapping), encoding through `v4l2h264enc` when the
  kernel offers a stateful encoder, else `x264enc`/`openh264enc`;
  decoding through `v4l2slh264dec`/`v4l2h264dec`, else software.

## Control planes

Standalone:

```
luneos-rtc-engine --tx=/tmp/rtc-tx.sock --rx=/tmp/rtc-rx.sock \
                  --camera=0 --bitrate=2000000 --window-id=_Window_Id_1
```

LS2 (`--service`, bus-activated as `org.webosports.rtcengine`):

```
luna-send -n 1 luna://org.webosports.rtcengine/start \
    '{"camera":0,"bitrate":2000000,"windowId":"_Window_Id_1"}'
luna-send -n 1 luna://org.webosports.rtcengine/status '{}'
luna-send -n 1 luna://org.webosports.rtcengine/stop '{}'
```

`start` acquires VENC/VDEC units from uMediaServer's resource manager
and a resource policy action stops the session, so calls coexist with
camera recording and media playback.

The `capabilities` method reports the physical camera count and whether
video calling should be offered. A device adaptation can overrule the
derived answer with `deviceinfo_video_call="false"` (or `"true"`) —
luneos-device-config publishes it to `/run/luneos-device/video-call` —
because a HAL can enumerate cameras that are wired up but not usable.

## Notes

- `src/camera_window_manager.h` is the public header of
  `libcamera-window-manager` (webOS OSE g-camera-pipeline, Apache-2.0),
  vendored because the library ships no dev headers.
- droidcamsrc needs gst-droid with the recorder-in-raw-preview patch so
  the hardware encoder can run while the viewfinder branch stays live.
- The camera takes a few seconds to open; the engine defers switching to
  video mode and starting the capture accordingly.
