#include "core/query.h"

#include <algorithm>
#include <cctype>
#include <cwctype>
#include <cstring>
#include <vector>

#include <windows.h>

namespace argus {

namespace {

// ---------- Helpers ----------

inline wchar_t to_lower(wchar_t c) {
    if (c >= L'A' && c <= L'Z') return c + 32;
    if (c >= 0x00C0 && c <= 0x00DE && c != 0x00D7) return c + 32;
    if (c >= 0x0100 && c <= 0x0137 && (c & 1) == 0) return c + 1;
    if (c >= 0x0139 && c <= 0x0148 && (c & 1) == 1) return c + 1;
    if (c >= 0x014A && c <= 0x0177 && (c & 1) == 0) return c + 1;
    if (c >= 0x0179 && c <= 0x017E && (c & 1) == 1) return c + 1;
    if (c >= 0x0410 && c <= 0x042F) return c + 32;
    if (c >= 0x0400 && c <= 0x040F) return c + 80;
    if (c >= 0x0391 && c <= 0x03A1) return c + 32;
    if (c >= 0x03A3 && c <= 0x03A9) return c + 32;
    return c;
}

std::wstring to_lower_str(std::wstring_view s) {
    std::wstring r(s.begin(), s.end());
    for (auto& c : r) c = to_lower(c);
    return r;
}

bool contains_ci(std::wstring_view hay, std::wstring_view needle_lc) {
    if (needle_lc.empty()) return true;
    if (hay.size() < needle_lc.size()) return false;
    const size_t last = hay.size() - needle_lc.size();
    for (size_t i = 0; i <= last; ++i) {
        size_t j = 0;
        for (; j < needle_lc.size(); ++j) {
            if (to_lower(hay[i + j]) != needle_lc[j]) break;
        }
        if (j == needle_lc.size()) return true;
    }
    return false;
}

// "100MB", "1.5GB", "500K" -> bytes
uint64_t parse_size(std::wstring_view s) {
    double num = 0.0;
    size_t i = 0;
    // Zahl parsen.
    bool has_dot = false;
    while (i < s.size() && ((s[i] >= L'0' && s[i] <= L'9') || (s[i] == L'.' && !has_dot))) {
        if (s[i] == L'.') has_dot = true;
        ++i;
    }
    if (i == 0) return 0;
    std::wstring digits(s.substr(0, i));
    num = _wtof(digits.c_str());

    // Suffix.
    uint64_t mult = 1;
    if (i < s.size()) {
        wchar_t c = to_lower(s[i]);
        switch (c) {
            case L'k': mult = 1024ULL; break;
            case L'm': mult = 1024ULL * 1024; break;
            case L'g': mult = 1024ULL * 1024 * 1024; break;
            case L't': mult = 1024ULL * 1024 * 1024 * 1024; break;
        }
    }
    return uint64_t(num * double(mult));
}

// FILETIME is 100ns since 1601. Delta value from "7d" / "24h" / "30m".
// Returns seconds. Returns 0 if not recognized.
uint64_t parse_delta_seconds(std::wstring_view s) {
    double num = 0.0;
    size_t i = 0;
    while (i < s.size() && ((s[i] >= L'0' && s[i] <= L'9') || s[i] == L'.')) ++i;
    if (i == 0) return 0;
    std::wstring digits(s.substr(0, i));
    num = _wtof(digits.c_str());

    if (i >= s.size()) return uint64_t(num);          // bare seconds
    wchar_t c = to_lower(s[i]);
    switch (c) {
        case L's': return uint64_t(num);
        case L'm': return uint64_t(num * 60);          // minutes
        case L'h': return uint64_t(num * 3600);
        case L'd': return uint64_t(num * 86400);
        case L'w': return uint64_t(num * 604800);
        case L'y': return uint64_t(num * 31557600);
    }
    return uint64_t(num);
}

QueryPredicate::Op parse_op(std::wstring_view s, size_t& consumed) {
    consumed = 0;
    if (s.size() >= 2 && (s[0] == L'>' || s[0] == L'<') && s[1] == L'=') {
        consumed = 2;
        return s[0] == L'>' ? QueryPredicate::GE : QueryPredicate::LE;
    }
    if (s.size() >= 1) {
        if (s[0] == L'>') { consumed = 1; return QueryPredicate::GT; }
        if (s[0] == L'<') { consumed = 1; return QueryPredicate::LT; }
        if (s[0] == L'=') { consumed = 1; return QueryPredicate::EQ; }
    }
    return QueryPredicate::EQ;   // no operator = equal
}

// Extract the extension of a name (without leading dot).
std::wstring_view name_extension(std::wstring_view name) {
    for (size_t i = name.size(); i > 0; --i) {
        if (name[i - 1] == L'.') {
            return name.substr(i);
        }
        if (name[i - 1] == L'\\' || name[i - 1] == L'/') break;
    }
    return {};
}

// Extension-Whitelist fuer Content-Suche (Textformate).
bool is_text_extension(std::wstring_view ext_lc) {
    static const wchar_t* kExts[] = {
        L"txt", L"md", L"rst", L"log", L"csv", L"tsv", L"ini", L"conf", L"cfg",
        L"cpp", L"h", L"hpp", L"c", L"cc", L"cs", L"py", L"js", L"ts", L"jsx",
        L"tsx", L"go", L"rs", L"java", L"rb", L"php", L"pl", L"lua", L"swift",
        L"html", L"htm", L"xml", L"yaml", L"yml", L"toml", L"json", L"css",
        L"scss", L"less", L"sh", L"bat", L"ps1", L"cmake", L"diff", L"patch",
        L"sql", L"env", L"gitignore", L"m", L"mm", L"kt", L"dart", L"vue", L"svelte",
    };
    for (auto* e : kExts) if (ext_lc == e) return true;
    return false;
}

// Case-insensitive Bytes-Substring (ASCII-Fold).
bool bytes_contain_ci(const char* hay, size_t hay_len,
                    const char* needle, size_t needle_len) {
    if (needle_len == 0) return true;
    if (hay_len < needle_len) return false;
    auto tolow = [](char c) -> char {
        return (c >= 'A' && c <= 'Z') ? (c + 32) : c;
    };
    const size_t last = hay_len - needle_len;
    for (size_t i = 0; i <= last; ++i) {
        size_t j = 0;
        for (; j < needle_len; ++j) {
            if (tolow(hay[i + j]) != needle[j]) break;
        }
        if (j == needle_len) return true;
    }
    return false;
}

// Datei lesen (bis 10 MB) und darin nach needle_lc suchen.
bool file_contains(const std::wstring& path, const std::string& needle_lc) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ,
                          FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                          nullptr, OPEN_EXISTING,
                          FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
                          nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    struct Closer { HANDLE h; ~Closer(){ CloseHandle(h); } } cl{h};

