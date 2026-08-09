# DeckLink SDI input and output

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

Headers are vendored (gitignored, like the OMT SDK):

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

Our own SDI output counts as "something else": `--sdi-out 0` and
`--decklink-input 0` on the same sub-device will not both work. Use different
sub-devices (measured working: output on device 0 while device 1 captures).
The error is logged once every 10 s while the open loop retries, so the input
recovers on its own once the device frees up.

**`device has no input interface`** — wrong profile, see the table above.

**No frames but no error** — a `bmdFrameHasNoInputSource` flag means the card
is running but sees no signal (cable, wrong link count for quad-link 8K, or a
source that is off). The input reports disconnected and the render loop shows
the no-signal placeholder; `in<N>.noSignal` counts these frames.

**Stalls** — if frames stop arriving entirely for 3 s (card reset, another app
changing the profile) the input closes and reopens, counted by
`in<N>.reconnects`.

## Verified by loopback

With a BNC from SDI 1 to SDI 2, the card's own output feeds its own input, which
exercises both directions at once:

```sh
./build/kloud-headless --sdi-out 0 --decklink-input 1 --show 1920x1080 --duration 20
```

Measured 2026-08-09 on the 8K Pro at 1080p59.94, program carrying a
`kloud-testgen` pattern over OMT: input locked ~240 ms after playback started
and auto-detected `1920x1080 @ 60000/1001` with no mode hint; 1304 frames
captured with **zero drops** against 1321 sent; embedded audio arrived on the
capture (1,052,254 sample frames, tracking the source lane); and the multiview
showed the returned picture matching program, with the moving bar offset by the
loop delay. `in<N>.noSignal` counted 11 frames before lock, which is the
pre-playback interval and nothing more.

That covers the paths a signal-less card cannot: the `IDeckLinkVideoBuffer`
pixel read, format auto-detection, and audio capture.

## Bandwidth

8K UYVY at 59.94 is ~4.0 GB/s DMA into host RAM, then ~4.0 GB/s again uploading
to the GPU. That host-RAM round trip is real and does not exist on the SRT/NVDEC
path, which decodes straight into device memory — see `docs/bench-m5.md` for the
DDR5 saturation limit that bounds how many 8K streams a single box can carry.

## SDI output

`--sdi-out INDEX|decklink://REF` puts the program feed on a sub-device, and
`--clean-sdi-out` does the same for the clean feed (no DSK graphics). Both
flags work on `kloud-headless` and `8kloud-switcher`, take a bare index for
convenience, and persist in the GUI show file as `sdiOut` / `cleanSdiOut`.

It is cheap for the same reason the input is: `pack_uyvy.comp` already produces
the exact bytes `bmdFormat8BitYUV` wants, so the output reuses the pack ring the
network sender reads and does no conversion. It does copy each frame into an
SDK-allocated frame rather than wrapping the pack slot, deliberately — scheduled
playback holds a frame until the card has clocked it out, so wrapping would pin
one ring slot per frame of preroll and starve the ring.

**The card cannot rescale.** The sub-device must offer the show format exactly
or the output refuses to start and says so; an explicit `@mode` in the ref
overrides the format match.

**Clocking is not genlocked.** MediaClock stays master and the card's scheduler
absorbs the difference: three frames of preroll, and a frame is skipped
(`out.sdi.bufferDeep`) if the card's queue walks past the target depth. Measured
on this box, 3598 frames went out for 3598 render ticks over 60 s with no
correction needed at all. True genlock would mean pacing the render loop off
`GetHardwareReferenceClock` instead, which changes who owns the engine clock.

Audio is embedded as 32-bit PCM on the same sub-device. The output enters
`BeginAudioPreroll` at open and leaves it when playback starts: without that the
card's audio queue accepts only part of the pre-playback audio and silently
truncates the rest (measured 1680 sample frames = 35 ms), which would show up as
a permanent A/V offset.

Counters: `out.sdi.sent` / `out.sdi.clean.sent`, plus `late`, `dropped`,
`flushed`, `bufferDeep`, `poolStarved`, `droppedToLatest`, `audioShort`.

## Not implemented

- **10-bit v210.** The card offers it on every mode. Note it is a *packed*
  format, unlike the planar P216 in the 10-bit feasibility work, so it needs its
  own unpack — it does not come free with P216 support.
- Timecode (RP188), ancillary data, more than 2 audio channels (the card does
  up to 64), reference/genlock configuration, and capture groups for
  hardware-aligned multi-channel starts.
- **Genlocked output.** See the clocking note above.
- **Keying.** The card has a hardware keyer (`IDeckLinkKeyer`); the SDI output
  sends a filled program only.
