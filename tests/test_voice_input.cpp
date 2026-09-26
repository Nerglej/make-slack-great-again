// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
// VoiceInput state machine: a scripted fake recorder (setRecorderFactory) and
// FakeHttpServer standing in for the speech-to-text and chat endpoints.
// QSettings is redirected to a temp dir so the real user config (and its API
// keys) is never touched.
#include <catch2/catch_test_macros.hpp>

#include "test_main.h"

#include "fake_http_server.h"
#include "llm/llm_service.h"
#include "llm/voice_input.h"
#include "llm/voice_prompt.h"
#include "media/audio_recorder.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>
#include <QTemporaryDir>
#include <QTimer>
#include <QtEndian>

#include <cmath>
#include <cstring>

static QTemporaryDir *gSettingsDir = nullptr;

MSGA_TEST_MAIN(argc, argv) {
    QCoreApplication app(argc, argv);
    app.setApplicationName("msga-test");
    app.setOrganizationName("msga-test");
    gSettingsDir = new QTemporaryDir;
    QSettings::setPath(QSettings::NativeFormat, QSettings::UserScope, gSettingsDir->path());
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, gSettingsDir->path());
    const int rc = msga_test::runCatch(argc, argv);
    delete gSettingsDir;
    return rc;
}

namespace {

using State = VoiceInput::State;

void waitFor(const std::function<bool()> &done, int ms = 3000) {
    QElapsedTimer t;
    t.start();
    while (!done() && t.elapsed() < ms)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
}

// Let pending network replies / queued calls land.
void settle(int ms = 300) {
    waitFor([] { return false; }, ms);
}

// 16 kHz mono s16le WAV: `ms` of a 440 Hz tone at `amplitude` (0 = silence).
QByteArray makeWav(int ms, double amplitude = 0.3) {
    const int  frames = Media::Recorder::kSampleRate * ms / 1000;
    QByteArray pcm(frames * 2, '\0');
    auto      *p = reinterpret_cast<uchar *>(pcm.data());
    for (int i = 0; i < frames; ++i) {
        const double v =
            amplitude * std::sin(2 * 3.14159265358979 * 440.0 * i / Media::Recorder::kSampleRate);
        qToLittleEndian<qint16>(qint16(v * 32767), p + 2 * i);
    }
    QByteArray h(44, '\0');
    auto      *d = reinterpret_cast<uchar *>(h.data());
    std::memcpy(d, "RIFF", 4);
    qToLittleEndian<quint32>(quint32(36 + pcm.size()), d + 4);
    std::memcpy(d + 8, "WAVEfmt ", 8);
    qToLittleEndian<quint32>(16, d + 16);
    qToLittleEndian<quint16>(1, d + 20);
    qToLittleEndian<quint16>(1, d + 22);
    qToLittleEndian<quint32>(Media::Recorder::kSampleRate, d + 24);
    qToLittleEndian<quint32>(Media::Recorder::kSampleRate * 2, d + 28);
    qToLittleEndian<quint16>(2, d + 32);
    qToLittleEndian<quint16>(16, d + 34);
    std::memcpy(d + 36, "data", 4);
    qToLittleEndian<quint32>(quint32(pcm.size()), d + 40);
    return h + pcm;
}

// What the next recorder the factory hands out does.
struct Script {
    QByteArray wav         = makeWav(1500);
    bool       failOnStart = false;
    bool       syncFinish  = false; // finished() from inside stop()
    int        created     = 0;
    int        cancels     = 0;
};

class FakeRecorder : public Media::Recorder {
public:
    FakeRecorder(Script &s) : _s(s) {}
    void start() override {
        if (_s.failOnStart) {
            emit failed("no capture device");
            return;
        }
        _recording = true;
        emit started();
        emit level(0.42f);
    }
    void stop() override {
        _recording = false;
        if (_s.syncFinish)
            emit finished(_s.wav);
        else
            QTimer::singleShot(0, this, [this] { emit finished(_s.wav); });
    }
    void cancel() override {
        _recording = false;
        ++_s.cancels;
    }
    bool isRecording() const override { return _recording; }

private:
    Script &_s;
    bool    _recording = false;
};

// Signals VoiceInput emitted during one test.
struct Capture {
    QObject                          ctx;
    QList<QPair<QObject *, QString>> finished, failed;
    QList<State>                     states;
    QList<float>                     levels;
    explicit Capture(VoiceInput &vi) {
        QObject::connect(&vi, &VoiceInput::finished, &ctx, [this](QObject *o, const QString &t) {
            finished.append({o, t});
        });
        QObject::connect(&vi, &VoiceInput::failed, &ctx, [this](QObject *o, const QString &e) {
            failed.append({o, e});
        });
        QObject::connect(&vi, &VoiceInput::stateChanged, &ctx, [this](State s) { states << s; });
        QObject::connect(&vi, &VoiceInput::level, &ctx, [this](float p) { levels << p; });
    }
    bool settled() const { return !finished.isEmpty() || !failed.isEmpty(); }
};

// A connected custom OpenAI-compatible provider serving both chat and STT,
// made the default. `sttModel` gpt-* → keywords[]/languages[] are sent.
struct Fixture {
    FakeHttpServer srv;
    Script         script;
    LlmProvider   *provider = nullptr;
    Fixture(const QString &sttModel = "gpt-transcribe") {
        auto             &svc = LlmService::instance();
        LlmProviderConfig cfg = LlmProviderConfig::newCustom();
        cfg.name              = "Fake OpenAI";
        cfg.baseUrl           = srv.baseUrl().chopped(1) + "/v1";
        cfg.model             = "chat-model";
        cfg.sttModel          = sttModel;
        provider              = svc.addCustom(cfg, "k");
        svc.setDefaultProviderId(provider->id());
        svc.setNativeLanguage("sv");
        VoiceInput::instance().setRecorderFactory([this]() -> std::unique_ptr<Media::Recorder> {
            ++script.created;
            return std::make_unique<FakeRecorder>(script);
        });
    }
    ~Fixture() {
        VoiceInput::instance().cancel();
        LlmService::instance().removeCustom(provider->id());
    }
};

QByteArray chatReply(const QString &text) {
    const QJsonObject msg{{"role", "assistant"}, {"content", text}};
    const QJsonObject choice{{"finish_reason", "stop"}, {"message", msg}};
    return QJsonDocument(
               QJsonObject{{"choices", QJsonArray{choice}}}
    ).toJson(QJsonDocument::Compact);
}

Voice::Context sampleContext() {
    Voice::Context ctx;
    ctx.conversationName = "#backend";
    ctx.memberNames      = {"Anna Svensson"};
    ctx.recentMessages   = {"the fix touches llm_wire.cpp", "ship it before the k8s upgrade"};
    return ctx;
}

} // namespace

