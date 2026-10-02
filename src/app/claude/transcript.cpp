#include "app/claude/transcript.h"

#include "app/claude/common.h"
#include "app/claude/outputs.h" // trimmed, cleanPath
#include "app/claude/roles.h"

#include "base/crypto.h"
#include "base/file.h"
#include "base/json.h"
#include "base/str.h"
#include "base/time.h"
#include "base/utf8.h"

#include <algorithm>

namespace claude {
namespace {

// What a message of files alone says after its mentions, and is shown without.
constexpr std::string_view kFilesOnly = "(attached)";

bool isSpace(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f';
}

// QString::trimmed, as the old parser trimmed.
std::string_view trim(std::string_view s) {
    return trimmed(s);
}

bool isDigit(char c) {
    return c >= '0' && c <= '9';
}

// The first line, trimmed, at most `maxLen` code points ("…" marks a cut).
std::string oneLine(std::string_view in, size_t maxLen = 100) {
    std::string_view s = trim(in);
    std::string      out;
    if (const size_t nl = s.find('\n'); nl != std::string_view::npos)
        out = str::concat({trim(s.substr(0, nl)), " …"});
    else
        out = std::string(s);
    if (utf8::countCodePoints(out) > maxLen) {
        size_t i = 0;
        for (size_t n = 0; n + 1 < maxLen && i < out.size(); ++n)
            i = utf8::nextBoundary(out, i);
        out.resize(i);
        out += "…";
    }
    return out;
}

// The text between <tag> and </tag> (or the end), trimmed; "" when no <tag>.
std::string_view tagContent(std::string_view s, std::string_view tag) {
    const std::string open  = str::concat({"<", tag, ">"});
    const std::string close = str::concat({"</", tag, ">"});
    const size_t      a     = s.find(open);
    if (a == std::string_view::npos)
        return {};
    const size_t from = a + open.size();
    const size_t b    = s.find(close, from);
    return trim(b == std::string_view::npos ? s.substr(from) : s.substr(from, b - from));
}

// "<abc-def>" at the start: a tag Claude Code wraps its injections in.
bool startsWithTag(std::string_view s) {
    if (s.size() < 3 || s[0] != '<' || s[1] < 'a' || s[1] > 'z')
        return false;
    for (size_t i = 2; i < s.size(); ++i) {
        const char c = s[i];
        if (c == '>')
            return true;
        if (!((c >= 'a' && c <= 'z') || c == '-'))
            return false;
    }
    return false;
}

// What a typed prompt shows as — empty for the system text Claude Code records
// as "user" turns (command output, notifications, reminders, interruptions).
std::string cleanPrompt(std::string_view raw) {
    const std::string_view s = trim(raw);
    if (s.empty())
        return {};
    if (str::startsWith(s, "<command-name>")) {
        const std::string_view name = tagContent(s, "command-name");
        const std::string_view args = tagContent(s, "command-args");
        std::string out = str::startsWith(name, "/") ? std::string(name) : str::concat({"/", name});
        if (!args.empty())
            out = str::concat({out, " ", args});
        return out;
    }
    if (str::startsWith(s, "<bash-input>"))
        return str::concat({"! ", tagContent(s, "bash-input")});
    // Every other leading tag is an injection, not something the user typed:
    // <local-command-stdout>, <bash-stdout>, <task-notification>, <system-reminder>…
    if (startsWithTag(s))
        return {};
    if (str::startsWith(s, "[Request interrupted"))
        return {};
    return std::string(s);
}

// `s` without terminal colour codes (ESC [ params letter).
std::string stripAnsi(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\x1b' && i + 1 < s.size() && s[i + 1] == '[') {
            size_t j = i + 2;
            while (j < s.size() && (isDigit(s[j]) || s[j] == ';' || s[j] == '?'))
                ++j;
            if (j < s.size() && ((s[j] >= 'A' && s[j] <= 'Z') || (s[j] >= 'a' && s[j] <= 'z'))) {
                i = j;
                continue;
            }
        }
        out += s[i];
    }
    return out;
}

// A local command's output ("<local-command-stdout>…</local-command-stdout>"),
// terminal colours stripped, into *out; false when `raw` isn't command output.
bool commandOutput(std::string_view raw, std::string *out) {
    const std::string_view s = trim(raw);
    for (const std::string_view tag : {"local-command-stdout", "local-command-stderr"}) {
        if (s.size() < tag.size() + 2 || s[0] != '<' || s.substr(1, tag.size()) != tag ||
            s[tag.size() + 1] != '>')
            continue;
        *out = stripAnsi(tagContent(s, tag));
        return true;
    }
    return false;
}

bool isAgentTool(std::string_view name) {
    return name == "Agent" || name == "Task";
}

// Removes every " [Image #3]" / "[Image: source: …]" placeholder (one leading
// space or tab with it).
std::string dropImagePlaceholders(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    size_t i = 0;
    while (i < s.size()) {
        const size_t at = s.find("[Image", i);
        if (at == std::string_view::npos)
            break;
        size_t end = std::string_view::npos;
        size_t j   = at + 6;
        if (s.substr(j, 2) == " #" && j + 2 < s.size() && isDigit(s[j + 2])) {
            j += 2;
            while (j < s.size() && isDigit(s[j]))
                ++j;
            if (j < s.size() && s[j] == ']')
                end = j + 1;
        } else if (s.substr(j, 10) == ": source: ") {
            const size_t close = s.find(']', j + 10);
            if (close != std::string_view::npos)
                end = close + 1;
        }
        if (end == std::string_view::npos) {
            out.append(s.substr(i, at + 1 - i));
            i = at + 1;
            continue;
        }
        size_t from = at;
        if (from > i && (s[from - 1] == ' ' || s[from - 1] == '\t'))
            --from;
        out.append(s.substr(i, from - i));
        i = end;
    }
    out.append(s.substr(i));
    return out;
}

// Removes every "[Image #3]" with one space after it (promptHistory).
std::string dropImageMarks(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    size_t i = 0;
    while (i < s.size()) {
        const size_t at = s.find("[Image #", i);
        if (at == std::string_view::npos)
            break;
        size_t j = at + 8;
        while (j < s.size() && isDigit(s[j]))
            ++j;
        if (j == at + 8 || j >= s.size() || s[j] != ']') {
            out.append(s.substr(i, at + 1 - i));
            i = at + 1;
            continue;
        }
        out.append(s.substr(i, at - i));
        i = j + 1;
        if (i < s.size() && s[i] == ' ')
            ++i;
    }
    out.append(s.substr(i));
    return out;
}

std::string sha1Hex(std::string_view data) {
    return crypto::hex(crypto::bytes(crypto::sha1(data)));
}

} // namespace

