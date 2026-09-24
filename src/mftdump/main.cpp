// mftdump — Argus diagnostic CLI.
//
// Default:  Boot-Sektor + MFT-Record 0 Header + Attribute-Liste.
// --all:    zusaetzlich durch ALLE MFT-Records iterieren, Dateien zaehlen,
//           erste N Namen ausgeben.
//
// Braucht Administrator-Rechte (Manifest sorgt fuer UAC-Prompt).

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "core/ntfs.h"

namespace {

// Chunk-Groesse fuer sequentielles MFT-Lesen. 4 MB = 4096 Records auf einmal.
constexpr uint32_t kReadChunkBytes = 4 * 1024 * 1024;

// Erste N Dateinamen die wir im --all-Mode ausgeben.
constexpr uint32_t kPrintFirstN   = 30;

const char* attr_type_name(uint32_t t) {
    switch (t) {
        case argus::ntfs::kAttrStandardInformation: return "$STANDARD_INFORMATION";
        case argus::ntfs::kAttrAttributeList:       return "$ATTRIBUTE_LIST";
        case argus::ntfs::kAttrFileName:            return "$FILE_NAME";
        case argus::ntfs::kAttrData:                return "$DATA";
        case 0x40: return "$OBJECT_ID";
        case 0x50: return "$SECURITY_DESCRIPTOR";
        case 0x60: return "$VOLUME_NAME";
        case 0x70: return "$VOLUME_INFORMATION";
        case 0x90: return "$INDEX_ROOT";
        case 0xA0: return "$INDEX_ALLOCATION";
        case 0xB0: return "$BITMAP";
        default:   return "$?";
    }
}

bool read_at(HANDLE h, uint64_t abs_offset, void* buf, DWORD len) {
    LARGE_INTEGER li;
    li.QuadPart = static_cast<LONGLONG>(abs_offset);
    if (!SetFilePointerEx(h, li, nullptr, FILE_BEGIN)) return false;
    DWORD got = 0;
    if (!ReadFile(h, buf, len, &got, nullptr)) return false;
    return got == len;
}

std::string narrow(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(),
                                nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(),
                        s.data(), n, nullptr, nullptr);
    return s;
}

int usage() {
    std::fprintf(stderr,
        "mftdump — Argus diagnostic CLI\n"
        "Usage:\n"
        "  mftdump <drive-letter>          — boot sector + MFT record 0\n"
        "  mftdump <drive-letter> --all    — iterate ALL MFT records\n"
        "\n"
        "Requires Administrator (opens raw NTFS volume).\n");
    return 2;
}

