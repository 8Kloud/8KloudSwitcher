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

}  // namespace kloud