std::string summarizeToolInput(std::string_view toolName, const json::Value &input) {
    auto        arg = [&](const char *key) { return input[key].str(); };
    std::string s;
    if (toolName == "Bash")
        s = arg("description").empty() ? arg("command") : arg("description");
    else if (
        toolName == "Read" || toolName == "Write" || toolName == "Edit" ||
        toolName == "NotebookEdit"
    ) {
        const std::string_view path =
            arg("file_path").empty() ? arg("notebook_path") : arg("file_path");
        s = path.empty() ? std::string() : homeRelative(path);
    } else if (toolName == "Grep" || toolName == "Glob")
        s = arg("pattern");
    else if (toolName == "WebFetch")
        s = arg("url");
    else if (toolName == "WebSearch")
        s = arg("query");
    else if (isAgentTool(toolName))
        s = arg("description");
    else if (toolName == "Skill")
        s = arg("skill");
    if (s.empty()) {
        // Anything else: its first string argument says the most.
        for (const json::Value v : input)
            if (v.isString() && !trim(v.str()).empty()) {
                s = v.str();
                break;
            }
    }
    return oneLine(s);
}

// ── The parser ──────────────────────────────────────────────────────────────

void TranscriptParser::feed(std::string_view bytes) {
    _partial.append(bytes);
    const std::string_view all(_partial);
    size_t                 start = 0;
    for (;;) {
        const size_t nl = all.find('\n', start);
        if (nl == std::string_view::npos)
            break;
        const std::string_view line = trim(all.substr(start, nl - start));
        if (!line.empty()) {
            handleLine(line);
            while (_revs.size() < _items.size())
                _revs.push_back(++_rev);
        }
        start = nl + 1;
    }
    _partial.erase(0, start);
}

uint64_t TranscriptParser::revision(size_t index) const {
    return index < _revs.size() ? _revs[index] : 0;
}

void TranscriptParser::touch(size_t index) {
    // An item added by this line gets its revision when the line is done.
    if (index < _revs.size())
        _revs[index] = ++_rev;
}

void TranscriptParser::reserveTs(int64_t micros) {
    _lastMicros = std::max(_lastMicros, micros);
}

int64_t TranscriptParser::taskStoppedAt(std::string_view taskId) const {
    const auto it = _taskStopped.find(std::string(taskId));
    return it == _taskStopped.end() ? 0 : it->second;
}

int64_t TranscriptParser::nextTs(int64_t micros) {
    // Strictly increasing, so two records in the same millisecond (a text block
    // and a tool call streamed together) still get distinct, ordered ids.
    if (micros <= _lastMicros)
        micros = _lastMicros + 1;
    _lastMicros = micros;
    return micros;
}

void TranscriptParser::closeToolGroup() {
    _openToolGroup = -1;
}

void TranscriptParser::resolvePendingText(TranscriptItem::State state) {
    if (_pendingText < 0)
        return;
    _items[size_t(_pendingText)].state = state;
    touch(size_t(_pendingText));
    _pendingText = -1;
}

void TranscriptParser::endTurn() {
    resolvePendingText(TranscriptItem::State::Final);
    closeToolGroup();
    _turnOpen = false;
}

void TranscriptParser::openTurn(int64_t micros) {
    if (!_turnOpen)
        _turnStartedAt = micros; // a prompt joining a turn under way doesn't restart it
    _turnOpen = true;
}