TEST_CASE("Happy path: record, transcribe with prompt/keywords, clean up, finish") {
    Fixture f;
    auto   &vi = VoiceInput::instance();
    VoiceInput::setCleanupEnabled(true);
    VoiceInput::setGlossary({"Nisdos", " Nisdos ", ""});
    CHECK(VoiceInput::glossary() == QStringList{"Nisdos"});
    CHECK(vi.isAvailable());

    Capture cap(vi);
    f.srv.enqueue(R"({"text":"um so the llm wire fix is uh done"})");
    f.srv.enqueue(chatReply("  So the llm_wire fix is done.  "));

    QObject owner;
    vi.start(&owner, sampleContext());
    CHECK(vi.state() == State::Recording);
    CHECK(vi.owner() == &owner);
    CHECK(cap.levels == QList<float>{0.42f});
    vi.stop();
    CHECK(vi.state() == State::Transcribing);

    waitFor([&] { return cap.settled(); });
    REQUIRE(cap.finished.size() == 1);
    CHECK(cap.failed.isEmpty());
    CHECK(cap.finished[0].first == &owner);
    CHECK(cap.finished[0].second == "So the llm_wire fix is done.");
    CHECK(vi.state() == State::Idle);
    CHECK(vi.owner() == nullptr);
    CHECK(
        cap.states ==
        QList<State>{State::Recording, State::Transcribing, State::Cleaning, State::Idle}
    );

    REQUIRE(f.srv.requestPaths.size() == 2);
    CHECK(f.srv.requestPaths[0] == "/v1/audio/transcriptions");
    const QByteArray &stt = f.srv.requestBodies[0];
    CHECK(stt.contains("name=\"model\"\r\n\r\ngpt-transcribe\r\n"));
    CHECK(stt.contains("filename=\"voice.wav\""));
    CHECK(stt.contains("Content-Type: audio/wav"));
    CHECK(stt.contains("name=\"prompt\"\r\n\r\n" + QByteArray(VoicePrompt::kInstructionPreamble)));
    CHECK(stt.contains("Conversation: backend"));
    CHECK(stt.contains("name=\"keywords[]\"\r\n\r\nNisdos\r\n"));
    CHECK(stt.contains("name=\"keywords[]\"\r\n\r\nAnna Svensson\r\n"));
    CHECK(stt.contains("name=\"keywords[]\"\r\n\r\nllm_wire.cpp\r\n"));
    CHECK(stt.contains("name=\"keywords[]\"\r\n\r\nk8s\r\n"));
    CHECK(stt.contains("name=\"languages[]\"\r\n\r\nsv\r\n"));
    CHECK(stt.contains("name=\"languages[]\"\r\n\r\nen\r\n"));

    CHECK(f.srv.requestPaths[1] == "/v1/chat/completions");
    const QJsonObject chat = QJsonDocument::fromJson(f.srv.requestBodies[1]).object();
    CHECK(chat["model"].toString() == "chat-model"); // custom: light model == model
    CHECK(QJsonDocument(chat).toJson().contains("um so the llm wire fix is uh done"));
    VoiceInput::setGlossary({});
    CHECK_FALSE(QSettings("msga", "msga").contains("voice/glossary"));
}

