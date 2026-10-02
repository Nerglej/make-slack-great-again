// Win32 backend, data half: the one mapping between MIME types and clipboard
// formats (shared by the clipboard and OLE drag and drop), CF_HTML, CF_HDROP
// and DIB conversions, and the clipboard itself.
//
// Standard types map to what Windows apps actually exchange:
//   text/plain;charset=utf-8  CF_UNICODETEXT (CRLF; Windows synthesises CF_TEXT)
//   text/html                 "HTML Format" (CF_HTML: UTF-8 with an offset header)
//   image/png                 "PNG" (what browsers and Office register)
//   image/bmp                 CF_DIB (reading: CF_DIB/CF_DIBV5 + a file header)
//   text/uri-list             registered "text/uri-list" (exact bytes between
//                             plat apps) + CF_HDROP when every URI is a local
//                             file, so Explorer and every file-taking app
//                             understand it; reading also takes CF_HDROP and
//                             "UniformResourceLocatorW"
// Every other MIME type is a format registered under its own name, which
// round-trips between plat apps and matches Chromium's custom types.
#include "win32/win32.h"

#include <shlobj.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace plat::win32 {

namespace {

UINT registered(const wchar_t *name) {
    return RegisterClipboardFormatW(name);
}
UINT cfUrlW() {
    static const UINT f = registered(L"UniformResourceLocatorW");
    return f;
}
UINT cfUrlA() {
    static const UINT f = registered(L"UniformResourceLocator");
    return f;
}

std::string lfToCrlf(std::string_view s) {
    std::string out;
    out.reserve(s.size() + s.size() / 16);
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\n' && (i == 0 || s[i - 1] != '\r'))
            out += '\r';
        out += s[i];
    }
    return out;
}

std::string crlfToLf(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i)
        if (!(s[i] == '\r' && i + 1 < s.size() && s[i + 1] == '\n'))
            out += s[i];
    return out;
}

// NUL-terminated UTF-16 as bytes, the layout of CF_UNICODETEXT.
std::string wideBytes(std::wstring_view w) {
    std::string b(reinterpret_cast<const char *>(w.data()), w.size() * sizeof(wchar_t));
    b.append(sizeof(wchar_t), '\0');
    return b;
}

std::wstring_view wideUntilNul(std::string_view bytes) {
    const auto  *w = reinterpret_cast<const wchar_t *>(bytes.data());
    const size_t n = wcsnlen(w, bytes.size() / sizeof(wchar_t));
    return {w, n};
}

std::string_view untilNul(std::string_view s) {
    const size_t n = s.find('\0');
    return n == std::string_view::npos ? s : s.substr(0, n);
}

long headerValue(std::string_view header, std::string_view key) {
    const size_t at = header.find(key);
    if (at == std::string_view::npos)
        return -1;
    size_t i   = at + key.size();
    bool   neg = false;
    if (i < header.size() && header[i] == '-') {
        neg = true;
        ++i;
    }
    long v   = 0;
    bool any = false;
    for (; i < header.size() && header[i] >= '0' && header[i] <= '9'; ++i) {
        v   = v * 10 + (header[i] - '0');
        any = true;
        if (v > (1L << 30))
            return -1;
    }
    return any && !neg ? v : -1;
}

bool openClipboard(HWND owner) {
    // Another process may hold the clipboard for a moment (clipboard
    // managers, RDP); a few short retries is what every toolkit does.
    for (int i = 0; i < 10; ++i) {
        if (OpenClipboard(owner))
            return true;
        Sleep(5);
    }
    return false;
}

} // namespace

UINT cfHtml() {
    static const UINT f = registered(L"HTML Format");
    return f;
}
UINT cfPng() {
    static const UINT f = registered(L"PNG");
    return f;
}
UINT cfUriList() {
    static const UINT f = registered(L"text/uri-list");
    return f;
}

bool isTextMime(std::string_view m) {
    return core::isTextMime(m);
}

// ── CF_HTML ─────────────────────────────────────────────────────────────────

