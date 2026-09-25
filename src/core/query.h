// Argus — Query parser.
// Accepts strings like:
//   report ext:pdf size:>100MB modified:<7d
//   type:image path:downloads
//   !node_modules ext:cpp
// Whitespace-separated tokens are all AND'd. Prefix "!" negates a predicate.

#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "core/index.h"

namespace argus {

struct QueryPredicate {
    enum Kind {
        NameContains    = 0,
        Extension       = 1,   // ext:
        TypeCategory    = 2,   // type:image, type:video, ...
        PathContains    = 3,   // path:
        SizeCompare     = 4,   // size:>100MB
        ModifiedCmp     = 5,   // modified:<7d  (delta) or modified:>2026-01-01
        CreatedCmp      = 6,   // future — not populated yet
        ContentContains = 7,   // content:foo — search inside text files
    };
    enum Op { EQ = 0, LT = 1, LE = 2, GT = 3, GE = 4 };

    Kind         kind    = NameContains;
    bool         negate  = false;
    std::wstring text;               // for Name/Path/Ext/Type
    std::string  content_lc;         // lowercase UTF-8 for ContentContains
    std::string  category;           // "image", "video", ...
    Op           op      = EQ;
    uint64_t     numeric = 0;        // size in bytes, or FILETIME 100ns ticks
    bool         is_delta = false;   // for ModifiedCmp: value is "seconds ago"
};

struct Query {
    std::vector<QueryPredicate> preds;   // all AND'd

    bool empty() const { return preds.empty(); }
};

// Parses a raw query string into a Query object. Never fails: unknown fields
// fall through as plain name substring predicates so users always get some
// results.
Query ParseQuery(std::wstring_view input);

// Evaluate a query against a single entry. Uses idx.full_path only when a
// path: predicate is present.
bool MatchQuery(const Query& q, const Index& idx, uint32_t entry_idx);

} // namespace argus
