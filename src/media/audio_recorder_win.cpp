// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#include "media/audio_recorder.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QTimer>
#include <QtEndian>
#include <algorithm>
#include <cstdlib>
#include <cstring>

#include <windows.h>
// <mmsystem.h> must follow <windows.h>.
#include <mmsystem.h>

// Windows: waveIn (winmm, already linked for the notification sound) through
// WAVE_MAPPER, which resamples the default microphone to 16 kHz mono s16 for
// us. Opened with CALLBACK_NULL: a main-thread timer polls the rotating
// buffers for WHDR_DONE, so no waveIn call ever runs on the driver's callback
// thread (which the docs forbid) and nothing needs marshalling.

namespace Media {
namespace {

constexpr int kBuffers     = 4;
constexpr int kBufferMs    = 100;
constexpr int kBufferBytes = Recorder::kSampleRate * Recorder::kChannels * 2 * kBufferMs / 1000;
constexpr int kPollMs      = 50;   // also the level() cadence
constexpr int kFirstDataMs = 4000; // device open but silent this long = broken

float peakOf(const char *p, DWORD bytes) {
    int peak = 0;
    for (DWORD i = 0; i + 1 < bytes; i += 2)
        peak = std::max(peak, std::abs(int(qFromLittleEndian<qint16>(p + i))));
    return std::min(1.0f, peak / 32767.0f);
}

class WinRecorder : public Recorder {
public:
    explicit WinRecorder(QObject *parent) : Recorder(parent) {
        _poll.setInterval(kPollMs);
        QObject::connect(&_poll, &QTimer::timeout, this, [this] { poll(); });
    }
    ~WinRecorder() override { close(); }

    void start() override {
        if (_recording)
            return;
        if (waveInGetNumDevs() == 0) {
            emit failed(QCoreApplication::translate("Media::Recorder", "No microphone found"));
            return;
        }
        WAVEFORMATEX fmt{};
        fmt.wFormatTag      = WAVE_FORMAT_PCM;
        fmt.nChannels       = kChannels;
        fmt.nSamplesPerSec  = kSampleRate;
        fmt.wBitsPerSample  = 16;
        fmt.nBlockAlign     = fmt.nChannels * fmt.wBitsPerSample / 8;
        fmt.nAvgBytesPerSec = fmt.nSamplesPerSec * fmt.nBlockAlign;
        MMRESULT r          = waveInOpen(&_in, WAVE_MAPPER, &fmt, 0, 0, CALLBACK_NULL);
        if (r != MMSYSERR_NOERROR) {
            _in = nullptr;
            emit failed(openError(r));
            return;
        }
        for (int i = 0; i < kBuffers; ++i) {
            WAVEHDR &h       = _hdr[i];
            h                = WAVEHDR{};
            h.lpData         = _data[i];
            h.dwBufferLength = kBufferBytes;
            r                = waveInPrepareHeader(_in, &h, sizeof h);
            if (r == MMSYSERR_NOERROR)
                r = waveInAddBuffer(_in, &h, sizeof h);
            if (r != MMSYSERR_NOERROR)
                break;
        }
        if (r == MMSYSERR_NOERROR)
            r = waveInStart(_in);
        if (r != MMSYSERR_NOERROR) {
            close();
            emit failed(openError(r));
            return;
        }
        _pcm.clear();
        _next      = 0;
        _gotData   = false;
        _recording = true;
        _clock.start();
        _poll.start();
    }

    void stop() override {
        if (!_recording)
            return;
        _poll.stop();
        // waveInReset returns every queued buffer marked done (the one being
        // filled with what it has so far); collect them in order.
        waveInReset(_in);
        collect(false);
        close();
        QByteArray pcm;
        pcm.swap(_pcm);
        emit finished(wavFromPcm16(pcm, kSampleRate, kChannels));
    }

    void cancel() override {
        _poll.stop();
        if (_in)
            waveInReset(_in);
        close();
        _pcm.clear();
    }

    bool isRecording() const override { return _recording; }

private:
    void poll() {
        const float peak = collect(true);
        if (!_gotData) {
            if (_pcm.isEmpty()) {
                if (_clock.elapsed() > kFirstDataMs) {
                    cancel();
                    emit failed(
                        QCoreApplication::translate(
                            "Media::Recorder", "The microphone isn't delivering any audio"
                        )
                    );
                }
                return;
            }
            _gotData = true;
            emit started();
            if (!_recording)
                return; // cancelled from a started() slot
        }
        emit level(peak);
    }

    // Appends every finished buffer (in ring order) to _pcm; re-queues them
    // when `requeue`. Returns the peak of what was appended.
    float collect(bool requeue) {
        float peak = 0;
        for (int n = 0; n < kBuffers; ++n) {
            WAVEHDR &h = _hdr[_next];
            if (!(h.dwFlags & WHDR_DONE))
                break;
            if (h.dwBytesRecorded > 0) {
                _pcm.append(h.lpData, qsizetype(h.dwBytesRecorded));
                peak = std::max(peak, peakOf(h.lpData, h.dwBytesRecorded));
            }
            h.dwBytesRecorded = 0;
            if (requeue) {
                h.dwFlags &= ~WHDR_DONE;
                waveInAddBuffer(_in, &h, sizeof h);
            }
            _next = (_next + 1) % kBuffers;
        }
        return peak;
    }

    // Unprepares the buffers and closes the device; the caller has already
    // stopped it (waveInReset) or never started it.
    void close() {
        _recording = false;
        _poll.stop();
        if (!_in)
            return;
        waveInReset(_in);
        for (WAVEHDR &h : _hdr)
            if (h.dwFlags & WHDR_PREPARED)
                waveInUnprepareHeader(_in, &h, sizeof h);
        waveInClose(_in);
        _in = nullptr;
    }

    static QString openError(MMRESULT r) {
        switch (r) {
        case MMSYSERR_BADDEVICEID:
        case MMSYSERR_NODRIVER:
            return QCoreApplication::translate("Media::Recorder", "No microphone found");
        case MMSYSERR_ALLOCATED:
            return QCoreApplication::translate(
                "Media::Recorder", "The microphone is in use by another app"
            );
        default: {
            wchar_t text[MAXERRORLENGTH] = {};
            waveInGetErrorTextW(r, text, MAXERRORLENGTH);
            QString msg = QCoreApplication::translate(
                "Media::Recorder",
                "Couldn't open the microphone. Check that microphone access is allowed in "
                "Windows Settings → Privacy & security → Microphone."
            );
            const QString detail = QString::fromWCharArray(text).trimmed();
            return detail.isEmpty() ? msg : msg + QStringLiteral(" (") + detail + QLatin1Char(')');
        }
        }
    }

    HWAVEIN       _in = nullptr;
    WAVEHDR       _hdr[kBuffers]{};
    char          _data[kBuffers][kBufferBytes]{};
    QByteArray    _pcm;
    QTimer        _poll;
    QElapsedTimer _clock;
    int           _next      = 0;
    bool          _recording = false;
    bool          _gotData   = false;
};

} // namespace

std::unique_ptr<Recorder> createNativeRecorder(QObject *parent) {
    return std::make_unique<WinRecorder>(parent);
}

} // namespace Media