void TranscriptParser::addPrompt(
    std::string_view         text,
    int64_t                  micros,
    std::vector<std::string> images,
    std::vector<std::string> imageNames
) {
    TranscriptItem item;
    item.kind       = TranscriptItem::Kind::UserPrompt;
    item.ts         = nextTs(micros);
    item.date       = item.ts;
    item.images     = std::move(images);
    item.imageNames = std::move(imageNames);
    // Files sent from msga ride the text as mentions (see withAttachments).
    item.text       = takeAttachments(typedPrompt(text, &item.relayTo), &item.images);
    while (item.imageNames.size() < item.images.size())
        item.imageNames.emplace_back(file::baseName(item.images[item.imageNames.size()]));
    item.uuid = _lineUuid;
    _items.push_back(std::move(item));
    openTurn(micros);
}

bool TranscriptParser::addPeerMessage(const json::Value &origin, int64_t micros, bool newTurn) {
    if (!origin.isObject() || origin["kind"].str() != "peer")
        return false;
    TranscriptItem item;
    item.kind                   = TranscriptItem::Kind::PeerMessage;
    // The message as sent, without Claude Code's wrapping (<agent-message …>).
    const std::string_view body = origin["body"].str();
    std::string            text(body);
    if (origin["handback"].boolean()) {
        // "[Subagent hand-back] …(a note for the model)… The report follows:",
        // then the report with every line indented by two spaces.
        item.agentId                               = std::string(origin["from"].str());
        static constexpr std::string_view kFollows = "The report follows:\n";
        if (const size_t at = body.find(kFollows); at != std::string_view::npos) {
            text.clear();
            std::string_view rest = body.substr(at + kFollows.size());
            for (;;) {
                const size_t     nl   = rest.find('\n');
                std::string_view line = rest.substr(0, nl);
                if (str::startsWith(line, "  "))
                    line.remove_prefix(2);
                text += line;
                if (nl == std::string_view::npos)
                    break;
                text += '\n';
                rest.remove_prefix(nl + 1);
            }
        }
    } else {
        item.peerName = std::string(origin["name"].str());
    }
    item.text = std::string(trim(text));
    if (item.text.empty())
        return true; // nothing to show, but still nobody's prompt
    if (newTurn)
        resolvePendingText(TranscriptItem::State::Final);
    closeToolGroup();
    item.ts   = nextTs(micros);
    item.date = item.ts;
    _items.push_back(std::move(item));
    if (newTurn)
        openTurn(micros); // the session answers it
    return true;
}

void TranscriptParser::addCommandOutput(std::string_view output, int64_t micros) {
    closeToolGroup();
    const std::string_view text = trim(output);
    if (!text.empty()) {
        TranscriptItem item;
        item.kind = TranscriptItem::Kind::AssistantText;
        item.ts   = nextTs(micros);
        item.date = item.ts;
        // Several lines are a terminal layout (a chart, a list): kept monospaced.
        item.text = text.find('\n') != std::string_view::npos
                        ? str::concat({"```\n", text, "\n```"})
                        : std::string(text);
        item.uuid = _lineUuid;
        _items.push_back(std::move(item));
        _commandOutput = int(_items.size()) - 1;
    }
    endTurn();
}

void TranscriptParser::noteTaskStopped(std::string_view taskId, int64_t micros) {
    int64_t &at = _taskStopped[std::string(taskId)];
    at          = std::max(at, micros);
}

void TranscriptParser::noteTaskNotification(std::string_view text, int64_t micros) {
    // Only a notification itself: its <result> may quote anything.
    if (micros <= 0 || !str::startsWith(trim(text), "<task-notification>"))
        return;
    constexpr std::string_view kOpen = "<task-id>", kClose = "</task-id>";
    for (size_t at = text.find(kOpen); at != std::string_view::npos; at = text.find(kOpen, at)) {
        at += kOpen.size();
        const size_t end = text.find('<', at);
        if (end == std::string_view::npos)
            break;
        if (end > at && text.substr(end, kClose.size()) == kClose)
            noteTaskStopped(trim(text.substr(at, end - at)), micros);
        at = end;
    }
}

