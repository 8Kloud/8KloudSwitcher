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

#include <catch2/catch_test_macros.hpp>

#include "ctl/ControlProtocol.h"
#include "engine/InputSpec.h"

using kloud::ctl::Request;
using kloud::ctl::parseLine;

namespace {
Request must(const char* line) {
    std::string err;
    auto r = parseLine(line, err);
    INFO(line << " -> " << err);
    REQUIRE(r.has_value());
    return *r;
}

void mustFail(const char* line) {
    std::string err;
    auto r = parseLine(line, err);
    INFO(line);
    REQUIRE(!r.has_value());
    REQUIRE(!err.empty());
}
}  // namespace

TEST_CASE("control: simple commands parse case-insensitively") {
    CHECK(must("CUT").op == Request::Op::Cut);
    CHECK(must("cut").op == Request::Op::Cut);
    CHECK(must("Take").op == Request::Op::Cut);
    CHECK(must("AUTO").op == Request::Op::Auto);
    CHECK(must("ftb").op == Request::Op::Ftb);
    CHECK(must("SUBSCRIBE").op == Request::Op::Subscribe);
    CHECK(must("state").op == Request::Op::GetState);
    CHECK(must("PING").op == Request::Op::Ping);
}

TEST_CASE("control: empty and comment lines are silently ignored") {
    std::string err;
    CHECK(!parseLine("", err));
    CHECK(err.empty());
    CHECK(!parseLine("   ", err));
    CHECK(err.empty());
    CHECK(!parseLine("# comment", err));
    CHECK(err.empty());
}

TEST_CASE("control: bus assigns are 1-based on the wire") {
    const auto pgm = must("PGM 3");
    CHECK(pgm.op == Request::Op::SetProgram);
    CHECK(pgm.a == 2);
    const auto prog = must("PROGRAM 1");
    CHECK(prog.op == Request::Op::SetProgram);
    CHECK(prog.a == 0);
    const auto pvw = must("PVW 21");
    CHECK(pvw.op == Request::Op::SetPreview);
    CHECK(pvw.a == 20);
    CHECK(must("preview 2").op == Request::Op::SetPreview);
    mustFail("PGM 0");
    mustFail("PGM");
    mustFail("PGM x");
}

TEST_CASE("control: transition with optional duration and softness") {
    const auto keep = must("TRANSITION wipelr");
    CHECK(keep.op == Request::Op::SetTransition);
    CHECK(keep.a == 1);
    CHECK(keep.b == 0);       // keep duration
    CHECK(keep.f == -1.f);    // keep softness
    const auto full = must("transition WipeCircle 45 0.05");
    CHECK(full.a == 6);
    CHECK(full.b == 45);
    CHECK(full.f == 0.05f);
    mustFail("TRANSITION spin");
    mustFail("TRANSITION mix 0");
    mustFail("TRANSITION mix 30 2.0");
}

TEST_CASE("control: tbar") {
    CHECK(must("TBAR BEGIN").op == Request::Op::TbarBegin);
    CHECK(must("TBAR END").op == Request::Op::TbarEnd);
    const auto mid = must("TBAR 0.5");
    CHECK(mid.op == Request::Op::TbarSet);
    CHECK(mid.f == 0.5f);
    mustFail("TBAR 1.5");
    mustFail("TBAR");
}

TEST_CASE("control: dsk") {
    const auto on = must("DSK 1 ON");
    CHECK(on.op == Request::Op::DskSet);
    CHECK(on.a == 0);
    CHECK(on.b == 1);
    const auto off = must("dsk 2 off");
    CHECK(off.a == 1);
    CHECK(off.b == 0);
    CHECK(must("DSK 1 TOGGLE").b == 2);
    const auto src = must("DSK 2 SRC 5");
    CHECK(src.op == Request::Op::SetDskSource);
    CHECK(src.a == 1);
    CHECK(src.b == 4);
    const auto fade = must("DSK 1 FADE 30");
    CHECK(fade.op == Request::Op::SetDskFade);
    CHECK(fade.b == 30);
    const auto tie = must("DSK 1 TIE ON");
    CHECK(tie.op == Request::Op::DskTie);
    CHECK(tie.a == 0);
    CHECK(tie.b == 1);
    CHECK(must("dsk 2 tie").b == 2);  // bare form toggles
    const auto afv = must("DSK 2 AFV OFF");
    CHECK(afv.op == Request::Op::DskAudioFollow);
    CHECK(afv.a == 1);
    CHECK(afv.b == 0);
    CHECK(must("DSK 1 FOLLOW TOGGLE").op == Request::Op::DskAudioFollow);
    mustFail("DSK 0 ON");
    mustFail("DSK 1 BLINK");
    mustFail("DSK 1 SRC 0");
    mustFail("DSK 1 TIE MAYBE");
    mustFail("DSK 1 TIE ON EXTRA");
}

