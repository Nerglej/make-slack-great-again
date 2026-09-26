// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#include "llm_wire.h"

#include <QCoreApplication>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QUuid>

namespace LlmWire {

namespace {

constexpr const char *kAnthropicVersion = "2023-06-01";

QString tr(const char *s) {
    return QCoreApplication::translate("LlmWire", s);
}

void addHeader(HttpRequest &r, const char *name, const QByteArray &value) {
    r.headers.append(qMakePair(QByteArray(name), value));
}

QString joinUrl(const QString &base, const QString &path) {
    QString b = base;
    while (b.endsWith('/'))
        b.chop(1);
    return b + path;
}

QJsonArray chatMessages(const Llm::Request &req, bool systemAsMessage) {
    QJsonArray messages;
    if (systemAsMessage && !req.system.isEmpty())
        messages.append(QJsonObject{{"role", "system"}, {"content", req.system}});
    for (const auto &m : req.messages) {
        messages.append(
            QJsonObject{
                {"role", m.role == Llm::Message::Role::User ? "user" : "assistant"},
                {"content", m.text},
            }
        );
    }
    return messages;
}

// Text of a `content` field that is either a string or an array of
// {type:"text", text} parts (some compat servers emit the latter).
QString contentText(const QJsonValue &content) {
    if (content.isString())
        return content.toString();
    QString out;
    for (const auto &partRef : content.toArray()) {
        const QJsonObject part = partRef.toObject();
        if (part.value("type").toString() == "text" || part.contains("text"))
            out += part.value("text").toString();
    }
    return out;
}

// Error message carried by a JSON body, in any of the shapes seen in the wild:
//   {"error":{"message":…}}          OpenAI, Anthropic, Ollama, newer vLLM
//   {"error":"…"}                    some gateways
//   {"object":"error","message":…}   older vLLM (flat)
//   {"message":…} on a 4xx/5xx       generic
QString jsonErrorMessage(const QJsonObject &obj, int httpStatus) {
    const QJsonValue err = obj.value("error");
    if (err.isObject()) {
        const QString msg = err.toObject().value("message").toString();
        return msg.isEmpty() ? tr("unknown error") : msg;
    }
    if (err.isString())
        return err.toString();
    if (obj.value("object").toString() == "error" || httpStatus >= 400) {
        const QString msg = obj.value("message").toString();
        if (!msg.isEmpty())
            return msg;
    }
    return {};
}

// What a 404 means for the endpoint being called — the chat URL is the one
// users mistype; a missing transcription route is a server without STT.
enum class Route { Chat, Transcription };

QString httpFailure(int httpStatus, const QByteArray &body, Route route = Route::Chat) {
    if (httpStatus == 0)
        return body.isEmpty() ? tr("network error") : QString::fromUtf8(body);
    QString snippet = QString::fromUtf8(body).simplified();
    if (snippet.size() > 200)
        snippet = snippet.left(200) + QChar(0x2026);
    if (httpStatus == 404 && route == Route::Transcription)
        return tr("This server has no speech-to-text endpoint (HTTP 404)");
    if (httpStatus == 404)
        return tr("No chat endpoint at this URL (HTTP 404) — most servers expect it to end in /v1");
    if (snippet.isEmpty())
        return tr("HTTP %1").arg(httpStatus);
    return tr("HTTP %1: %2").arg(httpStatus).arg(snippet);
}

// Shared prelude: transport errors, HTTP errors and JSON-carried errors. Returns
// the parsed object on success; sets `error` otherwise.
bool preflight(
    int               httpStatus,
    const QByteArray &body,
    QJsonObject      &obj,
    QString          &error,
    Route             route = Route::Chat
) {
    if (httpStatus == 0) {
        error = httpFailure(0, body);
        return false;
    }
    QJsonParseError     perr{};
    const QJsonDocument doc = QJsonDocument::fromJson(body, &perr);
    if (perr.error != QJsonParseError::NoError || !doc.isObject()) {
        error = httpStatus >= 400 ? httpFailure(httpStatus, body, route)
                                  : tr("Unexpected response from server (not JSON)");
        return false;
    }
    obj               = doc.object();
    const QString msg = jsonErrorMessage(obj, httpStatus);
    if (!msg.isEmpty()) {
        error = msg;
        return false;
    }
    if (httpStatus >= 400) {
        error = httpFailure(httpStatus, body, route);
        return false;
    }
    return true;
}

void addFormField(
    QByteArray &body, const QByteArray &boundary, const char *name, const QByteArray &value
) {
    body += "--" + boundary + "\r\n";
    body += "Content-Disposition: form-data; name=\"" + QByteArray(name) + "\"\r\n\r\n";
    body += value + "\r\n";
}

} // namespace

HttpRequest buildChat(const Endpoint &ep, const Llm::Request &req) {
    HttpRequest out;
    addHeader(out, "Content-Type", "application/json");
    QJsonObject body{
        {"model", req.model}, {"messages", chatMessages(req, ep.format == Format::OpenAiChat)}
    };

    if (ep.format == Format::AnthropicMessages) {
        out.url = QUrl(joinUrl(ep.baseUrl, "/v1/messages"));
        addHeader(out, "anthropic-version", kAnthropicVersion);
        addHeader(out, "x-api-key", ep.apiKey.toUtf8());
        body["max_tokens"] = req.maxTokens;
        if (!req.system.isEmpty())
            body["system"] = req.system;
        if (req.temperature)
            body["temperature"] = std::clamp(*req.temperature, 0.0, 1.0);
    } else {
        out.url = QUrl(joinUrl(ep.baseUrl, "/chat/completions"));
        if (!ep.apiKey.isEmpty())
            addHeader(out, "Authorization", "Bearer " + ep.apiKey.toUtf8());
        body[ep.maxCompletionTokens ? "max_completion_tokens" : "max_tokens"] = req.maxTokens;
        const bool reasoningOn = !ep.reasoningEffort.isEmpty() && ep.reasoningEffort != "none";
        if (!ep.reasoningEffort.isEmpty())
            body["reasoning_effort"] = ep.reasoningEffort;
        if (req.temperature && !reasoningOn)
            body["temperature"] = *req.temperature;
    }
    out.body = QJsonDocument(body).toJson(QJsonDocument::Compact);
    return out;
}

HttpRequest buildListModels(const Endpoint &ep) {
    HttpRequest out;
    if (ep.format == Format::AnthropicMessages) {
        out.url = QUrl(joinUrl(ep.baseUrl, "/v1/models"));
        addHeader(out, "anthropic-version", kAnthropicVersion);
        addHeader(out, "x-api-key", ep.apiKey.toUtf8());
    } else {
        out.url = QUrl(joinUrl(ep.baseUrl, "/models"));
        if (!ep.apiKey.isEmpty())
            addHeader(out, "Authorization", "Bearer " + ep.apiKey.toUtf8());
    }
    return out;
}

bool supportsTranscription(Format format) {
    return format == Format::OpenAiChat;
}

HttpRequest
buildTranscription(const Endpoint &ep, const TranscriptionInput &in, const QByteArray &boundaryIn) {
    HttpRequest out;
    out.url = QUrl(joinUrl(ep.baseUrl, "/audio/transcriptions"));
    if (!ep.apiKey.isEmpty())
        addHeader(out, "Authorization", "Bearer " + ep.apiKey.toUtf8());
    // A UUID never occurs inside compressed audio; no need to scan the bytes.
    const QByteArray boundary =
        boundaryIn.isEmpty() ? "msga-" + QUuid::createUuid().toString(QUuid::Id128).toLatin1()
                             : boundaryIn;
    addHeader(out, "Content-Type", "multipart/form-data; boundary=" + boundary);

    QByteArray &body = out.body;
    addFormField(body, boundary, "model", in.model.toUtf8());
    addFormField(body, boundary, "response_format", "json");
    if (!in.prompt.trimmed().isEmpty())
        addFormField(body, boundary, "prompt", in.prompt.toUtf8());
    if (in.model.startsWith(QLatin1String("gpt-"))) {
        for (const QString &k : sanitizeTranscriptionKeywords(in.keywords))
            addFormField(body, boundary, "keywords[]", k.toUtf8());
        QStringList seen;
        for (const QString &raw : in.languages) {
            const QString lang = raw.trimmed().toLower();
            if (lang.isEmpty() || seen.contains(lang) || lang.contains('\r') || lang.contains('\n'))
                continue;
            seen << lang;
            addFormField(body, boundary, "languages[]", lang.toUtf8());
        }
    }
    // Quotes in the file name would break the header; the id-based names the
    // caller passes never carry any, but be safe.
    QByteArray fileName = in.fileName.toUtf8();
    fileName.replace('"', '_').replace('\r', ' ').replace('\n', ' ');
    body += "--" + boundary + "\r\n";
    body += "Content-Disposition: form-data; name=\"file\"; filename=\"" + fileName + "\"\r\n";
    body +=
        "Content-Type: " +
        (in.mimeType.isEmpty() ? QByteArray("application/octet-stream") : in.mimeType.toUtf8()) +
        "\r\n\r\n";
    body += in.audio + "\r\n";
    body += "--" + boundary + "--\r\n";
    return out;
}

QStringList sanitizeTranscriptionKeywords(const QStringList &keywords) {
    QStringList out;
    QStringList seenLower;
    for (const QString &raw : keywords) {
        if (raw.contains('<') || raw.contains('>') || raw.contains('\r') || raw.contains('\n'))
            continue;
        const QString k = raw.trimmed();
        if (k.isEmpty())
            continue;
        const QString lower = k.toLower();
        if (seenLower.contains(lower))
            continue;
        seenLower << lower;
        out << k;
    }
    return out;
}

TranscriptionResult parseTranscription(int httpStatus, const QByteArray &body) {
    TranscriptionResult r;
    QJsonObject         obj;
    if (!preflight(httpStatus, body, obj, r.error, Route::Transcription)) {
        // Some servers answer a plain-text transcript regardless of
        // response_format — a 2xx that isn't JSON is that, not a failure.
        if (httpStatus >= 200 && httpStatus < 300 && !body.trimmed().isEmpty() &&
            !body.trimmed().startsWith('{')) {
            r.ok = true;
            r.error.clear();
            r.text = QString::fromUtf8(body).trimmed();
        }
        return r;
    }
    if (!obj.contains("text")) {
        r.error = tr("Unexpected response from server (no text)");
        return r;
    }
    r.text = obj.value("text").toString().trimmed();
    r.ok   = true;
    return r;
}

QString audioMimeForExtension(const QString &extIn) {
    const QString ext = extIn.toLower();
    if (ext == "mp3" || ext == "mpga" || ext == "mpeg")
        return "audio/mpeg";
    if (ext == "mp4" || ext == "m4a")
        return "audio/mp4";
    if (ext == "wav")
        return "audio/wav";
    if (ext == "flac")
        return "audio/flac";
    if (ext == "ogg" || ext == "oga")
        return "audio/ogg";
    if (ext == "opus")
        return "audio/opus";
    if (ext == "webm")
        return "audio/webm";
    if (ext == "aac")
        return "audio/aac";
    return {};
}

ChatResult parseChat(Format format, int httpStatus, const QByteArray &body) {
    ChatResult  r;
    QJsonObject obj;
    if (!preflight(httpStatus, body, obj, r.error))
        return r;

    r.response.model = obj.value("model").toString();
    if (format == Format::AnthropicMessages) {
        r.response.stopReason = obj.value("stop_reason").toString();
        // Safety classifiers can refuse with an empty content array — surface
        // that as an error rather than an empty completion.
        if (r.response.stopReason == "refusal") {
            r.error = "refusal";
            return r;
        }
        for (const auto &blockRef : obj.value("content").toArray()) {
            const QJsonObject block = blockRef.toObject();
            if (block.value("type").toString() == "text")
                r.response.text += block.value("text").toString();
        }
    } else {
        const QJsonArray choices = obj.value("choices").toArray();
        if (choices.isEmpty()) {
            r.error = tr("Unexpected response from server (no choices)");
            return r;
        }
        const QJsonObject choice = choices.at(0).toObject();
        r.response.stopReason    = choice.value("finish_reason").toString();
        r.response.text          = contentText(choice.value("message").toObject().value("content"));
    }
    r.ok = true;
    return r;
}

ModelsResult parseModels(Format, int httpStatus, const QByteArray &body) {
    ModelsResult r;
    QJsonObject  obj;
    if (!preflight(httpStatus, body, obj, r.error))
        return r;
    // OpenAI and Anthropic agree on {"data":[{"id":…},…]}.
    for (const auto &mRef : obj.value("data").toArray()) {
        const QString id = mRef.toObject().value("id").toString();
        if (!id.isEmpty())
            r.models.append(id);
    }
    r.ok = true;
    return r;
}

QString normalizeOpenAiBaseUrl(const QString &raw) {
    QString s = raw.trimmed();
    if (s.isEmpty())
        return {};
    if (!s.contains("://"))
        s.prepend("http://");
    while (s.endsWith('/'))
        s.chop(1);
    if (s.endsWith("/chat/completions"))
        s.chop(int(qstrlen("/chat/completions")));
    const QUrl url(s);
    if (!url.isValid() || url.host().isEmpty() ||
        (url.scheme() != "http" && url.scheme() != "https"))
        return {};
    if (url.path().isEmpty())
        s += "/v1";
    return s;
}

bool isCleartextRemote(const QString &urlStr) {
    const QUrl url(urlStr);
    if (url.scheme() != "http")
        return false;
    const QString host = url.host().toLower();
    if (host == "localhost" || host.endsWith(".localhost") || host.endsWith(".local"))
        return false;
    QHostAddress addr;
    if (!addr.setAddress(host))
        return true; // a DNS name over plain http → assume remote
    if (addr.isLoopback() || addr.isLinkLocal())
        return false;
    if (addr.protocol() == QAbstractSocket::IPv4Protocol) {
        const quint32 v = addr.toIPv4Address();
        if ((v >> 24) == 10 || (v >> 20) == 0xAC1 ||
            (v >> 16) == 0xC0A8) // 10/8, 172.16/12, 192.168/16
            return false;
    }
    if (addr.isUniqueLocalUnicast()) // fc00::/7
        return false;
    return true;
}

} // namespace LlmWire