void TranscriptParser::handleLine(std::string_view line) {
    json::Document doc;
    if (!doc.parse(std::string(line), nullptr) || !doc.root().isObject() || doc.root().size() == 0)
        return; // a torn or foreign line — skip, never fail the whole transcript
    const json::Value      o    = doc.root();
    const std::string_view type = o["type"].str();
    _lineUuid                   = o["uuid"].str();
    // A copy of a session starts with the records it was copied from, same
    // uuids and all — the ones read already, from the session it continues.
    if (!_lineUuid.empty() && !_seenUuids.insert(_lineUuid).second)
        return;
    const int64_t micros = base::parseIsoMicros(o["timestamp"].str());
    if (micros > _lastActivity)
        _lastActivity = micros;
    if (micros > 0)
        _activity.push_back(micros);

    if (const std::string_view v = o["version"].str(); !v.empty())
        _version = v;
    if (const std::string_view m = o["permissionMode"].str(); !m.empty())
        _permissionMode = m;
    if (type == "queue-operation") {
        // A notification queued for the session, the moment its task stopped
        // (the prompt that delivers it may come much later).
        if (o["operation"].str() == "enqueue")
            noteTaskNotification(o["content"].str(), micros);
        return;
    }
    if (type == "ai-title") {
        _aiTitle = trim(o["aiTitle"].str());
        return;
    }
    if (type == "system") {
        const std::string_view subtype = o["subtype"].str();
        if (subtype == "turn_duration") {
            endTurn();
        } else if (subtype == "local_command") {
            // A command Claude Code runs itself (/context, /model…): the command,
            // then its output — which is all there is to that turn.
            const std::string_view content = o["content"].str();
            std::string            output;
            if (commandOutput(content, &output)) {
                addCommandOutput(output, micros);
            } else if (const std::string cmd = cleanPrompt(content); !cmd.empty()) {
                resolvePendingText(TranscriptItem::State::Final);
                closeToolGroup();
                addPrompt(cmd, micros);
            }
        }
        return;
    }
    if (type == "attachment") {
        // A prompt typed while Claude was busy: recorded as a queued command that
        // the running turn picks up — the turn goes on, so nothing is resolved.
        const json::Value      a     = o["attachment"];
        const std::string_view atype = a["type"].str();
        // The system prompt as sent, recorded once and sent again on resume:
        // a role msga started the session with is its last part (roles.h).
        if (atype == "prompt_snapshot") {
            RoleMark mark = roleInSystemPrompt(a["systemPrompt"]);
            _role         = std::move(mark.id);
            _roleName     = std::move(mark.name);
            return;
        }
        // The subagent types offered: the whole list first, then what changed.
        if (atype == "agent_listing_delta") {
            if (a["isInitial"].boolean())
                _agentTypes.clear();
            for (const json::Value v : a["addedTypes"])
                _agentTypes.emplace(v.str());
            for (const json::Value v : a["removedTypes"])
                _agentTypes.erase(std::string(v.str()));
            return;
        }
        if (atype == "queued_command") {
            noteTaskNotification(a["prompt"].str(), micros);
            if (a["commandMode"].str() == "prompt" && !addPeerMessage(a["origin"], micros, false)) {
                const std::string text = cleanPrompt(a["prompt"].str());
                if (!text.empty()) {
                    closeToolGroup();
                    addPrompt(text, micros);
                }
            }
        }
        return;
    }

    const json::Value message = o["message"];
    if (type == "user")
        handleUser(o, message["content"], micros);
    else if (type == "assistant")
        handleAssistant(o, message, micros);
}

void TranscriptParser::handleUser(
    const json::Value &o, const json::Value &content, int64_t micros
) {
    if (addPeerMessage(o["origin"], micros, true))
        return;
    // What Claude Code tells the model (skill bodies, caveats) and the
    // summary a compaction starts from aren't anything anyone typed.
    if (o["isMeta"].boolean()) {
        // /context also writes its report as markdown for the model, right
        // after the terminal drawing: that reads far better here.
        const std::string_view md = trim(content.str());
        if (_commandOutput >= 0 && _commandOutput == int(_items.size()) - 1 && !md.empty() &&
            md[0] != '<') {
            _items[size_t(_commandOutput)].text = md;
            touch(size_t(_commandOutput));
        }
        _commandOutput = -1;
        return;
    }
    if (o["isCompactSummary"].boolean())
        return;
    const json::Value origin = o["origin"];
    if (origin.isObject() && origin["kind"].str() != "human") {
        if (content.isString())
            noteTaskNotification(content.str(), micros);
        return; // task notifications and other machine-originated turns
    }

    std::string              text;
    std::vector<std::string> images;
    if (content.isString()) {
        text = content.str();
    } else if (content.isArray()) {
        bool sawToolResult = false;
        for (const json::Value b : content) {
            const std::string_view bt = b["type"].str();
            if (bt == "tool_result") {
                sawToolResult                  = true;
                const std::string_view id      = b["tool_use_id"].str();
                const bool             failed  = b["is_error"].boolean();
                const json::Value      result  = o["toolUseResult"];
                const std::string_view agentId = result["agentId"].str();
                // Newest first: the call is almost always in the latest item.
                for (auto it = _items.rbegin(); it != _items.rend(); ++it) {
                    auto call =
                        std::find_if(it->tools.begin(), it->tools.end(), [&](const ToolCall &c) {
                            return c.toolUseId == id;
                        });
                    if (call == it->tools.end())
                        continue;
                    call->error = failed;
                    touch(size_t(std::distance(it, _items.rend()) - 1));
                    if (it->kind == TranscriptItem::Kind::Subagent && !agentId.empty()) {
                        it->agentId = agentId;
                        // A foreground subagent's result comes when it's
                        // done ("completed"), and no notification follows:
                        // the result is its stop. A background one's
                        // ("async_launched") only says it started.
                        if (result["status"].str() != "async_launched")
                            noteTaskStopped(agentId, micros);
                    }
                    break;
                }
            } else if (bt == "text") {
                if (!text.empty())
                    text += '\n';
                text += b["text"].str();
            } else if (bt == "image") {
                // A pasted image rides the prompt as base64.
                const json::Value src = b["source"];
                if (src["type"].str() == "base64") {
                    std::string path = cachePastedImage(src["media_type"].str(), src["data"].str());
                    if (!path.empty())
                        images.push_back(std::move(path));
                }
            }
        }
        if (sawToolResult)
            return; // tool output rides a "user" record; it isn't a prompt
    }
    if (std::string output; commandOutput(text, &output)) {
        addCommandOutput(output, micros); // e.g. what /compact says when done
        return;
    }
    const bool isCommand = str::startsWith(trim(text), "<command-name>");
    text                 = cleanPrompt(text);
    // /compact records the prompt as typed, then again as the command it ran.
    if (isCommand && !_items.empty() && _items.back().kind == TranscriptItem::Kind::UserPrompt &&
        _items.back().text == text)
        return;
    if (!images.empty()) {
        // The images are attached; drop their "[Image #3]" placeholders (and
        // the "[Image: source: …]" lines some clients add) from the text.
        text = std::string(trim(dropImagePlaceholders(text)));
    }
    if (text.empty() && images.empty())
        return;
    // A new prompt starts a new turn: whatever Claude said last in the
    // previous one was its answer.
    resolvePendingText(TranscriptItem::State::Final);
    closeToolGroup();
    // Named after Claude Code's paste numbers ("[Image #3]"), in order.
    std::vector<std::string> names;
    const json::Value        ids = o["imagePasteIds"];
    for (size_t i = 0; i < images.size(); ++i) {
        const std::string_view ext = file::extension(images[i]);
        names.push_back(
            i < ids.size() ? str::concat({"Image ", str::number(ids[i].integer()), ".", ext})
                           : str::concat({"Image.", ext})
        );
    }
    addPrompt(text, micros, std::move(images), std::move(names));
}

