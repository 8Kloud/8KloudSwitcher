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
 * 8Kloud Switcher against the proprietary NDI SDK, the NVIDIA CUDA / Video
 * Codec SDK runtime (CUDA, NVENC, NVDEC), the OMT (libomt / libvmx)
 * runtime, and the Blackmagic DeckLink SDK, and distribute the combined
 * work. See EXCEPTIONS.md for the full exception text. */

#pragma once
#include <string>

namespace kloud {

// A DeckLink input reference, parsed from a show file / CLI / control command.
// Grammar (the scheme is what makes a ref sniffable as DeckLink):
//
//   decklink://0                 device index 0, mode auto-detected
//   decklink://0@2160p59.94      device index 0, mode forced
//   decklink://DeckLink 8K Pro (1)          device by display name
//   decklink://DeckLink 8K Pro (1)@4320p60  ... with a forced mode
//
// Auto-detect is the default and the right answer for SDI: the card reports
// the incoming mode and the input follows it (see docs/decklink.md). A forced
// mode only matters for sources the card cannot detect.
struct DeckLinkRef {
    bool valid = false;
    int index = -1;      // >= 0 when the ref named a device index
    std::string name;    // non-empty when the ref named a device by name
    std::string mode;    // empty = auto-detect

    bool byIndex() const { return index >= 0; }
    bool operator==(const DeckLinkRef&) const = default;
};

inline constexpr const char* kDeckLinkScheme = "decklink://";

inline bool isDeckLinkRef(const std::string& ref) {
    return ref.rfind(kDeckLinkScheme, 0) == 0;
}

inline DeckLinkRef parseDeckLinkRef(const std::string& ref) {
    DeckLinkRef r;
    if (!isDeckLinkRef(ref)) return r;
    std::string body = ref.substr(std::string(kDeckLinkScheme).size());

    // A trailing @mode is optional. Device *names* can contain '@' in
    // principle, so split on the last one.
    if (const auto at = body.rfind('@'); at != std::string::npos) {
        r.mode = body.substr(at + 1);
        body = body.substr(0, at);
    }
    if (body.empty()) return r;  // "decklink://" alone names no device

    const bool digits =
        body.find_first_not_of("0123456789") == std::string::npos;
    if (digits) {
        try {
            r.index = std::stoi(body);
        } catch (...) {
            return r;  // absurdly long digit run
        }
    } else {
        r.name = body;
    }
    r.valid = true;
    return r;
}

}  // namespace kloud