std::string cfHtmlEncode(std::string_view fragment) {
    // Offsets are byte positions from the start of the data; zero-padded to a
    // fixed width so the header's own length does not depend on them.
    static constexpr char kPre[]  = "<html>\r\n<body>\r\n<!--StartFragment-->";
    static constexpr char kPost[] = "<!--EndFragment-->\r\n</body>\r\n</html>";
    static constexpr char kFmt[]  = "Version:0.9\r\nStartHTML:%010ld\r\nEndHTML:%010ld\r\n"
                                    "StartFragment:%010ld\r\nEndFragment:%010ld\r\n";
    char                  header[160];
    const int             headerLen = std::snprintf(header, sizeof(header), kFmt, 0L, 0L, 0L, 0L);
    const long            startHtml = headerLen;
    const long            startFrag = startHtml + long(sizeof(kPre) - 1);
    const long            endFrag   = startFrag + long(fragment.size());
    const long            endHtml   = endFrag + long(sizeof(kPost) - 1);
    std::snprintf(header, sizeof(header), kFmt, startHtml, endHtml, startFrag, endFrag);
    std::string out(header, size_t(headerLen));
    out += kPre;
    out += fragment;
    out += kPost;
    out += '\0'; // readers expect a terminated string; the offsets exclude it
    return out;
}

std::string cfHtmlDecode(std::string_view data) {
    data                          = untilNul(data);
    const size_t           first  = data.find('<');
    // The header is everything before the markup; offsets are only trusted
    // when they fall inside the data (some producers write -1 or garbage).
    const std::string_view header = data.substr(0, first == std::string_view::npos ? 0 : first);
    const long             size   = long(data.size());
    const long sf = headerValue(header, "StartFragment:"), ef = headerValue(header, "EndFragment:");
    if (sf >= 0 && ef >= sf && ef <= size)
        return std::string(data.substr(size_t(sf), size_t(ef - sf)));
    const std::string_view kStart = "<!--StartFragment-->", kEnd = "<!--EndFragment-->";
    const size_t           ms = data.find(kStart), me = data.rfind(kEnd);
    if (ms != std::string_view::npos && me != std::string_view::npos && me >= ms + kStart.size())
        return std::string(data.substr(ms + kStart.size(), me - ms - kStart.size()));
    const long sh = headerValue(header, "StartHTML:"), eh = headerValue(header, "EndHTML:");
    if (sh >= 0 && eh >= sh && eh <= size)
        return std::string(data.substr(size_t(sh), size_t(eh - sh)));
    return first == std::string_view::npos ? std::string() : std::string(data.substr(first));
}

// ── CF_HDROP ────────────────────────────────────────────────────────────────

std::string hdropToUriList(const void *p, size_t size) {
    if (size < sizeof(DROPFILES))
        return {};
    const auto *df  = static_cast<const DROPFILES *>(p);
    const char *raw = static_cast<const char *>(p);
    if (df->pFiles >= size)
        return {};
    std::string out;
    if (df->fWide) {
        const auto  *w = reinterpret_cast<const wchar_t *>(raw + df->pFiles);
        const size_t n = (size - df->pFiles) / sizeof(wchar_t);
        for (size_t i = 0; i < n && w[i];) {
            const size_t len = wcsnlen(w + i, n - i);
            out += fileUri(std::wstring_view(w + i, len)) + "\r\n";
            i += len + 1;
        }
    } else {
        const char  *a = raw + df->pFiles;
        const size_t n = size - df->pFiles;
        for (size_t i = 0; i < n && a[i];) {
            const size_t len = strnlen(a + i, n - i);
            const int    wn  = MultiByteToWideChar(CP_ACP, 0, a + i, int(len), nullptr, 0);
            std::wstring w(size_t(wn), L'\0');
            MultiByteToWideChar(CP_ACP, 0, a + i, int(len), w.data(), wn);
            out += fileUri(w) + "\r\n";
            i += len + 1;
        }
    }
    return out;
}

std::string hdropFromUriList(std::string_view list) {
    std::wstring files;
    for (const auto &u : parseUriList(list)) {
        auto path = pathFromFileUri(u);
        if (!path || path->empty())
            return {}; // one non-file URI: CF_HDROP would silently drop it
        files += *path;
        files += L'\0';
    }
    if (files.empty())
        return {};
    files += L'\0';
    DROPFILES df{};
    df.pFiles = sizeof(DROPFILES);
    df.fWide  = TRUE;
    std::string out(reinterpret_cast<const char *>(&df), sizeof(df));
    out.append(reinterpret_cast<const char *>(files.data()), files.size() * sizeof(wchar_t));
    return out;
}

// ── DIB ─────────────────────────────────────────────────────────────────────