    LARGE_INTEGER sz{};
    if (!GetFileSizeEx(h, &sz) || sz.QuadPart == 0) return false;
    constexpr uint64_t kMaxRead = 10ULL * 1024 * 1024;   // 10 MB
    const uint32_t to_read = uint32_t(std::min<uint64_t>(sz.QuadPart, kMaxRead));

    std::vector<char> buf(to_read);
    DWORD got = 0;
    if (!ReadFile(h, buf.data(), to_read, &got, nullptr)) return false;
    return bytes_contain_ci(buf.data(), got, needle_lc.data(), needle_lc.size());
}

// Type -> extension set (all lowercase).
bool ext_in_type(std::wstring_view ext_lc, const std::string& type) {
    static const struct { const char* type; const wchar_t* exts; } kTable[] = {
        {"image",    L"|jpg|jpeg|png|gif|bmp|webp|svg|tif|tiff|heic|ico|"},
        {"video",    L"|mp4|mkv|avi|mov|wmv|flv|webm|m4v|mpg|mpeg|"},
        {"audio",    L"|mp3|wav|ogg|flac|m4a|aac|wma|opus|"},
        {"document", L"|pdf|doc|docx|xls|xlsx|ppt|pptx|odt|txt|md|rtf|"},
        {"archive",  L"|zip|rar|7z|tar|gz|bz2|xz|zst|"},
        {"code",     L"|cpp|h|hpp|c|cc|cs|py|js|ts|jsx|tsx|go|rs|java|rb|php|sh|ps1|bat|"},
        {"exe",      L"|exe|msi|dll|sys|"},
    };
    for (const auto& row : kTable) {
        if (type == row.type) {
            std::wstring needle;
            needle.push_back(L'|');
            needle.append(ext_lc);
            needle.push_back(L'|');
            std::wstring exts = row.exts;
            return exts.find(needle) != std::wstring::npos;
        }
    }
    return false;
}

} // namespace

