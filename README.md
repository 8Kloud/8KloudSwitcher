# 8Kloud Switcher

A live video switcher for Linux + NVIDIA: OMT, SDI and SRT inputs, program/preview
switching with transitions, OMT and SRT (HEVC/NVENC) program outputs, full audio mixer,
Qt 6 GUI with Vulkan multiview. Built for low latency at up to 8K 59.94p.

Status: **v1 complete (M0–M6), v2 frame sync landed**. 8K-hardened engine (30-min soak: zero
tick overruns, 1.5-frame latency, <2 cores full pipeline, NVENC 54%), full audio mixer (A/V
within ±8 ms on network and SRT paths at 1080p and 8K), live source picker (swap
OMT/SDI/SRT sources per input mid-show), show-file persistence (restart restores
everything), health banners,
runtime counters in the GUI, HEVC/AAC program recording, paced local clip
playlists with trim, speed, and transport controls, and static raster inputs
with native alpha. A parallel clean feed can be recorded or sent over OMT
without DSK graphics. Per-input **frame sync**: re-times a source onto the output tick
grid (1–4 frame buffer absorbs bursty delivery, rate slip becomes counted repeats/drops) and
auto-aligns the input's audio to the re-timed video — the cross-session A/V phase lottery
collapses to a constant (design + measurements: `docs/design-framesync.md`,
`docs/bench-framesync.md`). Bench record: `docs/bench-m5.md`; tuning: `docs/tuning.md`.
For 8K ingest prefer SRT/HEVC (NVDEC) or SDI; OMT/VMX carries realistic 8K content
(`docs/bench-omt.md`). Milestones: M6 (v1 close), M5 (8K hardening), M4 (audio),
M3+M3.5 (SRT/HEVC both directions), M2 (switching/multiview), M1 (Vulkan engine), M0 (bench).

OMT sources are addressed by their full discovery name, `HOSTNAME (Name)` — the
GUI picker fills that in; on the CLI, quote it. An `omt://host:port` URL also works.

```sh
# SRT out (listener) + receive with any ffplay/OBS caller:
./build/8kloud-switcher --input "HOST (CamA)" --input "HOST (CamB)" \
    --srt-out "srt://:9710?mode=listener&latency=120000"
ffplay "srt://HOST:9710?mode=caller"     # latency option is MICROseconds
# ffplay buffers seconds on live streams; for a low-latency audio monitor use:
ffmpeg -fflags nobuffer -flags low_delay -i "srt://HOST:9710?mode=caller" -vn -f pulse Mon
# SRT ingest as an input (audio decodes too; trim its lateness per input):
./build/8kloud-switcher --srt-input "srt://HOST:9710?mode=caller&latency=120000" \
    --input "HOST (CamB)"
# A/V sync check on a TS capture (needs testgen content on program):
ffmpeg -y -copyts -i "srt://HOST:9710?mode=caller" -c copy -avoid_negative_ts disabled \
       -muxpreload 0 -muxdelay 0 -t 20 cap.ts && python scripts/av_offset_ts.py cap.ts
```

Run it:
```sh
./build/kloud-testgen --name CamA &  ./build/kloud-testgen --name CamB &
# or kloud-headless for no GUI
./build/8kloud-switcher --input "$(hostname -s | tr a-z A-Z) (CamA)" \
                        --input "$(hostname -s | tr a-z A-Z) (CamB)"
```

The show (inputs, outputs, transition, program/preview, full mixer state) persists to
`~/.config/8KloudSwitcher/show.ini` (or `--show-file PATH`) and restores on restart; CLI flags
override what they name. Click an input's name in the mixer to pick a different source live —
the browser lists OMT discovery (Open Media Transport, 8K-capable for realistic content:
build steps `docs/omt.md`, measurements `docs/bench-omt.md`; headless `--input` or
`--omt-input`) and any Blackmagic **DeckLink** cards as `SDI ·` entries
(8-bit UYVY capture up to 8K, auto-detecting the incoming mode: `docs/decklink.md`;
headless `--decklink-input 0`), and the manual field takes an `srt://`, `omt://` or
`decklink://` URL or an OMT discovery name.
The same dialog sets the input's frame sync (Off / Trim only /
1–4 frames; headless: `--framesync IDX[:FRAMES]`). Use 1 frame for free-running cameras
you switch between often (constant A/V at +1 frame latency); Trim only suits audio-early
sources like SRT loopbacks. Shortcuts: `Space` cut, `Enter` auto, `F` FTB, `1–9` program,
`Shift+1–9` preview, `D`/`Shift+D` DSK 1/2.

