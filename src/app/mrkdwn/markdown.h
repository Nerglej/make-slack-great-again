// Markdown → Slack mrkdwn: msga's MarkdownCompose (the composer's text on the
// way out, and Claude Code's answers on the way in).
//
// People type CommonMark by habit; Slack speaks mrkdwn and does not
// translate, so the official client (and msga) convert on the way out:
//   - existing mrkdwn passes unchanged (*x*, _x_, ~x~, `x`, <@U…>, <#C…>,
//     <!here>, <url|label> tokens are opaque, __x__ stays);
//   - **x** → *x*, ***x*** → *_x_*, ~~x~~ → ~x~, [label](url) → <url|label>;
//   - a fence's language hint (```js) is dropped;
//   - "- a" / "1. a" lists are the one thing mrkdwn cannot carry: they
//     become a rich_text block (rich_text_list, nested by indentation), and
//     the text is the official client's fallback, "• a" / "1. a" numbered
//     consecutively, nested 4 spaces per level.
// The block is built ONLY when the message has a list: for everything else
// the mrkdwn alone renders the same in every client, and a block Slack
// rejects (invalid_blocks) fails the whole send.
#pragma once

#include <string>
#include <string_view>

namespace mrkdwn {

struct Composed {
    std::string mrkdwn; // the `text`: what the pending copy shows and edits reopen
    // Block Kit `blocks` as a JSON array: "" or exactly one rich_text block
    // mirroring the whole message.
    std::string blocks;
};
Composed    compose(std::string_view composerText);
std::string convertOutgoing(std::string_view composerText); // compose().mrkdwn
std::string convertInline(std::string_view line);           // one line's inline rewrites
// Parsed mrkdwn → {"elements":[…]}: rich_text inline elements ("text" with
// a style object, "user", "channel", "usergroup", "emoji", "link",
// "broadcast"), adjacent same-style text merged. For tests.
std::string richTextElements(std::string_view mrkdwn);

} // namespace mrkdwn
