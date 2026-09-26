// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#include "media/audio_recorder.h"

#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QTimer>
#include <QtEndian>
#include <cmath>
#include <cstring>

// Platform-neutral half of the recorder: the WAV wrapper and the fake recorder
// behind MSGA_VOICE_FAKE_WAV. The real backends live in the platform TUs and
// are reached through createNativeRecorder().

namespace Media {

// Defined by exactly one of audio_recorder_{linux,mac,win}.
std::unique_ptr<Recorder> createNativeRecorder(QObject *parent);

namespace {

void putLe16(char *p, quint16 v) {
    qToLittleEndian(v, p);
}
void putLe32(char *p, quint32 v) {
    qToLittleEndian(v, p);
}

// "Records" a fixed WAV file: started() on the next event-loop turn, a gentle
// level() pulse while running, and the file's bytes (as-is) on stop(). Lets
// tests and headless GUI runs exercise the whole voice-input flow without a
// microphone.
class FakeRecorder : public Recorder {
public:
    FakeRecorder(const QString &path, QObject *parent) : Recorder(parent), _path(path) {
        _pulse.setInterval(50);
        QObject::connect(&_pulse, &QTimer::timeout, this, [this] {
            ++_tick;
            emit level(float(0.5 + 0.4 * std::sin(_tick * 0.6)));
        });
    }

    void start() override {
        if (_recording)
            return;
        _recording    = true;
        _tick         = 0;
        const int gen = ++_generation;
        QTimer::singleShot(0, this, [this, gen] {
            if (gen != _generation || !_recording)
                return;
            emit started();
            _pulse.start();
        });
    }

    void stop() override {
        if (!_recording)
            return;
        _recording = false;
        _pulse.stop();
        ++_generation;
        QFile f(_path);
        if (!f.open(QIODevice::ReadOnly)) {
            emit failed(
                QCoreApplication::translate("Media::Recorder", "Couldn't read the recording")
            );
            return;
        }
        emit finished(f.readAll());
    }

    void cancel() override {
        _recording = false;
        _pulse.stop();
        ++_generation;
    }

    bool isRecording() const override { return _recording; }

private:
    QString _path;
    QTimer  _pulse;
    int     _tick       = 0;
    int     _generation = 0;
    bool    _recording  = false;
};

} // namespace

QByteArray wavFromPcm16(const QByteArray &pcm, int sampleRate, int channels) {
    const quint32 dataSize   = quint32(pcm.size());
    const quint16 blockAlign = quint16(channels * 2);
    QByteArray    wav(44, '\0');
    char         *h = wav.data();
    memcpy(h, "RIFF", 4);
    putLe32(h + 4, 36 + dataSize);
    memcpy(h + 8, "WAVEfmt ", 8);
    putLe32(h + 16, 16); // fmt chunk size
    putLe16(h + 20, 1);  // PCM
    putLe16(h + 22, quint16(channels));
    putLe32(h + 24, quint32(sampleRate));
    putLe32(h + 28, quint32(sampleRate) * blockAlign); // byte rate
    putLe16(h + 32, blockAlign);
    putLe16(h + 34, 16); // bits per sample
    memcpy(h + 36, "data", 4);
    putLe32(h + 40, dataSize);
    wav += pcm;
    return wav;
}

std::unique_ptr<Recorder> createPlatformRecorder(QObject *parent) {
    const QString fake = qEnvironmentVariable("MSGA_VOICE_FAKE_WAV");
    if (!fake.isEmpty() && QFileInfo(fake).isReadable())
        return std::make_unique<FakeRecorder>(fake, parent);
    return createNativeRecorder(parent);
}

} // namespace Media
