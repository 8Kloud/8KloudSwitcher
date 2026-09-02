/* 8Kloud Switcher — a live video switcher for Linux + NVIDIA.
 * Copyright (c) 2026 Devin Block
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#pragma once
#include <string>
#include <vector>

#include "engine/Engine.h"

namespace kloud::app {

// Show-file persistence: everything needed to restore a show after a
// restart, in a human-readable INI the operator can copy between machines.
// The layout is the one the Qt GUI wrote through QSettings (core/Ini.h keeps
// the dialect), so an existing show.ini keeps working. Load builds the
// EngineConfig before engine start; the control-surface and mixer parts are
// applied after. The app saves on a debounced timer.
class ShowFile {
public:
    struct ChannelState {
        float gain = 1.f;
        bool mute = false;
        bool solo = false;
        int delayMs = 0;

        bool operator==(const ChannelState&) const = default;
    };

    struct DskState {
        int source = 0;
        int fadeDurTicks = 30;
        bool on = false;
        bool tie = false;
        bool audioFollow = false;

        bool operator==(const DskState&) const = default;
    };

    struct State {
        EngineConfig cfg;
        int program = 0;
        int preview = 1;
        int transType = 0;
        int transDurTicks = 30;
        std::vector<ChannelState> chans;
        DskState dsk[kDskCount];

        bool operator==(const State& o) const {
            return program == o.program && preview == o.preview &&
                   transType == o.transType && transDurTicks == o.transDurTicks &&
                   chans == o.chans && dsk[0] == o.dsk[0] && dsk[1] == o.dsk[1] &&
                   cfgEquals(cfg, o.cfg);
        }

    private:
        static bool cfgEquals(const EngineConfig& a, const EngineConfig& b);
    };

    // Empty = the default location, $XDG_CONFIG_HOME/8KloudSwitcher/show.ini
    // (~/.config/8KloudSwitcher/show.ini). The directory is created.
    explicit ShowFile(std::string path);

    const std::string& path() const { return path_; }
    bool exists() const;

    // Overwrites cfg/control fields with the stored show; returns false when
    // no file exists (state left untouched).
    bool load(State& state) const;
    bool save(const State& state) const;

private:
    std::string path_;
};

}  // namespace kloud::app