Query ParseQuery(std::wstring_view input) {
    Query q;

    // Whitespace-Split. Kein Quote-Support fuer MVP — Namen mit Leerzeichen
    // muss man mit Wildcard *foo bar* umgehen.
    size_t i = 0;
    while (i < input.size()) {
        while (i < input.size() && iswspace(input[i])) ++i;
        if (i >= input.size()) break;

        size_t start = i;
        while (i < input.size() && !iswspace(input[i])) ++i;
        std::wstring_view tok = input.substr(start, i - start);
        if (tok.empty()) continue;

        QueryPredicate p;
        if (tok.front() == L'!') {
            p.negate = true;
            tok = tok.substr(1);
            if (tok.empty()) continue;
        }

        // field:value?
        size_t colon = tok.find(L':');
        if (colon == std::wstring_view::npos) {
            p.kind = QueryPredicate::NameContains;
            p.text = to_lower_str(tok);
            q.preds.push_back(std::move(p));
            continue;
        }

        std::wstring field(tok.substr(0, colon));
        std::wstring_view value = tok.substr(colon + 1);
        std::string field_lc;
        for (auto c : field) field_lc.push_back(char(to_lower(c)));

        if (field_lc == "name") {
            p.kind = QueryPredicate::NameContains;
            p.text = to_lower_str(value);
        } else if (field_lc == "ext" || field_lc == "extension") {
            p.kind = QueryPredicate::Extension;
            p.text = to_lower_str(value);
            // Fuehrenden Punkt tolerieren: ext:.pdf oder ext:pdf.
            if (!p.text.empty() && p.text.front() == L'.') p.text.erase(0, 1);
        } else if (field_lc == "type") {
            p.kind = QueryPredicate::TypeCategory;
            std::string cat;
            for (auto c : value) cat.push_back(char(to_lower(c)));
            p.category = std::move(cat);
        } else if (field_lc == "path") {
            p.kind = QueryPredicate::PathContains;
            p.text = to_lower_str(value);
        } else if (field_lc == "content" || field_lc == "contains") {
            p.kind = QueryPredicate::ContentContains;
            std::wstring v = to_lower_str(value);
            // In UTF-8 zum Byte-Vergleich runter.
            int need = WideCharToMultiByte(CP_UTF8, 0, v.data(), (int)v.size(),
                                          nullptr, 0, nullptr, nullptr);
            p.content_lc.assign(need, '\0');
            WideCharToMultiByte(CP_UTF8, 0, v.data(), (int)v.size(),
                                p.content_lc.data(), need, nullptr, nullptr);
        } else if (field_lc == "size") {
            p.kind = QueryPredicate::SizeCompare;
            size_t consumed = 0;
            p.op = parse_op(value, consumed);
            p.numeric = parse_size(value.substr(consumed));
        } else if (field_lc == "modified" || field_lc == "changed") {
            p.kind = QueryPredicate::ModifiedCmp;
            size_t consumed = 0;
            p.op = parse_op(value, consumed);
            std::wstring_view rest = value.substr(consumed);
            // Wenn die Zeichen alle Digit + einheit-suffix sind: als Delta parsen.
            bool looks_like_delta = !rest.empty();
            for (wchar_t c : rest) {
                if (!(iswdigit(c) || iswalpha(c) || c == L'.')) { looks_like_delta = false; break; }
            }
            if (looks_like_delta && rest.find(L'-') == std::wstring_view::npos) {
                p.numeric = parse_delta_seconds(rest);
                p.is_delta = true;
            }
        } else {
            // unbekanntes Feld: als Name-Substring behandeln.
            p.kind = QueryPredicate::NameContains;
            p.text = to_lower_str(tok);
        }
        q.preds.push_back(std::move(p));
    }
    return q;
}

