// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
// Unit tests for llm/llm_wire — request shaping and response parsing for the
// OpenAI-compatible and Anthropic Messages formats, against captured payloads.
#include <catch2/catch_test_macros.hpp>

#include "llm/llm_wire.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>

using namespace LlmWire;

namespace {

QJsonObject bodyOf(const HttpRequest &r) {
    return QJsonDocument::fromJson(r.body).object();
}

QByteArray header(const HttpRequest &r, const char *name) {
    for (const auto &[k, v] : r.headers)
        if (k.compare(name, Qt::CaseInsensitive) == 0)
            return v;
    return {};
}

Llm::Request sampleRequest() {
    Llm::Request req;
    req.system   = "Be brief.";
    req.messages = {
        {Llm::Message::Role::User, "hi"},
        {Llm::Message::Role::Assistant, "hello"},
        {Llm::Message::Role::User, "summarize"}
    };
    req.model     = "some-model";
    req.maxTokens = 512;
    return req;
}

Endpoint openAiCompat(const QString &key = {}) {
    Endpoint ep;
    ep.format  = Format::OpenAiChat;
    ep.baseUrl = "http://localhost:8000/v1";
    ep.apiKey  = key;
    return ep;
}

Endpoint openAiProper() {
    Endpoint ep            = openAiCompat("sk-test");
    ep.baseUrl             = "https://api.openai.com/v1";
    ep.maxCompletionTokens = true;
    return ep;
}

Endpoint anthropic() {
    Endpoint ep;
    ep.format  = Format::AnthropicMessages;
    ep.baseUrl = "https://api.anthropic.com";
    ep.apiKey  = "sk-ant-test";
    return ep;
}

} // namespace

// ── Request shaping ───────────────────────────────────────────────────

TEST_CASE("OpenAI wire: system prompt leads the messages, max_tokens for compat servers") {
    const auto r = buildChat(openAiCompat(), sampleRequest());
    CHECK(r.url.toString() == "http://localhost:8000/v1/chat/completions");
    CHECK(header(r, "Content-Type") == "application/json");

    const auto body = bodyOf(r);
    CHECK(body["model"].toString() == "some-model");
    const auto msgs = body["messages"].toArray();
    REQUIRE(msgs.size() == 4);
    CHECK(msgs[0].toObject()["role"].toString() == "system");
    CHECK(msgs[0].toObject()["content"].toString() == "Be brief.");
    CHECK(msgs[1].toObject()["role"].toString() == "user");
    CHECK(msgs[2].toObject()["role"].toString() == "assistant");
    CHECK(body["max_tokens"].toInt() == 512);
    CHECK(!body.contains("max_completion_tokens"));
    CHECK(!body.contains("reasoning_effort"));
    CHECK(!body.contains("temperature"));
}

TEST_CASE("OpenAI wire: no key → no Authorization header (vLLM without --api-key)") {
    CHECK(header(buildChat(openAiCompat(), sampleRequest()), "Authorization").isEmpty());
    CHECK(
        header(buildChat(openAiCompat("ollama"), sampleRequest()), "Authorization") ==
        "Bearer ollama"
    );
}

TEST_CASE("OpenAI wire: OpenAI proper gets max_completion_tokens, never both") {
    const auto body = bodyOf(buildChat(openAiProper(), sampleRequest()));
    CHECK(body["max_completion_tokens"].toInt() == 512);
    CHECK(!body.contains("max_tokens"));
}

TEST_CASE("OpenAI wire: reasoning_effort only when set; temperature dropped when reasoning is on") {
    auto req        = sampleRequest();
    req.temperature = 0.0;

    // Compat server, no effort configured → temperature goes through.
    CHECK(bodyOf(buildChat(openAiCompat(), req))["temperature"].toDouble() == 0.0);

    // Light tier: effort "none" is sent and temperature is allowed.
    auto ep            = openAiProper();
    ep.reasoningEffort = "none";
    auto body          = bodyOf(buildChat(ep, req));
    CHECK(body["reasoning_effort"].toString() == "none");
    CHECK(body.contains("temperature"));

    // Reasoning on → OpenAI rejects temperature, so it must be omitted.
    ep.reasoningEffort = "medium";
    body               = bodyOf(buildChat(ep, req));
    CHECK(body["reasoning_effort"].toString() == "medium");
    CHECK(!body.contains("temperature"));
}

