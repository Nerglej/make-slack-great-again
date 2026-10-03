#include "app/screens/common/user_search.h"

#include "base/str.h"
#include "base/utf8.h"

namespace screens {

std::string userSearchKey(const model::User &u, bool withTitle) {
    return utf8::foldCase(
        withTitle ? str::concat({u.label(), " ", u.name, " ", u.title})
                  : str::concat({u.label(), " ", u.name})
    );
}

} // namespace screens
