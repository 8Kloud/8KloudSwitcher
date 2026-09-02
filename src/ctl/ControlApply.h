/* 8Kloud Switcher — a live video switcher for Linux + NVIDIA.
 * Copyright (c) 2026 Devin Block
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#pragma once
#include <string>

#include "ctl/ControlProtocol.h"

namespace kloud {
class Engine;
}

namespace kloud::ctl {

// The request -> engine binding shared by every control transport (the TCP
// port and the web GUI's WebSocket), so a command means exactly the same
// thing however it arrives.
struct ApplyResult {
    bool ok = true;
    std::string error;  // set when !ok; never disconnects a client
    std::string reply;  // one-line JSON event for GetState / Ping
};

// Applies everything except Subscribe/Unsubscribe, which are per-connection
// and stay with the transport (they come back as ok with no reply).
ApplyResult apply(Engine& engine, const Request& r);

// Live engine state in wire form.
Snapshot snapshot(const Engine& engine);

// `~/Videos/8Kloud Switcher[-Clean]-YYYYMMDD-HHMMSS.mkv` (or `~` when there
// is no Videos directory).
std::string defaultRecordPath(bool clean);

}  // namespace kloud::ctl
