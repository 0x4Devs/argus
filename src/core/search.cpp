#include "core/search.h"

#include <algorithm>
#include <cctype>
#include <climits>
#include <regex>

#include "core/query.h"

namespace argus {

namespace {

// Fast case-folding covering the ranges most Windows filenames use:
// ASCII, Latin-1 Supplement, Latin Extended-A, Cyrillic uppercase, Greek
// uppercase. Everything else is passed through (rare on Windows names).
inline wchar_t to_lower(wchar_t c) {
    if (c >= L'A' && c <= L'Z') return c + 32;
    // Latin-1 Sup upper: 0xC0..0xDE excluding 0xD7 (multiplication sign)
    if (c >= 0x00C0 && c <= 0x00DE && c != 0x00D7) return c + 32;
    // Latin Extended-A: many pairs are (even upper, odd lower) at
    // 0x0100..0x017F, but not all. Handle the systematic pairs.
    if (c >= 0x0100 && c <= 0x0137 && (c & 1) == 0) return c + 1;
    if (c >= 0x0139 && c <= 0x0148 && (c & 1) == 1) return c + 1;
    if (c >= 0x014A && c <= 0x0177 && (c & 1) == 0) return c + 1;
    if (c >= 0x0179 && c <= 0x017E && (c & 1) == 1) return c + 1;
    // Cyrillic uppercase A..Ya: 0x0410..0x042F -> lower 0x0430..0x044F
    if (c >= 0x0410 && c <= 0x042F) return c + 32;
    // Cyrillic supplement: 0x0400..0x040F -> 0x0450..0x045F
    if (c >= 0x0400 && c <= 0x040F) return c + 80;
    // Greek: 0x0391..0x03A9 excluding 0x03A2 (unused) -> 0x03B1..0x03C9
    if (c >= 0x0391 && c <= 0x03A1) return c + 32;
    if (c >= 0x03A3 && c <= 0x03A9) return c + 32;
    return c;
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

// Simple fzf-style scoring. Positive = better match. Returns INT_MIN on no-match.
// Rewards consecutive characters, word-boundary starts, and short gaps.
int fuzzy_score(std::wstring_view name, const std::wstring& query_lc) {
    if (query_lc.empty()) return 0;
    int score = 0;
    int last_pos = -1;
    int consecutive = 0;
    for (wchar_t q : query_lc) {
        size_t found = std::wstring_view::npos;
        for (size_t i = size_t(last_pos + 1); i < name.size(); ++i) {
            wchar_t c = name[i];
            if (c >= L'A' && c <= L'Z') c += 32;
            if (c == q) { found = i; break; }
        }
        if (found == std::wstring_view::npos) return INT_MIN;
        int gap = int(found) - last_pos - 1;
        if (gap == 0) { consecutive++; score += 15 + consecutive * 5; }
        else { consecutive = 0; score -= gap; }
        if (found == 0 ||
            name[found - 1] == L' ' || name[found - 1] == L'_' ||
            name[found - 1] == L'-' || name[found - 1] == L'.') {
            score += 8;
        }
        last_pos = int(found);
    }
    // Small penalty for very long names (favor tight matches).
    score -= int(name.size()) / 32;
    return score;
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

    // Fuzzy: score every candidate, keep top N.
    if (opt.mode == SearchMode::Fuzzy) {
        std::wstring q(query);
        for (auto& c : q) c = to_lower(c);
        struct Scored { int score; uint32_t idx; };
        std::vector<Scored> scored;
        scored.reserve(std::min<size_t>(n, 100000));
        for (uint32_t i = 0; i < n; ++i) {
            if ((i & 0xFFFF) == 0 && cancel && cancel->load(std::memory_order_relaxed))
                return out;
            const auto& e = entries[i];
            if (e.flags & kFlagDeleted) continue;
            if (opt.files_only && (e.flags & kFlagDirectory)) continue;
            if (opt.dirs_only  && !(e.flags & kFlagDirectory)) continue;
            std::wstring_view sv(pool.data() + e.name_offset, e.name_length);
            int s = fuzzy_score(sv, q);
            if (s > INT_MIN) {
                if (opt.advanced_query && !MatchQuery(*opt.advanced_query, index, i)) continue;
                scored.push_back({s, i});
            }
        }
        // Top-N nach Score sortieren (partial_sort ist O(n log k)).
        size_t k = std::min(scored.size(), opt.max_results);
        std::partial_sort(scored.begin(), scored.begin() + k, scored.end(),
            [](const Scored& a, const Scored& b){ return a.score > b.score; });
        scored.resize(k);
        out.reserve(k);
        for (auto& s : scored) out.push_back(s.idx);
        return out;
    }

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
            if (!contains_ci(pool.data() + e.name_offset, e.name_length,
                             q.data(), q.size())) continue;
            if (opt.advanced_query && !MatchQuery(*opt.advanced_query, index, i))
                continue;
            out.push_back(i);
            if (out.size() >= opt.max_results) break;
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
        if (!std::regex_search(p, p + e.name_length, re)) continue;
        if (opt.advanced_query && !MatchQuery(*opt.advanced_query, index, i)) continue;
        out.push_back(i);
        if (out.size() >= opt.max_results) break;
    }
    return out;
}

} // namespace argus
