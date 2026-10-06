#include "app/slack/web_api.h"

#include <algorithm>

#include "base/str.h"
#include "plat/plat.h"

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

void retireSocket(plat::App &app, std::unique_ptr<net::WebSocket> &sock) {
    if (!sock)
        return;
    sock->onOpen   = nullptr;
    sock->onText   = nullptr;
    sock->onClosed = nullptr;
    std::shared_ptr<net::WebSocket> dead(sock.release());
    app.post([dead] {});
}

int64_t testSpeedup() {
    const char *v = std::getenv("MSGA_SLACK_TEST_SPEEDUP");
    return v && std::atoi(v) > 1 ? std::atoi(v) : 1;
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

namespace {

// The answer parsed on the net worker (a users.list page or a history
// page is hundreds of KB of JSON); the UI thread gets the Document ready.
class ApiAnswer final : public net::Handler {
public:
    void finished(net::Response &r) override { error = parseApiResponse(std::move(r), &doc); }
    json::Document doc;
    std::string    error;
};

} // namespace

net::RequestId apiCall(
    net::Client &client, const Auth &auth, std::string_view method, std::string form, ApiDone done
) {
    net::Request req;
    req.method = "POST";
    req.url    = apiBase(auth).append(method);
    req.body   = std::move(form);
    req.headers.push_back({"Content-Type", "application/x-www-form-urlencoded; charset=utf-8"});
    addAuthHeaders(req.headers, auth);
    auto answer = std::make_shared<ApiAnswer>();
    req.handler = answer;
    return client.send(std::move(req), [answer, done = std::move(done)](net::Response) {
        done(answer->doc, answer->error);
    });
}

std::string parseApiResponse(net::Response r, json::Document *doc) {
    if (!r.error.empty())
        return r.error;
    // Throttled: callers wait out Retry-After (Slack throttles per method),
    // which only the header carries — fold it into the answer.
    if (r.status == 429) {
        const int64_t secs = std::atoll(std::string(r.header("Retry-After")).c_str());
        doc->parse(
            str::concat(
                {R"({"ok":false,"error":"ratelimited","retry_after":)",
                 str::number(secs > 0 ? secs : 1),
                 "}"}
            )
        );
        return "ratelimited";
    }
    // Slack answers errors with JSON too.
    if (!doc->parse(std::move(r.body), nullptr))
        return "bad_json";
    const json::Value root = doc->root();
    if (root["ok"].boolean())
        return {};
    std::string err(root["error"].str());
    return err.empty() ? "unknown_error" : err;
}

bool isTransportError(const std::string &e) {
    static const char *const kReasons[] = {
        "dns",
        "connect",
        "tls",
        "timeout",
        "protocol",
        "too_many_redirects",
        "bad_json",
    };
    const std::string_view head = std::string_view(e).substr(0, e.find(':'));
    for (const char *r : kReasons)
        if (head == r)
            return true;
    return false;
}

bool isTransientSlackError(const std::string &e) {
    return e == "internal_error" || e == "service_unavailable" || e == "fatal_error";
}

bool isMethodUnavailable(const std::string &e) {
    static const char *const kCodes[] = {
        "unknown_method",
        "method_deprecated",
        "method_not_supported_for_channel_type",
        "not_allowed_token_type",
        "missing_scope",
        "no_permission",
        "invalid_arguments",
        "org_login_required",
        "enterprise_is_restricted",
        "user_is_restricted",
        "ekm_access_denied",
    };
    for (const char *c : kCodes)
        if (e == c)
            return true;
    return false;
}

bool isAuthError(const std::string &e) {
    return e == "invalid_auth" || e == "not_authed" || e == "token_revoked" ||
           e == "token_expired" || e == "account_inactive";
}

int retryBackoffMs(int attempt) {
    return std::min(1000 << std::min(std::max(attempt, 0), 6), 60000);
}

} // namespace slack