TEST_CASE("Clean-up off: the raw transcript is the result, one request") {
    Fixture f;
    auto   &vi = VoiceInput::instance();
    VoiceInput::setCleanupEnabled(false);
    CHECK_FALSE(VoiceInput::cleanupEnabled());
    Capture cap(vi);
    f.srv.enqueue(R"({"text":" raw words "})");

    QObject owner;
    vi.start(&owner, {});
    vi.stop();
    waitFor([&] { return cap.settled(); });
    REQUIRE(cap.finished.size() == 1);
    CHECK(cap.finished[0].second == "raw words");
    CHECK(f.srv.requestPaths.size() == 1);
    CHECK_FALSE(cap.states.contains(State::Cleaning));
    VoiceInput::setCleanupEnabled(true);
}

TEST_CASE("Clean-up failure, empty or runaway reply falls back to the raw transcript") {
    Fixture f;
    auto   &vi = VoiceInput::instance();
    VoiceInput::setCleanupEnabled(true);
    QObject owner;

    // HTTP error from the chat endpoint.
    {
        Capture cap(vi);
        f.srv.enqueue(R"({"text":"keep my words"})");
        f.srv.enqueueStatus(
            500, "Internal Server Error", "application/json", R"({"error":{"message":"boom"}})"
        );
        vi.start(&owner, {});
        vi.stop();
        waitFor([&] { return cap.settled(); });
        REQUIRE(cap.finished.size() == 1);
        CHECK(cap.finished[0].second == "keep my words");
        CHECK(cap.failed.isEmpty());
    }
    // Empty reply.
    {
        Capture cap(vi);
        f.srv.enqueue(R"({"text":"still mine"})");
        f.srv.enqueue(chatReply("   "));
        vi.start(&owner, {});
        vi.stop();
        waitFor([&] { return cap.settled(); });
        REQUIRE(cap.finished.size() == 1);
        CHECK(cap.finished[0].second == "still mine");
    }
    // The model answered instead of cleaning up.
    {
        Capture cap(vi);
        f.srv.enqueue(R"({"text":"what is k8s"})");
        f.srv.enqueue(
            chatReply(QString("Kubernetes is a container orchestration system. ").repeated(5))
        );
        vi.start(&owner, {});
        vi.stop();
        waitFor([&] { return cap.settled(); });
        REQUIRE(cap.finished.size() == 1);
        CHECK(cap.finished[0].second == "what is k8s");
    }
}

TEST_CASE("Cancel during transcription drops the result") {
    Fixture f;
    auto   &vi = VoiceInput::instance();
    VoiceInput::setCleanupEnabled(false);
    f.script.syncFinish = true; // the STT request is on the wire when stop() returns
    Capture cap(vi);
    f.srv.enqueue(R"({"text":"too late"})");

    QObject owner;
    vi.start(&owner, {});
    vi.stop();
    CHECK(vi.state() == State::Transcribing);
    vi.cancel();
    CHECK(vi.state() == State::Idle);

    waitFor([&] { return f.srv.requestCount == 1; });
    settle();
    CHECK(f.srv.requestCount == 1);
    CHECK(cap.finished.isEmpty());
    CHECK(cap.failed.isEmpty());
    CHECK(vi.state() == State::Idle);

    // The next recording works normally.
    f.srv.enqueue(R"({"text":"fresh"})");
    vi.start(&owner, {});
    vi.stop();
    waitFor([&] { return cap.settled(); });
    REQUIRE(cap.finished.size() == 1);
    CHECK(cap.finished[0].second == "fresh");
    VoiceInput::setCleanupEnabled(true);
}

TEST_CASE("Cancel while recording stops the recorder; a new start replaces the old owner") {
    Fixture f;
    auto   &vi = VoiceInput::instance();
    Capture cap(vi);
    QObject a, b;

    vi.start(&a, {});
    CHECK(vi.owner() == &a);
    vi.start(&b, {}); // A's recording is cancelled first
    CHECK(f.script.cancels == 1);
    CHECK(f.script.created == 2);
    CHECK(vi.owner() == &b);
    vi.cancel();
    CHECK(f.script.cancels == 2);
    CHECK(vi.state() == State::Idle);
    vi.stop(); // ignored when not recording
    settle(100);
    CHECK(cap.finished.isEmpty());
    CHECK(cap.failed.isEmpty());
    CHECK(f.srv.requestCount == 0);
}

