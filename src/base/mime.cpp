#include "base/mime.h"

#include "base/file.h"
#include "base/str.h"

#include <string>

namespace mime {

namespace {

struct Type {
    const char *ext, *mime;
};
constexpr Type kTypes[] = {
    {"png", "image/png"},     {"jpg", "image/jpeg"},      {"jpeg", "image/jpeg"},
    {"gif", "image/gif"},     {"webp", "image/webp"},     {"bmp", "image/bmp"},
    {"svg", "image/svg+xml"}, {"pdf", "application/pdf"}, {"csv", "text/csv"},
    {"txt", "text/plain"},    {"md", "text/markdown"},    {"json", "application/json"},
    {"html", "text/html"},    {"htm", "text/html"},       {"zip", "application/zip"},
    {"mp3", "audio/mpeg"},    {"m4a", "audio/mp4"},       {"ogg", "audio/ogg"},
    {"wav", "audio/wav"},     {"flac", "audio/flac"},     {"mp4", "video/mp4"},
    {"webm", "video/webm"},   {"mov", "video/quicktime"},
};

struct Label {
    const char *mime, *label;
};
constexpr Label kLabels[] = {
    {"image/png", "PNG"},       {"image/jpeg", "JPEG"},     {"image/gif", "GIF"},
    {"image/webp", "WebP"},     {"image/bmp", "BMP"},       {"image/svg+xml", "SVG"},
    {"application/pdf", "PDF"}, {"text/csv", "CSV"},        {"text/plain", "Plain text"},
    {"text/html", "HTML"},      {"application/zip", "Zip"}, {"audio/mpeg", "MP3"},
    {"audio/mp4", "M4A"},       {"audio/ogg", "OGG"},       {"audio/wav", "WAV"},
    {"audio/flac", "FLAC"},     {"video/mp4", "MP4"},       {"video/webm", "WebM"},
    {"video/quicktime", "MOV"},
};

bool starts(std::string_view s, std::string_view prefix) {
    return s.substr(0, prefix.size()) == prefix;
}

} // namespace

std::string_view fromName(std::string_view name) {
    const std::string ext = str::asciiLower(file::extension(file::baseName(name)));
    for (const Type &t : kTypes)
        if (ext == t.ext)
            return t.mime;
    return {};
}

std::string_view fromNameOr(std::string_view name) {
    const std::string_view m = fromName(name);
    return m.empty() ? std::string_view("application/octet-stream") : m;
}

std::string_view sniff(std::string_view h) {
    if (starts(h, "\x89PNG\r\n\x1a\n"))
        return "image/png";
    if (starts(h, "\xFF\xD8\xFF"))
        return "image/jpeg";
    if (starts(h, "GIF87a") || starts(h, "GIF89a"))
        return "image/gif";
    if (h.size() >= 12 && starts(h, "RIFF") && h.substr(8, 4) == "WEBP")
        return "image/webp";
    if (starts(h, "%PDF-"))
        return "application/pdf";
    if (starts(h, "ID3"))
        return "audio/mpeg";
    return {};
}

std::string_view label(std::string_view m) {
    for (const Label &l : kLabels)
        if (m == l.mime)
            return l.label;
    return {};
}

} // namespace mime
