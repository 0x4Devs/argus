#include "core/search.h"

#include <cctype>
#include <regex>

namespace argus {

namespace {

inline wchar_t to_lower(wchar_t c) {
    if (c >= L'A' && c <= L'Z') return c + 32;
    switch (c) {
        case L'Ä': return L'ä';
        case L'Ö': return L'ö';
        case L'Ü': return L'ü';
        default:   return c;
    }
}

// Case-insensitive Substring, needle bereits lowercase.
bool contains_ci(const wchar_t* hay, size_t hay_len,
                 const wchar_t* needle, size_t needle_len) {
    if (needle_len == 0) return true;
    if (hay_len < needle_len) return false;
    const size_t last = hay_len - needle_len;
    for (size_t i = 0; i <= last; ++i) {
        size_t j = 0;
        for (; j < needle_len; ++j) {
            if (to_lower(hay[i + j]) != needle[j]) break;
        }
        if (j == needle_len) return true;
    }
    return false;
}

std::wstring wildcard_to_regex(std::wstring_view p) {
    std::wstring r;
    r.reserve(p.size() * 2);
    for (wchar_t c : p) {
        switch (c) {
            case L'*':  r += L".*";   break;
            case L'?':  r += L".";    break;
            case L'.':  case L'\\': case L'+':  case L'(':
            case L')':  case L'{':   case L'}': case L'|':
            case L'^':  case L'$':   case L'[': case L']':
                r += L'\\'; r += c;   break;
            default:    r += c;       break;
        }
    }
    return r;
}

} // namespace

std::vector<uint32_t> Search(const Index& index,
                             std::wstring_view query,
                             const SearchOptions& opt,
                             std::atomic<bool>* cancel) {
    std::vector<uint32_t> out;
    out.reserve(std::min<size_t>(opt.max_results, 1024));

    const auto& entries = index.entries();
    const auto& pool    = index.name_pool();
    const size_t n      = entries.size();

    // Substring: eigener Fast-Path.
    if (opt.mode == SearchMode::Substring) {
        std::wstring q(query);
        for (auto& c : q) c = to_lower(c);

        for (uint32_t i = 0; i < n; ++i) {
            if ((i & 0xFFFF) == 0 && cancel && cancel->load(std::memory_order_relaxed))
                return out;
            const auto& e = entries[i];
            if (e.flags & kFlagDeleted) continue;
            if (opt.files_only && (e.flags & kFlagDirectory)) continue;
            if (opt.dirs_only  && !(e.flags & kFlagDirectory)) continue;
            if (contains_ci(pool.data() + e.name_offset, e.name_length,
                            q.data(), q.size())) {
                out.push_back(i);
                if (out.size() >= opt.max_results) break;
            }
        }
        return out;
    }

    // Wildcard oder Regex: std::wregex bauen.
    std::wregex re;
    try {
        std::wstring pattern = (opt.mode == SearchMode::Wildcard)
                              ? wildcard_to_regex(query)
                              : std::wstring(query);
        re.assign(pattern, std::regex::ECMAScript | std::regex::icase);
    } catch (...) {
        return out;  // ungueltiges Muster
    }

    for (uint32_t i = 0; i < n; ++i) {
        if ((i & 0xFFFF) == 0 && cancel && cancel->load(std::memory_order_relaxed))
            return out;
        const auto& e = entries[i];
        if (e.flags & kFlagDeleted) continue;
        if (opt.files_only && (e.flags & kFlagDirectory)) continue;
        if (opt.dirs_only  && !(e.flags & kFlagDirectory)) continue;
        const wchar_t* p = pool.data() + e.name_offset;
        if (std::regex_search(p, p + e.name_length, re)) {
            out.push_back(i);
            if (out.size() >= opt.max_results) break;
        }
    }
    return out;
}

} // namespace argus
