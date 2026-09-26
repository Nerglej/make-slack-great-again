// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#include "media/audio_recorder.h"

#include <QCoreApplication>
#include <QProcess>
#include <QStandardPaths>
#include <QStringList>
#include <QTimer>
#include <QtEndian>
#include <algorithm>
#include <cstdlib>
#include <deque>
#include <vector>

// Linux: the sound server's own capture CLI writing raw 16 kHz mono s16le to
// stdout — the mirror of the playback engine's pw-cat / paplay / aplay sink
// (audio_engine_linux.cpp), and for the same reason: every in-process capture
// API needs a shared lib the fully static release binary can't load.
//
// The first helper that launches wins. One that launches but exits before any
// audio arrives (pw-record without a running PipeWire, arecord on a busy
// device, …) hands over to the next; when the chain runs out, failed() carries
// the last helper's stderr. started() fires on the first PCM chunk, so a
// helper that runs but never delivers audio times out into failed() as well.

namespace Media {
namespace {

constexpr int kLevelIntervalMs = 50;   // level() at ~20 Hz
constexpr int kFirstDataMs     = 4000; // helper running but silent this long = broken
constexpr int kStopGraceMs     = 1000; // helper ignoring SIGTERM this long = SIGKILL

constexpr qsizetype kLevelWindowBytes =
    Recorder::kSampleRate * Recorder::kChannels * 2 * kLevelIntervalMs / 1000;

// Peak |sample| of the s16le samples in [from, to), 0..1.
float peakOf(const QByteArray &buf, qsizetype from, qsizetype to) {
    int         peak = 0;
    const char *p    = buf.constData();
    for (qsizetype i = from; i + 1 < to; i += 2)
        peak = std::max(peak, std::abs(int(qFromLittleEndian<qint16>(p + i))));
    return std::min(1.0f, peak / 32767.0f);
}

class LinuxRecorder : public Recorder {
public:
    explicit LinuxRecorder(QObject *parent) : Recorder(parent) {
        _firstData.setSingleShot(true);
        _firstData.setInterval(kFirstDataMs);
        QObject::connect(&_firstData, &QTimer::timeout, this, [this] {
            const QString err = errorText(
                QCoreApplication::translate(
                    "Media::Recorder", "The microphone isn't delivering any audio"
                )
            );
            abandon();
            emit failed(err);
        });
        // level() on a steady clock, not per chunk: the helpers' stdio
        // buffers hand PCM over in ~4 KiB bursts (~130 ms of audio), so each
        // burst is cut into 50 ms windows that the tick plays out one by one.
        _levelTick.setInterval(kLevelIntervalMs);
        QObject::connect(&_levelTick, &QTimer::timeout, this, [this] {
            if (_levels.empty())
                return;
            const float peak = _levels.front();
            _levels.pop_front();
            emit level(peak);
        });
        _stopGrace.setSingleShot(true);
        _stopGrace.setInterval(kStopGraceMs);
        QObject::connect(&_stopGrace, &QTimer::timeout, this, [this] {
            if (_proc)
                _proc->kill(); // finished() still fires and delivers the take
        });
    }
    ~LinuxRecorder() override { abandon(); }

    void start() override {
        if (_recording)
            return;
        _recording = true;
        _stopping  = false;
        _gotData   = false;
        _pcm.clear();
        _stderr.clear();
        _levelFrom = 0;
        _levels.clear();
        if (!launch(0)) {
            _recording = false;
            emit failed(
                QCoreApplication::translate(
                    "Media::Recorder",
                    "No audio capture tool found (install PipeWire, PulseAudio or ALSA utilities)"
                )
            );
        }
    }

    void stop() override {
        if (!_recording || _stopping)
            return;
        if (!_proc) { // cannot happen while recording, but never leave the caller hanging
            finish();
            return;
        }
        _stopping = true;
        _firstData.stop();
        _proc->terminate(); // SIGTERM: every helper flushes and exits
        _stopGrace.start();
    }

    void cancel() override { abandon(); }

    bool isRecording() const override { return _recording; }

private:
    struct Candidate {
        QString     program;
        QStringList args;
    };
    static std::vector<Candidate> candidates() {
        const QString rate = QString::number(kSampleRate), ch = QString::number(kChannels);
        return {
            {"pw-record", {"--raw", "--format", "s16", "--rate", rate, "--channels", ch, "-"}},
            {"parecord", {"--raw", "--format=s16le", "--rate=" + rate, "--channels=" + ch}},
            {"arecord", {"-q", "-t", "raw", "-f", "S16_LE", "-r", rate, "-c", ch, "-"}},
        };
    }