TEST_CASE("Anthropic wire: native Messages format") {
    auto req        = sampleRequest();
    req.temperature = 1.7; // Anthropic caps at 1.0
    const auto r    = buildChat(anthropic(), req);
    CHECK(r.url.toString() == "https://api.anthropic.com/v1/messages");
    CHECK(header(r, "x-api-key") == "sk-ant-test");
    CHECK(header(r, "anthropic-version") == "2023-06-01");
    CHECK(header(r, "Authorization").isEmpty());

    const auto body = bodyOf(r);
    CHECK(body["system"].toString() == "Be brief.");
    CHECK(body["max_tokens"].toInt() == 512);
    CHECK(body["temperature"].toDouble() == 1.0);
    const auto msgs = body["messages"].toArray();
    REQUIRE(msgs.size() == 3); // no system message in the array
    CHECK(msgs[0].toObject()["role"].toString() == "user");
}

TEST_CASE("Model listing requests") {
    const auto o = buildListModels(openAiCompat("k"));
    CHECK(o.url.toString() == "http://localhost:8000/v1/models");
    CHECK(o.body.isEmpty());
    CHECK(header(o, "Authorization") == "Bearer k");

    const auto a = buildListModels(anthropic());
    CHECK(a.url.toString() == "https://api.anthropic.com/v1/models");
    CHECK(header(a, "x-api-key") == "sk-ant-test");
}

TEST_CASE("Trailing slashes on the base URL don't double up") {
    Endpoint ep = openAiCompat();
    ep.baseUrl  = "http://host:8000/v1/";
    CHECK(buildChat(ep, sampleRequest()).url.toString() == "http://host:8000/v1/chat/completions");
}

// ── Response parsing ──────────────────────────────────────────────────

TEST_CASE("OpenAI response: string content") {
    const auto r = parseChat(
        Format::OpenAiChat,
        200,
        R"({"id":"x","model":"gpt-5.6-luna","choices":[{"index":0,"finish_reason":"stop",
            "message":{"role":"assistant","content":"Hello there"}}]})"
    );
    REQUIRE(r.ok);
    CHECK(r.response.text == "Hello there");
    CHECK(r.response.model == "gpt-5.6-luna");
    CHECK(r.response.stopReason == "stop");
}

TEST_CASE("OpenAI response: content as an array of text parts, reasoning_content ignored") {
    const auto r = parseChat(
        Format::OpenAiChat,
        200,
        R"({"choices":[{"finish_reason":"length","message":{"role":"assistant",
            "reasoning_content":"thinking…",
            "content":[{"type":"text","text":"Part one. "},{"type":"text","text":"Part two."}]}}]})"
    );
    REQUIRE(r.ok);
    CHECK(r.response.text == "Part one. Part two.");
    CHECK(r.response.stopReason == "length");
}

TEST_CASE("OpenAI response: error object under `error`") {
    const auto r = parseChat(
        Format::OpenAiChat,
        401,
        R"({"error":{"message":"Incorrect API key provided","type":"invalid_request_error"}})"
    );
    CHECK(!r.ok);
    CHECK(r.error == "Incorrect API key provided");
}

TEST_CASE("OpenAI response: flat vLLM error body") {
    const auto r = parseChat(
        Format::OpenAiChat,
        400,
        R"({"object":"error","message":"The model `foo` does not exist.","type":"NotFoundError","param":null,"code":404})"
    );
    CHECK(!r.ok);
    CHECK(r.error == "The model `foo` does not exist.");
}

TEST_CASE("OpenAI response: Ollama-style error and string error") {
    CHECK(
        parseChat(Format::OpenAiChat, 404, R"({"error":{"message":"model 'x' not found"}})")
            .error == "model 'x' not found"
    );
    CHECK(
        parseChat(Format::OpenAiChat, 502, R"({"error":"upstream down"})").error == "upstream down"
    );
}