TEST_CASE("control: media") {
    const auto play = must("MEDIA 4 PLAY");
    CHECK(play.op == Request::Op::MediaPlay);
    CHECK(play.a == 3);
    CHECK(must("MEDIA 4 PAUSE").op == Request::Op::MediaPause);
    CHECK(must("MEDIA 4 RESTART").op == Request::Op::MediaRestart);
    CHECK(must("MEDIA 4 NEXT").b == 1);
    CHECK(must("MEDIA 4 PREV").b == -1);
    const auto loop = must("MEDIA 4 LOOP OFF");
    CHECK(loop.op == Request::Op::MediaLoop);
    CHECK(loop.b == 0);
    mustFail("MEDIA 4 LOOP TOGGLE");
    mustFail("MEDIA 4 EJECT");
}

TEST_CASE("control: record paths keep embedded spaces") {
    const auto bare = must("RECORD START");
    CHECK(bare.op == Request::Op::RecordStart);
    CHECK(bare.s.empty());
    const auto path = must("RECORD START /tmp/my show 1.mkv");
    CHECK(path.s == "/tmp/my show 1.mkv");
    CHECK(must("RECORD STOP").op == Request::Op::RecordStop);
    CHECK(must("RECORD TOGGLE").op == Request::Op::RecordToggle);
    CHECK(must("CLEAN START").op == Request::Op::CleanRecordStart);
    CHECK(must("CLEAN STOP").op == Request::Op::CleanRecordStop);
    CHECK(must("clean toggle x.mkv").s == "x.mkv");
    mustFail("RECORD");
    mustFail("RECORD EJECT");
}

TEST_CASE("control: audio") {
    const auto mute = must("AUDIO 3 MUTE TOGGLE");
    CHECK(mute.op == Request::Op::AudioMute);
    CHECK(mute.a == 2);
    CHECK(mute.b == 2);
    CHECK(must("AUDIO 1 SOLO ON").op == Request::Op::AudioSolo);
    const auto gain = must("AUDIO 2 GAIN 0.75");
    CHECK(gain.op == Request::Op::AudioGain);
    CHECK(gain.f == 0.75f);
    mustFail("AUDIO 1 GAIN 9");
    mustFail("AUDIO 1 MUTE");
}

TEST_CASE("control: non-finite floats are rejected, not range-checked") {
    // Every float range check is `x < lo || x > hi`, and both comparisons are
    // false against NaN -- so the range checks alone let it through. A NaN
    // gain poisons the mixer, a NaN tbar wedges the transition (tbarPos_ >= 1
    // never trips, and std::clamp passes NaN through untouched), and either
    // makes the state JSON emit a bare `nan` that no client can parse.
    for (const char* v : {"nan", "NaN", "-nan", "inf", "-inf", "INF"}) {
        mustFail((std::string("AUDIO 1 GAIN ") + v).c_str());
        mustFail((std::string("TBAR ") + v).c_str());
        mustFail((std::string("TRANSITION mix 30 ") + v).c_str());
    }
    // Finite values in range still parse.
    CHECK(must("AUDIO 1 GAIN 0").f == 0.f);
    CHECK(must("AUDIO 1 GAIN 4").f == 4.f);
    CHECK(must("TBAR 0.5").f == 0.5f);
    CHECK(must("TRANSITION mix 30 0.25").f == 0.25f);
}

