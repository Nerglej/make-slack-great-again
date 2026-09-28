// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#include "llm/voice_input.h"

#include "llm/llm_service.h"
#include "llm/voice_prompt.h"
#include "media/audio_recorder.h"

#include <QLocale>
#include <QSettings>
#include <QTimer>
#include <QtEndian>

#include <algorithm>
#include <cstdlib>

using namespace Qt::StringLiterals;

namespace {

constexpr const char *kGlossaryKey = "voice/glossary";
constexpr const char *kCleanupKey  = "voice/cleanup";

// Below this there is nothing worth an API call (a click on the mic and
// straight off again), and Whisper-style models hallucinate text on silence.
constexpr int   kMinSpeechMs = 300;
// Peak amplitude (0..1) a recording must reach somewhere to count as sound:
// ~−46 dBFS, well under quiet speech, above a muted mic's digital zero.
constexpr float kSilencePeak = 0.005f;

// Duration and peak of a 16-bit PCM RIFF/WAVE file. Unknown layouts read as
// empty — the recorder only ever produces s16le.
struct WavStats {
    qint64 durationMs = 0;
    float  peak       = 0.0f;
};

WavStats analyseWav(const QByteArray &wav) {
    WavStats st;
    if (wav.size() < 12 || !wav.startsWith("RIFF") || wav.mid(8, 4) != "WAVE")
        return st;
    int         channels = Media::Recorder::kChannels;
    int         rate     = Media::Recorder::kSampleRate;
    int         bits     = 16;
    const auto *d        = reinterpret_cast<const uchar *>(wav.constData());
    qsizetype   pos      = 12;
    while (pos + 8 <= wav.size()) {
        const QByteArray id   = wav.mid(pos, 4);
        const qsizetype  size = qFromLittleEndian<quint32>(d + pos + 4);
        const qsizetype  body = pos + 8;
        if (id == "fmt " && size >= 16 && body + 16 <= wav.size()) {
            channels = qFromLittleEndian<quint16>(d + body + 2);
            rate     = int(qFromLittleEndian<quint32>(d + body + 4));
            bits     = qFromLittleEndian<quint16>(d + body + 14);
        } else if (id == "data") {
            if (bits != 16 || channels <= 0 || rate <= 0)
                return st;
            // A streaming writer may leave the size unset/oversized: clamp.
            const qsizetype len    = std::min<qsizetype>(size, wav.size() - body);
            const qsizetype frames = len / (2 * channels);
            st.durationMs          = frames * 1000 / rate;
            int maxAbs             = 0;
            for (qsizetype i = 0; i + 1 < len; i += 2)
                maxAbs = std::max(maxAbs, std::abs(int(qFromLittleEndian<qint16>(d + body + i))));
            st.peak = float(maxAbs) / 32768.0f;
            return st;
        }
        pos = body + size + (size & 1); // chunks are word-aligned
    }
    return st;
}

} // namespace

VoiceInput &VoiceInput::instance() {
    static VoiceInput vi;
    return vi;
}

VoiceInput::VoiceInput()
    : _factory([this] { return Media::createPlatformRecorder(this); }),
      _maxTimer(new QTimer(this)) {
    _maxTimer->setSingleShot(true);
    _maxTimer->setInterval(kMaxRecordingMs);
    connect(_maxTimer, &QTimer::timeout, this, &VoiceInput::stop);
    connect(
        &LlmService::instance(),
        &LlmService::availabilityChanged,
        this,
        &VoiceInput::availabilityChanged
    );
}

VoiceInput::~VoiceInput() = default;

bool VoiceInput::isAvailable() const {
    return LlmService::instance().sttProvider() != nullptr;
}

void VoiceInput::setRecorderFactory(std::function<std::unique_ptr<Media::Recorder>()> factory) {
    _factory = std::move(factory);
}

void VoiceInput::setState(State s) {
    if (_state == s)
        return;
    _state = s;
    emit stateChanged(s);
}

void VoiceInput::start(QObject *owner, Voice::Context ctx) {
    cancel();
    if (!isAvailable()) {
        if (owner)
            emit failed(
                owner,
                tr("Voice input needs a speech-to-text provider. Connect OpenAI or an "
                   "OpenAI-compatible server in Settings → AI assistance.")
            );
        return;
    }
    _owner    = owner;
    _ctx      = std::move(ctx);
    _recorder = _factory ? _factory() : nullptr;
    if (!_recorder) {
        fail(tr("No microphone recording is available on this system"));
        return;
    }
    const quint64 gen = _generation;
    auto         *rec = _recorder.get();
    connect(rec, &Media::Recorder::level, this, [this, gen](float peak) {
        if (gen == _generation && _state == State::Recording)
            emit level(peak);
    });
    connect(rec, &Media::Recorder::finished, this, [this, gen](const QByteArray &wav) {
        if (gen == _generation)
            onRecorded(wav);
    });
    connect(rec, &Media::Recorder::failed, this, [this, gen](const QString &error) {
        if (gen == _generation)
            fail(tr("Couldn't record: %1").arg(error));
    });
    setState(State::Recording);
    if (gen != _generation)
        return; // a stateChanged() handler cancelled already
    _maxTimer->start();
    rec->start(); // may fail synchronously → fail() → Idle
}

void VoiceInput::stop() {
    if (_state != State::Recording || !_recorder)
        return;
    _maxTimer->stop();
    // Transcribing from the moment the user lets go: the recorder's finished()
    // may take a moment (helper process flushing its pipe).
    setState(State::Transcribing);
    if (_recorder) // a stateChanged() handler may have cancelled
        _recorder->stop();
}

