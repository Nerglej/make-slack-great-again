// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#include "oauth_loopback.h"

#include "network/form_urlencode.h"
#include "network/oauth_pkce.h"

#include <QTcpServer>
#include <QTcpSocket>
#include <QDesktopServices>
#include <QUrl>
#include <QUrlQuery>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QJsonDocument>
#include <QCoreApplication>

using namespace Qt::StringLiterals;

OAuthLoopbackFlow::OAuthLoopbackFlow(OAuthConfig cfg, QObject *parent)
    : QObject(parent), _cfg(std::move(cfg)) {}

QString OAuthLoopbackFlow::redirectUri() const {
    return QStringLiteral("http://localhost:%1%2").arg(_cfg.port).arg(_cfg.callbackPath);
}

void OAuthLoopbackFlow::start() {
    _finished = false;

    const auto pkce = net::oauth::makePkce();
    _codeVerifier   = pkce.verifier;
    _state          = pkce.state;

    _server = new QTcpServer(this);
    if (!_server->listen(QHostAddress::LocalHost, _cfg.port)) {
        emit failed(
            tr("Could not listen on port %1: %2").arg(_cfg.port).arg(_server->errorString())
        );
        _server->deleteLater();
        _server = nullptr;
        return;
    }
    connect(_server, &QTcpServer::newConnection, this, &OAuthLoopbackFlow::onNewConnection);

    QUrl      url(_cfg.authorizeUrl);
    QUrlQuery q;
    q.addQueryItem(u"response_type"_s, u"code"_s);
    q.addQueryItem(u"client_id"_s, _cfg.clientId);
    q.addQueryItem(u"redirect_uri"_s, redirectUri());
    q.addQueryItem(u"scope"_s, _cfg.scopes);
    q.addQueryItem(u"state"_s, _state);
    q.addQueryItem(u"code_challenge"_s, QString::fromLatin1(pkce.challenge));
    q.addQueryItem(u"code_challenge_method"_s, u"S256"_s);
    for (const auto &[k, v] : _cfg.extraAuthParams)
        q.addQueryItem(k, v);
    url.setQuery(q);

    QDesktopServices::openUrl(url);
}

void OAuthLoopbackFlow::cancel() {
    _finished = true;
    if (_server) {
        _server->close();
        _server->deleteLater();
        _server = nullptr;
    }
}

void OAuthLoopbackFlow::onNewConnection() {
    auto *sock = _server->nextPendingConnection();
    if (!sock)
        return;

    connect(sock, &QTcpSocket::readyRead, this, [this, sock] {
        // Only the request line matters: "GET /callback?code=…&state=… HTTP/1.1"
        if (!sock->canReadLine())
            return;
        const QByteArray        line  = sock->readLine();
        const QList<QByteArray> parts = line.split(' ');
        const QString target = parts.size() >= 2 ? QString::fromLatin1(parts[1]) : QString();
        const QUrl    url(u"http://localhost"_s + target);

        const QString    page = tr("You can close this window and return to msga.");
        const QByteArray body =
            ("<html><body style=\"font-family:sans-serif;padding:40px\">" + page.toUtf8() +
             "</body></html>");
        sock->write(
            "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\nContent-Length: " +
            QByteArray::number(body.size()) + "\r\nConnection: close\r\n\r\n" + body
        );
        sock->flush();
        sock->disconnectFromHost();

        if (_finished || url.path() != _cfg.callbackPath)
            return;
        _finished = true;
        _server->close();

        const auto cb = net::oauth::parseCallback(QUrlQuery(url.query()), _state);
        if (!cb.error.isEmpty()) {
            emit failed(cb.error);
            return;
        }
        exchangeCode(cb.code);
    });
    connect(sock, &QTcpSocket::disconnected, sock, &QObject::deleteLater);
}

void OAuthLoopbackFlow::exchangeCode(const QString &code) {
    postTokenRequest({
        {u"grant_type"_s, u"authorization_code"_s},
        {u"code"_s, code},
        {u"redirect_uri"_s, redirectUri()},
        {u"client_id"_s, _cfg.clientId},
        {u"code_verifier"_s, _codeVerifier},
        {u"state"_s, _state},
    });
}

void OAuthLoopbackFlow::refresh(const QString &refreshToken) {
    postTokenRequest({
        {u"grant_type"_s, u"refresh_token"_s},
        {u"refresh_token"_s, refreshToken},
        {u"client_id"_s, _cfg.clientId},
    });
}

void OAuthLoopbackFlow::postTokenRequest(const QList<QPair<QString, QString>> &paramsIn) {
    QList<QPair<QString, QString>> params = paramsIn;
    if (!_cfg.clientSecret.isEmpty()) // Google "Desktop app" clients require it
        params.append({QStringLiteral("client_secret"), _cfg.clientSecret});

    QNetworkRequest req((QUrl(_cfg.tokenUrl)));

    QByteArray payload;
    if (_cfg.jsonTokenRequest) {
        QJsonObject obj;
        for (const auto &[k, v] : params)
            obj[k] = v;
        payload = QJsonDocument(obj).toJson(QJsonDocument::Compact);
        req.setHeader(QNetworkRequest::ContentTypeHeader, u"application/json"_s);
    } else {
        QUrlQuery q;
        for (const auto &[k, v] : params)
            q.addQueryItem(k, v);
        payload = net::formUrlEncode(q);
        req.setHeader(QNetworkRequest::ContentTypeHeader, u"application/x-www-form-urlencoded"_s);
    }

    net::oauth::post(this, req, payload, [this](QNetworkReply *reply) {
        const QJsonObject obj = QJsonDocument::fromJson(reply->readAll()).object();
        if (reply->error() != QNetworkReply::NoError) {
            const QString detail =
                obj.value(u"error_description"_s)
                    .toString(obj.value(u"error"_s).toString(reply->errorString()));
            emit failed(detail);
            return;
        }
        if (!obj.contains(u"access_token"_s)) {
            emit failed(obj.value(u"error"_s).toString(u"no_access_token"_s));
            return;
        }
        emit done(obj);
    });
}