void TranscriptParser::handleAssistant(
    const json::Value &o, const json::Value &message, int64_t micros
) {
    const json::Value content = message["content"];
    if (!content.isArray())
        return;
    const std::string_view model = message["model"].str();
    if (!model.empty() && model != "<synthetic>")
        _model = model;
    // Claude Code's stand-in answer to a turn that asked for none (after a
    // command's output) — not something Claude said.
    if (model == "<synthetic>" && content.size() == 1 &&
        trim(content[0]["text"].str()) == "No response requested.")
        return;
    // An API error Claude Code reports as the answer (verified 2.1.283):
    // {"type":"assistant","isApiErrorMessage":true,"error":"authentication_failed",…}.
    const bool loginError =
        o["isApiErrorMessage"].boolean() && o["error"].str() == "authentication_failed";
    if (loginError)
        _loginFailedAt = std::max(_loginFailedAt, micros);
    for (const json::Value b : content) {
        const std::string_view bt = b["type"].str();
        if (bt == "text") {
            const std::string_view text = trim(b["text"].str());
            if (text.empty())
                continue;
            // Two texts in a row within one turn: the earlier one wasn't the end.
            resolvePendingText(TranscriptItem::State::Progress);
            closeToolGroup();
            TranscriptItem item;
            item.kind       = TranscriptItem::Kind::AssistantText;
            item.state      = TranscriptItem::State::Pending;
            item.ts         = nextTs(micros);
            item.date       = item.ts;
            item.text       = text;
            item.uuid       = _lineUuid;
            item.loginError = loginError;
            _items.push_back(std::move(item));
            _pendingText = int(_items.size()) - 1;
            openTurn(micros);
        } else if (bt == "tool_use") {
            // Claude kept working after its text, so that text was an update.
            resolvePendingText(TranscriptItem::State::Progress);
            ToolCall call;
            call.toolUseId          = b["id"].str();
            call.name               = b["name"].str();
            const json::Value input = b["input"];
            call.summary            = summarizeToolInput(call.name, input);
            openTurn(micros);
            if (call.name == "SubagentHandback") {
                // A subagent's report to its session: its answer, in its thread.
                const std::string_view report = trim(input["message"].str());
                if (!report.empty()) {
                    closeToolGroup();
                    TranscriptItem item;
                    item.kind = TranscriptItem::Kind::AssistantText;
                    item.ts   = nextTs(micros);
                    item.date = item.ts;
                    item.text = report; // no uuid: a tool call can't go without its result
                    _items.push_back(std::move(item));
                    continue;
                }
            }
            if (isAgentTool(call.name)) {
                closeToolGroup();
                TranscriptItem item;
                item.kind      = TranscriptItem::Kind::Subagent;
                item.ts        = nextTs(micros);
                item.date      = item.ts;
                item.text      = call.summary;
                item.agentType = input["subagent_type"].str();
                item.agentRole = roleInAgentPrompt(input["prompt"].str());
                item.tools.push_back(std::move(call));
                _items.push_back(std::move(item));
                continue;
            }
            if (_openToolGroup < 0) {
                TranscriptItem item;
                item.kind = TranscriptItem::Kind::ToolGroup;
                item.ts   = nextTs(micros);
                item.date = item.ts;
                _items.push_back(std::move(item));
                _openToolGroup = int(_items.size()) - 1;
            }
            _items[size_t(_openToolGroup)].tools.push_back(std::move(call));
            touch(size_t(_openToolGroup));
        }
        // thinking, redacted_thinking, …: not shown
    }
}

