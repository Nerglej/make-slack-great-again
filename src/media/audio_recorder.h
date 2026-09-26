// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#pragma once

#include <QByteArray>
#include <QObject>
#include <QString>
#include <memory>

namespace Media {

// Platform microphone capture behind voice input: one recording at a time,
// 16 kHz mono signed 16-bit PCM (what speech-to-text wants; no encoder, so no
// binary-size cost). Every call is main-thread; results arrive as signals on
// the main thread too. Backends (see audio_recorder_{linux,mac,win}):
//   • Linux   — the desktop's sound-server CLI (pw-record / parecord / arecord)
//               writing raw PCM to a pipe. No shared-lib deps, so the fully
//               static release binary keeps working (miniaudio capture would
//               need dlopen, which static musl can't do).
//   • macOS   — AVFoundation (needs NSMicrophoneUsageDescription + the
//               audio-input entitlement under the hardened runtime).
//   • Windows — waveIn (winmm).
class Recorder : public QObject {
    Q_OBJECT
public:
    static constexpr int kSampleRate = 16000;
    static constexpr int kChannels   = 1;

    using QObject::QObject;

    // Begin capturing. Emits started() once audio flows, or failed() (no
    // capture helper, permission denied, no device). May fail synchronously —
    // connect before calling.
    virtual void start()  = 0;
    // End the recording; emits finished() with everything captured so far.
    virtual void stop()   = 0;
    // Drop the recording; emits nothing.
    virtual void cancel() = 0;

    [[nodiscard]] virtual bool isRecording() const = 0;

signals:
    void started();
    // Peak amplitude of the latest chunk, 0..1, roughly 20 times a second.
    void level(float peak);
    // A complete RIFF/WAVE file (16 kHz, mono, s16le).
    void finished(QByteArray wav);
    void failed(QString error);
};

// The recorder for the platform this binary was built for. When the
// MSGA_VOICE_FAKE_WAV environment variable names a WAV file, a fake recorder
// that "records" that file is returned instead (tests, headless verification).
std::unique_ptr<Recorder> createPlatformRecorder(QObject *parent);

// Wraps raw little-endian s16 PCM in a 44-byte RIFF/WAVE header.
QByteArray wavFromPcm16(const QByteArray &pcm, int sampleRate, int channels);

} // namespace Media