TEST_CASE("Non-JSON failures surface the HTTP status and a body snippet") {
    const auto r = parseChat(Format::OpenAiChat, 502, "<html><body>Bad Gateway</body></html>");
    CHECK(!r.ok);
    CHECK(r.error.contains("502"));
    CHECK(r.error.contains("Bad Gateway"));

    const auto notFound = parseChat(Format::OpenAiChat, 404, "not found");
    CHECK(notFound.error.contains("/v1")); // hints at the usual cause

    const auto empty = parseChat(Format::OpenAiChat, 500, "");
    CHECK(empty.error == "HTTP 500");

    // A 200 with garbage is still a failure, not an empty completion.
    CHECK(!parseChat(Format::OpenAiChat, 200, "<html>login page</html>").ok);
    CHECK(!parseChat(Format::OpenAiChat, 200, R"({"object":"list"})").ok); // no choices
}

TEST_CASE("Transport failure (status 0) passes the Qt error string through") {
    const auto r = parseChat(Format::OpenAiChat, 0, "Connection refused");
    CHECK(!r.ok);
    CHECK(r.error == "Connection refused");
}

TEST_CASE("Anthropic response: text blocks concatenated, refusal is an error") {
    const auto ok = parseChat(
        Format::AnthropicMessages,
        200,
        R"({"model":"claude-haiku-4-5","stop_reason":"end_turn",
            "content":[{"type":"text","text":"A"},{"type":"tool_use","id":"t"},{"type":"text","text":"B"}]})"
    );
    REQUIRE(ok.ok);
    CHECK(ok.response.text == "AB");
    CHECK(ok.response.stopReason == "end_turn");

    const auto refused =
        parseChat(Format::AnthropicMessages, 200, R"({"stop_reason":"refusal","content":[]})");
    CHECK(!refused.ok);
    CHECK(refused.error == "refusal");

    const auto err = parseChat(
        Format::AnthropicMessages,
        401,
        R"({"type":"error","error":{"type":"authentication_error","message":"invalid x-api-key"}})"
    );
    CHECK(!err.ok);
    CHECK(err.error == "invalid x-api-key");
}

TEST_CASE("Model list parsing works for both vendors' shapes") {
    const auto o = parseModels(
        Format::OpenAiChat,
        200,
        R"({"object":"list","data":[{"id":"Qwen3-32B","object":"model","owned_by":"vllm"},{"id":"other"}]})"
    );
    REQUIRE(o.ok);
    CHECK(o.models == QStringList{"Qwen3-32B", "other"});

    const auto a = parseModels(
        Format::AnthropicMessages,
        200,
        R"({"data":[{"type":"model","id":"claude-opus-5","display_name":"Claude Opus 5"}],"has_more":false})"
    );
    REQUIRE(a.ok);
    CHECK(a.models == QStringList{"claude-opus-5"});

    CHECK(!parseModels(Format::OpenAiChat, 401, R"({"error":{"message":"nope"}})").ok);
}

// ── URL helpers ───────────────────────────────────────────────────────

TEST_CASE("normalizeOpenAiBaseUrl") {
    CHECK(normalizeOpenAiBaseUrl("http://localhost:8000") == "http://localhost:8000/v1");
    CHECK(normalizeOpenAiBaseUrl("http://localhost:8000/") == "http://localhost:8000/v1");
    CHECK(normalizeOpenAiBaseUrl("  http://localhost:8000/v1/  ") == "http://localhost:8000/v1");
    CHECK(normalizeOpenAiBaseUrl("http://h:8000/v1/chat/completions") == "http://h:8000/v1");
    // Non-standard but legitimate paths are kept as typed.
    CHECK(normalizeOpenAiBaseUrl("https://openrouter.ai/api/v1") == "https://openrouter.ai/api/v1");
    // A bare host gets a scheme.
    CHECK(normalizeOpenAiBaseUrl("ai.corp:8000") == "http://ai.corp:8000/v1");
    CHECK(normalizeOpenAiBaseUrl("").isEmpty());
    CHECK(normalizeOpenAiBaseUrl("ftp://x/v1").isEmpty());
    CHECK(normalizeOpenAiBaseUrl("http://").isEmpty());
}

