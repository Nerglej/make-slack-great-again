// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
//
// Media::Recorder: the WAV wrapper, the MSGA_VOICE_FAKE_WAV fake recorder, and
// on Linux the capture-helper chain driven by stub helper scripts on a private
// PATH (fallthrough, stderr in failed(), SIGTERM-ignoring helper). A real
// microphone capture runs only with MSGA_TEST_LIVE_MIC=1.

#include <catch2/catch_test_macros.hpp>

#include "test_main.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QTemporaryDir>
#include <QtEndian>
#include <algorithm>
#include <functional>

#include "media/audio_recorder.h"

MSGA_TEST_MAIN(argc, argv) {
    QCoreApplication app(argc, argv);
    app.setApplicationName("msga-test-audio-recorder");
    app.setOrganizationName("msga-test");
    return msga_test::runCatch(argc, argv);
}

using Media::Recorder;

namespace {

quint32 le32(const QByteArray &b, int at) {
    return qFromLittleEndian<quint32>(b.constData() + at);
}
quint16 le16(const QByteArray &b, int at) {
    return qFromLittleEndian<quint16>(b.constData() + at);
}

// Every signal a recorder emits, in order.
struct Probe {
    QStringList events;
    QByteArray  wav;
    QString     error;
    int         levels  = 0;
    float       maxPeak = 0;

    explicit Probe(Recorder *r) {
        QObject::connect(r, &Recorder::started, [this] { events << "started"; });
        QObject::connect(r, &Recorder::level, [this](float p) {
            CHECK(p >= 0.0f);
            CHECK(p <= 1.0f);
            maxPeak = std::max(maxPeak, p);
            if (levels++ == 0)
                events << "level";
        });
        QObject::connect(r, &Recorder::finished, [this](QByteArray w) {
            events << "finished";
            wav = w;
        });
        QObject::connect(r, &Recorder::failed, [this](QString e) {
            events << "failed";
            error = e;
        });
    }
};

bool spinUntil(const std::function<bool()> &done, int timeoutMs = 5000) {
    QElapsedTimer t;
    t.start();
    while (!done() && t.elapsed() < timeoutMs)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    return done();
}
void spinFor(int ms) {
    spinUntil([] { return false; }, ms);
}

void checkHeader(const QByteArray &wav, quint32 pcmBytes) {
    REQUIRE(wav.size() == 44 + qsizetype(pcmBytes));
    CHECK(wav.left(4) == "RIFF");
    CHECK(le32(wav, 4) == 36 + pcmBytes);
    CHECK(wav.mid(8, 8) == "WAVEfmt ");
    CHECK(le32(wav, 16) == 16);
    CHECK(le16(wav, 20) == 1);
    CHECK(le16(wav, 22) == Recorder::kChannels);
    CHECK(le32(wav, 24) == Recorder::kSampleRate);
    CHECK(le32(wav, 28) == Recorder::kSampleRate * 2 * Recorder::kChannels);
    CHECK(le16(wav, 32) == 2 * Recorder::kChannels);
    CHECK(le16(wav, 34) == 16);
    CHECK(wav.mid(36, 4) == "data");
    CHECK(le32(wav, 40) == pcmBytes);
}

// Sets an environment variable for one scope.
struct EnvGuard {
    QByteArray name, old;
    bool       had;
    EnvGuard(const char *n, const QByteArray &value) : name(n), had(qEnvironmentVariableIsSet(n)) {
        old = qgetenv(n);
        qputenv(n, value);
    }
    ~EnvGuard() {
        if (had)
            qputenv(name.constData(), old);
        else
            qunsetenv(name.constData());
    }
};

} // namespace

TEST_CASE("wavFromPcm16 writes a 44-byte PCM header", "[audio_recorder]") {
    QByteArray pcm(3200, '\x7f');
    const auto wav = Media::wavFromPcm16(pcm, 16000, 1);
    checkHeader(wav, 3200);
    CHECK(wav.mid(44) == pcm);

    const auto stereo = Media::wavFromPcm16({}, 48000, 2);
    REQUIRE(stereo.size() == 44);
    CHECK(le16(stereo, 22) == 2);
    CHECK(le32(stereo, 28) == 48000u * 4);
    CHECK(le16(stereo, 32) == 4);
    CHECK(le32(stereo, 40) == 0);
}

