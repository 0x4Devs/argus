#include "core/search.h"

#include <cctype>

namespace argus {

namespace {

// ASCII-lowercase — echte Unicode-Fall-Ordnung ist eine spaetere Ausbaustufe.
inline wchar_t to_lower(wchar_t c) {
    if (c >= L'A' && c <= L'Z') return c + 32;
    // Deutsche Umlaute + sz mitmachen.
    switch (c) {
        case L'Ä': return L'ä';
        case L'Ö': return L'ö';
        case L'Ü': return L'ü';
        default:   return c;
    }
}

// Case-insensitive Substring, needle bereits lowercase.
bool contains(const wchar_t* hay, size_t hay_len,
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

} // namespace

std::vector<uint32_t> Search(const Index& index,
                             std::wstring_view query,
                             const SearchOptions& opt,
                             std::atomic<bool>* cancel) {
    std::vector<uint32_t> out;
    out.reserve(std::min<size_t>(opt.max_results, 1024));

    std::wstring q(query);
    for (auto& c : q) c = to_lower(c);

    const auto& entries = index.entries();
    const auto& pool    = index.name_pool();
    const size_t n      = entries.size();

    for (uint32_t i = 0; i < n; ++i) {
        if ((i & 0xFFFF) == 0 && cancel && cancel->load(std::memory_order_relaxed))
            return out;

        const auto& e = entries[i];
        if (opt.files_only && (e.flags & kFlagDirectory)) continue;
        if (opt.dirs_only  && !(e.flags & kFlagDirectory)) continue;

        if (contains(pool.data() + e.name_offset, e.name_length,
                     q.data(), q.size())) {
            out.push_back(i);
            if (out.size() >= opt.max_results) break;
        }
    }
    return out;
}

} // namespace argus