    // Launches the first candidate at or after `from`; false when none did.
    bool launch(int from) {
        const auto cands = candidates();
        for (int i = from; i < (int)cands.size(); ++i) {
            const QString exe = QStandardPaths::findExecutable(cands[i].program);
            if (exe.isEmpty())
                continue;
            auto *p = new QProcess(this);
            p->setStandardInputFile(QProcess::nullDevice());
            p->start(exe, cands[i].args);
            if (!p->waitForStarted(500)) {
                delete p;
                continue;
            }
            _proc    = p;
            _attempt = i;
            QObject::connect(p, &QProcess::readyReadStandardOutput, this, [this] { readPcm(); });
            QObject::connect(p, &QProcess::readyReadStandardError, this, [this] {
                _stderr += _proc->readAllStandardError();
                if (_stderr.size() > 2048)
                    _stderr = _stderr.right(1024);
            });
            QObject::connect(p, &QProcess::finished, this, [this] { onExited(); });
            _firstData.start();
            return true;
        }
        return false;
    }

    void readPcm() {
        _pcm += _proc->readAllStandardOutput();
        if (_pcm.isEmpty())
            return;
        if (!_gotData) {
            _gotData = true;
            _firstData.stop();
            _levelTick.start();
            emit started();
            if (!_proc)
                return; // cancelled from a started() slot
        }
        for (; _levelFrom + kLevelWindowBytes <= _pcm.size(); _levelFrom += kLevelWindowBytes)
            _levels.push_back(peakOf(_pcm, _levelFrom, _levelFrom + kLevelWindowBytes));
        while (_levels.size() > 4) // keep the meter at most ~200 ms behind
            _levels.pop_front();
    }

    void onExited() {
        _stderr += _proc->readAllStandardError();
        readPcm();
        if (!_proc)
            return;
        if (_stopping) {
            finish();
            return;
        }
        // Exited on its own. Before any audio this is a helper that can't reach
        // its sound server — try the next one; after audio it's a lost device.
        const QString err = errorText(
            _gotData
                ? QCoreApplication::translate("Media::Recorder", "Recording stopped unexpectedly")
                : QCoreApplication::translate(
                      "Media::Recorder", "Couldn't start recording from the microphone"
                  )
        );
        const int next = _attempt + 1;
        dropProcess();
        if (!_gotData) {
            _stderr.clear();
            if (launch(next))
                return;
        }
        _recording = false;
        _firstData.stop();
        _levelTick.stop();
        _pcm.clear();
        emit failed(err);
    }

    void finish() {
        _stopGrace.stop();
        _levelTick.stop();
        dropProcess();
        _recording = false;
        _stopping  = false;
        QByteArray pcm;
        pcm.swap(_pcm);
        pcm.truncate(pcm.size() & ~qsizetype(1)); // whole samples only
        emit finished(wavFromPcm16(pcm, kSampleRate, kChannels));
    }

    // Kill without signalling (cancel, destruction, first-data timeout). Safe
    // from inside the helper's own signal handlers: nothing is deleted inline.
    void abandon() {
        _firstData.stop();
        _stopGrace.stop();
        _levelTick.stop();
        if (QProcess *p = _proc) {
            dropProcess();
            if (p->state() != QProcess::NotRunning) {
                p->setParent(nullptr); // may outlive us until the kill lands
                QObject::connect(p, &QProcess::finished, p, &QObject::deleteLater);
                p->kill();
            }
        }
        _recording = false;
        _stopping  = false;
        _pcm.clear();
    }

    // Detach _proc from this recorder; deletes it later if it already exited.
    void dropProcess() {
        if (!_proc)
            return;
        QProcess *p = _proc;
        _proc       = nullptr;
        p->disconnect(this);
        if (p->state() == QProcess::NotRunning)
            p->deleteLater();
    }

    // `summary`, plus the helper's last stderr line when it said anything.
    QString errorText(const QString &summary) const {
        const QStringList lines = QString::fromLocal8Bit(_stderr).split('\n', Qt::SkipEmptyParts);
        return lines.isEmpty() ? summary : summary + QStringLiteral(": ") + lines.last().trimmed();
    }

    QProcess         *_proc = nullptr;
    QByteArray        _pcm, _stderr;
    QTimer            _firstData, _stopGrace, _levelTick;
    qsizetype         _levelFrom = 0;
    std::deque<float> _levels; // pending level() values, oldest first
    int               _attempt   = 0;
    bool              _recording = false;
    bool              _stopping  = false;
    bool              _gotData   = false;
};

} // namespace

std::unique_ptr<Recorder> createNativeRecorder(QObject *parent) {
    return std::make_unique<LinuxRecorder>(parent);
}

} // namespace Media
