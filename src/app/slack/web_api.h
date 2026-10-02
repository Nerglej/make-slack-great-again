// A Slack Web API call: POST https://slack.com/api/<method> with a form
// body, the token as a Bearer header and, for session workspaces, the `d`
// cookie. Write methods must be POSTs anyway (a retransmitted GET was the
// old app's duplicate-message bug), so every call is one.
#pragma once

#include "base/json.h"
#include "net/net.h"

#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace slack {

inline constexpr const char *kApiBase = "https://slack.com/api/";

struct Auth {
    std::string token;  // xoxp- / xoxc-
    std::string cookie; // the d cookie value (session auth); "" for OAuth
    // "https://team.slack.com/api/" for session workspaces (a Grid session
    // token only resolves on its own workspace host); "" = kApiBase.
    std::string base;
};

// Where calls go: MSGA_SLACK_API_BASE (tests point it at a local fake;
// one process may run several), else auth.base, else kApiBase.
std::string apiBase(const Auth &auth);

// `error` is "" when Slack answered {"ok":true}; else Slack's error code
// ("invalid_auth", "ratelimited" …) or the transport's reason ("dns",
// "timeout" …, see net::Response::error), or "bad_json". The document is
// the parsed answer either way (empty on transport failures). An HTTP 429
// answers {"ok":false,"error":"ratelimited","retry_after":<Retry-After secs>}.
using ApiDone = std::function<void(const json::Document &doc, const std::string &error)>;

// The one classification of an ApiDone error both the read queue and the
// write actions use. A transport failure: no answer at all (net's "dns",
// "connect", "tls", "timeout", "protocol", "too_many_redirects", with or
// without ": detail") or one that isn't Slack's ("bad_json": a proxy's 5xx
// or HTML gateway page). The request may or may not have reached Slack.
bool isTransportError(const std::string &error);
// Slack's own "likely a transient issue on our end" codes.
bool isTransientSlackError(const std::string &error);
// msga's retry backoff: 1 s, 2 s, 4 s … capped at 60 s, for retry `attempt`
// (0-based).
int  retryBackoffMs(int attempt);

net::RequestId apiCall(
    net::Client &client, const Auth &auth, std::string_view method, std::string form, ApiDone done
);

// The headers every authenticated request to Slack carries (files, images).
void addAuthHeaders(std::vector<net::Header> &headers, const Auth &auth);

// The workspace a Slack file URL belongs to: the team id its path starts
// with (files.slack.com/files-pri/T0123-F0456/…, files-tmb/T0123-F0456-…/…);
// "" for any other URL.
std::string fileTeamId(std::string_view url);

// The credentials a download of `url` carries (msga fetched through each
// session's own downloadFile): none off Slack's own https hosts (its CDNs,
// *.slack-edge.com, are public), else the file's own workspace when it is
// one of `signedIn` (fileTeamId), else `onScreen` (may be null).
struct TeamAuth {
    std::string_view teamId;
    const Auth      *auth = nullptr;
};
const Auth *
downloadAuth(std::string_view url, const std::vector<TeamAuth> &signedIn, const Auth *onScreen);

} // namespace slack
