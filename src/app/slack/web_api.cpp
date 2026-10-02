#include "app/slack/web_api.h"

#include "base/str.h"

#include <cstdlib>

namespace slack {

std::string apiBase(const Auth &auth) {
    // Read per call (a getenv is cheap next to a request): test suites with
    // fakes of their own point it elsewhere in turn.
    const char *override = std::getenv("MSGA_SLACK_API_BASE");
    if (override && *override)
        return override;
    return auth.base.empty() ? std::string(kApiBase) : auth.base;
}

void addAuthHeaders(std::vector<net::Header> &headers, const Auth &auth) {
    if (!auth.token.empty())
        headers.push_back({"Authorization", "Bearer " + auth.token});
    if (!auth.cookie.empty())
        headers.push_back({"Cookie", "d=" + auth.cookie});
}

std::string fileTeamId(std::string_view url) {
    net::Url u;
    if (!u.parse(url))
        return {};
    std::string_view path = u.target;
    for (std::string_view prefix : {"/files-pri/", "/files-tmb/"}) {
        if (!str::startsWith(path, prefix))
            continue;
        path.remove_prefix(prefix.size());
        const size_t dash = path.find('-');
        if (dash == std::string_view::npos || dash < 2 || path.find('/') < dash)
            return {};
        const std::string_view team = path.substr(0, dash);
        if (team[0] != 'T' && team[0] != 'E')
            return {};
        return std::string(team);
    }
    return {};
}

const Auth *
downloadAuth(std::string_view url, const std::vector<TeamAuth> &signedIn, const Auth *onScreen) {
    net::Url u;
    if (!u.parse(url) || !u.secure() ||
        (u.host != "slack.com" && !str::endsWith(u.host, ".slack.com")))
        return nullptr;
    if (const std::string team = fileTeamId(url); !team.empty())
        for (const TeamAuth &t : signedIn)
            if (t.teamId == team && t.auth)
                return t.auth;
    return onScreen;
}

net::RequestId apiCall(
    net::Client &client, const Auth &auth, std::string_view method, std::string form, ApiDone done
) {
    net::Request req;
    req.method = "POST";
    req.url    = apiBase(auth).append(method);
    req.body   = std::move(form);
    req.headers.push_back({"Content-Type", "application/x-www-form-urlencoded; charset=utf-8"});
    addAuthHeaders(req.headers, auth);
    return client.send(std::move(req), [done = std::move(done)](net::Response r) {
        json::Document doc;
        if (!r.error.empty()) {
            done(doc, r.error);
            return;
        }
        // Throttled: callers wait out Retry-After (Slack throttles per method),
        // which only the header carries — fold it into the answer.
        if (r.status == 429) {
            const int64_t secs = std::atoll(std::string(r.header("Retry-After")).c_str());
            doc.parse(
                str::concat(
                    {R"({"ok":false,"error":"ratelimited","retry_after":)",
                     str::number(secs > 0 ? secs : 1),
                     "}"}
                )
            );
            done(doc, "ratelimited");
            return;
        }
        // Slack answers errors with JSON too (and 429 with "ratelimited").
        if (!doc.parse(std::move(r.body), nullptr)) {
            done(doc, r.status == 429 ? "ratelimited" : "bad_json");
            return;
        }
        const json::Value root = doc.root();
        if (root["ok"].boolean()) {
            done(doc, std::string());
            return;
        }
        std::string err(root["error"].str());
        done(doc, err.empty() ? "unknown_error" : err);
    });
}

} // namespace slack
