#include "core/index.h"

#include <windows.h>

#include <algorithm>
#include <cstring>
#include <memory>

#include "core/ntfs.h"

namespace argus {

namespace {

constexpr uint32_t kNoEntry           = UINT32_MAX;
constexpr uint32_t kRootMftId         = 5;               // Root-Verzeichnis
constexpr uint32_t kReadChunkBytes    = 8 * 1024 * 1024; // 8 MB pro Read
constexpr uint32_t kNamePoolInitial   = 32 * 1024 * 1024; // 32 MB reservieren
constexpr uint32_t kEntriesInitial    = 256 * 1024;

// Wie ReadFileName in ntfs.cpp aber ohne allocation: schreibt Name direkt
// in einen gemeinsamen Pool und gibt Offset + Length zurueck.
struct BestName {
    uint32_t parent_mft = 0;
    uint32_t name_offset = 0;
    uint16_t name_length = 0;
    uint16_t flags = 0;
    uint64_t size = 0;
    uint64_t modified = 0;
    uint8_t  ns_priority = 255;   // niedriger = besser
    bool     found = false;
};

// Nur die "besten" FILE_NAME-Attribute des Records extrahieren.
// Priority: Win32AndDos (0) < Win32 (1) < Posix (2) < DOS (3).
// Standardmaessig FILE_NAME hat kompakte Metadaten fuer Size/Time — die
// uebernehmen wir auch (statt zusaetzlich STANDARD_INFORMATION zu lesen).
BestName ExtractBestName(const uint8_t* record, size_t record_size,
                        std::vector<wchar_t>& name_pool) {
    BestName out;
    ntfs::WalkAttributes(record, record_size, [&](const ntfs::AttributeHeader* a) {
        if (a->type != ntfs::kAttrFileName || a->non_resident) return true;
        const auto* r = reinterpret_cast<const ntfs::ResidentAttribute*>(a);
        if (r->value_length < sizeof(ntfs::FileNameAttribute)) return true;
        const auto* fn = reinterpret_cast<const ntfs::FileNameAttribute*>(
            reinterpret_cast<const uint8_t*>(a) + r->value_offset);
        const uint32_t name_bytes = uint32_t(fn->name_length) * 2u;
        if (sizeof(ntfs::FileNameAttribute) + name_bytes > r->value_length) return true;

        uint8_t prio = 255;
        switch (fn->namespace_code) {
            case ntfs::kNsWin32AndDos: prio = 0; break;
            case ntfs::kNsWin32:       prio = 1; break;
            case ntfs::kNsPosix:       prio = 2; break;
            case ntfs::kNsDos:         prio = 3; break;
        }
        if (prio >= out.ns_priority) return true; // schon besser

        const wchar_t* name_ptr = reinterpret_cast<const wchar_t*>(
            reinterpret_cast<const uint8_t*>(fn) + sizeof(ntfs::FileNameAttribute));
        const uint32_t off = uint32_t(name_pool.size());
        name_pool.insert(name_pool.end(), name_ptr, name_ptr + fn->name_length);

        out.parent_mft   = uint32_t(fn->parent_ref & 0x0000FFFFFFFFULL);
        out.name_offset  = off;
        out.name_length  = fn->name_length;
        out.flags        = (fn->file_attributes & 0x10000000u) ? kFlagDirectory : 0;
        out.size         = fn->data_size;
        out.modified     = fn->modified_time;
        out.ns_priority  = prio;
        out.found        = true;
        return true; // weiter nach besserem suchen
    });
    return out;
}

bool ReadAt(HANDLE h, uint64_t offset, void* buf, DWORD len) {
    LARGE_INTEGER li;
    li.QuadPart = static_cast<LONGLONG>(offset);
    if (!SetFilePointerEx(h, li, nullptr, FILE_BEGIN)) return false;
    DWORD got = 0;
    if (!ReadFile(h, buf, len, &got, nullptr)) return false;
    return got == len;
}

} // namespace

std::wstring_view Index::name(uint32_t i) const {
    const auto& e = entries_[i];
    return std::wstring_view(name_pool_.data() + e.name_offset, e.name_length);
}

std::wstring Index::full_path(uint32_t i) const {
    // Parent-Kette bis Root laufen, dann umkehren.
    const uint32_t kMaxDepth = 64;
    std::wstring parts[kMaxDepth];
    uint32_t depth = 0;

    // Aktuellen Namen zuerst.
    parts[depth++] = std::wstring(name(i));
    uint32_t parent = entries_[i].parent_mft;
    // Sicherheitshalber: verhindere Endlosschleifen wenn die Kette gebrochen ist.
    while (parent != kRootMftId && depth < kMaxDepth) {
        if (parent >= mft_to_idx_.size()) break;
        uint32_t pidx = mft_to_idx_[parent];
        if (pidx == kNoEntry) break;
        parts[depth++] = std::wstring(name(pidx));
        parent = entries_[pidx].parent_mft;
    }

    std::wstring out;
    out.reserve(4 + depth * 12);
    out.push_back(drive_letter_);
    out.append(L":\\");
    for (uint32_t k = depth; k > 0; --k) {
        out.append(parts[k - 1]);
        if (k > 1) out.push_back(L'\\');
    }
    return out;
}

bool Index::ScanDrive(wchar_t drive, ScanStats* stats) {
    if ((drive < L'A' || drive > L'Z') && (drive < L'a' || drive > L'z')) return false;
    if (drive >= L'a') drive -= 32;
    drive_letter_ = drive;

    entries_.clear();
    name_pool_.clear();
    mft_to_idx_.clear();
    entries_.reserve(kEntriesInitial);
    name_pool_.reserve(kNamePoolInitial);

    wchar_t path[16];
    swprintf(path, 16, L"\\\\.\\%c:", drive);
    HANDLE h = CreateFileW(path, GENERIC_READ,
                          FILE_SHARE_READ | FILE_SHARE_WRITE,
                          nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;

    struct Closer { HANDLE h; ~Closer(){ CloseHandle(h); } } closer{h};

    // Boot-Sektor lesen.
    ntfs::BootSector bs{};
    if (!ReadAt(h, 0, &bs, sizeof(bs))) return false;
    if (std::memcmp(bs.oem_id, "NTFS    ", 8) != 0) return false;
    auto g = ntfs::ParseBootSector(bs);

    // MFT-Record 0 lesen.
    std::vector<uint8_t> rec0(g.bytes_per_mft_record);
    if (!ReadAt(h, g.mft_byte_offset, rec0.data(), (DWORD)rec0.size())) return false;
    if (!ntfs::ApplyFixup(rec0.data(), rec0.size(), g.bytes_per_sector)) return false;

    // Nicht-residentes $DATA in Record 0 finden.
    const ntfs::NonResidentAttribute* data_attr = nullptr;
    ntfs::WalkAttributes(rec0.data(), rec0.size(),
        [&](const ntfs::AttributeHeader* a) {
            if (a->type == ntfs::kAttrData && a->non_resident) {
                data_attr = reinterpret_cast<const ntfs::NonResidentAttribute*>(a);
                return false;
            }
            return true;
        });
    if (!data_attr) return false;

    // Runlist decodieren.
    const uint8_t* rl_ptr = reinterpret_cast<const uint8_t*>(data_attr) + data_attr->runlist_offset;
    const size_t   rl_max = data_attr->hdr.length - data_attr->runlist_offset;
    auto runs = ntfs::DecodeRunlist(rl_ptr, rl_max);
    if (runs.empty()) return false;

    const uint64_t total_records = data_attr->data_size / g.bytes_per_mft_record;
    if (stats) stats->total_records.store(total_records);

    // mft_to_idx auf total_records dimensionieren.
    mft_to_idx_.assign(size_t(total_records), kNoEntry);

    // Buffer fuer chunked Reads.
    std::vector<uint8_t> buf(kReadChunkBytes);
    const uint32_t clusters_per_chunk = kReadChunkBytes / g.bytes_per_cluster;

    uint64_t records_seen = 0;

    for (const auto& run : runs) {
        if (run.lcn < 0) continue;
        for (uint64_t c = 0; c < run.length; c += clusters_per_chunk) {
            const uint64_t chunk_clusters = std::min<uint64_t>(clusters_per_chunk,
                                                              run.length - c);
            const uint64_t abs_off  = (uint64_t(run.lcn) + c) * g.bytes_per_cluster;
            const uint32_t read_len = uint32_t(chunk_clusters * g.bytes_per_cluster);
            if (!ReadAt(h, abs_off, buf.data(), read_len)) return false;
            if (stats) stats->bytes_read.fetch_add(read_len);

            const uint32_t recs_here = read_len / g.bytes_per_mft_record;
            for (uint32_t k = 0; k < recs_here && records_seen < total_records;
                 ++k, ++records_seen) {
                uint8_t* rec = buf.data() + k * g.bytes_per_mft_record;
                if (std::memcmp(rec, "FILE", 4) != 0) continue;
                if (!ntfs::ApplyFixup(rec, g.bytes_per_mft_record, g.bytes_per_sector)) continue;

                const auto* h_rec = reinterpret_cast<const ntfs::MftRecordHeader*>(rec);
                if (!(h_rec->flags & ntfs::kMftFlagInUse)) continue;
                // Extension-Records ohne eigene FILE_NAME skippen (base_record != 0).
                if (h_rec->base_record != 0) continue;

                BestName bn = ExtractBestName(rec, g.bytes_per_mft_record, name_pool_);
                if (!bn.found) continue;
                if (h_rec->flags & ntfs::kMftFlagDirectory) bn.flags |= kFlagDirectory;

                Entry e{};
                e.parent_mft    = bn.parent_mft;
                e.name_offset   = bn.name_offset;
                e.name_length   = bn.name_length;
                e.flags         = bn.flags;
                e.size          = bn.size;
                e.modified_time = bn.modified;

                const uint32_t idx = uint32_t(entries_.size());
                entries_.push_back(e);
                if (records_seen < mft_to_idx_.size()) {
                    mft_to_idx_[records_seen] = idx;
                }

                if (stats) stats->entries.fetch_add(1);
            }
            if (stats) stats->records_seen.store(records_seen);
        }
    }

    if (stats) {
        stats->records_seen.store(records_seen);
        stats->ok.store(true);
        stats->done.store(true);
    }
    return true;
}

} // namespace argus
