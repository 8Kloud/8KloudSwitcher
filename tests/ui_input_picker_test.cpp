/* 8Kloud Switcher — a live video switcher for Linux + NVIDIA.
 * Copyright (c) 2026 Devin Block
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include <catch2/catch_test_macros.hpp>

#include <QApplication>
#include <QComboBox>
#include <QThread>

#include <functional>
#include <memory>

#include "engine/Engine.h"
#include "ui/EngineBridge.h"
#include "ui/MainWindow.h"

using namespace kloud;
using namespace kloud::ui;

namespace {

QApplication& application() {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    static int argc = 1;
    static char name[] = "kloud-ui-tests";
    static char* argv[] = {name, nullptr};
    static auto app = std::make_unique<QApplication>(argc, argv);
    return *app;
}

bool waitFor(const std::function<bool()>& done, int timeoutMs = 3000) {
    for (int elapsed = 0; elapsed < timeoutMs; elapsed += 10) {
        if (done()) return true;
        QCoreApplication::processEvents();
        QThread::msleep(10);
    }
    return done();
}

}  // namespace

TEST_CASE("input picker assigns and clears sources") {
    (void)application();

    Engine engine;
    EngineConfig cfg;
    cfg.omtOut = false;
    cfg.audio = false;
    cfg.inputs = {{InputSpec::Type::Omt, ""}, {InputSpec::Type::Omt, ""}};
    REQUIRE(engine.start(cfg));

    {
        EngineBridge bridge(engine);
        MainWindow window(bridge, {QString(), QString()});

        const auto pickers =
            window.findChildren<QComboBox*>(QStringLiteral("inputPicker"));
        REQUIRE(pickers.size() == 2);
        QComboBox* picker = pickers[0];

        // Closed pickers show only the current assignment.
        CHECK(picker->count() == 1);
        CHECK(picker->currentText() == QStringLiteral("BLACK"));

        // Opening rebuilds from live discovery: BLACK heads the list, the
        // manual SRT entry ends it, and an unassigned input selects BLACK.
        picker->showPopup();
        picker->hidePopup();
        REQUIRE(picker->count() >= 2);
        CHECK(picker->itemText(0) == QStringLiteral("BLACK"));
        CHECK(picker->itemText(picker->count() - 1) ==
              QStringLiteral("SRT URL…"));
        CHECK(picker->currentIndex() == 0);

        // Activating a (synthetic) discovery row patches the engine input.
        picker->addItem(QStringLiteral("OMT · KloudCam"),
                        int(InputSpec::Type::Omt));
        picker->setItemData(picker->count() - 1, QStringLiteral("KloudCam"),
                            Qt::UserRole + 1);
        emit picker->activated(picker->count() - 1);
        REQUIRE(waitFor([&] { return engine.inputRef(0) == "KloudCam"; }));
        CHECK(engine.inputType(0) == InputSpec::Type::Omt);

        // The bridge poll collapses the picker to the new assignment.
        REQUIRE(waitFor([&] {
            return picker->count() == 1 &&
                   picker->currentText() == QStringLiteral("KloudCam");
        }));

        // Reopening keeps the offline assignment visible and selected while
        // BLACK stays first; picking BLACK unassigns the input again.
        picker->showPopup();
        picker->hidePopup();
        CHECK(picker->currentText() == QStringLiteral("KloudCam"));
        CHECK(picker->itemText(0) == QStringLiteral("BLACK"));
        emit picker->activated(0);
        REQUIRE(waitFor([&] { return engine.inputRef(0).empty(); }));
    }

    // Replaced inputs are destroyed on detached threads; give them time to
    // finish before process teardown tears the runtime out from under them.
    QThread::msleep(400);
    engine.stop();
}

TEST_CASE("input picker assigns a DeckLink source") {
    (void)application();

    Engine engine;
    EngineConfig cfg;
    cfg.omtOut = false;
    cfg.audio = false;
    cfg.inputs = {{InputSpec::Type::Omt, ""}};
    REQUIRE(engine.start(cfg));

    {
        EngineBridge bridge(engine);
        MainWindow window(bridge, {QString()});

        const auto pickers =
            window.findChildren<QComboBox*>(QStringLiteral("inputPicker"));
        REQUIRE(pickers.size() == 1);
        QComboBox* picker = pickers[0];

        // A decklink:// row carries the DeckLink type explicitly; the ref is
        // what reaches the engine. Synthetic so the test does not need a card.
        picker->showPopup();
        picker->hidePopup();
        picker->addItem(QStringLiteral("SDI · DeckLink 8K Pro (1)"),
                        int(InputSpec::Type::DeckLink));
        picker->setItemData(picker->count() - 1,
                            QStringLiteral("decklink://0"), Qt::UserRole + 1);
        emit picker->activated(picker->count() - 1);

        REQUIRE(waitFor([&] { return engine.inputRef(0) == "decklink://0"; }));
        CHECK(engine.inputType(0) == InputSpec::Type::DeckLink);

        // Collapsed label uses the SDI prefix, not the raw URL.
        REQUIRE(waitFor([&] {
            return picker->count() == 1 &&
                   picker->currentText() == QStringLiteral("SDI · 0");
        }));
    }

    QThread::msleep(400);
    engine.stop();
}
