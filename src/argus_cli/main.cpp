// argus-cli.exe — console interface to Argus.
// Uses the persistent index cache written by the GUI (argus.exe). If no cache
// exists yet, prints an instruction to run the GUI once so it can index.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "core/index.h"
#include "core/multi_index.h"
#include "core/query.h"
#include "core/search.h"

namespace {

std::string narrow(std::wstring_view w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

std::vector<wchar_t> ntfs_drives() {
    std::vector<wchar_t> out;
    DWORD mask = GetLogicalDrives();
    for (int i = 0; i < 26; ++i) {
        if (!(mask & (1u << i))) continue;
        wchar_t root[] = { wchar_t(L'A' + i), L':', L'\\', 0 };
        if (GetDriveTypeW(root) != DRIVE_FIXED) continue;
        wchar_t fs[16] = {0};
        if (!GetVolumeInformationW(root, nullptr, 0, nullptr, nullptr, nullptr, fs, 16)) continue;
        if (wcscmp(fs, L"NTFS") != 0) continue;
        out.push_back(wchar_t(L'A' + i));
    }
    return out;
}

int usage() {
    std::fprintf(stderr,
        "argus-cli — command-line search over the Argus index\n"
        "\n"
        "Usage:\n"
        "  argus-cli find [--drive C] [--limit N] <query>\n"
        "  argus-cli list-drives\n"
        "  argus-cli help\n"
        "\n"
        "Query syntax (same as the GUI):\n"
        "  bare words            match filename substring\n"
        "  ext:pdf               file extension\n"
        "  type:image|video|...  semantic categories\n"
        "  path:downloads        path substring\n"
        "  size:>100MB           K/M/G/T suffixes and ><= operators\n"
        "  modified:<7d          s/m/h/d/w/y suffixes\n"
        "  !term                 negate a predicate\n"
        "\n"
        "Notes:\n"
        "  * The index is built by the GUI (argus.exe). Run it once as\n"
        "    Administrator so a cache is written to %%LOCALAPPDATA%%\\Argus\\.\n"
        "  * argus-cli only reads the cache — it never needs elevation.\n");
    return 2;
}

int cmd_list_drives() {
    for (wchar_t d : ntfs_drives()) std::printf("%c:\n", char(d));
    return 0;
}

int cmd_find(int argc, wchar_t** argv) {
    std::vector<wchar_t> drives;
    size_t limit = 200;
    std::wstring query;

    for (int i = 0; i < argc; ++i) {
        std::wstring_view a = argv[i];
        if (a == L"--drive" && i + 1 < argc) {
            wchar_t d = argv[++i][0];
            if (d >= L'a' && d <= L'z') d -= 32;
            drives.push_back(d);
        } else if (a == L"--limit" && i + 1 < argc) {
            limit = size_t(_wtoi64(argv[++i]));
        } else {
            if (!query.empty()) query.push_back(L' ');
            query.append(a);
        }
    }
    if (query.empty()) return usage();
    if (drives.empty()) drives = ntfs_drives();
    if (drives.empty()) {
        std::fprintf(stderr, "No NTFS drives found.\n");
        return 1;
    }

    // Split raw query into name part + advanced part (same rule as GUI).
    std::wstring name_part, adv_part;
    size_t pos = 0;
    while (pos < query.size()) {
        while (pos < query.size() && iswspace(query[pos])) ++pos;
        if (pos >= query.size()) break;
        size_t s = pos;
        while (pos < query.size() && !iswspace(query[pos])) ++pos;
        std::wstring_view tok(query.data() + s, pos - s);
        bool has_colon = tok.find(L':') != std::wstring_view::npos;
        bool has_bang  = !tok.empty() && tok.front() == L'!';
        if (has_colon || has_bang) {
            if (!adv_part.empty()) adv_part.push_back(L' ');
            adv_part.append(tok);
        } else {
            if (!name_part.empty()) name_part.push_back(L' ');
            name_part.append(tok);
        }
    }
    argus::Query adv = argus::ParseQuery(adv_part);

    size_t emitted = 0;
    for (wchar_t d : drives) {
        auto cache = argus::CacheFilePath(d);
        if (cache.empty()) continue;
        argus::Index idx;
        if (!idx.LoadFrom(cache)) {
            std::fprintf(stderr,
                "No cache for %c:. Run the GUI (argus.exe) once as Administrator.\n",
                char(d));
            continue;
        }
        argus::SearchOptions opt;
        opt.max_results = limit - emitted;
        opt.advanced_query = adv.empty() ? nullptr : &adv;
        auto ids = argus::Search(idx, name_part, opt);
        for (uint32_t id : ids) {
            auto p = idx.full_path(id);
            std::printf("%s\n", narrow({p.data(), p.size()}).c_str());
            if (++emitted >= limit) return 0;
        }
    }
    return 0;
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    SetConsoleOutputCP(CP_UTF8);
    if (argc < 2) return usage();
    std::wstring_view cmd = argv[1];
    if (cmd == L"help" || cmd == L"-h" || cmd == L"--help") return usage() == 2 ? 0 : 0;
    if (cmd == L"list-drives") return cmd_list_drives();
    if (cmd == L"find")        return cmd_find(argc - 2, argv + 2);
    return usage();
}