// ── Images and files ────────────────────────────────────────────────────────

std::string cachePastedImage(std::string_view mediaType, std::string_view base64) {
    if (!str::startsWith(mediaType, "image/") || base64.empty())
        return {};
    std::string ext = str::asciiLower(mediaType.substr(6));
    if (ext == "jpeg")
        ext = "jpg";
    const std::string path = str::concat({dirs().cache, "/images/", sha1Hex(base64), ".", ext});
    if (file::exists(path))
        return path;
    std::string bytes;
    if (!crypto::base64Decode(base64, &bytes) || !file::writeAtomic(path, bytes))
        return {};
    return path;
}

std::string uploadsDir() {
    return dirs().cache + "/uploads";
}

std::string cacheUpload(std::string_view path) {
    std::string data;
    if (file::isDir(path) || !file::readAll(path, &data))
        return {};
    const std::string copy =
        str::concat({uploadsDir(), "/", sha1Hex(data), "/", file::baseName(path)});
    if (file::exists(copy))
        return copy;
    return file::writeAtomic(copy, data) ? copy : std::string();
}

std::string withAttachments(std::string_view text, const std::vector<std::string> &paths) {
    if (paths.empty())
        return std::string(text);
    std::string mentions;
    for (const std::string &p : paths) {
        if (!mentions.empty())
            mentions += ' ';
        const bool spaced = std::any_of(p.begin(), p.end(), isSpace);
        mentions += spaced ? str::concat({"@\"", p, "\""}) : str::concat({"@", p});
    }
    const std::string_view body = trim(text);
    // A slash command keeps its place at the start (it goes by `--resume`,
    // never typed, so the mentions may end it).
    if (str::startsWith(body, "/"))
        return str::concat({body, " ", mentions});
    return str::concat({mentions, " ", body.empty() ? kFilesOnly : body});
}

std::string takeAttachments(std::string_view prompt, std::vector<std::string> *paths) {
    // @"<uploads>/…" or @<uploads>/… (to the next whitespace), one space after.
    const std::string dir = uploadsDir() + "/";
    std::string       text;
    text.reserve(prompt.size());
    size_t i = 0;
    for (size_t at = prompt.find('@'); at != std::string_view::npos;
         at        = prompt.find('@', at + 1)) {
        if (at < i)
            continue;
        const std::string_view rest = prompt.substr(at + 1);
        std::string_view       path;
        size_t                 end = 0; // past the mention, in `rest`
        if (str::startsWith(rest, "\"") && str::startsWith(rest.substr(1), dir)) {
            const size_t close = rest.find('"', 1 + dir.size());
            if (close != std::string_view::npos && close > 1 + dir.size()) {
                path = rest.substr(1, close - 1);
                end  = close + 1;
            }
        } else if (str::startsWith(rest, dir)) {
            size_t j = dir.size();
            while (j < rest.size() && !isSpace(rest[j]))
                ++j;
            if (j > dir.size()) {
                path = rest.substr(0, j);
                end  = j;
            }
        }
        if (path.empty())
            continue;
        if (paths)
            paths->emplace_back(path);
        if (end < rest.size() && rest[end] == ' ')
            ++end;
        text.append(prompt.substr(i, at - i));
        i = at + 1 + end;
    }
    text.append(prompt.substr(std::min(i, prompt.size())));
    const std::string_view trimmed = trim(text);
    return trimmed == kFilesOnly ? std::string() : std::string(trimmed);
}

// ── Prompts ─────────────────────────────────────────────────────────────────

std::string typedPrompt(std::string_view prompt, std::string *relayTo) {
    std::string text = withoutTeammateNote(prompt); // msga's own addition to what was typed
    // "The user replied in the thread of subagent <id>. Pass their message on
    // to it verbatim with SendMessage (to: "<id>"):\n\n<reply>"
    constexpr std::string_view kHead = "The user replied in the thread of subagent ";
    const std::string_view     t(text);
    if (!str::startsWith(t, kHead))
        return text;
    size_t j = kHead.size();
    while (j < t.size() && ((t[j] >= 'A' && t[j] <= 'Z') || (t[j] >= 'a' && t[j] <= 'z') ||
                            isDigit(t[j]) || t[j] == '_' || t[j] == '-'))
        ++j;
    const std::string_view id = t.substr(kHead.size(), j - kHead.size());
    if (id.empty())
        return text;
    const std::string mid = str::concat(
        {". Pass their message on to it verbatim with SendMessage (to: \"", id, "\"):\n\n"}
    );
    if (t.substr(j, mid.size()) != mid || j + mid.size() >= t.size())
        return text;
    if (relayTo)
        *relayTo = id;
    return std::string(t.substr(j + mid.size()));
}

