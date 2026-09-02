# Test fixtures

`qt-show-fixture.ini` was written by Qt 6 `QSettings::IniFormat` (the format
the retired Qt GUI used for `show.ini`) from this program, so `core/Ini.cpp`
can be held to the real dialect byte for byte (`tests/ini_test.cpp`). To
regenerate it, build against Qt6Core and run it in this directory:

```cpp
#include <QCoreApplication>
#include <QSettings>
#include <QStringList>
int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    QSettings s("qt-show-fixture.ini", QSettings::IniFormat);
    s.beginGroup("show");
    s.setValue("width", 1920);
    s.setValue("omtOut", true);
    s.setValue("cleanOmtOut", false);
    s.setValue("omtOutName", "8Kloud Switcher PGM");
    s.setValue("empty", "");
    s.setValue("fpsN", qlonglong(60000));
    s.setValue("gainD", 0.5);
    s.setValue("gainOne", 1.0);
    s.setValue("gainThird", 1.0 / 3.0);
    s.setValue("paren", "HOST (CamA)");
    s.setValue("comma", "a,b");
    s.setValue("semi", "a;b");
    s.setValue("eq", "a=b");
    s.setValue("quote", "say \"hi\"");
    s.setValue("backslash", "C:\\x\\y");
    s.setValue("lead", " lead");
    s.setValue("trail", "trail ");
    s.setValue("unicode", QString::fromUtf8("SRT \xc2\xb7 caf\xc3\xa9 \xe2\x80\x94 \xf0\x9f\x90\x84"));
    s.setValue("newline", "a\nb\tc");
    s.setValue("ctrl", QString("x") + QChar(1) + "y");
    s.setValue("at", "@Invalid()");
    s.setValue("hash", "#notcomment");
    s.setValue("url", "srt://host:9710?mode=caller&latency=120000");
    s.setValue("listEmpty", QStringList());
    s.setValue("listOne", QStringList{"/a b/c.mkv"});
    s.setValue("listTwo", QStringList{"/a,b/c.mkv", "/d/e f.mkv"});
    s.setValue("listNums", QStringList{"500", "1250", "0"});
    s.setValue("listWithEmpty", QStringList{"", "x"});
    s.endGroup();
    s.beginWriteArray("inputs", 2);
    s.setArrayIndex(0);
    s.setValue("type", "omt");
    s.setValue("ref", "HOST (CamA)");
    s.setArrayIndex(1);
    s.setValue("type", "media");
    s.setValue("ref", "/shows/roll in.mkv");
    s.setValue("mediaPlaylist", QStringList{"/shows/roll in.mkv", "/shows/b,c.mkv"});
    s.endArray();
    s.setValue("topLevel", 5);
    s.beginGroup("nested");
    s.beginGroup("deep");
    s.setValue("k", "v");
    s.endGroup();
    s.endGroup();
    return 0;
}
```

```sh
g++ -std=c++20 -fPIC gen.cpp -o gen $(pkg-config --cflags --libs Qt6Core) && ./gen
```
