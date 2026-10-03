// What a people search matches: a person's label (the display name, else
// the handle) and handle, optionally the job title, folded once so each
// keystroke only compares (utf8::containsPrefolded).
#pragma once

#include "app/model/types.h"

#include <string>

namespace screens {

// "label handle[ title]", folded.
std::string userSearchKey(const model::User &u, bool withTitle = false);

} // namespace screens