TEST_CASE("isCleartextRemote flags plain http to a real network only") {
    CHECK(!isCleartextRemote("https://ai.corp:8000/v1"));
    CHECK(!isCleartextRemote("http://localhost:11434/v1"));
    CHECK(!isCleartextRemote("http://127.0.0.1:8000/v1"));
    CHECK(!isCleartextRemote("http://[::1]:8000/v1"));
    CHECK(!isCleartextRemote("http://192.168.1.20:8000/v1"));
    CHECK(!isCleartextRemote("http://10.0.0.5/v1"));
    CHECK(!isCleartextRemote("http://172.16.4.4:8000/v1"));
    CHECK(!isCleartextRemote("http://box.local:1234/v1"));
    CHECK(isCleartextRemote("http://ai.corp:8000/v1"));
    CHECK(isCleartextRemote("http://8.8.8.8/v1"));
    CHECK(isCleartextRemote("http://172.32.0.1/v1")); // just outside 172.16/12
}

// ── Speech-to-text ─────────────────────────────────────────────────────────────

TEST_CASE(
    "buildTranscription: multipart POST to /audio/transcriptions with model + file", "[stt]"
) {
    CHECK(supportsTranscription(Format::OpenAiChat));
    CHECK_FALSE(supportsTranscription(Format::AnthropicMessages));

    TranscriptionInput in;
    in.audio            = QByteArray("ID3\x03\x00binary\r\n--not-a-boundary\x00", 30);
    in.fileName         = "F0123.mp3";
    in.mimeType         = "audio/mpeg";
    in.model            = "whisper-1";
    const HttpRequest r = buildTranscription(openAiCompat("sk-x"), in, "BOUNDARY");

    CHECK(r.url.toString() == "http://localhost:8000/v1/audio/transcriptions");
    CHECK(header(r, "Authorization") == "Bearer sk-x");
    CHECK(header(r, "Content-Type") == "multipart/form-data; boundary=BOUNDARY");

    const QByteArray &b = r.body;
    CHECK(b.startsWith("--BOUNDARY\r\n"));
    CHECK(b.endsWith("\r\n--BOUNDARY--\r\n"));
    CHECK(b.contains("Content-Disposition: form-data; name=\"model\"\r\n\r\nwhisper-1\r\n"));
    CHECK(b.contains("Content-Disposition: form-data; name=\"response_format\"\r\n\r\njson\r\n"));
    CHECK(b.contains(
        "Content-Disposition: form-data; name=\"file\"; filename=\"F0123.mp3\"\r\n"
        "Content-Type: audio/mpeg\r\n\r\n"
    ));
    // The audio bytes travel verbatim (NULs included), between the header and
    // the closing delimiter.
    const int at = b.indexOf(in.audio);
    REQUIRE(at > 0);
    CHECK(b.mid(at + in.audio.size()).startsWith("\r\n--BOUNDARY--"));

    // No key → no Authorization; unknown MIME → octet-stream; a random boundary otherwise.
    in.mimeType          = {};
    const HttpRequest r2 = buildTranscription(openAiCompat(), in);
    CHECK(header(r2, "Authorization").isEmpty());
    CHECK(r2.body.contains("Content-Type: application/octet-stream\r\n"));
    const QByteArray ct = header(r2, "Content-Type");
    CHECK(ct.startsWith("multipart/form-data; boundary=msga-"));
    CHECK(r2.body.startsWith("--" + ct.mid(ct.indexOf('=') + 1) + "\r\n"));
}

TEST_CASE("parseTranscription: text, plain-text fallback, and the error shapes", "[stt]") {
    auto ok = parseTranscription(200, R"({"text":"  Hello there.  "})");
    CHECK(ok.ok);
    CHECK(ok.text == "Hello there.");

    // A server that ignores response_format and answers text/plain.
    auto plain = parseTranscription(200, "Just the words.\n");
    CHECK(plain.ok);
    CHECK(plain.text == "Just the words.");
    CHECK(plain.error.isEmpty());

    auto noText = parseTranscription(200, R"({"segments":[]})");
    CHECK_FALSE(noText.ok);
    CHECK(noText.error.contains("no text"));

    // 404 = the server has no STT route (not the "/v1" hint the chat route gives).
    auto missing = parseTranscription(404, "Not Found");
    CHECK_FALSE(missing.ok);
    CHECK(missing.error.contains("speech-to-text"));
    CHECK_FALSE(missing.error.contains("/v1"));

    auto apiErr = parseTranscription(
        400, R"({"error":{"message":"Unsupported file format","type":"invalid_request_error"}})"
    );
    CHECK_FALSE(apiErr.ok);
    CHECK(apiErr.error == "Unsupported file format");

    auto transport = parseTranscription(0, "Connection refused");
    CHECK_FALSE(transport.ok);
    CHECK(transport.error == "Connection refused");
}

