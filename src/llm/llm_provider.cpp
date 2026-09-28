// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#include "llm_provider.h"

#include "llm_token_store.h"
#include "network/shared_nam.h"

#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>
#include <QUuid>

using namespace Qt::StringLiterals;

namespace {

// Slow local models: a 512-token summary on a CPU-hosted 7B can take a minute.
constexpr int kTransferTimeoutMs = 120'000;

QNetworkRequest toNetworkRequest(const LlmWire::HttpRequest &r) {
    QNetworkRequest req(r.url);
    for (const auto &[name, value] : r.headers)
        req.setRawHeader(name, value);
    req.setTransferTimeout(kTransferTimeoutMs);
    return req;
}

QNetworkReply *send(const LlmWire::HttpRequest &r) {
    auto *nam = net::sharedNam();
    return r.body.isEmpty() ? nam->get(toNetworkRequest(r))
                            : nam->post(toNetworkRequest(r), r.body);
}

// HTTP status of a finished reply, 0 for a transport failure (body then holds
// the Qt error string so the parser can surface it).
int statusOf(QNetworkReply *reply, QByteArray &body) {
    body             = reply->readAll();
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (status == 0 && reply->error() != QNetworkReply::NoError)
        body = reply->errorString().toUtf8();
    return status;
}

void requestModels(
    const LlmWire::Endpoint &ep, Llm::OnModels onModels, Llm::OnError onError, QObject *ctx
) {
    auto             *reply = send(LlmWire::buildListModels(ep));
    QPointer<QObject> guard(ctx);
    QObject::connect(
        reply,
        &QNetworkReply::finished,
        reply,
        [reply,
         guard,
         format   = ep.format,
         onModels = std::move(onModels),
         onError  = std::move(onError)] {
            reply->deleteLater();
            if (!guard)
                return;
            QByteArray body;
            const int  status = statusOf(reply, body);
            const auto r      = LlmWire::parseModels(format, status, body);
            if (r.ok) {
                if (onModels)
                    onModels(r.models);
            } else if (onError) {
                onError(r.error);
            }
        }
    );
}

} // namespace

// ── Config presets ────────────────────────────────────────────────────

LlmProviderConfig LlmProviderConfig::anthropicPreset() {
    LlmProviderConfig c;
    c.id           = u"anthropic"_s;
    c.name         = u"Anthropic"_s;
    c.wire         = LlmWire::Format::AnthropicMessages;
    c.baseUrl      = u"https://api.anthropic.com"_s;
    c.defaultModel = u"claude-opus-5"_s;
    c.lightModel   = u"claude-haiku-4-5"_s;
    c.isPreset     = true;
    c.apiKeyUrl    = u"https://console.anthropic.com/settings/keys"_s;
    c.knownModels  = {
        u"claude-opus-5"_s, u"claude-sonnet-5"_s, u"claude-haiku-4-5"_s, u"claude-fable-5-1"_s
    };
    // No audio endpoint in the Messages API — transcribe() refuses up front.
    return c;
}

LlmProviderConfig LlmProviderConfig::openAiPreset() {
    LlmProviderConfig c;
    c.id                   = u"openai"_s;
    c.name                 = u"OpenAI"_s;
    c.wire                 = LlmWire::Format::OpenAiChat;
    c.baseUrl              = u"https://api.openai.com/v1"_s;
    c.defaultModel         = u"gpt-5.6-terra"_s;
    // GPT-5.6 models default to medium reasoning effort, and
    // max_completion_tokens counts reasoning tokens — a 512-token summary
    // budget would be eaten by thinking. The light tier turns reasoning off.
    c.lightModel           = u"gpt-5.6-luna"_s;
    c.lightReasoningEffort = u"none"_s;
    c.maxCompletionTokens  = true;
    c.isPreset             = true;
    c.apiKeyUrl            = u"https://platform.openai.com/api-keys"_s;
    c.knownModels     = {u"gpt-5.6-terra"_s, u"gpt-5.6-sol"_s, u"gpt-5.6-luna"_s, u"gpt-6-astra"_s};
    // OpenAI's recommended file-transcription model (successor of
    // gpt-4o-transcribe; whisper-1 still works for those who override).
    c.defaultSttModel = u"gpt-transcribe"_s;
    return c;
}

LlmProviderConfig LlmProviderConfig::newCustom() {
    LlmProviderConfig c;
    c.id              = u"custom-"_s + QUuid::createUuid().toString(QUuid::Id128).left(8);
    c.wire            = LlmWire::Format::OpenAiChat;
    // What every self-hosted Whisper front (speaches, LocalAI, LiteLLM) answers
    // to; vLLM wants the served model's own name — hence the override.
    c.defaultSttModel = u"whisper-1"_s;
    return c;
}

LlmProviderConfig LlmProviderConfig::presetById(const QString &id) {
    if (id == u"anthropic"_s)
        return anthropicPreset();
    if (id == u"openai"_s)
        return openAiPreset();
    return {};
}

// ── LlmProvider ───────────────────────────────────────────────────────

LlmProvider::LlmProvider(LlmProviderConfig cfg, QObject *parent)
    : QObject(parent), _cfg(std::move(cfg)), _apiKey(LlmTokenStore::loadApiKey(_cfg.id)) {}

