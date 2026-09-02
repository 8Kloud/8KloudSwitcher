/* 8Kloud Switcher — a live video switcher for Linux + NVIDIA.
 * Copyright (c) 2026 Devin Block
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#pragma once
#include <mutex>
#include <string>
#include <thread>

#include "app/ShowFile.h"
#include "engine/Engine.h"

namespace kloud::app {

// The operator's show as a whole: the running engine's live state plus the
// settings that only take effect on the next start (output format, senders,
// SRT/SDI outputs, bitrates). Collects both into a ShowFile::State and saves
// on a debounced timer whenever anything changed, like the old GUI's 2 s
// saver, so a crash or a power cut loses at most a couple of seconds.
class Session {
public:
    // `initial` is the loaded show with CLI overrides applied; its cfg is
    // what the engine was started with and becomes the pending settings'
    // starting point. showFile may be null (no persistence; tests).
    Session(Engine& engine, ShowFile* showFile, const ShowFile::State& initial);
    ~Session();

    // Restart-to-apply settings, edited from the web GUI.
    EngineConfig pendingConfig() const;
    void setPendingConfig(const EngineConfig& cfg);  // saves promptly
    const EngineConfig& activeConfig() const { return active_; }
    bool restartRequired() const;

    ShowFile::State collect() const;
    void saveIfChanged();
    // Stops the saver and writes the final state. Call BEFORE Engine::stop():
    // a collect() against a stopped engine would see no inputs and no mixer
    // and overwrite the show with an empty one. The destructor only joins.
    void shutdown();
    std::string showPath() const { return showFile_ ? showFile_->path() : ""; }

private:
    void saver(std::stop_token st);

    Engine& engine_;
    ShowFile* showFile_;
    EngineConfig active_;
    mutable std::mutex m_;
    EngineConfig pending_;
    ShowFile::State lastSaved_;
    bool everSaved_ = false;
    bool dirty_ = false;
    std::jthread thread_;
};

}  // namespace kloud::app