TEST_CASE("audioMimeForExtension covers what Slack serves", "[stt]") {
    CHECK(audioMimeForExtension("mp3") == "audio/mpeg");
    CHECK(audioMimeForExtension("MP4") == "audio/mp4");
    CHECK(audioMimeForExtension("m4a") == "audio/mp4");
    CHECK(audioMimeForExtension("wav") == "audio/wav");
    CHECK(audioMimeForExtension("webm") == "audio/webm");
    CHECK(audioMimeForExtension("xyz").isEmpty());
    CHECK(audioMimeForExtension({}).isEmpty());
}

TEST_CASE("buildTranscription: prompt, keywords[] and languages[] for gpt-* models", "[stt]") {
    TranscriptionInput in;
    in.audio     = "RIFFxxxxWAVE";
    in.fileName  = "voice.wav";
    in.mimeType  = "audio/wav";
    in.model     = "gpt-transcribe";
    in.prompt    = "Clean transcript.\nConversation: backend";
    in.keywords  = {"  llm_wire.cpp ", "Qt6", "qt6", "", "<script>", "a>b", "two\nlines", "k8s"};
    in.languages = {"sv", " EN ", "en", ""};
    const HttpRequest r = buildTranscription(openAiProper(), in, "B");
    const QByteArray &b = r.body;

    CHECK(b.contains(
        "Content-Disposition: form-data; name=\"prompt\"\r\n\r\n"
        "Clean transcript.\nConversation: backend\r\n"
    ));
    // One field per keyword, trimmed and deduped case-insensitively (first
    // spelling wins); the ones OpenAI would reject the whole request for are gone.
    CHECK(b.count("name=\"keywords[]\"") == 3);
    CHECK(b.contains("name=\"keywords[]\"\r\n\r\nllm_wire.cpp\r\n"));
    CHECK(b.contains("name=\"keywords[]\"\r\n\r\nQt6\r\n"));
    CHECK(b.contains("name=\"keywords[]\"\r\n\r\nk8s\r\n"));
    CHECK_FALSE(b.contains("qt6"));
    CHECK_FALSE(b.contains("<script>"));
    CHECK_FALSE(b.contains("a>b"));
    CHECK_FALSE(b.contains("two\nlines"));
    CHECK(b.count("name=\"languages[]\"") == 2);
    CHECK(b.contains("name=\"languages[]\"\r\n\r\nsv\r\n"));
    CHECK(b.contains("name=\"languages[]\"\r\n\r\nen\r\n"));
    // Still well-formed: the file part comes last.
    CHECK(b.endsWith("RIFFxxxxWAVE\r\n--B--\r\n"));
}

TEST_CASE("buildTranscription: Whisper-style models get the prompt only", "[stt]") {
    TranscriptionInput in;
    in.audio           = "x";
    in.fileName        = "voice.wav";
    in.model           = "whisper-1";
    in.prompt          = "Earlier: deploy the llm_wire.cpp fix.";
    in.keywords        = {"llm_wire.cpp"};
    in.languages       = {"en"};
    const QByteArray b = buildTranscription(openAiCompat(), in, "B").body;
    CHECK(b.contains("name=\"prompt\"\r\n\r\nEarlier: deploy the llm_wire.cpp fix.\r\n"));
    CHECK_FALSE(b.contains("keywords[]"));
    CHECK_FALSE(b.contains("languages[]"));

    // Nothing set → no optional fields at all (the pre-existing request shape).
    in.prompt = "   ";
    in.model  = "gpt-transcribe";
    in.keywords.clear();
    in.languages.clear();
    const QByteArray b2 = buildTranscription(openAiCompat(), in, "B").body;
    CHECK_FALSE(b2.contains("name=\"prompt\""));
    CHECK_FALSE(b2.contains("keywords[]"));
    CHECK_FALSE(b2.contains("languages[]"));
}

TEST_CASE("sanitizeTranscriptionKeywords", "[stt]") {
    CHECK(
        sanitizeTranscriptionKeywords({" Foo ", "foo", "FOO", "bar\r", "<b>", "", "  ", "Baz"}) ==
        QStringList{"Foo", "Baz"}
    );
    CHECK(sanitizeTranscriptionKeywords({}).isEmpty());
}
