/* 8Kloud Switcher — a live video switcher for Linux + NVIDIA.
 * Copyright (c) 2026 Devin Block
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <https://www.gnu.org/licenses/>.
 *
 * Additional permission under GNU GPL version 3 section 7: you may link
 * 8Kloud Switcher against the NVIDIA CUDA / Video Codec SDK runtime (CUDA,
 * NVENC, NVDEC), the OMT (libomt / libvmx) runtime, and the Blackmagic
 * DeckLink SDK, and distribute the combined work. See EXCEPTIONS.md for
 * the full exception text. */

#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace kloud::ctl {

// Wire protocol for the TCP remote-control port (docs/remote-control.md).
// Requests are single text lines (case-insensitive keywords), so the surface
// works from Bitfocus Companion's generic TCP module or netcat as well as the
// bundled Companion module. State flows back as one-line JSON events.
//
// ALL input and DSK numbers on the wire are 1-based (matching the operator
// labels in the GUI); parse converts to the engine's 0-based indices.

struct Request {
    enum class Op {
        Cut,
        Auto,
        Ftb,
        SetProgram,   // a = input
        SetPreview,   // a = input
        SetTransition,  // a = TransitionType, b = duration ticks (0 = keep),
                        // f = softness (< 0 = keep)
        TbarBegin,
        TbarSet,      // f = position 0..1
        TbarEnd,
        DskSet,       // a = keyer, b = 0 off / 1 on / 2 toggle
        SetDskSource,  // a = keyer, b = input
        SetDskFade,    // a = keyer, b = duration ticks
        DskTie,        // a = keyer, b as DskSet (ride the next transition)
        DskAudioFollow,  // a = keyer, b as DskSet
        MediaPlay,     // a = input
        MediaPause,    // a = input
        MediaRestart,  // a = input
        MediaStep,     // a = input, b = -1 previous / +1 next
        MediaLoop,     // a = input, b = 0 off / 1 on
        RecordStart,   // s = path ("" = server picks a timestamped default)
        RecordStop,
        RecordToggle,  // s = path used when this starts a recording
        CleanRecordStart,
        CleanRecordStop,
        CleanRecordToggle,
        AudioMute,  // a = input (-1 = master n/a), b = 0 off / 1 on / 2 toggle
        AudioSolo,  // a = input, b as AudioMute
        AudioGain,  // a = input, f = linear gain 0..4
        Subscribe,
        Unsubscribe,
        GetState,
        Ping,
    } op = Op::Ping;
    int a = 0;
    int b = 0;
    float f = 0.f;
    std::string s;
};

// Parses one wire line (no trailing newline). Empty/comment lines return
// nullopt with err empty; bad input returns nullopt with err set.
std::optional<Request> parseLine(std::string_view line, std::string& err);

// TransitionType names accepted by TRANSITION and reported in state JSON,
// indexed by kloud::TransitionType value.
const std::vector<std::string>& transitionNames();

// Engine state mirrored to clients. The server fills one per poll; a push
// goes out when the serialized form changes.
struct DskState {
    bool on = false;
    float level = 0.f;
    int src = 0;  // 0-based here; serialized 1-based
    bool tie = false;
    bool audioFollow = false;
};

struct MediaControlState {
    bool available = false;
    bool playing = false;
    bool loop = true;
    bool atEnd = false;
    int playlistIndex = 0;  // 0-based here; serialized 1-based
    int playlistSize = 0;
};

struct InputControlState {
    std::string ref;   // empty = unassigned (deliberate BLACK)
    // Transport as a stable wire name ("omt", "srt", "media", "still",
    // "decklink"). Deliberately not InputSpec::Type's value: that enum has
    // been renumbered before (NDI's removal shifted every member but Srt),
    // and a client should never have to track our internal numbering.
    std::string type;
    bool connected = false;
    MediaControlState media;
    bool audioMute = false;
    bool audioSolo = false;
    float audioGain = 1.f;
};

struct RecordControlState {
    bool active = false;
    bool pending = false;
    bool error = false;
    int64_t frames = 0;
    std::string path;
};

// A program/clean sender. `up` is the live health an operator acts on: for
// OMT the sender exists, for SDI the card is open, in the right mode, and
// playing out.
struct OutputControlState {
    bool configured = false;  // asked for in the show/CLI
    bool up = false;
    std::string name;  // OMT sender name, or the SDI decklink:// ref
    int64_t frames = 0;
};

struct Snapshot {
    int program = 0;  // 0-based here; serialized 1-based
    int preview = 1;
    double fps = 60000.0 / 1001.0;  // output rate (recording-time display)
    bool inTransition = false;
    bool ftb = false;
    float ftbLevel = 0.f;
    int transitionType = 0;
    std::vector<DskState> dsk;
    RecordControlState record;
    RecordControlState cleanRecord;
    OutputControlState omtOut;
    OutputControlState cleanOmtOut;
    OutputControlState mvOmtOut;
    OutputControlState sdiOut;
    OutputControlState cleanSdiOut;
    bool srtConfigured = false;
    bool srtConnected = false;
    bool audioAvailable = false;
    std::vector<InputControlState> inputs;
};

// One-line {"event":"state",...} JSON (no trailing newline). Floats are
// fixed-precision so an idle engine serializes identically every poll.
std::string toJson(const Snapshot& s);

std::string jsonEscape(std::string_view s);

}  // namespace kloud::ctl