std::string subagentReplyPrompt(std::string_view agentId, std::string_view reply) {
    return str::concat(
        {"The user replied in the thread of subagent ",
         agentId,
         ". Pass their message on to it verbatim with SendMessage (to: \"",
         agentId,
         "\"):\n\n",
         reply}
    );
}

std::vector<std::string> promptHistory(
    std::string_view historyPath,
    std::string_view pasteDir,
    std::string_view project,
    std::string_view sessionId,
    int              max
) {
    std::string data;
    if (!file::readAll(historyPath, &data))
        return {};
    const std::string        dir = cleanPath(project);
    std::vector<std::string> own, others; // oldest first
    const std::string_view   all(data);
    for (size_t start = 0; start < all.size();) {
        size_t nl = all.find('\n', start);
        if (nl == std::string_view::npos)
            nl = all.size();
        const std::string_view line = all.substr(start, nl - start);
        start                       = nl + 1;
        if (line.find("\"display\"") == std::string_view::npos)
            continue; // skip the parse for what can't be an entry
        json::Document doc;
        if (!doc.parse(std::string(line), nullptr))
            continue;
        const json::Value o = doc.root();
        if (cleanPath(o["project"].str()) != dir)
            continue;
        std::string text(o["display"].str());
        // A long paste shows as "[Pasted text #3 +24 lines]"; its text is in
        // the entry, or (newer) in paste-cache/<contentHash>.txt.
        for (const json::Value p : o["pastedContents"]) {
            if (p["type"].str() != "text")
                continue;
            std::string content(p["content"].str());
            if (content.empty()) {
                const std::string_view hash = p["contentHash"].str();
                if (hash.empty() ||
                    !file::readAll(str::concat({pasteDir, "/", hash, ".txt"}), &content))
                    continue; // leave the placeholder: better than a silent hole
            }
            // "[Pasted text #<key>]" or "[Pasted text #<key> +<n> lines]".
            const std::string head = str::concat({"[Pasted text #", p.key()});
            for (size_t at = text.find(head); at != std::string::npos;
                 at        = text.find(head, at + 1)) {
                size_t j = at + head.size();
                if (text.compare(j, 2, " +") == 0) {
                    size_t k = j + 2;
                    while (k < text.size() && isDigit(text[k]))
                        ++k;
                    if (k > j + 2 && text.compare(k, 6, " lines") == 0)
                        j = k + 6;
                }
                if (j < text.size() && text[j] == ']') {
                    text.replace(at, j + 1 - at, content);
                    break;
                }
            }
        }
        // A pasted image can't come back as text, and "[Image #1]" alone would
        // only confuse the next prompt.
        text = std::string(trim(typedPrompt(dropImageMarks(text))));
        if (text.empty())
            continue;
        const bool mine = !sessionId.empty() && o["sessionId"].str() == sessionId;
        (mine ? own : others).push_back(std::move(text));
    }
    std::vector<std::string> out;
    for (const std::vector<std::string> *part : {&own, &others}) {
        for (auto it = part->crbegin(); it != part->crend() && int(out.size()) < max; ++it)
            if (out.empty() || out.back() != *it) // the same prompt twice in a row: once
                out.push_back(*it);
    }
    return out;
}

// ── Rewriting the transcript ────────────────────────────────────────────────

namespace {

// Thinking only: the reasoning half of an answer, written as a record of its own.
bool isThinkingRecord(const json::Value &o) {
    if (o["type"].str() != "assistant")
        return false;
    const json::Value blocks = o["message"]["content"];
    if (!blocks.isArray() || blocks.size() == 0)
        return false;
    for (const json::Value v : blocks) {
        const std::string_view bt = v["type"].str();
        if (bt != "thinking" && bt != "redacted_thinking")
            return false;
    }
    return true;
}

// Where the next turn starts: a turn's end, or a prompt someone typed.
bool endsTurn(const json::Value &o) {
    const std::string_view type = o["type"].str();
    if (type == "system")
        return o["subtype"].str() == "turn_duration";
    if (type != "user" || o["isMeta"].boolean())
        return false;
    const json::Value content = o["message"]["content"];
    if (content.isString())
        return true;
    for (const json::Value v : content)
        if (v["type"].str() == "tool_result")
            return false;
    return true;
}

} // namespace

bool hasTurnSince(std::string_view path, int64_t from, int64_t afterMs) {
    std::string data;
    if (!file::readRange(path, from, size_t(-1) >> 1, &data))
        return false;
    const std::string_view all(data);
    for (size_t start = 0; start < all.size();) {
        size_t nl = all.find('\n', start);
        if (nl == std::string_view::npos)
            nl = all.size();
        const std::string_view line = all.substr(start, nl - start);
        start                       = nl + 1;
        json::Document doc;
        if (!doc.parse(std::string(line), nullptr))
            continue;
        const json::Value      rec  = doc.root();
        const std::string_view type = rec["type"].str();
        if (type != "user" && type != "assistant")
            continue;
        if (afterMs <= 0)
            return true;
        const int64_t at = base::parseIsoMicros(rec["timestamp"].str());
        if (at > 0 && at / 1000 > afterMs)
            return true;
    }
    return false;
}

