# Remote control (TCP + Bitfocus Companion)

8Kloud Switcher listens for remote-control clients on a TCP port: the console
(`8kloud-switcher`) on **9923** by default (`--control-port N` to change, `0` to
disable), and
`kloud-headless` only when given `--control-port N` (benches run several
instances side by side, so headless defaults off). The listener binds
`0.0.0.0` — the LAN is trusted, same as the media transports. A failed bind (port taken)
logs and disables remote control; it never stops the show.

Up to 16 clients; a client that stops reading for ~1 MiB of queued pushes
is dropped.

## Bitfocus Companion

The bundled module lives in `companion/`. Build it once:

```sh
cd companion && npm install && npm run package   # -> 8kloud-switcher-1.1.0.tgz
```

- **Companion 4.x**: import `8kloud-switcher-1.1.0.tgz` under
  Modules → Import module package.
- **Companion 3.x**: extract the tgz and point the launcher's *Developer
  modules path* at a directory containing the extracted `pkg/` folder
  (rename it e.g. `8kloud-switcher/`).
- **Any Companion / no module**: the wire protocol below is plain text —
  the built-in *Generic TCP/UDP* module works for actions (no feedback).

Add the connection with the switcher's IP and port. The module provides
actions (buses, transitions, DSK, media, record, audio), tally feedbacks
(program red / preview green, DSK, FTB, recording, SRT, output-down,
signal-loss, mute), variables (`$(8kloud-switcher:program_name)`, `record_time`,
`input_N_name`, …) and drag-and-drop presets under Program / Preview /
Transport. `companion/test/smoke.js` is an integration test that drives a
live switcher through the real module code.

## Wire protocol

One request per line, `\n`-terminated (`\r\n` fine), case-insensitive
keywords. **All input and keyer numbers are 1-based**, matching the GUI.
Responses and pushes are one-line JSON events. Try it interactively:
`nc <host> 9923`, then type `SUBSCRIBE`.

| Command | Effect |
| --- | --- |
| `CUT` (alias `TAKE`) | Cut program/preview |
| `AUTO` (alias `TRANS`) | Auto transition |
| `FTB` | Toggle fade to black |
| `PGM n` / `PVW n` | Set program / preview bus |
| `TRANSITION type [frames [softness]]` | `mix wipelr wiperl wipetb wipebt wipebox wipecircle`; omitted frames/softness keep current |
| `TBAR BEGIN` / `TBAR 0..1` / `TBAR END` | Manual T-bar |
| `DSK k ON\|OFF\|TOGGLE` | Keyer on air |
| `DSK k SRC n` / `DSK k FADE frames` | Keyer fill source / fade duration |
| `DSK k TIE [ON\|OFF\|TOGGLE]` | Tie keyer to the next transition (bare = toggle) |
| `DSK k AFV [ON\|OFF\|TOGGLE]` (alias `FOLLOW`) | Keyer audio-follow (bare = toggle) |
| `MEDIA n PLAY\|PAUSE\|RESTART\|NEXT\|PREV` | Media input transport |
| `MEDIA n LOOP ON\|OFF` | Media loop |
| `RECORD START [path]\|STOP\|TOGGLE` | Program recording (no path → timestamped file in `~/Videos`) |
| `CLEAN START [path]\|STOP\|TOGGLE` | Clean-feed recording |
| `AUDIO n MUTE\|SOLO ON\|OFF\|TOGGLE` | Mixer lane flags |
| `AUDIO n GAIN 0..4` | Fader (linear, 1 = unity) |
| `SUBSCRIBE` / `UNSUBSCRIBE` | State push on every change (~33 Hz max) |
| `STATE` (alias `GET`) | One-shot state |
| `PING` | → `{"event":"pong"}` |

Events: `hello` (on connect: `name`, `protocol`), `state`, `error`
(`message`; bad commands never disconnect), `pong`. The state event
carries program/preview, transition type, FTB, per-keyer
on/level/src/tie/afv, record + clean-record status (incl. frames and
fps for timecode), every program output (below), SRT status, and
per-input ref/type/connected plus media and audio-lane state.
Unassigned inputs have `"ref":""`. Commands apply on the next render
tick; confirmation is the next state push (single-frame latency).

### Program outputs

`omtOut`, `cleanOmtOut`, `sdiOut` and `cleanSdiOut` each carry
`{configured, up, name, frames}`. `configured` is what the show asked
for; `up` is live health. **`configured && !up` is the operator alarm** —
an OMT sender that failed to start, or an SDI sub-device that is absent,
already capturing (half duplex), or cannot do the show format. `name` is
the OMT sender name or the SDI `decklink://` ref. The bundled Companion
module exposes this as the `output_down` feedback (per output or "any")
plus `omt_out_state` / `sdi_out_state` / `sdi_out_name` variables.

### Input `type` is a name, not a number

`"type"` is one of `omt`, `srt`, `media`, `still`, `decklink` — the same
strings the show file uses. It was an `InputSpec::Type` integer until
v1.1; removing NDI renumbered that enum (every member but `srt` shifted),
which would have silently changed the meaning of the old numbers, so the
wire now carries a stable name. A client reading the pre-v1.1 integers
must be updated.

Implementation: parsing/serialization in `src/ctl/ControlProtocol.*`
(pure, unit-tested), the request → engine binding in `src/ctl/ControlApply.*`
(shared with the web GUI's WebSocket, so a command means the same thing on
either transport), socket loop in `src/ctl/ControlServer.*` (one poll thread,
applies requests via `Engine::post` — mutex-serialized for its producers —
plus the recording/audio control surfaces). The web GUI (`docs/web-gui.md`)
accepts these same lines as WebSocket text messages.
