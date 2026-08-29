# luneos-rtc-engine API reference

Three surfaces: the LS2 service (`org.webosports.rtcengine`), the AU
socket wire contract that call connectors speak, and the standalone
CLI. The droidmedia runtime-control C API the droid backend uses is
documented at the end.

One session runs at a time. `start` while a session is running fails
with `already running` — call `stop` first.

## LS2 service: `org.webosports.rtcengine`

Bus-activated (ls-hubd launches the binary with `--service` on first
call). All methods live in the ACG group `rtc.operation`. Every reply
carries `returnValue`; failures carry `errorText` instead of the
success fields.

### start

Starts a session: opens the camera, brings up the TX encode chain and
the RX decode chain, and acquires `VENC`/`VDEC` units from
uMediaServer's resource manager. A uMS policy action (another client
needs the codecs) stops the session gracefully.

Parameters (all optional):

| Key | Type | Default | Meaning |
|---|---|---|---|
| `txSocket` | string | `/tmp/rtc-tx.sock` | Path the engine listens on for the TX (outgoing AU) client |
| `rxSocket` | string | `/tmp/rtc-rx.sock` | Path the engine listens on for the RX (incoming AU) client |
| `txEnabled` | boolean | `true` | `false` disables the TX socket entirely |
| `rxEnabled` | boolean | `true` | `false` disables the RX socket entirely |
| `camera` | number | `0` | Camera index (droid: HAL camera id; v4l2: index into the capture-capable nodes) |
| `videoDevice` | string | — | v4l2 only: explicit capture node (e.g. `/dev/video1`), overrides `camera` |
| `width` | number | `1280` | Encoded width |
| `height` | number | `720` | Encoded height |
| `bitrate` | number | `2000000` | Target video bitrate in bits/s |
| `rotation` | number | `0` | Degrees clockwise applied at render on the RX path: 0, 90, 180 or 270 |
| `windowId` | string | — | Render RX video into this `wl_webos_foreign` exported window (compositor punch-through; OSE-style clients) |
| `shmPath` | string | — | Publish decoded RX frames on this gst-shm socket for in-app rendering (see docs/QML.md) |
| `loopback` | boolean | `false` | Feed TX access units straight back into the decoder (self-view without a network stack). Sockets are then disabled unless explicitly passed |

The frame rate is fixed at 30 fps.

Success response:

```json
{"returnValue": true, "txSocket": "/tmp/rtc-tx.sock", "rxSocket": "/tmp/rtc-rx.sock"}
```

Failure response: `{"returnValue": false, "errorText": "<reason>"}` —
the reasons are `already running`, `resource acquisition denied` (uMS
refused the VENC/VDEC units), `tx start failed` and `rx start failed`
(socket bind, missing encoder/decoder, window import). Note the droid
camera opens asynchronously: a hardware camera failure can surface
*after* a successful `start` — a session whose `txAus` stays at 0 in
`status` never got frames.

### stop

Stops the session, releases the camera and the uMS `VENC`/`VDEC`
units, closes the sockets.

```json
{"returnValue": true, "wasRunning": true}
```

### status

```json
{
  "returnValue": true,
  "running": true,
  "txAus": 16691,      // access units sent to the TX client
  "txBytes": 12345678, // sum of TX AU payload bytes
  "txKeyframes": 9,    // AUs containing an IDR NAL (detected by NAL scan)
  "rxAus": 16691,      // access units received from the RX client
  "rxFrames": 16665,   // frames the decoder produced
  "txProfile": "baseline",  // negotiated H.264 profile ("?" until known)
  "txLevel": "3.1"          // negotiated H.264 level  ("?" until known)
}
```

The counters let a connector's congestion controller observe the real
keyframe cadence and byte rate instead of guessing.

### capabilities

Answers "should this device offer video calling", and how the engine
would run. Callable without a session; this is what UIs gate their
video button on (apps cannot query `com.webos.service.camera2`
directly — its ACG groups do not admit application clients).

```json
{
  "returnValue": true,
  "cameraCount": 2,           // physical cameras the backend can open
  "videoCallCapable": true,
  "policy": "derived",        // "derived" (from cameraCount) or "deviceinfo"
  "backend": "droid",         // "droid" or "v4l2"
  "decoder": "droidvdec"      // decoder element that would be used, or "none"
}
```

