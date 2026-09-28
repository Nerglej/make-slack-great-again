// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#include "gif_search.h"
#include "app_credentials.h"
#include "network/shared_nam.h"
#include "util/secret_store.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>

using namespace Qt::StringLiterals;

namespace net {

static constexpr char kSecretKey[] = "gif/giphy/apiKey";
static constexpr char kBase[]      = "https://api.giphy.com/v1/gifs/";

// G through PG-13: excludes GIPHY's explicit tier while leaving the picker's
// results looking like a normal GIF picker. Tightening this to "pg" is a
// one-word change if a deployment wants it.
static constexpr char kRating[] = "pg-13";

#if defined(MSGA_DEMO)
static QString s_demoBase; // "http://127.0.0.1:PORT/v1/gifs/" while the demo runs

void GifSearch::setDemoEndpoint(const QString &baseUrl) {
    s_demoBase = baseUrl.isEmpty() ? QString() : baseUrl + QStringLiteral("/v1/gifs/");
}
#endif

QString GifSearch::apiKey() {
#if defined(MSGA_DEMO)
    if (!s_demoBase.isEmpty())
        return QStringLiteral("demo");
#endif
    const QString user = userApiKey();
    if (!user.isEmpty())
        return user;
    return QString::fromUtf8(AppCredentials::giphyKey).trimmed();
}

QString GifSearch::userApiKey() {
    return SecretStore::readMigrating(QString::fromUtf8(kSecretKey)).trimmed();
}

void GifSearch::setUserApiKey(const QString &key) {
    SecretStore::writeScrubbingLegacy(QString::fromUtf8(kSecretKey), key.trimmed());
}

QString GifSearch::apiKeyUrl() {
    return QStringLiteral("https://developers.giphy.com/dashboard/");
}

QString GifSearch::attributionText() {
    return QStringLiteral("Powered By GIPHY");
}

GifSearch::GifSearch(QObject *parent) : QObject(parent) {}

GifSearch::~GifSearch() {
    // The finished lambda is auto-disconnected (this is the receiver), but the
    // reply itself is parented to the process-wide sharedNam() and would sit
    // there holding its socket and buffered body for the rest of the session.
    cancel();
}

QString GifSearch::errorMessage(int httpStatus) {
    if (httpStatus == 429)
        return tr("GIPHY rate limit reached. Try again shortly.");
    if (httpStatus > 0)
        return tr("GIPHY request failed (HTTP %1).").arg(httpStatus);
    return tr("Could not reach GIPHY — check your connection.");
}

QString GifSearch::requestUrl(const QString &query, int limit, const QString &key) {
    const QString trimmed  = query.trimmed();
    const bool    trending = trimmed.isEmpty();
    QString       base     = QString::fromUtf8(kBase);
#if defined(MSGA_DEMO)
    if (!s_demoBase.isEmpty())
        base = s_demoBase;
#endif
    QUrl      url(base + (trending ? u"trending"_s : u"search"_s));
    QUrlQuery q;
    q.addQueryItem(QStringLiteral("api_key"), key);
    if (!trending)
        // GIPHY caps the search term at 50 characters and 400s past it.
        q.addQueryItem(QStringLiteral("q"), trimmed.left(50));
    q.addQueryItem(QStringLiteral("limit"), QString::number(qBound(1, limit, 50)));
    q.addQueryItem(QStringLiteral("rating"), QString::fromUtf8(kRating));
    // Ask for the messaging rendition set rather than all ~20 of them; the
    // picker only ever reads a preview and one send-size rendition.
    q.addQueryItem(QStringLiteral("bundle"), QStringLiteral("messaging_non_clips"));
    url.setQuery(q);
    return url.toString();
}

void GifSearch::cancel() {
    if (!_inflight)
        return;
    QNetworkReply *reply = _inflight;
    _inflight            = nullptr;
    reply->disconnect(this);
    reply->abort();
    reply->deleteLater();
}

void GifSearch::clearCache() {
    _cache.clear();
}

void GifSearch::search(const QString &query, int limit) {
    cancel();

    const QString cacheKey = query.trimmed().toCaseFolded();
    if (const auto it = _cache.constFind(cacheKey); it != _cache.constEnd()) {
        // Re-emit asynchronously so a cached answer and a fetched one reach the
        // caller the same way — never before its own search() call returns.
        const QList<GifResult> hit = it.value();
        QTimer::singleShot(0, this, [this, query, hit] { emit results(query, hit); });
        return;
    }

    const QString key = apiKey();
    if (key.isEmpty()) {
        emit failed(query, tr("No GIPHY API key configured."), true);
        return;
    }

    QNetworkRequest req{QUrl(requestUrl(query, limit, key))};
    req.setTransferTimeout(15000);
    req.setAttribute(
        QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy
    );

    _inflight = sharedNam()->get(req);
    connect(_inflight, &QNetworkReply::finished, this, [this, query] {
        QNetworkReply *reply = _inflight;
        if (!reply)
            return;
        _inflight = nullptr;
        onReply(reply, query);
        reply->deleteLater();
    });
}

void GifSearch::onReply(QNetworkReply *reply, const QString &query) {
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (reply->error() != QNetworkReply::NoError) {
        // NEVER pass reply->errorString() on. Qt formats it as "Error
        // transferring <url> - server replied: …", and our url carries the
        // user's api_key in its query string — the picker renders whatever
        // arrives here directly in its error state, so the raw string would put
        // the secret on screen (and into any screenshot of "GIFs are broken").
        if (status == 401 || status == 403)
            emit failed(query, tr("GIPHY rejected this API key."), true);
        else
            emit failed(query, errorMessage(status), false);
        return;
    }
    const QList<GifResult> parsed = parseResponse(reply->readAll());
    _cache.insert(query.trimmed().toCaseFolded(), parsed);
    emit results(query, parsed);
}

// Read one rendition out of a GIPHY images object. Dimensions come back as
// strings ("480"), not numbers, so they are converted rather than read as ints.
static bool
readRendition(const QJsonObject &images, const char *name, QString *url, QSize *dims = nullptr) {
    const QJsonObject r = images.value(QLatin1String(name)).toObject();
    const QString     u = r.value(QLatin1String("url")).toString();
    if (u.isEmpty())
        return false;
    *url = u;
    if (dims) {
        const int w = r.value(QLatin1String("width")).toString().toInt();
        const int h = r.value(QLatin1String("height")).toString().toInt();
        if (w > 0 && h > 0)
            *dims = QSize(w, h);
    }
    return true;
}

QList<GifResult> GifSearch::parseResponse(const QByteArray &body) {
    QList<GifResult>  out;
    const QJsonObject root = QJsonDocument::fromJson(body).object();
    const QJsonArray  hits = root.value(QLatin1String("data")).toArray();
    out.reserve(hits.size());

    for (const QJsonValue &v : hits) {
        const QJsonObject obj    = v.toObject();
        const QJsonObject images = obj.value(QLatin1String("images")).toObject();

        GifResult r;
        // Preview: fixed-width renditions all come back 200px wide, which is
        // about the picker's column width. Coverage is uneven per GIF, so this
        // walks from lightest to heaviest and skips the entry if none exist.
        if (!readRendition(images, "fixed_width_downsampled", &r.previewUrl, &r.previewSize) &&
            !readRendition(images, "fixed_width_small", &r.previewUrl, &r.previewSize) &&
            !readRendition(images, "fixed_width", &r.previewUrl, &r.previewSize) &&
            !readRendition(images, "preview_gif", &r.previewUrl, &r.previewSize))
            continue;
        // Send size: the 200px-wide `fixed_width`, the rendition Slack's own GIF
        // picker posts (its "…/200w.gif"), so a GIF from here looks the same
        // in every client. Then "downsized" (capped at 2 MB) and
        // "downsized_medium" (5 MB). `original` is deliberately not in this
        // chain — it carries no `url` field at all, only mp4/webp links.
        if (!readRendition(images, "fixed_width", &r.postUrl) &&
            !readRendition(images, "downsized", &r.postUrl) &&
            !readRendition(images, "downsized_medium", &r.postUrl))
            r.postUrl = r.previewUrl;

        r.description = obj.value(QLatin1String("alt_text")).toString();
        if (r.description.isEmpty())
            r.description = obj.value(QLatin1String("title")).toString();
        out.append(r);
    }
    return out;
}

} // namespace net
