// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
// Voice input for the composer: record → speech-to-text (gpt-transcribe on the
// OpenAI wire) → optional AI clean-up pass → text handed back to the composer
// that started it. One recording app-wide; the UI never touches the recorder
// or the LLM layer directly.
//
//   auto &vi = VoiceInput::instance();
//   connect(&vi, &VoiceInput::finished, this, [this](QObject *owner, QString text) {
//       if (owner == this) insertAtCursor(text);
//   });
//   vi.start(this, contextForCurrentConversation());
//   …
//   vi.stop(); // or vi.cancel()
#pragma once

#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <functional>
#include <memory>

class QTimer;
namespace Media {
class Recorder;
}

namespace Voice {

// What the composer knows about where the dictated text will go. Drives the
// speech-to-text prompt/keywords and the clean-up pass. Everything optional.
struct Context {
    QString     conversationName; // "#backend", or the DM peer's name
    QStringList memberNames;      // display names of the conversation's members
    QStringList recentMessages;   // plain text, oldest → newest (the last ~30)
};

} // namespace Voice

class VoiceInput : public QObject {
    Q_OBJECT
public:
    enum class State { Idle, Recording, Transcribing, Cleaning };
    Q_ENUM(State)

    // Recordings longer than this stop on their own (OpenAI's upload limit is
    // 25 MB; 16 kHz mono s16 is ~1.9 MB a minute).
    static constexpr int kMaxRecordingMs = 10 * 60 * 1000;

    static VoiceInput &instance();

    // Whether a connected provider can do speech-to-text (drives whether the
    // composer shows the mic button). Re-emitted via availabilityChanged().
    [[nodiscard]] bool     isAvailable() const;
    [[nodiscard]] State    state() const { return _state; }
    // The composer that started the current recording (nullptr when Idle).
    [[nodiscard]] QObject *owner() const { return _owner; }

    // Start recording for `owner`. A recording already running for another
    // owner is cancelled first. No-op (plus failed()) when !isAvailable().
    void start(QObject *owner, Voice::Context ctx);
    // Stop recording and transcribe. Ignored unless Recording.
    void stop();
    // Abandon whatever is in flight (recording, transcription or clean-up);
    // no finished()/failed() follows.
    void cancel();

    // ── Settings (QSettings "msga": voice/glossary, voice/cleanup) ─────────
    // User glossary: terms always sent as speech-to-text keywords.
    [[nodiscard]] static QStringList glossary();
    static void                      setGlossary(const QStringList &terms);
    // Whether the AI clean-up pass runs after transcription.
    [[nodiscard]] static bool        cleanupEnabled();
    static void                      setCleanupEnabled(bool on);

    // Tests: replace the platform recorder factory.
    void setRecorderFactory(std::function<std::unique_ptr<Media::Recorder>()> factory);

signals:
    void availabilityChanged();
    void stateChanged(VoiceInput::State state);
    // Peak level 0..1 while Recording (for the meter).
    void level(float peak);
    // The final text for `owner` (already cleaned up when that is on).
    void finished(QObject *owner, const QString &text);
    // Human-readable, translated error for `owner`.
    void failed(QObject *owner, const QString &error);

private:
    VoiceInput();
    ~VoiceInput() override;

    void setState(State s);

    State                                             _state = State::Idle;
    QPointer<QObject>                                 _owner;
    std::function<std::unique_ptr<Media::Recorder>()> _factory;
    // Implementation details (recorder, context, request generation, max-length
    // timer…) are the implementer's to add below.

    void onRecorded(const QByteArray &wav);
    void onTranscribed(const QString &text);
    // Back to Idle, then finished()/failed() for the owner — unless it died.
    void succeed(const QString &text);
    void fail(const QString &error);
    // Drops the recorder (deferred: it may be the one emitting) and the timer.
    void releaseRecorder();
    void resetToIdle();

    std::unique_ptr<Media::Recorder> _recorder;
    Voice::Context                   _ctx;
    // Bumped by every start()/cancel()/completion: async callbacks capture
    // the value they were issued under and turn into no-ops once it moved on.
    quint64                          _generation = 0;
    QTimer                          *_maxTimer   = nullptr;
};