A device adaptation overrules the derived answer with the Tier-1
variable `deviceinfo_video_call="true"|"false"`, which
luneos-device-config publishes to `/run/luneos-device/video-call` at
boot. Use it when the HAL enumerates cameras that are wired up but not
actually usable.

### requestKeyframe

Ask the encoder for an IDR now (the PLI/`RequestKeyFrame` analog).
Requires an active TX chain.

```json
{"returnValue": true, "method": "force-key-unit"}
```

`method` reports what actually happened:

- `"force-key-unit"` — an on-demand IDR was requested (v4l2 always;
  droid when the container's libdroidmedia exports the runtime encoder
  controls).
- `"periodic-idr"` — no on-demand path on this stack; the vendor
  encoder's periodic GOP provides recovery (~2 s on sargo's Venus —
  watch `txKeyframes` for the real cadence).

Failure: `{"returnValue": false, "errorText": "no active tx"}`.

### setBitrate

Change the target video bitrate (the SCReAM/congestion-controller
analog). Requires an active TX chain.

Parameters: `bitrate` (number, bits/s).

```json
{"returnValue": true, "bitrate": 800000, "applied": "live"}
```

`applied` is `"live"` on both backends: v4l2 encoders take the new rate
directly, and the droid path hands it to gst-droid's live recorder.
One honesty caveat on droid: the engine cannot observe whether the
container's libdroidmedia actually supports the runtime control — on an
older Android side the value is retained but only takes effect when the
next session starts. `txBytes` in `status` shows whether the rate
really moved.

Failure: `{"returnValue": false, "errorText": "no active tx ..."}`.

## AU socket wire contract

This is the boundary a call connector (meowcaller, libtgvoip/tgcalls,
the Teams NGC stack, …) plugs into.

- Both sockets are `AF_UNIX` / `SOCK_SEQPACKET`. The **engine binds and
  listens** on the paths returned by `start`; the connector connects as
  a client. One client per socket (backlog 1); a new connection
  replaces the previous peer.
- **One complete H.264 access unit per datagram**, byte-stream
  (Annex-B) format with start codes, AU-aligned. `SOCK_SEQPACKET`
  preserves message boundaries — no length prefix is needed, and
  partial reads do not occur. Size your receive buffer for keyframes
  (hundreds of KB at HD resolutions).
- **TX socket** (engine → connector): every encoded AU, exactly as the
  network side should send it (`Call.SendVideo(auBytes)`). The first AU
  of a session carries AUD/SPS/PPS/IDR; parameter sets repeat on the
  encoder's IDR cadence.
- **RX socket** (connector → engine): write one complete received AU
  per `send()` (`Call.ReceiveVideo` delivery). The engine parses and
  decodes; out-of-band parameter sets are not required as long as AUs
  arrive intact from an H.264 byte-stream.
- Disconnecting a socket does not stop the session; reconnect and the
  stream continues (after a TX reconnect, wait for the next IDR — or
  call `requestKeyframe` — before feeding a decoder downstream).

## Standalone CLI

For development without the bus:

```
luneos-rtc-engine [--service] [--tx=SOCK] [--rx=SOCK] [--camera=N]
                  [--bitrate=BPS] [--window-id=ID] [--rotation=DEG]
                  [--shm=PATH] [--video-device=NODE] [--loopback]
```

`--service` registers on LS2 (this is what the systemd/bus activation
uses); the other flags start an immediate session with the same
semantics as the LS2 `start` parameters.

## droidmedia runtime encoder controls (C API)

The droid backend's live `requestKeyframe`/`setBitrate` support rides
on two functions added to droidmedia (branch
`luneos/recorder-runtime-params`; forwarded to the recorder codec's
`MediaCodec::setParameters` with `request-sync` / `video-bitrate`):

```c
bool droid_media_recorder_request_sync_frame(DroidMediaRecorder *recorder);
bool droid_media_recorder_set_video_bitrate(DroidMediaRecorder *recorder,
                                            int32_t bitrate);
```

The glibc-side glue resolves both symbols tolerantly at runtime: against
a container libdroidmedia that predates them, the functions return
`false` and the engine reports the honest fallback semantics
(`periodic-idr` / `next-session`) instead of failing.
