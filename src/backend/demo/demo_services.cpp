// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#include "demo_services.h"

#include <QDir>
#include <QFile>
#include <QImageReader>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTcpSocket>
#include <QUrl>
#include <QUrlQuery>

using namespace Qt::StringLiterals;

namespace demo {

namespace {
QString g_servicesBase;
} // namespace

QString servicesBaseUrl() {
    return g_servicesBase;
}
void setServicesBaseUrl(const QString &url) {
    g_servicesBase = url;
}

FakeServices::FakeServices(const Fixture &fx, QObject *parent)
    : QObject(parent), _fx(fx), _gifs(scanGifs(fx.dir)) {
    connect(&_server, &QTcpServer::newConnection, this, [this] {
        while (auto *sock = _server.nextPendingConnection()) {
            auto buffer = std::make_shared<QByteArray>();
            connect(sock, &QTcpSocket::readyRead, this, [this, sock, buffer] {
                buffer->append(sock->readAll());
                const int headerEnd = buffer->indexOf("\r\n\r\n");
                if (headerEnd < 0)
                    return;
                // Wait for the whole body before routing (POSTs carry JSON).
                int contentLength = 0;
                for (const auto &line : buffer->left(headerEnd).split('\n'))
                    if (line.toLower().startsWith("content-length:"))
                        contentLength = line.mid(15).trimmed().toInt();
                if (buffer->size() < headerEnd + 4 + contentLength)
                    return;
                handle(sock, *buffer);
                buffer->clear();
            });
            connect(sock, &QTcpSocket::disconnected, sock, &QObject::deleteLater);
        }
    });
}

bool FakeServices::start(QString *error) {
    if (!_server.listen(QHostAddress::LocalHost)) {
        if (error)
            *error = QStringLiteral("demo services: %1").arg(_server.errorString());
        return false;
    }
    _baseUrl = QStringLiteral("http://127.0.0.1:%1").arg(_server.serverPort());
    setServicesBaseUrl(_baseUrl);
    return true;
}

std::vector<FakeServices::GifAsset> FakeServices::scanGifs(const QString &fixtureDir) {
    std::vector<GifAsset> out;
    QDir                  dir(QDir(fixtureDir).filePath(QStringLiteral("assets/gifs")));
    for (const auto &name : dir.entryList({QStringLiteral("*.gif")}, QDir::Files, QDir::Name)) {
        GifAsset g;
        g.file  = name;
        g.title = QFileInfo(name).completeBaseName().replace(QLatin1Char('-'), QLatin1Char(' '));
        if (!g.title.isEmpty())
            g.title[0] = g.title[0].toUpper();
        const QSize sz = QImageReader(dir.filePath(name)).size();
        g.width        = sz.width();
        g.height       = sz.height();
        out.push_back(std::move(g));
    }
    return out;
}

QByteArray FakeServices::giphyJson(const QString &baseUrl, const std::vector<GifAsset> &gifs) {
    // The renditions GifSearch::parseResponse reads: a fixed_width preview and a
    // downsized send rendition. Both point at the same file here.
    QJsonArray data;
    int        n = 0;
    for (const auto &g : gifs) {
        const QString url = baseUrl + QStringLiteral("/gif/") + g.file;
        QJsonObject   rendition{
            {u"url"_s, url},
            {u"width"_s, QString::number(g.width)},
            {u"height"_s, QString::number(g.height)},
        };
        data.append(
            QJsonObject{
                {u"id"_s, QStringLiteral("demo%1").arg(++n)},
                {u"title"_s, g.title},
                {u"alt_text"_s, g.title},
                {u"images"_s,
                 QJsonObject{{u"fixed_width"_s, rendition}, {u"downsized"_s, rendition}}},
            }
        );
    }
    return QJsonDocument(
               QJsonObject{{u"data"_s, data}, {u"meta"_s, QJsonObject{{u"status"_s, 200}}}}
    ).toJson(QJsonDocument::Compact);
}

QByteArray FakeServices::chatCompletionJson(const QString &text) {
    return QJsonDocument(
               QJsonObject{
                   {u"id"_s, u"chatcmpl-demo"_s},
                   {u"object"_s, u"chat.completion"_s},
                   {u"model"_s, u"lumen-1"_s},
                   {u"choices"_s,
                    QJsonArray{QJsonObject{
                        {u"index"_s, 0},
                        {u"finish_reason"_s, u"stop"_s},
                        {u"message"_s,
                         QJsonObject{{u"role"_s, u"assistant"_s}, {u"content"_s, text}}},
                    }}},
                   {u"usage"_s, QJsonObject{{u"prompt_tokens"_s, 0}, {u"completion_tokens"_s, 0}}},
               }
    )
        .toJson(QJsonDocument::Compact);
}

QString FakeServices::cannedReply(const QByteArray &requestBody, const Fixture &fx) {
    const QString body = QString::fromUtf8(requestBody);
    for (const auto &r : fx.aiReplies)
        if (body.contains(r.match, Qt::CaseInsensitive))
            return r.text;
    return fx.aiDefault.isEmpty() ? QStringLiteral("Nothing to add — the thread speaks for itself.")
                                  : fx.aiDefault;
}

void FakeServices::handle(QTcpSocket *sock, const QByteArray &request) {
    const int               lineEnd = request.indexOf("\r\n");
    const QByteArray        line    = request.left(lineEnd);
    const QList<QByteArray> parts   = line.split(' ');
    if (parts.size() < 2) {
        reply(sock, 400, "text/plain", "bad request");
        return;
    }
    const QByteArray method = parts[0];
    const QUrl       url(QString::fromUtf8(parts[1]));
    const QString    path   = url.path();
    const int        bodyAt = request.indexOf("\r\n\r\n") + 4;
    const QByteArray body   = request.mid(bodyAt);

    if (path.startsWith(QLatin1String("/v1/gifs/"))) {
        reply(sock, 200, "application/json", giphyJson(_baseUrl, _gifs));
    } else if (path.startsWith(QLatin1String("/gif/"))) {
        const QString name = QFileInfo(path.mid(5)).fileName(); // no traversal
        QFile         f(QDir(_fx.dir).filePath(QStringLiteral("assets/gifs/") + name));
        if (f.open(QIODevice::ReadOnly))
            reply(sock, 200, "image/gif", f.readAll());
        else
            reply(sock, 404, "text/plain", "no such gif");
    } else if (path.endsWith(QLatin1String("/models"))) {
        reply(
            sock,
            200,
            "application/json",
            R"({"object":"list","data":[{"id":"lumen-1","object":"model"}]})"
        );
    } else if (path.endsWith(QLatin1String("/chat/completions")) && method == "POST") {
        reply(sock, 200, "application/json", chatCompletionJson(cannedReply(body, _fx)));
    } else {
        reply(sock, 404, "text/plain", "not found");
    }
}

void FakeServices::reply(
    QTcpSocket *sock, int status, const QByteArray &type, const QByteArray &body
) {
    QByteArray head = "HTTP/1.1 " + QByteArray::number(status) +
                      (status == 200 ? " OK" : " Error") + "\r\nContent-Type: " + type +
                      "\r\nContent-Length: " + QByteArray::number(body.size()) +
                      "\r\nAccess-Control-Allow-Origin: *\r\nConnection: close\r\n\r\n";
    sock->write(head + body);
    sock->flush();
    sock->disconnectFromHost();
}

} // namespace demo