TEST_CASE("fake recorder plays back MSGA_VOICE_FAKE_WAV", "[audio_recorder]") {
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const QString    path = dir.filePath("voice.wav");
    const QByteArray file = Media::wavFromPcm16(QByteArray(16000, '\x10'), 16000, 1);
    {
        QFile f(path);
        REQUIRE(f.open(QIODevice::WriteOnly));
        f.write(file);
    }
    EnvGuard env("MSGA_VOICE_FAKE_WAV", path.toLocal8Bit());

    SECTION("start, level, stop delivers the file as-is") {
        auto  rec = Media::createPlatformRecorder(nullptr);
        Probe probe(rec.get());
        rec->start();
        CHECK(rec->isRecording());
        CHECK(probe.events.isEmpty()); // started() is queued, never synchronous
        REQUIRE(spinUntil([&] { return probe.levels >= 2; }));
        rec->stop();
        CHECK_FALSE(rec->isRecording());
        CHECK(probe.events == QStringList{"started", "level", "finished"});
        CHECK(probe.wav == file);
    }

    SECTION("cancel emits nothing") {
        auto  rec = Media::createPlatformRecorder(nullptr);
        Probe probe(rec.get());
        rec->start();
        rec->cancel();
        CHECK_FALSE(rec->isRecording());
        spinFor(200);
        CHECK(probe.events.isEmpty());

        // And after audio started.
        rec->start();
        REQUIRE(spinUntil([&] { return probe.levels > 0; }));
        rec->cancel();
        probe.events.clear();
        spinFor(150);
        CHECK(probe.events.isEmpty());
    }

    SECTION("an unreadable file falls back to the native recorder") {
        EnvGuard missing("MSGA_VOICE_FAKE_WAV", dir.filePath("missing.wav").toLocal8Bit());
        auto     rec = Media::createPlatformRecorder(nullptr);
        REQUIRE(rec);
        CHECK_FALSE(rec->isRecording());
    }
}

#if defined(Q_OS_LINUX)

namespace {

// A private PATH holding only the given stub helpers (name -> sh body).
struct StubPath {
    QTemporaryDir dir;
    EnvGuard      path;
    explicit StubPath(const QList<std::pair<QString, QString>> &stubs)
        : path("PATH", QByteArray()) {
        qputenv("PATH", dir.path().toLocal8Bit());
        for (const auto &[name, body] : stubs) {
            QFile f(dir.filePath(name));
            REQUIRE(f.open(QIODevice::WriteOnly));
            f.write(("#!/bin/sh\n" + body + "\n").toUtf8());
            f.close();
            f.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
        }
    }
};

} // namespace

