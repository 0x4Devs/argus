// Argus — Case-insensitive Substring-Search ueber einen Index.

#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "core/index.h"

namespace argus {

enum class SearchMode {
    Substring = 0,   // "foo"      matches any name containing "foo"
    Wildcard  = 1,   // "*.mp4"    * = any, ? = one char
    Regex     = 2,   // ".*\\.mp4$" full ECMAScript regex
};

struct SearchOptions {
    size_t     max_results = 5000;
    bool       files_only  = false;
    bool       dirs_only   = false;
    SearchMode mode        = SearchMode::Substring;
};

std::vector<uint32_t> Search(const Index& index,
                             std::wstring_view query,
                             const SearchOptions& opt = {},
                             std::atomic<bool>* cancel = nullptr);

} // namespace argus
