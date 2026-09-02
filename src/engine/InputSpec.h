/* 8Kloud Switcher — a live video switcher for Linux + NVIDIA.
 * Copyright (c) 2026 Devin Block
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#pragma once
#include <cstdio>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "media/Playlist.h"

// What an input is patched to. Split out of Engine.h so the show file, the
// remote-control protocol and their tests can name a transport without
// pulling in the GPU and CUDA headers the engine needs.

namespace kloud {

struct InputSpec {
    enum class Type { Omt, Srt, Media, Still, DeckLink } type = Type::Omt;
    // OMT discovery name or omt:// URL, SRT URL, decklink:// ref, local
    // video, or still image
    std::string ref;
    // Frame sync (docs/design-framesync.md): -1 = off (v1 latest-frame
    // behavior), 0 = measure-only (auto A/V trim, no added latency),
    // 1..4 = buffered re-timing by that many source frames.
    int syncFrames = -1;
    bool mediaPlaying = true;
    bool mediaLoop = true;
    // Ordered local clips. Empty means the legacy/single-clip `ref`.
    std::vector<media::PlaylistItem> mediaPlaylist;

    InputSpec() = default;
    InputSpec(Type inputType, std::string inputRef, int frames = -1)
        : type(inputType), ref(std::move(inputRef)), syncFrames(frames) {}
};

// Stable external name for a transport, used by both the show file and the
// remote-control state. Never serialize InputSpec::Type's numeric value:
// removing NDI renumbered every member but Srt, and anything that had learned
// the old numbers would have gone on parsing them without complaint.
inline const char* inputTypeName(InputSpec::Type t) {
    switch (t) {
        case InputSpec::Type::Srt: return "srt";
        case InputSpec::Type::Media: return "media";
        case InputSpec::Type::Still: return "still";
        case InputSpec::Type::DeckLink: return "decklink";
        case InputSpec::Type::Omt: break;
    }
    return "omt";
}

// Inverse of inputTypeName. Unknown names (a show file from a build that had
// transports this one does not) fall back to OMT, which simply fails to
// resolve and leaves the input black.
inline InputSpec::Type inputTypeFromName(std::string_view name) {
    if (name == "srt") return InputSpec::Type::Srt;
    if (name == "media") return InputSpec::Type::Media;
    if (name == "still") return InputSpec::Type::Still;
    if (name == "decklink") return InputSpec::Type::DeckLink;
    return InputSpec::Type::Omt;
}

// Operator-facing name for a source: a transport badge plus the meaningful
// part of the ref. Shared by the multiview label atlas, the web GUI and the
// show-file diagnostics so every surface calls an input the same thing.
//   ""                          -> "BLACK"
//   "HOST (CamA)"      (omt)    -> "HOST (CamA)"
//   "omt://h:1234"              -> "OMT · h:1234"
//   "srt://h:9710?..."          -> "SRT · h:9710?..."
//   "decklink://0"              -> "SDI · 0"
//   "/shows/roll-in.mkv"        -> "MEDIA · roll-in.mkv"
//   "/shows/logo.png"  (still)  -> "STILL · logo.png"
inline std::string inputDisplayName(InputSpec::Type type, std::string_view ref) {
    auto startsWith = [&](std::string_view prefix) {
        if (ref.size() < prefix.size()) return false;
        for (size_t i = 0; i < prefix.size(); ++i) {
            char a = ref[i], b = prefix[i];
            if (a >= 'A' && a <= 'Z') a = char(a - 'A' + 'a');
            if (a != b) return false;
        }
        return true;
    };
    auto basename = [&] {
        const size_t slash = ref.find_last_of('/');
        return std::string(slash == std::string_view::npos ? ref
                                                            : ref.substr(slash + 1));
    };
    if (ref.empty()) return "BLACK";
    if (startsWith("srt://")) return "SRT \xc2\xb7 " + std::string(ref.substr(6));
    if (startsWith("omt://")) return "OMT \xc2\xb7 " + std::string(ref.substr(6));
    if (startsWith("decklink://"))
        return "SDI \xc2\xb7 " + std::string(ref.substr(11));
    if (type == InputSpec::Type::Still) return "STILL \xc2\xb7 " + basename();
    if (type == InputSpec::Type::Media) return "MEDIA \xc2\xb7 " + basename();
    return std::string(ref);
}

// The multiview strip text: "NN NAME" in the 5x7 atlas font, which has no
// middle dot, so the badge separator becomes a plain space.
inline std::string inputLabel(const InputSpec& spec, int index) {
    std::string name = inputDisplayName(spec.type, spec.ref);
    for (size_t pos; (pos = name.find(" \xc2\xb7 ")) != std::string::npos;)
        name.replace(pos, 4, " ");
    char num[16];
    snprintf(num, sizeof num, "%02d ", index + 1);
    return num + name;
}

}  // namespace kloud