bool removeFromTranscript(std::string_view path, std::string_view uuid, std::string *error) {
    auto fail = [error](std::string why) {
        if (error)
            *error = std::move(why);
        return false;
    };
    std::string data;
    if (uuid.empty() || !file::readAll(path, &data))
        return fail(str::concat({"can't read ", path}));
    std::vector<std::string_view> lines;
    for (size_t start = 0; start <= data.size();) {
        size_t nl = data.find('\n', start);
        if (nl == std::string::npos)
            nl = data.size();
        lines.push_back(std::string_view(data).substr(start, nl - start));
        start = nl + 1;
    }
    if (!lines.empty() && lines.back().empty())
        lines.pop_back();
    std::vector<json::Document> docs(lines.size()); // empty for a line that isn't JSON
    int                         target = -1;
    for (size_t i = 0; i < lines.size(); ++i) {
        if (!docs[i].parse(std::string(lines[i]), nullptr) || !docs[i].root().isObject())
            docs[i] = json::Document();
        if (docs[i].root()["uuid"].str() == uuid)
            target = int(i);
    }
    if (target < 0)
        return fail(str::concat({"no record ", uuid, " in ", path}));
    auto rec = [&docs](size_t i) { return docs[i].root(); };

    // What goes, each with the parent that records linked to it are moved to.
    std::unordered_map<std::string, std::string> gone;
    std::vector<bool>                            dropped(lines.size(), false);
    auto                                         drop = [&](size_t i) {
        dropped[i]                = true;
        const std::string_view id = rec(i)["uuid"].str();
        if (!id.empty())
            gone[std::string(id)] = rec(i)["parentUuid"].str();
    };
    const json::Value t = rec(size_t(target));
    drop(size_t(target));
    if (t["type"].str() == "assistant") {
        // An answer: the thinking written as part of the same message.
        const std::string_view id = t["message"]["id"].str();
        for (size_t i = 0; i < lines.size(); ++i)
            if (!id.empty() && isThinkingRecord(rec(i)) && rec(i)["message"]["id"].isString() &&
                rec(i)["message"]["id"].str() == id)
                drop(i);
    } else {
        // A prompt: the thinking of the turn it started.
        for (size_t i = size_t(target) + 1; i < lines.size() && !endsTurn(rec(i)); ++i)
            if (isThinkingRecord(rec(i)))
                drop(i);
    }
    // Claude Code's own bookkeeping keeps a typed prompt's text as well (its
    // input queue, the last prompt for `claude --resume`): none of it reaches
    // Claude, but a deleted message shouldn't linger in the file.
    const json::Value promptText = t["message"]["content"];
    if (t["type"].str() == "user" && promptText.isString())
        for (size_t i = 0; i < lines.size(); ++i) {
            const json::Value      r    = rec(i);
            const std::string_view type = r["type"].str();
            if ((type == "queue-operation" && r["content"].isString() &&
                 r["content"].str() == promptText.str()) ||
                (type == "last-prompt" && r["lastPrompt"].isString() &&
                 r["lastPrompt"].str() == promptText.str()))
                dropped[i] = true;
        }
    auto relink = [&gone](std::string id) {
        for (size_t hops = 0; hops < gone.size(); ++hops) {
            const auto it = gone.find(id);
            if (it == gone.end())
                break;
            id = it->second;
        }
        return id;
    };

    constexpr std::string_view kLinks[] = {"parentUuid", "logicalParentUuid", "leafUuid"};
    std::string                out;
    out.reserve(data.size());
    for (size_t i = 0; i < lines.size(); ++i) {
        const json::Value o = rec(i);
        if (dropped[i] || gone.count(std::string(o["uuid"].str())))
            continue;
        bool changed = false;
        for (const std::string_view key : kLinks) {
            const std::string_view link = o[key].str();
            if (!link.empty() && gone.count(std::string(link)))
                changed = true;
        }
        if (!changed) {
            out.append(lines[i]); // untouched lines are kept byte for byte
            out += '\n';
            continue;
        }
        json::Writer w;
        w.beginObject();
        for (const json::Value v : o) {
            const std::string_view key = v.key();
            const bool             link =
                std::find(std::begin(kLinks), std::end(kLinks), key) != std::end(kLinks);
            w.key(key);
            if (link && !v.str().empty() && gone.count(std::string(v.str()))) {
                const std::string to = relink(std::string(v.str()));
                if (to.empty())
                    w.null();
                else
                    w.value(to);
            } else {
                w.value(v);
            }
        }
        w.endObject();
        out += w.str();
        out += '\n';
    }
    // In place, not by replacing the file: a writer holding it open keeps
    // appending to the same file.
    if (!file::overwrite(path, out))
        return fail(str::concat({"can't write ", path}));
    return true;
}

} // namespace claude