TEST_CASE("linux recorder: helper chain", "[audio_recorder]") {
    qunsetenv("MSGA_VOICE_FAKE_WAV");

    SECTION("no helper installed fails synchronously") {
        StubPath stubs({});
        auto     rec = Media::createPlatformRecorder(nullptr);
        Probe    probe(rec.get());
        rec->start();
        CHECK(probe.events == QStringList{"failed"});
        CHECK(probe.error.contains("PipeWire"));
        CHECK_FALSE(rec->isRecording());
    }

    SECTION("a helper that dies hands over to the next; stop wraps the PCM") {
        // 0.25 s of a full-scale square wave, then wait for SIGTERM.
        StubPath stubs({
            {"pw-record", "echo 'no PipeWire here' >&2; exit 1"},
            {"arecord",
             "printf '\\377\\177\\001\\200%.0s' $(/usr/bin/seq 2000); exec /bin/sleep 30"},
        });
        auto     rec = Media::createPlatformRecorder(nullptr);
        Probe    probe(rec.get());
        rec->start();
        REQUIRE(spinUntil([&] { return probe.events.contains("started"); }));
        REQUIRE(spinUntil([&] { return probe.levels > 0; }));
        spinFor(100);
        rec->stop();
        CHECK(rec->isRecording()); // until the helper has exited and drained
        REQUIRE(spinUntil([&] { return probe.events.contains("finished"); }));
        CHECK_FALSE(probe.events.contains("failed"));
        checkHeader(probe.wav, 8000);
        CHECK(qFromLittleEndian<qint16>(probe.wav.constData() + 44) == 32767);
    }

    SECTION("every helper dying surfaces the last stderr line") {
        StubPath stubs({
            {"pw-record", "echo 'first' >&2; exit 1"},
            {"parecord", "echo 'Connection refused' >&2; exit 1"},
        });
        auto     rec = Media::createPlatformRecorder(nullptr);
        Probe    probe(rec.get());
        rec->start();
        REQUIRE(spinUntil([&] { return !probe.events.isEmpty(); }));
        CHECK(probe.events == QStringList{"failed"});
        CHECK(probe.error.endsWith("Connection refused"));
        CHECK_FALSE(rec->isRecording());
    }

    SECTION("a helper ignoring SIGTERM is killed and the take still delivered") {
        StubPath stubs({
            {"pw-record", "trap '' TERM; /usr/bin/head -c 3200 /dev/zero; exec /bin/sleep 30"},
        });
        auto     rec = Media::createPlatformRecorder(nullptr);
        Probe    probe(rec.get());
        rec->start();
        REQUIRE(spinUntil([&] { return probe.events.contains("started"); }));
        spinFor(100);
        QElapsedTimer t;
        t.start();
        rec->stop();
        REQUIRE(spinUntil([&] { return probe.events.contains("finished"); }));
        CHECK(t.elapsed() < 3000);
        checkHeader(probe.wav, 3200);
    }

    SECTION("a helper that runs but never delivers audio times out") {
        StubPath stubs({
            {"pw-record", "echo 'waiting for a source' >&2; exec /bin/sleep 30"},
        });
        auto     rec = Media::createPlatformRecorder(nullptr);
        Probe    probe(rec.get());
        rec->start();
        REQUIRE(spinUntil([&] { return !probe.events.isEmpty(); }, 8000));
        CHECK(probe.events == QStringList{"failed"});
        CHECK(probe.error.endsWith("waiting for a source"));
        CHECK_FALSE(rec->isRecording());
    }

    SECTION("cancel kills the helper and emits nothing") {
        StubPath stubs({
            {"pw-record", "/usr/bin/head -c 3200 /dev/zero; exec /bin/sleep 30"},
        });
        auto     rec = Media::createPlatformRecorder(nullptr);
        Probe    probe(rec.get());
        rec->start();
        REQUIRE(spinUntil([&] { return probe.events.contains("started"); }));
        rec->cancel();
        CHECK_FALSE(rec->isRecording());
        probe.events.clear();
        spinFor(300);
        CHECK(probe.events.isEmpty());
    }
}

// Real capture from the default source: MSGA_TEST_LIVE_MIC=1 msga_tests
// test_audio_recorder "[live]". Needs a sound server with a capture device.
TEST_CASE("linux recorder: live microphone", "[audio_recorder][live]") {
    if (qgetenv("MSGA_TEST_LIVE_MIC") != "1")
        SKIP("set MSGA_TEST_LIVE_MIC=1 to record from the real microphone");
    qunsetenv("MSGA_VOICE_FAKE_WAV");
    auto          rec = Media::createPlatformRecorder(nullptr);
    Probe         probe(rec.get());
    QElapsedTimer t;
    t.start();
    rec->start();
    REQUIRE(spinUntil([&] { return !probe.events.isEmpty(); }));
    REQUIRE(probe.events.first() == "started");
    const qint64 startMs = t.elapsed();
    spinFor(2000);
    t.restart();
    rec->stop();
    REQUIRE(spinUntil([&] { return probe.events.contains("finished"); }));
    const qint64 stopMs = t.elapsed();
    WARN(
        "start latency " << startMs << " ms, stop latency " << stopMs << " ms, " << probe.levels
                         << " level() in ~2 s (max " << probe.maxPeak << "), " << probe.wav.size()
                         << " bytes"
    );
    REQUIRE(probe.wav.size() > 1024);
    checkHeader(probe.wav, quint32(probe.wav.size() - 44));
}

#endif