// Sucht in einem MFT-Record die bevorzugte $FILE_NAME (Win32 oder Win32+DOS).
// Faellt auf jeden vorhandenen FILE_NAME zurueck.
std::optional<argus::ntfs::FileNameInfo>
best_file_name(const uint8_t* record, size_t record_size) {
    std::optional<argus::ntfs::FileNameInfo> preferred;
    std::optional<argus::ntfs::FileNameInfo> fallback;
    argus::ntfs::WalkAttributes(record, record_size,
        [&](const argus::ntfs::AttributeHeader* a) {
            if (a->type != argus::ntfs::kAttrFileName) return true;
            auto info = argus::ntfs::ReadFileName(a);
            if (!info) return true;
            if (info->ns == argus::ntfs::kNsWin32 ||
                info->ns == argus::ntfs::kNsWin32AndDos ||
                info->ns == argus::ntfs::kNsPosix) {
                preferred = std::move(info);
                return false;                 // gut genug, abbrechen
            }
            if (!fallback) fallback = std::move(info);
            return true;
        });
    return preferred ? preferred : fallback;
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    SetConsoleOutputCP(CP_UTF8);
    if (argc < 2 || wcslen(argv[1]) < 1) return usage();
    const bool all_mode = (argc >= 3 && wcscmp(argv[2], L"--all") == 0);

    wchar_t drive = argv[1][0];
    if ((drive < L'A' || drive > L'Z') && (drive < L'a' || drive > L'z')) return usage();
    if (drive >= L'a') drive -= 32;

    wchar_t path[16];
    swprintf(path, 16, L"\\\\.\\%c:", drive);
    std::wprintf(L"Opening %ls (raw)…\n", path);

    HANDLE h = CreateFileW(
        path,
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        DWORD err = GetLastError();
        std::fprintf(stderr, "CreateFileW failed: 0x%08lX ", err);
        if (err == ERROR_ACCESS_DENIED)
            std::fprintf(stderr, "(als Administrator ausfuehren)\n");
        else
            std::fprintf(stderr, "\n");
        return 1;
    }

    argus::ntfs::BootSector bs{};
    if (!read_at(h, 0, &bs, sizeof(bs))) {
        std::fprintf(stderr, "Konnte Boot-Sektor nicht lesen: 0x%08lX\n", GetLastError());
        CloseHandle(h);
        return 1;
    }
    if (std::memcmp(bs.oem_id, "NTFS    ", 8) != 0) {
        std::fprintf(stderr, "Volume ist nicht NTFS (OEM = \"%.8s\")\n", bs.oem_id);
        CloseHandle(h);
        return 1;
    }

    auto g = argus::ntfs::ParseBootSector(bs);
    std::printf("\n== Volume Geometry ==\n");
    std::printf("  Bytes per sector   : %u\n", g.bytes_per_sector);
    std::printf("  Sectors per cluster: %u\n", g.sectors_per_cluster);
    std::printf("  Bytes per cluster  : %u\n", g.bytes_per_cluster);
    std::printf("  Bytes per MFT rec  : %u\n", g.bytes_per_mft_record);
    std::printf("  MFT byte offset    : 0x%llX\n", (unsigned long long)g.mft_byte_offset);

    // ---------- MFT-Record 0 lesen ----------
    std::vector<uint8_t> rec0(g.bytes_per_mft_record);
    if (!read_at(h, g.mft_byte_offset, rec0.data(), (DWORD)rec0.size())) {
        std::fprintf(stderr, "Konnte MFT-Record 0 nicht lesen: 0x%08lX\n", GetLastError());
        CloseHandle(h);
        return 1;
    }
    if (!argus::ntfs::ApplyFixup(rec0.data(), rec0.size(), g.bytes_per_sector)) {
        std::fprintf(stderr, "Fixup fehlgeschlagen.\n");
        CloseHandle(h);
        return 1;
    }

    // ---------- $DATA-Attribut aus Record 0 finden ----------
    const argus::ntfs::NonResidentAttribute* data_attr = nullptr;
    argus::ntfs::WalkAttributes(rec0.data(), rec0.size(),
        [&](const argus::ntfs::AttributeHeader* a) {
            if (a->type == argus::ntfs::kAttrData && a->non_resident) {
                data_attr = reinterpret_cast<const argus::ntfs::NonResidentAttribute*>(a);
                return false;
            }
            return true;
        });
    if (!data_attr) {
        std::fprintf(stderr, "Kein non-resident $DATA im MFT-Record 0.\n");
        CloseHandle(h);
        return 1;
    }

    const uint8_t* rl_ptr = reinterpret_cast<const uint8_t*>(data_attr) + data_attr->runlist_offset;
    const size_t   rl_max = data_attr->hdr.length - data_attr->runlist_offset;
    auto runs = argus::ntfs::DecodeRunlist(rl_ptr, rl_max);
    std::printf("  MFT data_size      : %llu bytes\n", (unsigned long long)data_attr->data_size);
    std::printf("  MFT runs           : %zu\n", runs.size());
    for (size_t i = 0; i < runs.size() && i < 8; ++i) {
        std::printf("    run[%zu]: vcn=%llu len=%llu lcn=%lld%s\n",
            i,
            (unsigned long long)runs[i].vcn_start,
            (unsigned long long)runs[i].length,
            (long long)runs[i].lcn,
            runs[i].lcn < 0 ? " (SPARSE)" : "");
    }
    if (runs.size() > 8) std::printf("    … %zu weitere Runs\n", runs.size() - 8);

    if (!all_mode) {
        // Kurz-Modus: nur Record-0-Attribute anzeigen und raus.
        std::printf("\n== MFT Record 0 Attributes ==\n");
        argus::ntfs::WalkAttributes(rec0.data(), rec0.size(),
            [](const argus::ntfs::AttributeHeader* a) {
                std::printf("  type=0x%02X (%-22s) len=%u %s\n",
                    a->type, attr_type_name(a->type), a->length,
                    a->non_resident ? "non-resident" : "resident");
                return true;
            });
        std::printf("\n(fuer volle Iteration:  mftdump %C --all)\n", drive);
        CloseHandle(h);
        return 0;
    }

    // ---------- --all: ALLE MFT-Records durchlaufen ----------
    const uint64_t total_records = data_attr->data_size / g.bytes_per_mft_record;
    std::printf("\n== Iterating %llu MFT records ==\n\n", (unsigned long long)total_records);

    std::vector<uint8_t> buffer(kReadChunkBytes);
    const uint32_t records_per_chunk = kReadChunkBytes / g.bytes_per_mft_record;
    const uint32_t clusters_per_chunk = kReadChunkBytes / g.bytes_per_cluster;

    uint64_t records_seen = 0, files = 0, dirs = 0, unused = 0, bad_signature = 0, fixup_failed = 0;
    uint32_t printed = 0;

    auto t0 = std::chrono::steady_clock::now();

    for (const auto& run : runs) {
        if (run.lcn < 0) continue;                        // sparse
        for (uint64_t c = 0; c < run.length; c += clusters_per_chunk) {
            const uint64_t chunk_clusters = std::min<uint64_t>(clusters_per_chunk,
                                                              run.length - c);
            const uint64_t abs_off  = (uint64_t(run.lcn) + c) * g.bytes_per_cluster;
            const uint32_t read_len = uint32_t(chunk_clusters * g.bytes_per_cluster);
            if (!read_at(h, abs_off, buffer.data(), read_len)) {
                std::fprintf(stderr, "\nread failed @0x%llX: 0x%08lX\n",
                    (unsigned long long)abs_off, GetLastError());
                break;
            }
            const uint32_t recs_here = read_len / g.bytes_per_mft_record;
            for (uint32_t i = 0; i < recs_here && records_seen < total_records;
                 ++i, ++records_seen) {
                uint8_t* rec = buffer.data() + i * g.bytes_per_mft_record;
                if (std::memcmp(rec, "FILE", 4) != 0) { ++bad_signature; continue; }
                if (!argus::ntfs::ApplyFixup(rec, g.bytes_per_mft_record, g.bytes_per_sector)) {
                    ++fixup_failed;
                    continue;
                }
                const auto* h_rec = reinterpret_cast<const argus::ntfs::MftRecordHeader*>(rec);
                if (!(h_rec->flags & argus::ntfs::kMftFlagInUse)) { ++unused; continue; }
                if (h_rec->flags & argus::ntfs::kMftFlagDirectory) ++dirs; else ++files;

                if (printed < kPrintFirstN) {
                    auto name = best_file_name(rec, g.bytes_per_mft_record);
                    if (name) {
                        std::printf("  #%08llu  parent=%08llu  %s%s\n",
                            (unsigned long long)records_seen,
                            (unsigned long long)name->parent_record,
                            narrow(name->name).c_str(),
                            (h_rec->flags & argus::ntfs::kMftFlagDirectory) ? "/" : "");
                        ++printed;
                    }
                }
            }
            // Progress alle ~1 %.
            if (records_seen % (total_records / 100 + 1) == 0) {
                std::printf("\r  … %llu / %llu (%.1f %%)   ",
                    (unsigned long long)records_seen,
                    (unsigned long long)total_records,
                    100.0 * records_seen / total_records);
                std::fflush(stdout);
            }
        }
    }

    auto t1 = std::chrono::steady_clock::now();
    double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

    std::printf("\r%-70s\r", "");
    std::printf("\n== Summary ==\n");
    std::printf("  Records durchgelaufen: %llu\n", (unsigned long long)records_seen);
    std::printf("  Dateien              : %llu\n", (unsigned long long)files);
    std::printf("  Verzeichnisse        : %llu\n", (unsigned long long)dirs);
    std::printf("  Frei / unused        : %llu\n", (unsigned long long)unused);
    std::printf("  Bad signature (skip) : %llu\n", (unsigned long long)bad_signature);
    std::printf("  Fixup-Fehler         : %llu\n", (unsigned long long)fixup_failed);
    std::printf("  Zeit                 : %.1f ms (%.1f k rec/s)\n",
        ms, records_seen / (ms / 1000.0) / 1000.0);

    CloseHandle(h);
    return 0;
}
