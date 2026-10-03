#include "base/winstr.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace base {

std::wstring wide(std::string_view s) {
    std::wstring w;
    if (s.empty())
        return w;
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
    if (n <= 0)
        return w;
    w.resize(size_t(n));
    MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n);
    return w;
}

std::wstring widePath(std::string_view s) {
    std::wstring w = wide(s);
    for (auto &c : w)
        if (c == L'/')
            c = L'\\';
    return w;
}

std::string narrow(std::wstring_view w) {
    std::string s;
    if (w.empty())
        return s;
    const int n =
        WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), nullptr, 0, nullptr, nullptr);
    if (n <= 0)
        return s;
    s.resize(size_t(n));
    WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

} // namespace base
