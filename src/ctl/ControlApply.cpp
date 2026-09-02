/* 8Kloud Switcher — a live video switcher for Linux + NVIDIA.
 * Copyright (c) 2026 Devin Block
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "ctl/ControlApply.h"

#include <sys/stat.h>

#include <cstdlib>
#include <ctime>

#include "engine/Engine.h"

namespace kloud::ctl {

std::string defaultRecordPath(bool clean) {
    const char* home = getenv("HOME");
    std::string dir = home ? home : ".";
    struct stat st{};
    if (const std::string videos = dir + "/Videos";
        stat(videos.c_str(), &st) == 0 && S_ISDIR(st.st_mode))
        dir = videos;
    char ts[32];
    const time_t now = time(nullptr);
    tm local{};
    localtime_r(&now, &local);
    strftime(ts, sizeof ts, "%Y%m%d-%H%M%S", &local);
    return dir + (clean ? "/8Kloud Switcher-Clean-" : "/8Kloud Switcher-") + ts +
           ".mkv";
}

Snapshot snapshot(const Engine& engine) {
    Snapshot s;
    const auto ui = engine.uiState();
    s.program = ui.program;
    s.preview = ui.preview;
    const auto fmt = engine.outputFormat();
    if (fmt.fpsD > 0) s.fps = double(fmt.fpsN) / double(fmt.fpsD);
    s.inTransition = ui.inTransition;
    s.ftb = ui.ftbEngaged;
    s.ftbLevel = ui.ftbLevel;
    s.transitionType = ui.transType;
    s.dsk.resize(kDskCount);
    for (int k = 0; k < kDskCount; ++k)
        s.dsk[size_t(k)] = {ui.dskOn[k], ui.dskLevel[k], ui.dskSrc[k],
                            ui.dskTie[k], ui.dskAudioFollow[k]};
    auto rec = [](const Engine::RecordingState& r) {
        return RecordControlState{r.active, r.pending, r.error, r.frames,
                                  r.path};
    };
    s.record = rec(engine.recordingState());
    s.cleanRecord = rec(engine.cleanRecordingState());

    s.omtOut = {engine.omtOutRequested(), engine.omtOutActive(),
                engine.omtOutName(), engine.omtOutFrames()};
    s.cleanOmtOut = {engine.cleanOmtOutRequested(),
                     engine.cleanOmtOutActive(), engine.cleanOmtOutName(),
                     engine.cleanOmtOutFrames()};
    s.mvOmtOut = {engine.mvOmtOutRequested(), engine.mvOmtOutActive(),
                  engine.mvOmtOutName(), engine.mvOmtOutFrames()};
    s.sdiOut = {!engine.sdiOutRef().empty(), engine.sdiOutOk(),
                engine.sdiOutRef(), engine.sdiOutFrames()};
    s.cleanSdiOut = {!engine.cleanSdiOutRef().empty(),
                     engine.cleanSdiOutOk(), engine.cleanSdiOutRef(),
                     engine.cleanSdiOutFrames()};

    s.srtConfigured = engine.srtConfigured();
    s.srtConnected = engine.srtConnected();
    auto* aud = engine.audio();
    s.audioAvailable = aud != nullptr;
    s.inputs.resize(size_t(engine.inputCount()));
    for (int i = 0; i < engine.inputCount(); ++i) {
        auto& in = s.inputs[size_t(i)];
        in.ref = engine.inputRef(i);
        in.type = inputTypeName(engine.inputType(i));
        in.connected = engine.inputStatus(i).connected;
        const auto m = engine.inputMediaState(i);
        in.media = {m.available,     m.playing,       m.loop,
                    m.atEnd,         m.playlistIndex, m.playlistSize};
        if (aud && i < aud->inputCount()) {
            const auto& ch = aud->channel(i);
            in.audioMute = ch.mute.load(std::memory_order_relaxed);
            in.audioSolo = ch.solo.load(std::memory_order_relaxed);
            in.audioGain = ch.gain.load(std::memory_order_relaxed);
        }
    }
    return s;
}

ApplyResult apply(Engine& engine, const Request& r) {
    using Op = Request::Op;
    ApplyResult res;
    auto fail = [&](std::string msg) {
        res.ok = false;
        res.error = std::move(msg);
    };
    const int nIn = engine.inputCount();
    auto checkInput = [&](int idx) {
        if (idx >= 0 && idx < nIn) return true;
        fail("input " + std::to_string(idx + 1) + " out of range (1.." +
             std::to_string(nIn) + ")");
        return false;
    };
    auto checkDsk = [&](int k) {
        if (k >= 0 && k < kDskCount) return true;
        fail("dsk " + std::to_string(k + 1) + " out of range (1.." +
             std::to_string(kDskCount) + ")");
        return false;
    };

    switch (r.op) {
        case Op::Cut: engine.post({Command::Type::Cut, 0, 0, 0.f}); break;
        case Op::Auto: engine.post({Command::Type::Auto, 0, 0, 0.f}); break;
        case Op::Ftb:
            engine.post({Command::Type::FadeToBlack, 0, 0, 0.f});
            break;
        case Op::SetProgram:
            if (checkInput(r.a))
                engine.post({Command::Type::SetProgram, r.a, 0, 0.f});
            break;
        case Op::SetPreview:
            if (checkInput(r.a))
                engine.post({Command::Type::SetPreview, r.a, 0, 0.f});
            break;
        case Op::SetTransition: {
            // 0 duration / negative softness = keep the current values.
            const auto ui = engine.uiState();
            engine.post({Command::Type::SetTransition, r.a,
                         r.b > 0 ? r.b : ui.transDur,
                         r.f >= 0.f ? r.f : ui.transSoftness});
            break;
        }
        case Op::TbarBegin:
            engine.post({Command::Type::TbarBegin, 0, 0, 0.f});
            break;
        case Op::TbarSet:
            engine.post({Command::Type::TbarSet, 0, 0, r.f});
            break;
        case Op::TbarEnd:
            engine.post({Command::Type::TbarEnd, 0, 0, 0.f});
            break;
        case Op::DskSet: {
            if (!checkDsk(r.a)) break;
            // The engine only has toggle; reach the requested end state by
            // toggling conditionally on the mirrored target.
            const bool on = engine.uiState().dskOn[r.a];
            if (r.b == 2 || (r.b == 1) != on)
                engine.post({Command::Type::DskToggle, r.a, 0, 0.f});
            break;
        }
        case Op::SetDskSource:
            if (checkDsk(r.a) && checkInput(r.b))
                engine.post({Command::Type::SetDskSource, r.a, r.b, 0.f});
            break;
        case Op::SetDskFade:
            if (checkDsk(r.a))
                engine.post({Command::Type::SetDskFade, r.a, r.b, 0.f});
            break;
        case Op::DskTie: {
            if (!checkDsk(r.a)) break;
            const bool on = r.b == 2 ? !engine.uiState().dskTie[r.a]
                                     : r.b == 1;
            engine.post({Command::Type::SetDskTie, r.a, on ? 1 : 0, 0.f});
            break;
        }
        case Op::DskAudioFollow: {
            if (!checkDsk(r.a)) break;
            const bool on = r.b == 2
                                ? !engine.uiState().dskAudioFollow[r.a]
                                : r.b == 1;
            engine.post(
                {Command::Type::SetDskAudioFollow, r.a, on ? 1 : 0, 0.f});
            break;
        }
        case Op::MediaPlay:
        case Op::MediaPause:
            if (checkInput(r.a))
                engine.post({Command::Type::MediaSetPlaying, r.a,
                             r.op == Op::MediaPlay ? 1 : 0, 0.f});
            break;
        case Op::MediaRestart:
            if (checkInput(r.a))
                engine.post({Command::Type::MediaRestart, r.a, 0, 0.f});
            break;
        case Op::MediaStep:
            if (checkInput(r.a))
                engine.post({Command::Type::MediaStep, r.a, r.b, 0.f});
            break;
        case Op::MediaLoop:
            if (checkInput(r.a))
                engine.post({Command::Type::MediaSetLoop, r.a, r.b, 0.f});
            break;
        case Op::RecordStart:
        case Op::CleanRecordStart:
        case Op::RecordToggle:
        case Op::CleanRecordToggle: {
            const bool clean =
                r.op == Op::CleanRecordStart || r.op == Op::CleanRecordToggle;
            const auto state = clean ? engine.cleanRecordingState()
                                     : engine.recordingState();
            const bool running = state.active || state.pending;
            const bool toggle =
                r.op == Op::RecordToggle || r.op == Op::CleanRecordToggle;
            if (running) {
                if (!toggle) {
                    fail(clean ? "clean record already running"
                               : "record already running");
                    break;
                }
                clean ? engine.requestCleanRecording({})
                      : engine.requestRecording({});
                break;
            }
            const std::string path =
                r.s.empty() ? defaultRecordPath(clean) : r.s;
            clean ? engine.requestCleanRecording(path)
                  : engine.requestRecording(path);
            break;
        }
        case Op::RecordStop: engine.requestRecording({}); break;
        case Op::CleanRecordStop: engine.requestCleanRecording({}); break;
        case Op::AudioMute:
        case Op::AudioSolo:
        case Op::AudioGain: {
            auto* aud = engine.audio();
            if (!aud) {
                fail("audio is disabled");
                break;
            }
            if (!checkInput(r.a) || r.a >= aud->inputCount()) break;
            auto& ch = aud->channel(r.a);
            if (r.op == Op::AudioGain) {
                ch.gain.store(r.f);
            } else {
                auto& flag = r.op == Op::AudioMute ? ch.mute : ch.solo;
                flag.store(r.b == 2 ? !flag.load() : r.b == 1);
            }
            break;
        }
        case Op::Subscribe:
        case Op::Unsubscribe: break;  // transport-level
        case Op::GetState: res.reply = toJson(snapshot(engine)); break;
        case Op::Ping: res.reply = "{\"event\":\"pong\"}"; break;
    }
    return res;
}

}  // namespace kloud::ctl
