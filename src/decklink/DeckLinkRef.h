/* 8Kloud Switcher — a live video switcher for Linux + NVIDIA.
 * Copyright (c) 2026 Devin Block
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

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
