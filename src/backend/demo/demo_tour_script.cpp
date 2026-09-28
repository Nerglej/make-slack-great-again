// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#include "demo_tour_script.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

using namespace Qt::StringLiterals;

namespace demo {

QString tourPathFromArgs(int argc, char **argv) {
    for (int i = 1; i < argc; ++i) {
        const QString arg = QString::fromLocal8Bit(argv[i]);
        if (arg == QLatin1String("--demo-tour") && i + 1 < argc)
            return QString::fromLocal8Bit(argv[i + 1]);
        if (arg.startsWith(QLatin1String("--demo-tour=")))
            return arg.mid(12);
    }
    return {};
}

std::optional<TourScript> loadTour(const QString &path, QString *error) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        if (error)
            *error = QStringLiteral("cannot open %1").arg(path);
        return std::nullopt;
    }
    return parseTour(f.readAll(), error);
}

namespace {

using K = TourStep::Kind;

struct Verb {
    const char *name;
    K           kind;
    enum Arg { None, Text, Number, Pair, Special, Post } arg;
};

// The verb is whichever known key the step object carries — QJsonObject
// iterates alphabetically, so "first key" would pick {"cps": …} over "type".
constexpr Verb kVerbs[] = {
    {"wait", K::Wait, Verb::Number},
    {"open", K::Open, Verb::Text},
    {"scroll", K::Scroll, Verb::Number},
    {"hover", K::Hover, Verb::Text},
    {"thread", K::Thread, Verb::Text},
    {"closeThread", K::CloseThread, Verb::None},
    {"type", K::Type, Verb::Text},
    {"key", K::Key, Verb::Text},
    {"send", K::Send, Verb::None},
    {"react", K::React, Verb::Pair},
    {"search", K::Search, Verb::Text},
    {"closeSearch", K::CloseSearch, Verb::None},
    {"quickSwitch", K::QuickSwitch, Verb::Text},
    {"theme", K::Theme, Verb::Text},
    {"settings", K::Settings, Verb::Special},
    {"messageMenu", K::MessageMenu, Verb::Text},
    {"channelMenu", K::ChannelMenu, Verb::Text},
    {"menuHover", K::MenuHover, Verb::Text},
    {"menuPick", K::MenuPick, Verb::Text},
    {"closeMenu", K::CloseMenu, Verb::None},
    {"moveToThread", K::MoveToThread, Verb::Pair},
    {"dialogButton", K::DialogButton, Verb::Text},
    {"closeDialog", K::CloseDialog, Verb::None},
    {"gif", K::Gif, Verb::Text},
    {"play", K::Play, Verb::Text},
    {"openImage", K::OpenImage, Verb::Text},
    {"closeImage", K::CloseImage, Verb::None},
    {"openThreads", K::OpenThreads, Verb::None},
    {"openSaved", K::OpenSaved, Verb::None},
    {"canvas", K::Canvas, Verb::None},
    {"messagesTab", K::MessagesTab, Verb::None},
    {"post", K::Post, Verb::Post},
    {"quit", K::Quit, Verb::None},
};

const QStringList kSettingsPages = {
    u"appearance"_s, u"notifications"_s, u"ai"_s, u"storage"_s, u"system"_s, u"about"_s
};
const QStringList kKeys = {u"Return"_s, u"Tab"_s, u"Escape"_s, u"Down"_s, u"Up"_s, u"Backspace"_s};

} // namespace

std::optional<TourScript> parseTour(const QByteArray &json, QString *error) {
    auto fail = [error](const QString &why) -> std::optional<TourScript> {
        if (error)
            *error = why;
        return std::nullopt;
    };
    QJsonParseError perr;
    const auto      doc = QJsonDocument::fromJson(json, &perr);
    if (!doc.isObject())
        return fail(QStringLiteral("tour: %1").arg(perr.errorString()));
    const auto root = doc.object();

    TourScript script;
    if (const auto win = root.value(u"window"_s).toArray(); win.size() == 2)
        script.window = QSize(win[0].toInt(), win[1].toInt());
    if (script.window.width() < 800 || script.window.height() < 600)
        return fail(QStringLiteral("tour: window must be at least 800x600"));
    script.pauseMs = root.value(u"pause"_s).toInt(script.pauseMs);

    int n = 0;
    for (const auto &sv : root.value(u"steps"_s).toArray()) {
        ++n;
        const auto  o    = sv.toObject();
        const Verb *verb = nullptr;
        for (const auto &v : kVerbs)
            if (o.contains(QLatin1String(v.name))) {
                verb = &v;
                break;
            }
        if (!verb)
            return fail(QStringLiteral("tour: step %1: unknown verb \"%2\"")
                            .arg(n)
                            .arg(o.isEmpty() ? QString() : o.begin().key()));
        TourStep st;
        st.kind        = verb->kind;
        const auto val = o.value(QLatin1String(verb->name));
        switch (verb->arg) {
        case Verb::None:
            break;
        case Verb::Text:
            st.arg = val.toString();
            if (st.arg.isEmpty())
                return fail(
                    QStringLiteral("tour: step %1: \"%2\" needs a value").arg(n).arg(verb->name)
                );
            break;
        case Verb::Number:
            st.num = val.toDouble();
            st.ms  = val.toInt();
            break;
        case Verb::Pair: {
            const auto a = val.toArray();
            if (a.size() != 2 || a[0].toString().isEmpty() || a[1].toString().isEmpty())
                return fail(
                    QStringLiteral("tour: step %1: %2 needs [text, text]").arg(n).arg(verb->name)
                );
            st.arg  = a[0].toString();
            st.arg2 = a[1].toString();
            break;
        }
        case Verb::Special: // settings: N (hold on Appearance) | {"pages": [...], "each": ms}
            if (val.isObject()) {
                for (const auto &p : val.toObject().value(u"pages"_s).toArray())
                    st.list << p.toString();
                st.ms = val.toObject().value(u"each"_s).toInt(1600);
            } else {
                st.ms = val.toInt(2500);
            }
            if (st.list.isEmpty())
                st.list << u"appearance"_s;
            for (const auto &p : st.list)
                if (!kSettingsPages.contains(p))
                    return fail(
                        QStringLiteral("tour: step %1: unknown settings page \"%2\"").arg(n).arg(p)
                    );
            break;
        case Verb::Post: { // post: {"conv": id, "user": id, "text": mrkdwn, "thread": fragment}
            const auto o2 = val.toObject();
            st.conv       = o2.value(u"conv"_s).toString();
            st.user       = o2.value(u"user"_s).toString();
            st.arg        = o2.value(u"text"_s).toString();
            st.arg2       = o2.value(u"thread"_s).toString();
            if (st.conv.isEmpty() || st.user.isEmpty() || st.arg.isEmpty())
                return fail(QStringLiteral("tour: step %1: post needs conv, user and text").arg(n));
            break;
        }
        }
        if (st.kind == K::Type)
            st.num = o.value(u"cps"_s).toDouble(16);
        if (st.kind == K::Theme && st.arg != u"light"_s && st.arg != u"dark"_s)
            return fail(QStringLiteral("tour: step %1: theme is light|dark").arg(n));
        if (st.kind == K::Key && !kKeys.contains(st.arg))
            return fail(
                QStringLiteral("tour: step %1: key is one of %2").arg(n).arg(kKeys.join(u", "_s))
            );
        script.steps.push_back(std::move(st));
    }
    if (script.steps.empty())
        return fail(QStringLiteral("tour: no steps"));
    return script;
}

} // namespace demo