Select the program output resolution and progressive frame rate from the **OUTPUT FORMAT**
controls above the multiview. The choice is saved immediately to the show file; restart
8Kloud Switcher when the amber **RESTART TO APPLY** badge appears. The selected format drives
both OMT and SRT program outputs on the next start.

Record the program mix with the red **RECORD** control in the top bar. Recordings
are HEVC video plus 48 kHz stereo AAC in a finalized Matroska (`.mkv`) file;
encoding and disk I/O run off the render thread, and recorder backpressure never
stalls program. Headless: `--record PATH.mkv [--record-bitrate KBPS]` (bitrate
defaults from the output format).

Use **CLEAN REC** for the switched A/B mix without DSK graphics. The clean feed
retains transitions, FTB, and master audio. Program and clean recordings can
run simultaneously with independent NVENC sessions and backpressure. Headless:
`--clean-record PATH.mkv`; `--record-bitrate` applies to both. An optional
clean OMT sender is enabled with `--clean-omt-out "8Kloud Switcher CLEAN"` in
either executable and can run alongside the normal program sender. Its enabled state
and name persist in a GUI show file. Design and validation:
`docs/design-clean-feed.md`.

Local H.264/HEVC clips can occupy any input: open the input source picker and
use **ADD CLIPS** to build and reorder a playlist, then use the **MEDIA** tab for
previous/next, play/pause, restart, and whole-list loop controls. Each clip is
timestamp-paced in real time, decoded through NVDEC, and its audio enters the
normal mixer lane. Set inclusive **IN** and exclusive **OUT** times per clip in
the playlist editor (`OUT=END` plays through EOF), plus a per-clip playback
speed from 25–400%; audio tempo follows without changing pitch. Playlist order,
trim points, speed, and loop mode persist in the show file; playback starts
from the first clip after an application restart. Headless:
`--media-input A.mkv --media-trim 500:2500 --media-speed 1.5 --media-item B.mkv`
(`--media-no-loop` stops at the end; trim values are milliseconds). Still
images use **ADD STILL** in the same source picker, decode and upload once,
then remain live until that input is replaced. PNG, JPEG, WebP, BMP, TIFF,
TGA, and EXR are recognized; non-opaque alpha is preserved for direct use as
a DSK graphic. Still selections persist in the show file. Headless:
`--still-input sponsor-logo.png`.
Design and validation notes: `docs/design-recorder-media.md`.

Two downstream keyers composite graphics with **native alpha (OMT UYVA or a local
raster still)** over program — point a DSK at an input carrying alpha (CasparCG, OBS with
alpha, `kloud-testgen --uyva`, or a transparent PNG/WebP still), and toggle it on; it fades
over its own duration, independent of transitions, and FTB takes it out with everything
else. A source without alpha keys fully opaque (a fadeable fullscreen overlay). Headless:
`--dsk K:SRC --dsk-fade K:TICKS --dsk-toggle-after S:K`. Design:
`docs/design-dsk.md`; measurements: `docs/bench-dsk.md` (an 8K UYVA key over an 8K program
holds full rate; keyers-off cost is nil).

## Encoder backends

HEVC program output (SRT and recording) runs through `hevc_nvenc` by default.
A second backend drives NVENC directly through `libnvidia-encode`, so an FFmpeg
build without `hevc_nvenc` cannot take program output down; `--encoder
auto|ffmpeg|direct` selects one in either executable (`auto` falls back to the
direct path only when FFmpeg has no usable encoder). The two are configured
identically (ultra-low-latency, CBR, single-frame VBV, no B-frames) and measure
the same at every resolution tested.