// ---------- Matching ----------

static uint64_t now_filetime_100ns() {
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    ULARGE_INTEGER u;
    u.LowPart  = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    return u.QuadPart;
}

static bool cmp_num(uint64_t val, QueryPredicate::Op op, uint64_t ref) {
    switch (op) {
        case QueryPredicate::EQ: return val == ref;
        case QueryPredicate::LT: return val <  ref;
        case QueryPredicate::LE: return val <= ref;
        case QueryPredicate::GT: return val >  ref;
        case QueryPredicate::GE: return val >= ref;
    }
    return false;
}

bool MatchQuery(const Query& q, const Index& idx, uint32_t entry_idx) {
    if (q.empty()) return true;
    const auto& e = idx.entry(entry_idx);
    if (e.flags & kFlagDeleted) return false;

    const auto name = idx.name(entry_idx);
    const std::wstring name_lc = to_lower_str(name);
    const auto ext = name_extension(name);
    const std::wstring ext_lc = to_lower_str(ext);

    // full_path nur berechnen wenn ein PathContains-Predicate existiert (teuer).
    std::wstring full_path_lc;
    auto need_path = [&](){ return !full_path_lc.empty(); };
    (void)need_path;
    bool path_computed = false;
    auto path = [&]() -> const std::wstring& {
        if (!path_computed) {
            auto p = idx.full_path(entry_idx);
            full_path_lc = to_lower_str(p);
            path_computed = true;
        }
        return full_path_lc;
    };

    const uint64_t now100 = now_filetime_100ns();

    for (const auto& p : q.preds) {
        bool ok = false;
        switch (p.kind) {
            case QueryPredicate::NameContains:
                ok = contains_ci(name_lc, p.text);
                break;
            case QueryPredicate::Extension:
                ok = (ext_lc == p.text);
                break;
            case QueryPredicate::TypeCategory:
                ok = ext_in_type(ext_lc, p.category);
                break;
            case QueryPredicate::PathContains:
                ok = contains_ci(path(), p.text);
                break;
            case QueryPredicate::SizeCompare:
                if (e.flags & kFlagDirectory) { ok = false; break; }  // Ordner haben keine Groesse
                ok = cmp_num(e.size, p.op, p.numeric);
                break;
            case QueryPredicate::ModifiedCmp: {
                if (p.is_delta) {
                    // "modified:<7d" heisst modified NEWER than (now - 7d).
                    // Wir gehen davon aus: <X = innerhalb der letzten X, >X = aelter als X.
                    uint64_t delta_ticks = p.numeric * 10000000ULL; // sec -> 100ns
                    uint64_t threshold = (now100 > delta_ticks) ? (now100 - delta_ticks) : 0;
                    switch (p.op) {
                        case QueryPredicate::LT:
                        case QueryPredicate::LE:
                        case QueryPredicate::EQ:
                            ok = e.modified_time >= threshold;   // "innerhalb der letzten X"
                            break;
                        case QueryPredicate::GT:
                        case QueryPredicate::GE:
                            ok = e.modified_time <= threshold;   // "aelter als X"
                            break;
                    }
                } else {
                    ok = cmp_num(e.modified_time, p.op, p.numeric);
                }
                break;
            }
            case QueryPredicate::CreatedCmp:
                ok = true;  // v0.4 speichert creation_time nicht separat
                break;
            case QueryPredicate::ContentContains: {
                if (e.flags & kFlagDirectory) { ok = false; break; }
                // Nur bei Textformaten - alles andere wuerde stundenlang lesen.
                if (!is_text_extension(ext_lc)) { ok = false; break; }
                // Sinnvoller Groessen-Filter — wenn Datei > 10MB, nur die ersten
                // 10MB werden gelesen, aber Datei-Objekte > 100MB skippen wir
                // ganz um nicht auf Riesen-Logs zu warten.
                if (e.size > 100ULL * 1024 * 1024) { ok = false; break; }
                ok = file_contains(idx.full_path(entry_idx), p.content_lc);
                break;
            }
        }
        if (p.negate) ok = !ok;
        if (!ok) return false;
    }
    return true;
}

} // namespace argus
