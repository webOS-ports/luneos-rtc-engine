# luneos-rtc-engine

Real-time video media engine for calls on LuneOS Halium devices.

It bridges the device cameras and hardware codecs to a call network
stack. Hardware-encoded H.264 access units stream out of (and decode
back in through) `SOCK_SEQPACKET` unix sockets, one access unit per
datagram — the interface shape messaging call engines (for example
meowcaller's `Call.SendVideo`/`Call.ReceiveVideo` for WhatsApp)
exchange video in.

```
 TX: droidcamsrc (recorder mode, hardware H.264, viewfinder-independent)
     → h264parse (AU aligned) → appsink → tx socket

 RX: rx socket → appsrc → h264parse → droidvdec (hardware decode)
     → waylandsink rendered into a wl_webos_foreign imported window
       the call UI exports (or a fakesink when headless)
```

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

## Notes

- `src/camera_window_manager.h` is the public header of
  `libcamera-window-manager` (webOS OSE g-camera-pipeline, Apache-2.0),
  vendored because the library ships no dev headers.
- droidcamsrc needs gst-droid with the recorder-in-raw-preview patch so
  the hardware encoder can run while the viewfinder branch stays live.
- The camera takes a few seconds to open; the engine defers switching to
  video mode and starting the capture accordingly.