TEST_CASE("Empty, too short or silent recordings fail without calling the API") {
    Fixture f;
    auto   &vi = VoiceInput::instance();
    QObject owner;

    for (const QByteArray &wav :
         {QByteArray(), makeWav(100), makeWav(2000, 0.0), QByteArray("junk")}) {
        Capture cap(vi);
        f.script.wav = wav;
        vi.start(&owner, {});
        vi.stop();
        waitFor([&] { return cap.settled(); });
        REQUIRE(cap.failed.size() == 1);
        CHECK(cap.failed[0].first == &owner);
        CHECK(cap.failed[0].second == "No speech was recorded");
        CHECK(vi.state() == State::Idle);
    }
    CHECK(f.srv.requestCount == 0);
}

TEST_CASE("An empty transcript is reported, not inserted") {
    Fixture f;
    auto   &vi = VoiceInput::instance();
    Capture cap(vi);
    f.srv.enqueue(R"({"text":"   "})");
    QObject owner;
    vi.start(&owner, {});
    vi.stop();
    waitFor([&] { return cap.settled(); });
    REQUIRE(cap.failed.size() == 1);
    CHECK(cap.failed[0].second == "No speech was recognised");
    CHECK(f.srv.requestCount == 1); // no clean-up call
}

TEST_CASE("Transcription and recorder errors reach failed(); a dead owner gets nothing") {
    Fixture f;
    auto   &vi = VoiceInput::instance();
    VoiceInput::setCleanupEnabled(false);

    {
        Capture cap(vi);
        f.srv.enqueueStatus(
            400, "Bad Request", "application/json", R"({"error":{"message":"Invalid file format"}})"
        );
        QObject owner;
        vi.start(&owner, {});
        vi.stop();
        waitFor([&] { return cap.settled(); });
        REQUIRE(cap.failed.size() == 1);
        CHECK(cap.failed[0].second == "Couldn't transcribe: Invalid file format");
    }
    {
        Capture cap(vi);
        f.script.failOnStart = true;
        QObject owner;
        vi.start(&owner, {});
        REQUIRE(cap.failed.size() == 1); // synchronous
        CHECK(cap.failed[0].second == "Couldn't record: no capture device");
        CHECK(vi.state() == State::Idle);
        f.script.failOnStart = false;
    }
    {
        Capture cap(vi);
        f.srv.enqueue(R"({"text":"orphan"})");
        auto *owner = new QObject;
        vi.start(owner, {});
        vi.stop();
        delete owner;
        waitFor([&] { return vi.state() == State::Idle; });
        settle(100);
        CHECK(vi.state() == State::Idle);
        CHECK(cap.finished.isEmpty());
        CHECK(cap.failed.isEmpty());
    }
    VoiceInput::setCleanupEnabled(true);
}

TEST_CASE("Availability follows speech-to-text providers") {
    auto &vi  = VoiceInput::instance();
    auto &svc = LlmService::instance();
    CHECK_FALSE(vi.isAvailable()); // nothing connected in this suite

    int     changes = 0;
    QObject ctx;
    QObject::connect(&vi, &VoiceInput::availabilityChanged, &ctx, [&] { ++changes; });
    Capture cap(vi);
    QObject owner;
    vi.start(&owner, {});
    REQUIRE(cap.failed.size() == 1);
    CHECK(cap.failed[0].second.contains("speech-to-text"));
    CHECK(vi.state() == State::Idle);

    {
        Fixture f;
        CHECK(vi.isAvailable());
        CHECK(changes >= 1);
    }
    CHECK_FALSE(vi.isAvailable());

    // Whisper-style STT model: prompt without instructions, no keywords[].
    Fixture f("whisper-1");
    Capture cap2(vi);
    VoiceInput::setCleanupEnabled(false);
    f.srv.enqueue(R"({"text":"hello"})");
    vi.start(&owner, sampleContext());
    vi.stop();
    waitFor([&] { return cap2.settled(); });
    REQUIRE(cap2.finished.size() == 1);
    const QByteArray &body = f.srv.requestBodies[0];
    CHECK(
        body.contains("name=\"prompt\"\r\n\r\n" + QByteArray(VoicePrompt::kTranscriptStylePreamble))
    );
    CHECK_FALSE(body.contains("keywords[]"));
    CHECK_FALSE(body.contains("languages[]"));
    CHECK(svc.sttProvider() == f.provider);
    VoiceInput::setCleanupEnabled(true);
}
