# OMT (Open Media Transport) input

OMT (vMix's open, MIT-licensed transport — VMX intra codec over TCP, mDNS
discovery) is 8Kloud Switcher's network transport, used for both input and
the program/clean/multiview senders. Support is optional at build time: CMake
enables it when `third_party/omt/` is populated, and without it there is no
OMT input or program output.

## Getting the libraries

`third_party/` is gitignored; build the two libraries once per machine:

```sh
cd third_party
git clone --depth 1 https://github.com/openmediatransport/libvmx
git clone --depth 1 https://github.com/openmediatransport/libomtnet
git clone --depth 1 https://github.com/openmediatransport/libomt

# 1. VMX codec: plain clang++ (no deps)
(cd libvmx/build && sh buildlinuxx64.sh)

# 2. .NET 8 SDK, unprivileged (skip if `dotnet` exists)
curl -sSL https://dot.net/v1/dotnet-install.sh | bash -s -- --channel 8.0
export DOTNET_ROOT=$HOME/.dotnet PATH=$HOME/.dotnet:$PATH

# 3. libomtnet (C# implementation), then libomt (NativeAOT C ABI wrapper)
(cd libomtnet && dotnet build libomtnet.sln -c Release)
(cd libomt && dotnet publish libomt.sln -r linux-x64 -c Release)

# 4. Lay out the SDK the build expects
mkdir -p omt/include omt/lib
cp libomt/libomt.h omt/include/
cp libomt/bin/Release/net8.0/linux-x64/publish/libomt.so omt/lib/
cp libvmx/build/libvmx.so omt/lib/
```

Re-run cmake; it reports `OMT SDK found`. `libomt.so` dlopens `libvmx.so`
from the same directory (rpath is wired by CMake).

## Using OMT inputs

- Headless: `kloud-headless --omt-input "HOSTNAME (Name)"` (discovery name)
  or `--omt-input omt://host:port` (direct, senders bind from 6400 up).
- GUI source picker: the discovery list shows OMT sources and DeckLink cards
  (badged `OMT`/`SDI`; each row carries its type and ref — first open may
  need one Refresh while
  mDNS answers arrive). The manual field accepts `omt://host:port` and
  `srt://` URLs; bare manual text is taken as an OMT discovery name, which
  must be the full `HOSTNAME (Name)` form. Show files
  store the true type (`type=omt`) for round-trip fidelity.
- Frame sync (`--framesync IDX[:N]`, per-input GUI checkbox) works on OMT
  inputs; sender timestamps are 100 ns units and get the same
  cadence sanity check + synthesized-pts fallback.
- Test sender: `kloud-testgen --omt [--noise] [--size WxH]` (VMX encode
  happens in-process; the 5 s stats line reports encoder ms/frame, Mbps,
  and transport-envelope drops).

## OMT outputs

Three senders, each an independent OMT source with the master audio bus
embedded. All are created before any input receiver — see the avahi ordering
note in `Engine::start`; a sender cannot be brought up mid-show.

| Sender | Enable | Content |
| --- | --- | --- |
| Program | on by default (`--no-omt-out`, `--omt-out-name NAME`) | show-format program, DSKs included |
| Clean | `--clean-omt-out NAME` | show-format program without DSK graphics |
| Multiview | `--mv-omt-out NAME` | the monitor wall, labels and tally borders included |

The multiview sender carries exactly what the GUI multiview shows, so a
remote operator position sees the same wall: input matrix on the left, the
PROGRAM and look-ahead PREVIEW monitors stacked on the right, red/green tally
on both. Its geometry is the multiview's, not the show's — `--multiview WxH`
(default 1920x1080, stored per show as `mvW`/`mvH`). Cost is one extra UYVY
pack of that size per tick plus the VMX encode: ~4 MB/frame at 1080p, which
is noise next to an 8K program path.

Receivers negotiate it like any other source: `kloud-headless --omt-input
"HOSTNAME (MV NAME)"`. Verified end to end on this box by loopback — 1080p59.94,
zero drops over ~600 frames, colors and tally borders byte-accurate through
pack → VMX → decode.

## Limits measured on this box

See `docs/bench-omt.md` for the 1080p/8K measurements, CPU costs, and the
10 MB-per-compressed-frame transport envelope (`OMTConstants.VIDEO_MAX_SIZE`
— oversize frames are counted drops at the sender, not a stall; patchable
in our vendored build if 8K noise content ever matters).

## libomt-c

The switcher also builds against [libomt-c](https://github.com/8Kloud/libomt-c),
the native C implementation of the same C API: lay it out as
`third_party/omt/{include/libomt.h,lib/libomt.so,lib/libvmx.so}` exactly like
the stock SDK (or point `-DOMT_SDK_DIR` at such a directory). No source changes
are needed; senders and receivers interoperate with the stock library in both
directions.

### Decoder pre-build

Creating a VMX decoder instance is the one expensive step on a new OMT
connection (`VMX_Create` fills its planes and stream buffers), and the stock
library does it inside the first received frame. When CMake reports
`OMT decoder prewarm: available`, `Engine::start` pre-builds decoders for the
show format before Vulkan initialises (one per assigned OMT input, at most
four) and every OMT input asks its receiver to prepare its decoder while the
connection comes up; a re-patched input's decoder returns to the pool. Wrong
guesses (an input carrying a different format than the show) cost nothing but
the idle decoder's memory.

### Measurements (2026-09-01)

Same-host loopback, `kloud-testgen` → `kloud-latmeter`, same method as the
bench above (20 s steady-state window, CPU from `/proc`, one run each). "New"
is libomt-c at `92713c0` with libvmx patched to trim `VMX_Create`'s memsets
(openmediatransport/libvmx PR #13); "stock" is the NativeAOT libomt with the
unpatched libvmx. Throughput and CPU are the codec's and do not move; what
changes is memory and everything around the first frame.

| 8K 59.94 | stock | new |
|---|---|---|
| bars: send / recv CPU | 2.86 c / 2.87 c | 2.87 c / 2.87 c |
| bars: send / recv RSS | 1868 MB / 541 MB | 1688 MB / 360 MB |
| bars: fps, gaps | 59.84, 2 | 59.94, 0 |
| bars: latency avg / max | 8.3 / 80 ms | 8.0 / 25 ms |
| mid-entropy: send / recv CPU | 3.02 c / 3.09 c | 3.05 c / 3.05 c |
| mid-entropy: send / recv RSS | 1956 MB / 621 MB | 1699 MB / 377 MB |
| mid-entropy: fps, gaps | 59.83, 2 | 59.94, 0 |
| mid-entropy: latency avg / max | 10.2 / 82 ms | 10.1 / 32 ms |

1080p and 4K: CPU and latency identical within run-to-run noise (1080p noise
0.43/0.39 c vs 0.40/0.36 c send/recv; 4K mid 0.50/0.49 c vs 0.46/0.48 c),
receiver RSS 40 MB lower.

The "max" and "gaps" columns are the connect transient: a receiver joining a
running sender pays the decoder build on its first frame. Isolated with
receivers joining 15 s and 25 s after the sender started:

| 8K bars, first second after connect | max latency | dropped frames |
|---|---|---|
| stock libomt + stock libvmx | 80 ms | 2 |
| stock libomt + patched libvmx (`VMX_Create` 55 → 13 ms) | 27 ms | 0 |
| libomt-c, decoder pool prewarmed | 8–9 ms | 0 |

Steady state at 8K never exceeds 9 ms in either build. `VMX_Create` itself,
best of five: 1080p 3.2 → 1.6 ms, 4K 15.1 → 3.1 ms, 8K 55.2 → 12.8 ms.