TEST_CASE("control: state JSON is stable, 1-based, and escaped") {
    kloud::ctl::Snapshot s;
    s.program = 0;
    s.preview = 2;
    s.transitionType = 1;
    s.dsk.resize(2);
    s.dsk[1].on = true;
    s.dsk[1].level = 1.f;
    s.dsk[1].src = 4;
    s.dsk[1].tie = true;
    s.record.active = true;
    s.record.frames = 120;
    s.record.path = "/tmp/a \"b\".mkv";
    s.audioAvailable = true;
    s.inputs.resize(2);
    s.inputs[0].ref = "CAM \\ ONE";
    s.inputs[0].connected = true;
    s.inputs[1].media.available = true;
    s.inputs[1].media.playing = true;
    s.inputs[1].media.playlistSize = 3;

    const std::string j = kloud::ctl::toJson(s);
    CHECK(j == kloud::ctl::toJson(s));  // deterministic
    CHECK(j.find("\"program\":1") != std::string::npos);
    CHECK(j.find("\"preview\":3") != std::string::npos);
    CHECK(j.find("\"transition\":\"wipelr\"") != std::string::npos);
    CHECK(j.find("\"src\":5") != std::string::npos);
    CHECK(j.find("\"src\":5,\"tie\":true,\"afv\":false") != std::string::npos);
    CHECK(j.find("\\\"b\\\"") != std::string::npos);
    CHECK(j.find("CAM \\\\ ONE") != std::string::npos);
    CHECK(j.find("\"item\":1") != std::string::npos);
    CHECK(j.find("\"items\":3") != std::string::npos);
    // Unassigned media input 1 carries no media object.
    const size_t in1 = j.find("\"n\":1");
    const size_t in2 = j.find("\"n\":2");
    REQUIRE(in1 != std::string::npos);
    REQUIRE(in2 != std::string::npos);
    CHECK(j.find("\"media\"", in1) > in2);
}

TEST_CASE("control: input type is a stable name, not the enum's number") {
    // The numbering has already shifted once (removing NDI moved every member
    // but Srt), so the wire must never carry it.
    CHECK(std::string(kloud::inputTypeName(kloud::InputSpec::Type::Omt)) ==
          "omt");
    CHECK(std::string(kloud::inputTypeName(kloud::InputSpec::Type::Srt)) ==
          "srt");
    CHECK(std::string(kloud::inputTypeName(kloud::InputSpec::Type::Media)) ==
          "media");
    CHECK(std::string(kloud::inputTypeName(kloud::InputSpec::Type::Still)) ==
          "still");
    CHECK(std::string(kloud::inputTypeName(kloud::InputSpec::Type::DeckLink)) ==
          "decklink");

    // Round-trip, and the fallback an older/newer show file relies on.
    for (auto t : {kloud::InputSpec::Type::Omt, kloud::InputSpec::Type::Srt,
                   kloud::InputSpec::Type::Media, kloud::InputSpec::Type::Still,
                   kloud::InputSpec::Type::DeckLink})
        CHECK(kloud::inputTypeFromName(kloud::inputTypeName(t)) == t);
    CHECK(kloud::inputTypeFromName("ndi") == kloud::InputSpec::Type::Omt);
    CHECK(kloud::inputTypeFromName("") == kloud::InputSpec::Type::Omt);

    kloud::ctl::Snapshot s;
    s.inputs.resize(1);
    s.inputs[0].type = kloud::inputTypeName(kloud::InputSpec::Type::DeckLink);
    const std::string j = kloud::ctl::toJson(s);
    CHECK(j.find("\"type\":\"decklink\"") != std::string::npos);
}

TEST_CASE("control: state JSON reports every program output") {
    kloud::ctl::Snapshot s;
    s.omtOut = {true, true, "8Kloud Switcher PGM", 120};
    s.cleanOmtOut = {false, false, "", 0};
    // Configured but not up: the card is absent or already capturing. This is
    // the case an operator surface has to be able to show.
    s.sdiOut = {true, false, "decklink://0", 0};
    s.cleanSdiOut = {true, true, "decklink://1", 99};

    const std::string j = kloud::ctl::toJson(s);
    CHECK(j == kloud::ctl::toJson(s));  // deterministic
    CHECK(j.find("\"omtOut\":{\"configured\":true,\"up\":true,"
                 "\"name\":\"8Kloud Switcher PGM\",\"frames\":120}") !=
          std::string::npos);
    CHECK(j.find("\"cleanOmtOut\":{\"configured\":false,\"up\":false,"
                 "\"name\":\"\",\"frames\":0}") != std::string::npos);
    CHECK(j.find("\"sdiOut\":{\"configured\":true,\"up\":false,"
                 "\"name\":\"decklink://0\",\"frames\":0}") !=
          std::string::npos);
    CHECK(j.find("\"cleanSdiOut\":{\"configured\":true,\"up\":true,"
                 "\"name\":\"decklink://1\",\"frames\":99}") !=
          std::string::npos);
}
