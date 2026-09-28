// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#include "llm_service.h"

#include "llm_token_store.h"
#include "util/time_format.h"

#include <QSettings>

using namespace Qt::StringLiterals;

namespace {

constexpr const char *kDefaultKey   = "llm/defaultProvider";
constexpr const char *kCustomIdsKey = "llm/customProviders";

QString cfgKey(const QString &id, const char *field) {
    return QStringLiteral("llm/providers/%1/%2").arg(id, QLatin1String(field));
}

} // namespace

LlmService &LlmService::instance() {
    static LlmService service;
    return service;
}

LlmService::LlmService() {
    QSettings s(u"msga"_s, u"msga"_s);

    for (auto preset : {LlmProviderConfig::anthropicPreset(), LlmProviderConfig::openAiPreset()}) {
        LlmTokenStore::scrubLegacyOAuth(preset.id);
        preset.model    = s.value(cfgKey(preset.id, "model")).toString();
        preset.sttModel = s.value(cfgKey(preset.id, "sttModel")).toString();
        registerProvider(preset);
    }
    for (const QString &id : s.value(kCustomIdsKey).toStringList()) {
        LlmProviderConfig c = LlmProviderConfig::newCustom(); // for the STT default
        c.id                = id;
        c.name              = s.value(cfgKey(id, "name")).toString();
        c.baseUrl           = s.value(cfgKey(id, "baseUrl")).toString();
        c.model             = s.value(cfgKey(id, "model")).toString();
        c.sttModel          = s.value(cfgKey(id, "sttModel")).toString();
        if (c.baseUrl.isEmpty())
            continue; // half-written entry — skip rather than show a broken row
        registerProvider(c);
    }
}

LlmProvider *LlmService::registerProvider(const LlmProviderConfig &cfg) {
    auto *p = new LlmProvider(cfg, this);
    _providers << p;
    connect(p, &LlmProvider::authStateChanged, this, &LlmService::availabilityChanged);
    connect(p, &LlmProvider::configChanged, this, &LlmService::providersChanged);
    return p;
}

void LlmService::persistConfig(const LlmProvider *p) const {
    QSettings   s(u"msga"_s, u"msga"_s);
    const auto &c = p->config();
    if (c.sttModel.isEmpty())
        s.remove(cfgKey(c.id, "sttModel"));
    else
        s.setValue(cfgKey(c.id, "sttModel"), c.sttModel);
    if (c.isPreset) {
        if (c.model.isEmpty())
            s.remove(cfgKey(c.id, "model"));
        else
            s.setValue(cfgKey(c.id, "model"), c.model);
        return;
    }
    s.setValue(cfgKey(c.id, "name"), c.name);
    s.setValue(cfgKey(c.id, "baseUrl"), c.baseUrl);
    s.setValue(cfgKey(c.id, "model"), c.model);
    QStringList ids = s.value(kCustomIdsKey).toStringList();
    if (!ids.contains(c.id)) {
        ids << c.id;
        s.setValue(kCustomIdsKey, ids);
    }
}

LlmProvider *LlmService::provider(const QString &id) const {
    for (auto *p : _providers)
        if (p->id() == id)
            return p;
    return nullptr;
}

LlmProvider *LlmService::addCustom(const LlmProviderConfig &cfg, const QString &apiKey) {
    LlmProviderConfig c = cfg;
    c.isPreset          = false;
    c.wire              = LlmWire::Format::OpenAiChat;
    if (c.id.isEmpty())
        c.id = LlmProviderConfig::newCustom().id;
    if (c.defaultSttModel.isEmpty())
        c.defaultSttModel = LlmProviderConfig::newCustom().defaultSttModel;
    auto *p = registerProvider(c);
    persistConfig(p);
    p->setApiKey(apiKey);
    emit providersChanged();
    emit availabilityChanged();
    return p;
}

void LlmService::removeCustom(const QString &id) {
    auto *p = provider(id);
    if (!p || p->isPreset())
        return;
    _providers.removeOne(p);
    QSettings s(u"msga"_s, u"msga"_s);
    s.remove(QStringLiteral("llm/providers/%1").arg(id));
    QStringList ids = s.value(kCustomIdsKey).toStringList();
    ids.removeAll(id);
    s.setValue(kCustomIdsKey, ids);
    LlmTokenStore::clear(id);
    if (defaultProviderId() == id)
        s.remove(kDefaultKey);
    p->deleteLater();
    emit providersChanged();
    emit availabilityChanged();
}

void LlmService::updateProvider(
    const QString &id, const LlmProviderConfig &cfg, const QString &apiKey
) {
    auto *p = provider(id);
    if (!p)
        return;
    p->applyConfig(cfg);
    persistConfig(p);
    if (!apiKey.trimmed().isEmpty())
        p->setApiKey(apiKey);
    // A rename / URL change matters to the UI even when the key didn't move.
    emit providersChanged();
    emit availabilityChanged();
}

void LlmService::disconnectProvider(const QString &id) {
    auto *p = provider(id);
    if (!p)
        return;
    if (!p->isPreset()) {
        removeCustom(id);
        return;
    }
    p->setApiKey({});
}

QString LlmService::defaultProviderId() const {
    return QSettings(u"msga"_s, u"msga"_s).value(kDefaultKey).toString();
}

void LlmService::setDefaultProviderId(const QString &id) {
    if (defaultProviderId() == id)
        return;
    QSettings(u"msga"_s, u"msga"_s).setValue(kDefaultKey, id);
    emit availabilityChanged();
}

QString LlmService::nativeLanguage() const {
    const QString stored = QSettings(u"msga"_s, u"msga"_s).value("llm/nativeLanguage").toString();
    if (!stored.isEmpty())
        return stored;
    // Never set → follow the UI language ("system" resolves to the OS locale).
    return QLocale::languageToCode(TimeFmt::locale().language());
}

void LlmService::setNativeLanguage(const QString &code) {
    QSettings(u"msga"_s, u"msga"_s).setValue("llm/nativeLanguage", code);
}

LlmProvider *LlmService::activeProvider() const {
    auto *preferred = provider(defaultProviderId());
    if (preferred && preferred->isConnected())
        return preferred;
    for (auto *p : _providers)
        if (p->isConnected())
            return p;
    return nullptr;
}

LlmProvider *LlmService::sttProvider() const {
    auto *active = activeProvider();
    if (active && active->supportsTranscription())
        return active;
    for (auto *p : _providers)
        if (p->isConnected() && p->supportsTranscription())
            return p;
    return nullptr;
}

void LlmService::chat(const Llm::Request &req, Llm::OnResponse onResponse, Llm::OnError onError) {
    auto *p = activeProvider();
    if (!p) {
        if (onError)
            onError(tr("No AI provider connected — connect one in Settings → AI assistance"));
        return;
    }
    p->chat(req, std::move(onResponse), std::move(onError));
}

void LlmService::transcribe(
    LlmWire::TranscriptionInput in, Llm::OnText onText, Llm::OnError onError
) {
    auto *p = sttProvider();
    if (!p) {
        if (onError) {
            if (const auto *active = activeProvider())
                onError(tr("%1 does not support speech-to-text — connect an OpenAI-compatible "
                           "provider in Settings → AI assistance")
                            .arg(active->displayName()));
            else
                onError(tr("No AI provider connected — connect one in Settings → AI assistance"));
        }
        return;
    }
    p->transcribe(std::move(in), std::move(onText), std::move(onError));
}