std::string dibToBmpFile(std::string_view dib) {
    if (dib.size() < sizeof(BITMAPINFOHEADER))
        return {};
    BITMAPINFOHEADER h;
    std::memcpy(&h, dib.data(), sizeof(h));
    if (h.biSize < sizeof(BITMAPINFOHEADER) || h.biSize > dib.size())
        return {};
    // The pixels start after the header, the BI_BITFIELDS masks (only
    // outside the header for a plain BITMAPINFOHEADER) and the palette.
    size_t extra = 0;
    if (h.biSize == sizeof(BITMAPINFOHEADER) && h.biCompression == BI_BITFIELDS)
        extra = 12;
    else if (h.biSize == sizeof(BITMAPINFOHEADER) && h.biCompression == 6) // BI_ALPHABITFIELDS
        extra = 16;
    size_t colors = h.biClrUsed;
    if (!colors && h.biBitCount <= 8)
        colors = size_t(1) << h.biBitCount;
    const size_t off = sizeof(BITMAPFILEHEADER) + h.biSize + extra + colors * sizeof(RGBQUAD);
    if (off > sizeof(BITMAPFILEHEADER) + dib.size())
        return {};
    BITMAPFILEHEADER fh{};
    fh.bfType    = 0x4d42; // "BM"
    fh.bfSize    = DWORD(sizeof(fh) + dib.size());
    fh.bfOffBits = DWORD(off);
    std::string out(reinterpret_cast<const char *>(&fh), sizeof(fh));
    out.append(dib.data(), dib.size());
    return out;
}

// ── the mapping ─────────────────────────────────────────────────────────────

std::vector<NativeData> encodeForOs(const std::vector<DataItem> &items) {
    std::vector<NativeData> out;
    auto                    add = [&](UINT cf, std::string bytes) {
        if (!cf)
            return;
        for (const auto &n : out)
            if (n.cf == cf)
                return; // the first item of a type wins
        out.push_back({cf, std::move(bytes)});
    };
    for (const auto &i : items) {
        if (isTextMime(i.mime)) {
            // Windows text is CRLF; lone LFs paste as one line in Notepad & co.
            add(CF_UNICODETEXT, wideBytes(toWide(lfToCrlf(i.data))));
        } else if (i.mime == "text/html") {
            add(cfHtml(), cfHtmlEncode(i.data));
        } else if (i.mime == "image/png") {
            add(cfPng(), i.data);
        } else if (i.mime == "image/bmp") {
            if (i.data.size() > sizeof(BITMAPFILEHEADER) && i.data.compare(0, 2, "BM") == 0)
                add(CF_DIB, i.data.substr(sizeof(BITMAPFILEHEADER)));
        } else if (i.mime == "text/uri-list") {
            add(cfUriList(), i.data);
            if (std::string drop = hdropFromUriList(i.data); !drop.empty())
                add(CF_HDROP, std::move(drop));
            else if (auto uris = parseUriList(i.data); !uris.empty())
                add(cfUrlW(), wideBytes(toWide(uris[0]))); // browsers take one link
        } else {
            add(RegisterClipboardFormatW(toWide(i.mime).c_str()), i.data);
        }
    }
    return out;
}

std::optional<std::string> mimeForFormat(UINT cf) {
    switch (cf) {
    case CF_UNICODETEXT:
    case CF_TEXT:
    case CF_OEMTEXT:
        return "text/plain;charset=utf-8";
    case CF_DIB:
    case CF_DIBV5:
    case CF_BITMAP:
        return "image/bmp";
    case CF_HDROP:
        return "text/uri-list";
    default:
        break;
    }
    if (cf < 0xC000)
        return std::nullopt; // other predefined formats: GDI handles, locale, …
    if (cf == cfHtml())
        return "text/html";
    if (cf == cfPng())
        return "image/png";
    if (cf == cfUriList() || cf == cfUrlW() || cf == cfUrlA())
        return "text/uri-list";
    wchar_t   name[256];
    const int n = GetClipboardFormatNameW(cf, name, 256);
    if (n <= 0)
        return std::nullopt;
    return toUtf8(std::wstring_view(name, size_t(n)));
}

std::vector<std::string> mimesForFormats(const std::vector<UINT> &cfs) {
    std::vector<std::string> out;
    bool                     bmp = false;
    for (UINT cf : cfs) {
        auto m = mimeForFormat(cf);
        if (!m)
            continue;
        bmp |= *m == "image/bmp";
        if (std::find(out.begin(), out.end(), *m) == out.end())
            out.push_back(std::move(*m));
    }
    // Screenshots (PrtScn, Snipping Tool) put only a DIB on the clipboard;
    // decodeFromOs turns it into PNG through WIC, so offer that too.
    if (bmp && std::find(out.begin(), out.end(), "image/png") == out.end())
        out.push_back("image/png");
    return out;
}

