// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
// Wire codecs for the two chat-completion formats msga speaks — pure functions
// (no network, no QObject) so request shaping and response parsing are unit-
// testable against captured payloads.
//
//   OpenAiChat         POST <base>/chat/completions, GET <base>/models,
//                      "Authorization: Bearer". Spoken by OpenAI itself and by
//                      every OpenAI-compatible server (vLLM, Ollama, LM Studio,
//                      LiteLLM, OpenRouter, …). <base> ends in "/v1" by
//                      convention (OpenRouter: "/api/v1").
//   AnthropicMessages  POST <base>/v1/messages, GET <base>/v1/models,
//                      "x-api-key" + "anthropic-version". Anthropic's native
//                      API — kept because its OpenAI-compatibility layer is
//                      documented as "not production-ready" and drops
//                      structured outputs / prompt caching / refusal detail.
//
// Speech-to-text rides the OpenAI format only: POST <base>/audio/transcriptions
// (multipart/form-data, Whisper-style), answered by OpenAI and by the compat
// servers that host a Whisper model (speaches/faster-whisper-server, LocalAI,
// vLLM, LiteLLM, …). Anthropic's API has no audio endpoint at all.
#pragma once

#include "llm_types.h"

#include <QByteArray>
#include <QList>
#include <QPair>
#include <QString>
#include <QStringList>
#include <QUrl>

namespace LlmWire {

enum class Format { OpenAiChat, AnthropicMessages };

struct Endpoint {
    Format  format = Format::OpenAiChat;
    QString baseUrl;
    QString apiKey; // empty → no auth header (local servers without --api-key)
    // OpenAI proper wants `max_completion_tokens` (`max_tokens` is deprecated
    // and rejected by its reasoning models); compat servers universally
    // understand `max_tokens`, and sending both is an error. Never both.
    bool    maxCompletionTokens = false;
    // Sent as `reasoning_effort` when non-empty (OpenAI preset only — compat
    // servers may reject unknown fields). When reasoning is on, `temperature`
    // is dropped: OpenAI's reasoning models reject it.
    QString reasoningEffort;
};

struct HttpRequest {
    QUrl                                 url;
    QList<QPair<QByteArray, QByteArray>> headers; // Content-Type included for POSTs
    QByteArray                           body;    // empty → GET
};

struct ChatResult {
    bool          ok = false;
    Llm::Response response;
    QString       error; // human-readable, set when !ok
};

struct ModelsResult {
    bool        ok = false;
    QStringList models; // ids, server order
    QString     error;
};

HttpRequest buildChat(const Endpoint &ep, const Llm::Request &req);
HttpRequest buildListModels(const Endpoint &ep);

// ── Speech-to-text ────────────────────────────────────────────────────

struct TranscriptionInput {
    QByteArray  audio;
    QString     fileName; // with the real extension — servers sniff the format by it
    QString     mimeType; // part Content-Type ("audio/mpeg"); empty → octet-stream
    QString     model;    // "gpt-transcribe", "whisper-1", …
    // Context that steers recognition. `prompt` is sent to every server (for
    // Whisper it reads as the transcript preceding the audio; the gpt-*
    // transcribe family follows it as instructions). `keywords` (terms to
    // spell exactly) and `languages` (ISO 639-1 hints) exist only on OpenAI's
    // gpt-*-transcribe models, so they are sent only when `model` starts with
    // "gpt-" — Whisper-style servers don't know them.
    QString     prompt;
    QStringList keywords;
    QStringList languages;
};

struct TranscriptionResult {
    bool    ok = false;
    QString text;
    QString error;
};

// Whether the format has a transcription endpoint at all (OpenAiChat only).
bool        supportsTranscription(Format format);
// Multipart POST <base>/audio/transcriptions with `model`, `file` and
// `response_format=json`, plus `prompt` when set and — gpt-* models only —
// one `keywords[]` field per sanitised keyword and one `languages[]` field per
// language. `boundary` is generated when empty (tests pin it).
// Precondition: supportsTranscription(ep.format).
HttpRequest buildTranscription(
    const Endpoint &ep, const TranscriptionInput &in, const QByteArray &boundary = {}
);
// Speech-to-text keywords as OpenAI accepts them: trimmed, empty and
// case-insensitive duplicates dropped (first spelling wins), and any term
// containing '<', '>', CR or LF dropped — one such term makes OpenAI reject
// the whole request.
QStringList         sanitizeTranscriptionKeywords(const QStringList &keywords);
// {"text": …} on success; a 200 that isn't JSON is taken as the plain text
// (servers that ignore response_format).
TranscriptionResult parseTranscription(int httpStatus, const QByteArray &body);
// Part Content-Type for an audio file extension ("mp3" → "audio/mpeg"); empty
// when unknown.
QString             audioMimeForExtension(const QString &ext);

// httpStatus 0 = transport failure (body may hold the Qt error string).
ChatResult   parseChat(Format format, int httpStatus, const QByteArray &body);
ModelsResult parseModels(Format format, int httpStatus, const QByteArray &body);

// Tidies a user-typed OpenAI-compatible base URL: trims, drops a trailing "/",
// strips a pasted "/chat/completions", and appends "/v1" when there is no path
// at all ("http://host:8000" → "http://host:8000/v1"). Anything else is kept —
// OpenRouter's "/api/v1" is legitimate. Returns empty for an unusable URL.
QString normalizeOpenAiBaseUrl(const QString &raw);

// True for http:// URLs to a host that is not loopback / link-local / RFC 1918
// — the API key would travel in cleartext across a real network.
bool isCleartextRemote(const QString &url);

} // namespace LlmWire
