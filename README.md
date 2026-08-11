# 8Kloud Switcher

A live video switcher for Linux + NVIDIA: OMT, SDI and SRT inputs, program/preview
switching with transitions, OMT, SDI and SRT (HEVC/NVENC) program outputs, full audio
mixer, Qt 6 GUI with Vulkan multiview. Built for low latency at up to 8K 59.94p.

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

Put the program feed on **SDI** with `--sdi-out 0` (a bare device index or a
`decklink://` ref, on either executable; `--clean-sdi-out` does the same for the
clean feed). It reuses the same UYVY pack the network sender reads, so it costs
no conversion. The card cannot rescale — the sub-device must offer the show
format exactly — and half duplex means a sub-device sending SDI cannot also
capture, so use separate sub-devices for in and out. MediaClock stays master
rather than genlocking to the card; see `docs/decklink.md`.

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

Requires: gcc 14+/clang, CMake 3.25+, Ninja, `glslc`, the Vulkan loader, Qt 6
Widgets, and FFmpeg development libraries (`libavcodec`, `libavformat`,
`libavutil`, `libavfilter`, `libswresample`, `libswscale`) plus the FFmpeg NVIDIA
codec headers (`ffnvcodec`). OMT and DeckLink are optional but expected: without
the OMT SDK there is no OMT input or program output and the bench tools are not
built; without the DeckLink SDK there is no SDI input.

On Ubuntu 26.04 (the target platform):

```sh
sudo apt install build-essential cmake ninja-build pkgconf glslc \
    libvulkan-dev qt6-base-dev catch2 avahi-daemon \
    libavcodec-dev libavformat-dev libavutil-dev libavfilter-dev \
    libswresample-dev libswscale-dev libffmpeg-nvenc-dev
```

The CUDA driver API headers (`cuda.h`) are needed too, but Ubuntu 26.04 only
packages `nvidia-cuda-dev` at 12.4 — far behind the 610-series driver — so
install the CUDA toolkit from NVIDIA's runfile into `/usr/local/cuda`. CMake
accepts `/usr/local/cuda`, `/opt/cuda` or `/usr/include`.

Catch2 comes from the distro when installed; otherwise the build fetches
v3.8.0 from GitHub, which a package build cannot do.

```sh
# OMT: build libomt/libvmx once into third_party/omt (see docs/omt.md).
# DeckLink: copy the SDK headers into third_party/decklink (see docs/decklink.md).

cmake -B build -G Ninja
cmake --build build
ctest --test-dir build          # unit tests
```

## Debian package

`debian/` builds a single `8kloud-switcher` package with the GUI, the headless
engine, both bench tools, and the OMT runtime in a private libdir
(`/usr/lib/*/8kloud-switcher`, reached through each binary's RPATH). Build it
with the dependencies above in place:

```sh
dpkg-buildpackage -b -us -uc      # -> ../8kloud-switcher_1.0.0_amd64.deb
sudo apt install ../8kloud-switcher_1.0.0_amd64.deb
```

The NVIDIA driver and NVENC runtime come in as package dependencies; the
DeckLink Desktop Video driver is a `Suggests` because SDI is optional. Link-time
optimization is disabled in `debian/rules` — GCC 15's LTO drops inline
`std::string` symbols across this project's static archives.

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

8Kloud Switcher is licensed under the **Mozilla Public License 2.0**
(MPL-2.0) — see [`LICENSE.md`](LICENSE.md). MPL-2.0 is file-level copyleft:
changes to these source files stay under the MPL, while the work as a whole may
be combined into a Larger Work under other terms, including proprietary ones
(MPL §3.3). No linking exception is needed, so the old `EXCEPTIONS.md` is gone.

That covers the Bitfocus Companion module in `companion/` too. It carries its own
copy of the license ([`companion/LICENSE`](companion/LICENSE)) because it is also
distributed standalone through Bitfocus's module registry, where MPL §3.1 requires
the license text to travel with the source.

That only holds if the binary's other pieces permit it, which is why the FFmpeg
build matters:

| Component | License | Note |
| --- | --- | --- |
| FFmpeg (via `packaging/build-ffmpeg-lgpl.sh`) | LGPL-2.1+ | **Do not link a distro FFmpeg** — see below |
| Qt 6 Widgets/Gui/Core | LGPL-3.0 | dynamic linking only |
| libsrt | MPL-2.0 | |
| OMT — libomt, libvmx | MIT | |
| Vulkan loader | Apache-2.0 | |
| NVIDIA CUDA / NVENC / NVDEC runtime | proprietary | permitted under MPL §3.3 |
| Blackmagic DeckLink SDK | proprietary | dlopened at runtime |
| Catch2 | BSL-1.0 | tests only, not shipped |

**Ubuntu's FFmpeg is built `--enable-gpl`, so it is GPL-2.0-or-later as
shipped.** Linking it would force the whole binary to be conveyed under the GPL,
which defeats the MPL and cannot combine with the proprietary CUDA and DeckLink
pieces. Build the LGPL FFmpeg described under [Build](#build) and configure
against it with `-DKLOUD_FFMPEG_PREFIX=...`; the Debian package does this and
ships those libraries privately. A plain `cmake -B build` without the flag falls
back to the system FFmpeg and prints a loud warning — fine for development, not
for anything you distribute.