std::optional<std::string>
decodeFromOs(std::string_view mime, const std::function<std::optional<std::string>(UINT)> &get) {
    if (isTextMime(mime)) {
        if (auto b = get(CF_UNICODETEXT))
            return crlfToLf(toUtf8(wideUntilNul(*b)));
        if (auto b = get(CF_TEXT)) {
            const std::string_view a = untilNul(*b);
            const int    wn = MultiByteToWideChar(CP_ACP, 0, a.data(), int(a.size()), nullptr, 0);
            std::wstring w(size_t(wn), L'\0');
            MultiByteToWideChar(CP_ACP, 0, a.data(), int(a.size()), w.data(), wn);
            return crlfToLf(toUtf8(w));
        }
        return std::nullopt;
    }
    if (mime == "text/html") {
        if (auto b = get(cfHtml()))
            return cfHtmlDecode(*b);
        return get(registered(L"text/html"));
    }
    if (mime == "text/uri-list") {
        if (auto b = get(cfUriList()))
            return b;
        if (auto b = get(CF_HDROP))
            return hdropToUriList(b->data(), b->size());
        if (auto b = get(cfUrlW()))
            return toUtf8(wideUntilNul(*b)) + "\r\n";
        if (auto b = get(cfUrlA()))
            return std::string(untilNul(*b)) + "\r\n";
        return std::nullopt;
    }
    if (mime == "image/png") {
        if (auto b = get(cfPng()))
            return b;
        if (auto b = get(registered(L"image/png")))
            return b;
        for (UINT cf : {UINT(CF_DIBV5), UINT(CF_DIB)})
            if (auto b = get(cf))
                if (std::string png = bmpFileToPng(dibToBmpFile(*b)); !png.empty())
                    return png;
        return std::nullopt;
    }
    if (mime == "image/bmp") {
        for (UINT cf : {UINT(CF_DIB), UINT(CF_DIBV5)})
            if (auto b = get(cf))
                if (std::string bmp = dibToBmpFile(*b); !bmp.empty())
                    return bmp;
        return std::nullopt;
    }
    return get(RegisterClipboardFormatW(toWide(mime).c_str()));
}

// ── clipboard ───────────────────────────────────────────────────────────────

void Win32App::setClipboard(std::vector<DataItem> items, Selection sel) {
    if (sel != Selection::Clipboard)
        return; // no primary selection on Windows
    const std::vector<NativeData> native = encodeForOs(items);
    if (!openClipboard(_msgHwnd))
        return;
    EmptyClipboard(); // a new selection replaces every offered type
    for (const auto &n : native) {
        HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, std::max<size_t>(n.bytes.size(), 1));
        if (!g)
            continue;
        if (void *p = GlobalLock(g)) {
            std::memcpy(p, n.bytes.data(), n.bytes.size());
            GlobalUnlock(g);
            if (SetClipboardData(n.cf, g))
                continue; // ownership passed to the OS
        }
        GlobalFree(g);
    }
    CloseClipboard();
}

void Win32App::requestClipboard(
    std::string_view mime, std::function<void(std::optional<std::string>)> cb, Selection sel
) {
    // The data is local to the OS, so reading is synchronous; the callback is
    // still posted because the contract promises it never runs re-entrantly.
    std::optional<std::string> result;
    if (sel == Selection::Clipboard && openClipboard(_msgHwnd)) {
        result = decodeFromOs(mime, [](UINT cf) -> std::optional<std::string> {
            if (!cf || !IsClipboardFormatAvailable(cf))
                return std::nullopt;
            HANDLE h = GetClipboardData(cf);
            if (!h)
                return std::nullopt;
            // GlobalSize may round up; the terminator bounds text formats,
            // and binary types get what every other reader sees.
            const size_t size = GlobalSize(h);
            const void  *p    = GlobalLock(h);
            if (!p)
                return std::nullopt;
            std::string bytes(static_cast<const char *>(p), size);
            GlobalUnlock(h);
            return bytes;
        });
        CloseClipboard();
    }
    post([cb = std::move(cb), result = std::move(result)] { cb(result); });
}

void Win32App::requestClipboardMimes(
    std::function<void(std::vector<std::string>)> cb, Selection sel
) {
    std::vector<UINT> formats;
    if (sel == Selection::Clipboard && openClipboard(_msgHwnd)) {
        for (UINT f = EnumClipboardFormats(0); f; f = EnumClipboardFormats(f))
            formats.push_back(f);
        CloseClipboard();
    }
    post([cb = std::move(cb), m = mimesForFormats(formats)] { cb(m); });
}

} // namespace plat::win32