void VoiceInput::cancel() {
    ++_generation;
    if (_recorder)
        _recorder->cancel();
    resetToIdle();
}

void VoiceInput::onRecorded(const QByteArray &wav) {
    const quint64 gen = _generation;
    releaseRecorder();
    setState(State::Transcribing);
    if (gen != _generation)
        return;

    const WavStats st = analyseWav(wav);
    if (st.durationMs < kMinSpeechMs || st.peak < kSilencePeak) {
        fail(tr("No speech was recorded"));
        return;
    }

    auto &llm = LlmService::instance();
    auto *stt = llm.sttProvider();
    if (!stt) { // disconnected while recording
        fail(tr("Voice input needs a speech-to-text provider"));
        return;
    }
    const bool instructionModel = VoicePrompt::isInstructionFollowingSttModel(stt->sttModel());

    LlmWire::TranscriptionInput in;
    in.audio                       = wav;
    in.fileName                    = QStringLiteral("voice.wav");
    in.mimeType                    = QStringLiteral("audio/wav");
    // in.model empty → the provider's STT model.
    in.prompt                      = VoicePrompt::buildSttPrompt(_ctx, instructionModel);
    in.keywords                    = VoicePrompt::extractKeywords(_ctx, glossary());
    // Language hints: the user's native language plus English — dictation in
    // tech teams is often English (or mixed) even for non-native speakers, and
    // a native-only hint would push English speech into the wrong language.
    // Only sent to gpt-* models (see LlmWire::TranscriptionInput).
    const QString           native = llm.nativeLanguage().trimmed().toLower();
    const QLocale::Language lang   = QLocale::codeToLanguage(native, QLocale::ISO639Part1);
    if (lang != QLocale::AnyLanguage && lang != QLocale::C)
        in.languages << native;
    if (native != QLatin1String("en"))
        in.languages << QStringLiteral("en");

    llm.transcribe(
        std::move(in),
        [this, gen](QString text) {
            if (gen == _generation)
                onTranscribed(text);
        },
        [this, gen](QString error) {
            if (gen == _generation)
                fail(tr("Couldn't transcribe: %1").arg(error));
        }
    );
}

void VoiceInput::onTranscribed(const QString &textIn) {
    const quint64 gen = _generation;
    const QString raw = textIn.trimmed();
    if (raw.isEmpty()) {
        fail(tr("No speech was recognised"));
        return;
    }
    auto &llm = LlmService::instance();
    if (!cleanupEnabled() || !llm.isAvailable()) {
        succeed(raw);
        return;
    }

    setState(State::Cleaning);
    if (gen != _generation)
        return;
    Llm::Request req = VoicePrompt::buildCleanupRequest(raw, _ctx, llm.nativeLanguage());
    if (const auto *p = llm.activeProvider())
        req.model = p->lightModel();
    llm.chat(
        req,
        [this, gen, raw](Llm::Response r) {
            if (gen != _generation)
                return;
            const QString cleaned   = r.text.trimmed();
            // Never lose the user's words: an empty or cut-off reply, or one
            // far longer than what was said (the model answered or added
            // content instead of cleaning), falls back to the raw transcript.
            const bool    truncated = r.stopReason == QLatin1String("length") ||
                                      r.stopReason == QLatin1String("max_tokens");
            const bool    runaway   = cleaned.size() > raw.size() * 2 + 40;
            succeed(cleaned.isEmpty() || truncated || runaway ? raw : cleaned);
        },
        [this, gen, raw](QString) {
            if (gen == _generation)
                succeed(raw); // clean-up is best effort
        }
    );
}

void VoiceInput::succeed(const QString &text) {
    const QPointer<QObject> owner = _owner;
    ++_generation;
    resetToIdle();
    if (owner) // the composer is gone → nowhere to put the text
        emit finished(owner, text);
}

void VoiceInput::fail(const QString &error) {
    const QPointer<QObject> owner = _owner;
    ++_generation;
    resetToIdle();
    if (owner)
        emit failed(owner, error);
}

void VoiceInput::releaseRecorder() {
    _maxTimer->stop();
    if (!_recorder)
        return;
    Media::Recorder *rec = _recorder.release();
    rec->disconnect(this);
    rec->deleteLater(); // it may be mid-emit (finished/failed)
}

void VoiceInput::resetToIdle() {
    releaseRecorder();
    _owner = nullptr;
    _ctx   = {};
    setState(State::Idle);
}

// ── Settings ─────────────────────────────────────────────────────────────────

QStringList VoiceInput::glossary() {
    return QSettings(u"msga"_s, u"msga"_s).value(kGlossaryKey).toStringList();
}

void VoiceInput::setGlossary(const QStringList &terms) {
    QStringList clean;
    for (const QString &t : terms)
        if (const QString s = t.simplified(); !s.isEmpty() && !clean.contains(s))
            clean << s;
    QSettings s(u"msga"_s, u"msga"_s);
    if (clean.isEmpty())
        s.remove(kGlossaryKey);
    else
        s.setValue(kGlossaryKey, clean);
}

bool VoiceInput::cleanupEnabled() {
    return QSettings(u"msga"_s, u"msga"_s).value(kCleanupKey, true).toBool();
}

void VoiceInput::setCleanupEnabled(bool on) {
    QSettings(u"msga"_s, u"msga"_s).setValue(kCleanupKey, on);
}
