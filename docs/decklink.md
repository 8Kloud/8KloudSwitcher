# DeckLink SDI input

8-bit UYVY capture from Blackmagic DeckLink cards, up to 8K. Implemented in
`src/decklink/` behind the same `IInputSource` seam as the network inputs.

## Why this input is cheap

`bmdFormat8BitYUV` **is** UYVY 4:2:2 — byte-identical to our native
`PixFmt::UYVY8_422`. The SDK hands frames to a callback in host memory, exactly
the shape `NdiReceiver`/`OmtInput` produce, so a captured frame goes straight
into the upload ring with no pixel conversion and no CUDA interop.

The one structural difference: there is no capture thread of our own. The SDK
delivers on its own callback thread and the staging copy + upload submit happen
there. An open thread runs only to (re)open the device.

Frame sync gets a real hardware clock here — `GetHardwareReferenceTimestamp` is
the card's own reference, so pts is never synthesized and `senderClock` is
always true. This is the most accurate sync source in the switcher; the network
inputs fall back to a synthesized arrival grid when a sender's timestamps are
unusable, and this one cannot.

## Building

Headers are vendored (gitignored, like the NDI and OMT SDKs):

```sh
mkdir -p third_party/decklink/include
cp "$SDK/Linux/include"/*.h "$SDK/Linux/include/DeckLinkAPIDispatch.cpp" \
   third_party/decklink/include/
```

`DECKLINK_SDK_DIR` overrides the location. There is **no link-time library**:
`DeckLinkAPIDispatch.cpp` dlopens `libDeckLinkAPI.so` from the installed
Desktop Video driver at runtime, so the build does not depend on the driver
being present — only running does. Absent the headers, the input compiles out
and a `decklink://` spec logs an error and stays dark.

## Refs

```
decklink://0                            device index 0, mode auto-detected
decklink://0@4320p59.94                 forced mode
decklink://DeckLink 8K Pro (1)          device by display name
decklink://DeckLink 8K Pro (1)@2160p60  ... with a forced mode
```

Auto-detect is the default and is what you want for SDI: the card reports the
incoming mode and the input follows it, including mid-show format changes.
Forced modes exist for sources the card cannot detect. Mode names must match
the card's own spelling (`4320p59.94`, not `8Kp59.94`); an unknown name logs a
warning and falls back to auto-detect.

CLI: `--decklink-input 0` (a bare index is accepted and expanded). GUI: the
input picker lists cards as `SDI · <card name>`. Show files store
`type=decklink`.

## Profiles — one 8K input *or* four 4K inputs, never both

This is the thing that surprises people. A DeckLink 8K Pro presents **four
sub-devices**, and the active *profile* decides how the four 12G-SDI connectors
are divided between them:

| Profile | Capture shape |
|---|---|
| 1 sub-device, full duplex | one 8K input (quad-link) + playback |
| 1 sub-device, half duplex | one 8K input |
| 2 sub-devices, full duplex | two channels, ≤4K each |
| 4 sub-devices, half duplex | four independent inputs, ≤4K each |

In a one-sub-device profile, **device 0 owns all four connectors and devices
1–3 accept no input at all** — they have no capture interface, and opening them
logs `device has no input interface`. Switch profiles in Blackmagic Desktop
Video Setup (or via `IDeckLinkProfileManager`). The setting persists until
reboot.

Measured on the dev box's 8K Pro, device 0 offers 88 input modes, all of them
supporting both `8BitYUV` and `10BitYUV`: SD/720p/1080p (to 120p), 2K DCI,
2160p and 4K DCI (to 120p), **4320p 7680×4320 to 60p**, and **8K DCI 8192×4320
to 60p**.

## Troubleshooting

**`EnableVideoInput failed (0x80000008)`** — that is `E_FAIL`, and the usual
cause is that *something else holds the sub-device*. A DeckLink sub-device
doing playback in a half-duplex profile cannot simultaneously capture. Check
what has the card open:

```sh
fuser -v /dev/blackmagic/*
```

On this dev box the `kloudconnect` service (`/etc/kloudconnect/camera.json`,
`"sdi": {"enabled": true, "device": 0}`) holds device 0 for SDI output, which
blocks capture on the same sub-device. The error is logged once every 10 s
while the open loop retries, so the input recovers on its own once the device
frees up.

**`device has no input interface`** — wrong profile, see the table above.

**No frames but no error** — a `bmdFrameHasNoInputSource` flag means the card
is running but sees no signal (cable, wrong link count for quad-link 8K, or a
source that is off). The input reports disconnected and the render loop shows
the no-signal placeholder; `in<N>.noSignal` counts these frames.

**Stalls** — if frames stop arriving entirely for 3 s (card reset, another app
changing the profile) the input closes and reopens, counted by
`in<N>.reconnects`.

## Bandwidth

8K UYVY at 59.94 is ~4.0 GB/s DMA into host RAM, then ~4.0 GB/s again uploading
to the GPU. That host-RAM round trip is real and does not exist on the SRT/NVDEC
path, which decodes straight into device memory — see `docs/bench-m5.md` for the
DDR5 saturation limit that bounds how many 8K streams a single box can carry.

## Not implemented

- **10-bit v210.** The card offers it on every mode. Note it is a *packed*
  format, unlike the planar P216 in the 10-bit feasibility work, so it needs its
  own unpack — it does not come free with P216 support.
- Timecode (RP188), ancillary data, more than 2 audio channels (the card does
  up to 64), reference/genlock configuration, and capture groups for
  hardware-aligned multi-channel starts.
