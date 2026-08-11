/* 8Kloud Switcher — a live video switcher for Linux + NVIDIA.
 * Copyright (c) 2026 Devin Block
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "ui/ShowFile.h"

#include <QDir>
#include <QFileInfo>
#include <QSettings>
#include <QStandardPaths>

#include <algorithm>
#include <cmath>

namespace kloud::ui {

bool ShowFile::State::cfgEquals(const EngineConfig& a, const EngineConfig& b) {
    if (a.inputs.size() != b.inputs.size()) return false;
    for (size_t i = 0; i < a.inputs.size(); ++i)
        if (a.inputs[i].type != b.inputs[i].type ||
            a.inputs[i].ref != b.inputs[i].ref ||
            a.inputs[i].syncFrames != b.inputs[i].syncFrames ||
            a.inputs[i].mediaLoop != b.inputs[i].mediaLoop ||
            a.inputs[i].mediaPlaylist != b.inputs[i].mediaPlaylist)
            return false;
    return a.show == b.show && a.omtOut == b.omtOut &&
           a.omtOutName == b.omtOutName &&
           a.cleanOmtOut == b.cleanOmtOut &&
           a.cleanOmtOutName == b.cleanOmtOutName &&
           a.mvOmtOut == b.mvOmtOut && a.mvOmtOutName == b.mvOmtOutName &&
           a.mvW == b.mvW && a.mvH == b.mvH &&
           a.sdiOutRef == b.sdiOutRef &&
           a.cleanSdiOutRef == b.cleanSdiOutRef &&
           a.srtUrl == b.srtUrl &&
           a.srtBitrateKbps == b.srtBitrateKbps &&
           a.recordBitrateKbps == b.recordBitrateKbps && a.audio == b.audio &&
           a.masterAudioDelayMs == b.masterAudioDelayMs;
}

ShowFile::ShowFile(QString path) : path_(std::move(path)) {
    if (path_.isEmpty())
        path_ = QStandardPaths::writableLocation(
                    QStandardPaths::AppConfigLocation) +
                QStringLiteral("/show.ini");
    QDir().mkpath(QFileInfo(path_).absolutePath());
}

bool ShowFile::exists() const { return QFileInfo::exists(path_); }

bool ShowFile::load(State& st) const {
    if (!exists()) return false;
    QSettings s(path_, QSettings::IniFormat);

    s.beginGroup(QStringLiteral("show"));
    // Keep the defaults rather than refusing to start when a show file has
    // been hand-edited into something Engine::start would reject.
    const int w = s.value("width", st.cfg.show.width).toInt();
    const int h = s.value("height", st.cfg.show.height).toInt();
    if (w >= 2 && h >= 2 && !(w & 1) && !(h & 1) && w <= 8192 && h <= 8192) {
        st.cfg.show.width = w;
        st.cfg.show.height = h;
    }
    const qlonglong fpsN =
        s.value("fpsN", qlonglong(st.cfg.show.fpsN)).toLongLong();
    const qlonglong fpsD =
        s.value("fpsD", qlonglong(st.cfg.show.fpsD)).toLongLong();
    if (fpsN > 0 && fpsD > 0) {
        st.cfg.show.fpsN = fpsN;
        st.cfg.show.fpsD = fpsD;
    }
    st.cfg.omtOut = s.value("omtOut", st.cfg.omtOut).toBool();
    st.cfg.omtOutName =
        s.value("omtOutName", QString::fromStdString(st.cfg.omtOutName))
            .toString()
            .toStdString();
    st.cfg.cleanOmtOut =
        s.value("cleanOmtOut", st.cfg.cleanOmtOut).toBool();
    st.cfg.cleanOmtOutName =
        s.value("cleanOmtOutName",
                QString::fromStdString(st.cfg.cleanOmtOutName))
            .toString()
            .toStdString();
    st.cfg.mvOmtOut = s.value("mvOmtOut", st.cfg.mvOmtOut).toBool();
    st.cfg.mvOmtOutName =
        s.value("mvOmtOutName", QString::fromStdString(st.cfg.mvOmtOutName))
            .toString()
            .toStdString();
    // Guard the stored geometry: the pack ring and every OMT receiver size
    // themselves from it once, at start.
    const int mvW = s.value("mvW", st.cfg.mvW).toInt();
    const int mvH = s.value("mvH", st.cfg.mvH).toInt();
    if (mvW >= 320 && mvH >= 180 && mvW <= 7680 && mvH <= 4320) {
        st.cfg.mvW = mvW;
        st.cfg.mvH = mvH;
    }
    st.cfg.sdiOutRef =
        s.value("sdiOut", QString::fromStdString(st.cfg.sdiOutRef))
            .toString()
            .toStdString();
    st.cfg.cleanSdiOutRef =
        s.value("cleanSdiOut", QString::fromStdString(st.cfg.cleanSdiOutRef))
            .toString()
            .toStdString();
    st.cfg.srtUrl = s.value("srtOut", QString::fromStdString(st.cfg.srtUrl))
                        .toString()
                        .toStdString();
    st.cfg.srtBitrateKbps =
        s.value("srtBitrateKbps", st.cfg.srtBitrateKbps).toInt();
    st.cfg.recordBitrateKbps =
        s.value("recordBitrateKbps", st.cfg.recordBitrateKbps).toInt();
    st.cfg.audio = s.value("audio", st.cfg.audio).toBool();
    st.cfg.masterAudioDelayMs =
        s.value("masterDelayMs", st.cfg.masterAudioDelayMs).toInt();
    s.endGroup();

    const int n = s.beginReadArray(QStringLiteral("inputs"));
    if (n > 0) st.cfg.inputs.clear();
    for (int i = 0; i < n; ++i) {
        s.setArrayIndex(i);
        InputSpec spec;
        // Shared with the remote-control state so the two names cannot drift.
        // Unknown types (notably "ndi" from a pre-OMT show) fall through to
        // OMT: the ref will not resolve, so the input stays black until the
        // operator re-patches it.
        spec.type = inputTypeFromName(
            s.value("type").toString().toStdString());
        spec.ref = s.value("ref").toString().toStdString();
        // Absent in v1 show files -> stays off (-1).
        spec.syncFrames = s.value("framesync", spec.syncFrames).toInt();
        if (spec.syncFrames < -1 || spec.syncFrames > 4) spec.syncFrames = -1;
        spec.mediaLoop = s.value("mediaLoop", spec.mediaLoop).toBool();
        if (spec.type == InputSpec::Type::Media) {
            const QStringList playlist =
                s.value("mediaPlaylist").toStringList();
            const QStringList trimIns =
                s.value("mediaTrimInMs").toStringList();
            const QStringList trimOuts =
                s.value("mediaTrimOutMs").toStringList();
            const QStringList speeds =
                s.value("mediaSpeedPermille").toStringList();
            for (int item = 0; item < playlist.size(); ++item) {
                if (playlist[item].isEmpty()) continue;
                const int64_t inMs =
                    item < trimIns.size() ? trimIns[item].toLongLong() : 0;
                const int64_t outMs =
                    item < trimOuts.size() ? trimOuts[item].toLongLong() : 0;
                const int speedPermille =
                    item < speeds.size() ? speeds[item].toInt() : 1000;
                media::PlaylistItem clip{playlist[item].toStdString(), inMs,
                                         outMs, speedPermille};
                media::normalizePlaylistItem(clip);
                spec.mediaPlaylist.push_back(std::move(clip));
            }
            if (spec.mediaPlaylist.empty() && !spec.ref.empty())
                spec.mediaPlaylist.emplace_back(spec.ref);
            if (!spec.mediaPlaylist.empty())
                spec.ref = spec.mediaPlaylist.front().path;
        }
        st.cfg.inputs.push_back(std::move(spec));
    }
    s.endArray();

    s.beginGroup(QStringLiteral("switcher"));
    st.program = s.value("program", st.program).toInt();
    st.preview = s.value("preview", st.preview).toInt();
    st.transType = s.value("transType", st.transType).toInt();
    st.transDurTicks = s.value("transDurTicks", st.transDurTicks).toInt();
    s.endGroup();

    st.chans.assign(st.cfg.inputs.size(), ChannelState{});
    const int c = s.beginReadArray(QStringLiteral("audioChannels"));
    for (int i = 0; i < c && i < int(st.chans.size()); ++i) {
        s.setArrayIndex(i);
        auto& ch = st.chans[size_t(i)];
        // Same range the control protocol enforces; a non-finite or wild
        // stored gain would otherwise go straight into the mixer.
        const float g = s.value("gain", ch.gain).toFloat();
        if (std::isfinite(g)) ch.gain = std::clamp(g, 0.f, 4.f);
        ch.mute = s.value("mute", ch.mute).toBool();
        ch.solo = s.value("solo", ch.solo).toBool();
        ch.delayMs = s.value("delayMs", ch.delayMs).toInt();
    }
    s.endArray();

    // Absent in pre-DSK show files -> defaults (off).
    const int nd = s.beginReadArray(QStringLiteral("dsk"));
    for (int i = 0; i < nd && i < kDskCount; ++i) {
        s.setArrayIndex(i);
        auto& d = st.dsk[i];
        d.source = std::clamp(s.value("source", d.source).toInt(), 0,
                              std::max(0, int(st.cfg.inputs.size()) - 1));
        d.fadeDurTicks =
            std::clamp(s.value("fadeDurTicks", d.fadeDurTicks).toInt(), 1, 600);
        d.on = s.value("on", d.on).toBool();
        d.tie = s.value("tie", d.tie).toBool();
        d.audioFollow = s.value("audioFollow", d.audioFollow).toBool();
    }
    s.endArray();
    return true;
}

void ShowFile::save(const State& st) const {
    QSettings s(path_, QSettings::IniFormat);

    s.beginGroup(QStringLiteral("show"));
    s.setValue("width", st.cfg.show.width);
    s.setValue("height", st.cfg.show.height);
    s.setValue("fpsN", qlonglong(st.cfg.show.fpsN));
    s.setValue("fpsD", qlonglong(st.cfg.show.fpsD));
    s.setValue("omtOut", st.cfg.omtOut);
    s.setValue("omtOutName", QString::fromStdString(st.cfg.omtOutName));
    s.setValue("cleanOmtOut", st.cfg.cleanOmtOut);
    s.setValue("cleanOmtOutName",
               QString::fromStdString(st.cfg.cleanOmtOutName));
    s.setValue("mvOmtOut", st.cfg.mvOmtOut);
    s.setValue("mvOmtOutName", QString::fromStdString(st.cfg.mvOmtOutName));
    s.setValue("mvW", st.cfg.mvW);
    s.setValue("mvH", st.cfg.mvH);
    s.setValue("sdiOut", QString::fromStdString(st.cfg.sdiOutRef));
    s.setValue("cleanSdiOut", QString::fromStdString(st.cfg.cleanSdiOutRef));
    s.setValue("srtOut", QString::fromStdString(st.cfg.srtUrl));
    s.setValue("srtBitrateKbps", st.cfg.srtBitrateKbps);
    s.setValue("recordBitrateKbps", st.cfg.recordBitrateKbps);
    s.setValue("audio", st.cfg.audio);
    s.setValue("masterDelayMs", st.cfg.masterAudioDelayMs);
    s.endGroup();

    s.beginWriteArray(QStringLiteral("inputs"), int(st.cfg.inputs.size()));
    for (int i = 0; i < int(st.cfg.inputs.size()); ++i) {
        s.setArrayIndex(i);
        const auto& spec = st.cfg.inputs[size_t(i)];
        s.setValue("type", QString::fromLatin1(inputTypeName(spec.type)));
        s.setValue("ref", QString::fromStdString(spec.ref));
        s.setValue("framesync", spec.syncFrames);
        if (spec.type == InputSpec::Type::Media) {
            s.setValue("mediaLoop", spec.mediaLoop);
            QStringList playlist;
            QStringList trimIns;
            QStringList trimOuts;
            QStringList speeds;
            for (const auto& item : spec.mediaPlaylist) {
                playlist << QString::fromStdString(item.path);
                trimIns << QString::number(item.inMs);
                trimOuts << QString::number(item.outMs);
                speeds << QString::number(item.speedPermille);
            }
            if (playlist.isEmpty() && !spec.ref.empty())
                playlist << QString::fromStdString(spec.ref);
            s.setValue("mediaPlaylist", playlist);
            s.setValue("mediaTrimInMs", trimIns);
            s.setValue("mediaTrimOutMs", trimOuts);
            s.setValue("mediaSpeedPermille", speeds);
        } else {
            s.remove("mediaLoop");
            s.remove("mediaPlaylist");
            s.remove("mediaTrimInMs");
            s.remove("mediaTrimOutMs");
            s.remove("mediaSpeedPermille");
        }
        // A restored show cues media from its start and plays; pause is an
        // ephemeral transport state, not a startup mode.
        s.remove("mediaPlaying");
    }
    s.endArray();

    s.beginGroup(QStringLiteral("switcher"));
    s.setValue("program", st.program);
    s.setValue("preview", st.preview);
    s.setValue("transType", st.transType);
    s.setValue("transDurTicks", st.transDurTicks);
    s.endGroup();

    s.beginWriteArray(QStringLiteral("audioChannels"), int(st.chans.size()));
    for (int i = 0; i < int(st.chans.size()); ++i) {
        s.setArrayIndex(i);
        const auto& ch = st.chans[size_t(i)];
        s.setValue("gain", double(ch.gain));
        s.setValue("mute", ch.mute);
        s.setValue("solo", ch.solo);
        s.setValue("delayMs", ch.delayMs);
    }
    s.endArray();

    s.beginWriteArray(QStringLiteral("dsk"), kDskCount);
    for (int i = 0; i < kDskCount; ++i) {
        s.setArrayIndex(i);
        s.setValue("source", st.dsk[i].source);
        s.setValue("fadeDurTicks", st.dsk[i].fadeDurTicks);
        s.setValue("on", st.dsk[i].on);
        s.setValue("tie", st.dsk[i].tie);
        s.setValue("audioFollow", st.dsk[i].audioFollow);
    }
    s.endArray();
}

}  // namespace kloud::ui
