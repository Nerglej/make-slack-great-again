// Fuzzy subsequence matching for pick-lists driven by a search field (the
// Ctrl/Cmd+K switcher, msga's issue #60), msga's util/fuzzy_match: "xdg"
// finds "xd-general", "bb" finds "Bob Builder". Every query character must
// appear in the haystack in order, but not adjacently; the score ranks the
// candidates that pass.
//
// Scoring follows fzy's dynamic programme: the best alignment over all
// placements of the query characters, rewarding runs of adjacent matches (a
// plain substring beats a scattered one), matches that start a word (after a
// space, '-', '_', '.', ',', '/', '@', '#', ':'), camelCase humps and the
// very start of the haystack, while charging a small toll per skipped
// character so tighter, earlier alignments win. Case-insensitive (simple
// case folding, by code point).
#pragma once

#include <optional>
#include <string_view>

namespace shell {

// nullopt when `query` is not a subsequence of `haystack`; else a score,
// higher is better, comparable only across haystacks for the same query. An
// empty query matches everything with 0.
std::optional<double> fuzzyScore(std::string_view query, std::string_view haystack);

} // namespace shell
