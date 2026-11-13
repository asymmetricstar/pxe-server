#pragma once

#include <algorithm>
#include <filesystem>
#include <string>

namespace pxe {
namespace pathutil {

// Normalize separators to '/' without changing URL schemes (http://, https://).
inline std::string normalizeSlashes(std::string s) {
    // Preserve URL scheme separators.
    std::string scheme;
    const auto schemePos = s.find("://");
    if (schemePos != std::string::npos) {
        scheme = s.substr(0, schemePos + 3);
        s = s.substr(schemePos + 3);
    }
    std::replace(s.begin(), s.end(), '\\', '/');
    // Collapse duplicate slashes in path portion.
    std::string out;
    out.reserve(s.size());
    bool prevSlash = false;
    for (char c : s) {
        if (c == '/') {
            if (!prevSlash) out.push_back(c);
            prevSlash = true;
        } else {
            out.push_back(c);
            prevSlash = false;
        }
    }
    return scheme + out;
}

// Normalize a filesystem path string (lexical): separators -> '/', remove . and .. where possible.
inline std::string normalizeFsPath(const std::string& path) {
    if (path.empty()) return path;
    // Keep URLs untouched except slash normalization.
    if (path.find("://") != std::string::npos) return normalizeSlashes(path);
    std::filesystem::path p(path);
    auto n = p.lexically_normal();
    std::string s = n.generic_string();
    if (s.empty()) s = ".";
    // generic_string uses '/', but ensure no stray backslashes.
    std::replace(s.begin(), s.end(), '\\', '/');
    return s;
}

// Join root and relative path, normalize result.
inline std::string join(const std::string& root, const std::string& rel) {
    if (rel.empty()) return normalizeFsPath(root);
    std::filesystem::path p = std::filesystem::path(root) / rel;
    return normalizeFsPath(p.string());
}

} // namespace pathutil
} // namespace pxe
