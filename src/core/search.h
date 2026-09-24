// Argus — Case-insensitive Substring-Search ueber einen Index.

#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "core/index.h"

namespace argus {

struct SearchOptions {
    size_t max_results = 5000;
    // Falls true, matched nur Dateien (keine Ordner).
    bool   files_only  = false;
    bool   dirs_only   = false;
};

// Fuehrt eine Suche aus. Kann durch cancel abgebrochen werden (fuer async-Use).
// Gibt die Entry-Indices der Treffer zurueck.
std::vector<uint32_t> Search(const Index& index,
                             std::wstring_view query,
                             const SearchOptions& opt = {},
                             std::atomic<bool>* cancel = nullptr);

} // namespace argus