`--encoder-preset auto|p1..p7` sets the NVENC speed/quality preset. `auto`
picks P2 above 4K and P4 at or below it: measured against a real 4K source,
the two are quality-identical at the auto bitrate (PSNR Y within 0.01 dB),
but at 8K a P4 picture takes long enough that the render thread finds the pack
slot still busy at the next tick and drops the frame — P2 holds full 59.94
where P4 loses 25–40% of the SRT feed. Design and measurements:
`docs/design-encoder.md`.

## Build

Requires: gcc 14+/clang, CMake 3.25+, Ninja, and FFmpeg development libraries
(`libavcodec`, `libavformat`, `libavutil`, `libavfilter`, `libswresample`, and
`libswscale`). OMT and DeckLink are optional but expected: without the OMT SDK
there is no OMT input or program output and the bench tools are not built;
without the DeckLink SDK there is no SDI input.

```sh
# OMT: build libomt/libvmx once into third_party/omt (see docs/omt.md).
# DeckLink: copy the SDK headers into third_party/decklink (see docs/decklink.md).

cmake -B build -G Ninja
cmake --build build
ctest --test-dir build          # unit tests
```

OMT discovery needs `avahi-daemon` running.

## Tools

```sh
# Test pattern sender: counter/timestamp strips, flash+tone A/V sync burst
./build/kloud-testgen --size 7680x4320 --fps 60000/1001 --name Cam1

# Latency analyzer: fps, frame gaps, end-to-end latency, A/V offset
./build/kloud-latmeter --source Cam1 --csv out.csv

# Transport bench (run the pair across two machines to measure the wire path)
./scripts/bench_codec.sh 7680x4320 30 build
```

## Vulkan validation

`--validate` (both binaries) loads `VK_LAYER_KHRONOS_validation` and installs a
debug-utils messenger, so layer warnings and errors come out through the normal
log as `vk: …`. Without the messenger the layer stays silent, so a clean run is
only meaningful with this flag's plumbing in place — sanity-check the pipe with
`VK_KHRONOS_VALIDATION_VALIDATE_BEST_PRACTICES=true`, which should produce
sub-allocation advice on any run.

```sh
./build/kloud-headless --validate --autos --duration 25 --dsk 0:1
```

## Remote control

A TCP control port (GUI default 9923) accepts plain-text commands and
pushes JSON state — see `docs/remote-control.md`. A ready-made Bitfocus
Companion module with tally feedbacks and presets lives in `companion/`.

## Layout

- `src/core/` — MediaClock (rational fps, drift-free), SPSC ring / latest-frame mailbox, stats
- `src/engine/` — SwitcherCore: pure program/preview + transition state machine (unit-tested)
- `src/omt/`, `src/decklink/`, `src/in/` — OMT, SDI and SRT/media/still inputs
- `src/out/` — OMT program + clean senders, SRT output, file recorder
- `src/ctl/` — remote-control wire protocol + TCP server
- `companion/` — Bitfocus Companion module (`npm run package` → installable tgz)
- `tools/` — kloud-testgen / kloud-latmeter + shared pattern layout (`tools/common/pattern.h`)
- `docs/` — bench reports; the full v1 plan lives with the project owner

## License

Copyright © 2026 Devin Block.

8Kloud Switcher is licensed under the **GNU General Public License v3.0 or later**
(GPL-3.0-or-later) — see [`LICENSE.md`](LICENSE.md). As an additional permission
under GPLv3 section 7, 8Kloud Switcher may be linked against and distributed with the
**NVIDIA CUDA / Video Codec SDK** runtime (CUDA, NVENC, NVDEC), the **OMT**
(libomt / libvmx) runtime, and the **Blackmagic DeckLink SDK**; the full exception
text is in [`EXCEPTIONS.md`](EXCEPTIONS.md).