QString LlmProvider::model() const {
    return _cfg.model.isEmpty() ? _cfg.defaultModel : _cfg.model;
}

QString LlmProvider::lightModel() const {
    return _cfg.lightModel.isEmpty() ? model() : _cfg.lightModel;
}

bool LlmProvider::supportsTranscription() const {
    return LlmWire::supportsTranscription(_cfg.wire);
}

QString LlmProvider::sttModel() const {
    return _cfg.sttModel.isEmpty() ? _cfg.defaultSttModel : _cfg.sttModel;
}

bool LlmProvider::isConnected() const {
    return _cfg.isPreset ? hasApiKey() : !_cfg.baseUrl.isEmpty();
}

QString LlmProvider::accountLabel() const {
    if (_apiKey.isEmpty())
        return {};
    return _apiKey.length() > 10 ? _apiKey.left(5) + QStringLiteral("…") + _apiKey.right(4)
                                 : tr("API key");
}

void LlmProvider::setApiKey(const QString &key) {
    const QString trimmed = key.trimmed();
    if (trimmed == _apiKey)
        return;
    _apiKey = trimmed;
    LlmTokenStore::saveApiKey(_cfg.id, _apiKey);
    emit authStateChanged();
}

void LlmProvider::applyConfig(const LlmProviderConfig &cfg) {
    if (_cfg.isPreset) {
        if (_cfg.model == cfg.model && _cfg.sttModel == cfg.sttModel)
            return;
        _cfg.model    = cfg.model;
        _cfg.sttModel = cfg.sttModel;
    } else {
        if (_cfg.name == cfg.name && _cfg.baseUrl == cfg.baseUrl && _cfg.model == cfg.model &&
            _cfg.sttModel == cfg.sttModel)
            return;
        _cfg.name     = cfg.name;
        _cfg.baseUrl  = cfg.baseUrl;
        _cfg.model    = cfg.model;
        _cfg.sttModel = cfg.sttModel;
    }
    emit configChanged();
}

LlmWire::Endpoint LlmProvider::endpoint(const QString &forModel) const {
    LlmWire::Endpoint ep;
    ep.format              = _cfg.wire;
    ep.baseUrl             = _cfg.baseUrl;
    ep.apiKey              = _apiKey;
    ep.maxCompletionTokens = _cfg.maxCompletionTokens;
    if (!forModel.isEmpty() && forModel == _cfg.lightModel)
        ep.reasoningEffort = _cfg.lightReasoningEffort;
    return ep;
}

void LlmProvider::chat(const Llm::Request &req, Llm::OnResponse onResponse, Llm::OnError onError) {
    if (!isConnected()) {
        if (onError)
            onError(tr("%1 is not connected").arg(_cfg.name));
        return;
    }
    Llm::Request effective = req;
    if (effective.model.isEmpty())
        effective.model = model();

    auto *reply = send(LlmWire::buildChat(endpoint(effective.model), effective));
    connect(
        reply,
        &QNetworkReply::finished,
        this,
        [reply,
         format     = _cfg.wire,
         onResponse = std::move(onResponse),
         onError    = std::move(onError)] {
            reply->deleteLater();
            QByteArray body;
            const int  status = statusOf(reply, body);
            auto       r      = LlmWire::parseChat(format, status, body);
            if (r.ok) {
                if (onResponse)
                    onResponse(std::move(r.response));
            } else if (onError) {
                onError(r.error);
            }
        }
    );
}

void LlmProvider::transcribe(
    LlmWire::TranscriptionInput in, Llm::OnText onText, Llm::OnError onError
) {
    if (!supportsTranscription()) {
        if (onError)
            onError(tr("%1 does not support speech-to-text").arg(_cfg.name));
        return;
    }
    if (!isConnected()) {
        if (onError)
            onError(tr("%1 is not connected").arg(_cfg.name));
        return;
    }
    if (in.model.isEmpty())
        in.model = sttModel();
    auto *reply = send(LlmWire::buildTranscription(endpoint(), in));
    connect(
        reply,
        &QNetworkReply::finished,
        this,
        [reply, onText = std::move(onText), onError = std::move(onError)] {
            reply->deleteLater();
            QByteArray body;
            const int  status = statusOf(reply, body);
            auto       r      = LlmWire::parseTranscription(status, body);
            if (r.ok) {
                if (onText)
                    onText(std::move(r.text));
            } else if (onError) {
                onError(r.error);
            }
        }
    );
}

void LlmProvider::listModels(Llm::OnModels onModels, Llm::OnError onError) {
    requestModels(endpoint(), std::move(onModels), std::move(onError), this);
}

void LlmProvider::probe(
    const LlmProviderConfig &cfg,
    const QString           &apiKey,
    Llm::OnModels            onModels,
    Llm::OnError             onError,
    QObject                 *ctx
) {
    LlmWire::Endpoint ep;
    ep.format  = cfg.wire;
    ep.baseUrl = cfg.baseUrl;
    ep.apiKey  = apiKey.trimmed();
    requestModels(ep, std::move(onModels), std::move(onError), ctx);
}
