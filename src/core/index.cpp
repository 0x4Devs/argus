#include "core/index.h"

#include <windows.h>
#include <winioctl.h>
#include <shlobj.h>

#include <algorithm>
#include <cstdio>
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

uint32_t Index::mft_id_of(uint32_t entry_idx) const {
    for (size_t i = 0; i < mft_to_idx_.size(); ++i)
        if (mft_to_idx_[i] == entry_idx) return uint32_t(i);
    return UINT32_MAX;
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

    // Volume Serial + Geometrie + Runlist fuer On-demand-Reads merken.
    volume_serial_ = bs.volume_serial;
    geometry_.bytes_per_sector     = g.bytes_per_sector;
    geometry_.bytes_per_cluster    = g.bytes_per_cluster;
    geometry_.bytes_per_mft_record = g.bytes_per_mft_record;
    mft_runs_ = runs;

    // USN Journal Zustand abfragen — falls das Journal noch nicht existiert,
    // versuchen es zu erzeugen. Fehlschlag ist nicht fatal (Live-Updates dann
    // deaktiviert, Suche funktioniert trotzdem).
    USN_JOURNAL_DATA jd{};
    DWORD ret = 0;
    if (DeviceIoControl(h, FSCTL_QUERY_USN_JOURNAL, nullptr, 0,
                       &jd, sizeof(jd), &ret, nullptr)) {
        usn_journal_id_ = jd.UsnJournalID;
        next_usn_       = jd.NextUsn;
    } else {
        CREATE_USN_JOURNAL_DATA cj{};
        cj.MaximumSize   = 32ULL * 1024 * 1024;
        cj.AllocationDelta = 8ULL * 1024 * 1024;
        if (DeviceIoControl(h, FSCTL_CREATE_USN_JOURNAL, &cj, sizeof(cj),
                           nullptr, 0, &ret, nullptr) &&
            DeviceIoControl(h, FSCTL_QUERY_USN_JOURNAL, nullptr, 0,
                           &jd, sizeof(jd), &ret, nullptr)) {
            usn_journal_id_ = jd.UsnJournalID;
            next_usn_       = jd.NextUsn;
        }
    }

    if (stats) {
        stats->records_seen.store(records_seen);
        stats->ok.store(true);
        stats->done.store(true);
    }
    return true;
}

// ================== USN Journal ========================================

Index::UsnStats Index::ApplyUsnChanges() {
    UsnStats st{};
    if (drive_letter_ == 0 || usn_journal_id_ == 0) return st;

    wchar_t path[16];
    swprintf(path, 16, L"\\\\.\\%c:", drive_letter_);
    HANDLE h = CreateFileW(path, GENERIC_READ,
                          FILE_SHARE_READ | FILE_SHARE_WRITE,
                          nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return st;
    struct Closer { HANDLE h; ~Closer(){ CloseHandle(h); } } closer{h};

    std::vector<uint8_t> buf(64 * 1024);
    READ_USN_JOURNAL_DATA in{};
    in.UsnJournalID    = usn_journal_id_;
    in.StartUsn        = next_usn_;
    in.ReasonMask      = USN_REASON_FILE_CREATE | USN_REASON_FILE_DELETE
                        | USN_REASON_RENAME_NEW_NAME | USN_REASON_CLOSE;
    in.ReturnOnlyOnClose = 1;
    in.Timeout         = 0;
    in.BytesToWaitFor  = 0;

    // Bounded loop damit wir bei sehr aktiven Volumes nicht ewig blockieren.
    int rounds = 0;
    while (rounds++ < 16) {
        DWORD got = 0;
        BOOL ok = DeviceIoControl(h, FSCTL_READ_USN_JOURNAL, &in, sizeof(in),
                                  buf.data(), (DWORD)buf.size(), &got, nullptr);
        if (!ok) {
            DWORD err = GetLastError();
            if (err == ERROR_JOURNAL_ENTRY_DELETED) st.rolled_over = true;
            break;
        }
        if (got <= sizeof(USN)) break;    // nur die naechste USN, kein Record

        // Erste 8 Bytes = naechste USN.
        USN next = *reinterpret_cast<USN*>(buf.data());
        size_t off = sizeof(USN);
        while (off + sizeof(USN_RECORD) <= got) {
            auto* r = reinterpret_cast<USN_RECORD*>(buf.data() + off);
            if (r->RecordLength == 0) break;
            if (r->MajorVersion != 2) { off += r->RecordLength; continue; }

            const uint32_t mft_id    = uint32_t(r->FileReferenceNumber & 0xFFFFFFFFULL);
            const uint32_t parent_id = uint32_t(r->ParentFileReferenceNumber & 0xFFFFFFFFULL);
            const wchar_t* name_ptr  = reinterpret_cast<const wchar_t*>(
                reinterpret_cast<uint8_t*>(r) + r->FileNameOffset);
            const uint16_t name_len  = r->FileNameLength / 2u;
            const bool is_dir        = (r->FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
            const uint64_t timestamp = uint64_t(r->TimeStamp.QuadPart);

            const bool created  = (r->Reason & USN_REASON_FILE_CREATE)    != 0;
            const bool renamed  = (r->Reason & USN_REASON_RENAME_NEW_NAME) != 0;
            const bool deleted  = (r->Reason & USN_REASON_FILE_DELETE)    != 0;

            // Alten Eintrag suchen (falls existiert).
            uint32_t idx = UINT32_MAX;
            if (mft_id < mft_to_idx_.size()) idx = mft_to_idx_[mft_id];

            if (deleted && idx != UINT32_MAX) {
                entries_[idx].flags |= kFlagDeleted;
                st.deleted++;
            } else if (created && idx == UINT32_MAX) {
                // Neuer Eintrag: Name in Pool, Entry anhaengen, mft_to_idx aktualisieren.
                Entry e{};
                e.parent_mft    = parent_id;
                e.name_offset   = uint32_t(name_pool_.size());
                name_pool_.insert(name_pool_.end(), name_ptr, name_ptr + name_len);
                e.name_length   = name_len;
                e.flags         = is_dir ? kFlagDirectory : 0;
                e.size          = 0;         // USN kennt die Groesse nicht
                e.modified_time = timestamp;
                const uint32_t new_idx = uint32_t(entries_.size());
                entries_.push_back(e);
                if (mft_id >= mft_to_idx_.size())
                    mft_to_idx_.resize(mft_id + 1, UINT32_MAX);
                mft_to_idx_[mft_id] = new_idx;
                st.added++;
            } else if (renamed && idx != UINT32_MAX) {
                // Nur Name aktualisieren — Rest bleibt.
                Entry& e = entries_[idx];
                e.name_offset = uint32_t(name_pool_.size());
                name_pool_.insert(name_pool_.end(), name_ptr, name_ptr + name_len);
                e.name_length = name_len;
                e.parent_mft  = parent_id;
                e.modified_time = timestamp;
                if (e.flags & kFlagDeleted) e.flags &= ~kFlagDeleted;
                st.renamed++;
            }

            off += r->RecordLength;
        }
        next_usn_ = uint64_t(next);
        in.StartUsn = next;
    }
    return st;
}

// ================== Persistenz =========================================

namespace {

constexpr char     kMagic[8] = {'A','R','G','I','D','X','0','2'};
constexpr uint32_t kVersion  = 2;

bool WriteAll(FILE* f, const void* data, size_t n) {
    return std::fwrite(data, 1, n, f) == n;
}
bool ReadAll(FILE* f, void* data, size_t n) {
    return std::fread(data, 1, n, f) == n;
}

} // namespace

std::wstring CacheFilePath(wchar_t drive_letter) {
    wchar_t appdata[MAX_PATH];
    if (SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr,
                        SHGFP_TYPE_CURRENT, appdata) != S_OK) {
        return {};
    }
    std::wstring dir = appdata;
    dir.append(L"\\Argus");
    CreateDirectoryW(dir.c_str(), nullptr);
    wchar_t path[MAX_PATH];
    swprintf(path, MAX_PATH, L"%ls\\%c.aix", dir.c_str(), drive_letter);
    return path;
}

bool Index::SaveTo(const std::wstring& path) const {
    FILE* f = _wfopen(path.c_str(), L"wb");
    if (!f) return false;
    struct Closer { FILE* f; ~Closer(){ if (f) std::fclose(f); } } cl{f};

    if (!WriteAll(f, kMagic, 8)) return false;
    if (!WriteAll(f, &kVersion, 4)) return false;
    uint32_t dl = uint32_t(drive_letter_);
    if (!WriteAll(f, &dl, 4)) return false;
    if (!WriteAll(f, &volume_serial_, 8)) return false;
    if (!WriteAll(f, &usn_journal_id_, 8)) return false;
    if (!WriteAll(f, &next_usn_, 8)) return false;

    uint64_t ec = entries_.size();
    uint64_t nc = name_pool_.size();
    uint64_t mc = mft_to_idx_.size();
    if (!WriteAll(f, &ec, 8)) return false;
    if (!WriteAll(f, &nc, 8)) return false;
    if (!WriteAll(f, &mc, 8)) return false;

    if (ec > 0 && !WriteAll(f, entries_.data(), ec * sizeof(Entry))) return false;
    if (nc > 0 && !WriteAll(f, name_pool_.data(), nc * sizeof(wchar_t))) return false;
    if (mc > 0 && !WriteAll(f, mft_to_idx_.data(), mc * sizeof(uint32_t))) return false;

    // v2: Geometrie + MFT-Runlist.
    if (!WriteAll(f, &geometry_, sizeof(geometry_))) return false;
    uint64_t run_count = mft_runs_.size();
    if (!WriteAll(f, &run_count, 8)) return false;
    if (run_count > 0 && !WriteAll(f, mft_runs_.data(), run_count * sizeof(ntfs::DataRun)))
        return false;
    return true;
}

bool Index::LoadFrom(const std::wstring& path) {
    FILE* f = _wfopen(path.c_str(), L"rb");
    if (!f) return false;
    struct Closer { FILE* f; ~Closer(){ if (f) std::fclose(f); } } cl{f};

    char magic[8];
    uint32_t version, dl;
    uint64_t vs, jid, nusn, ec, nc, mc;
    if (!ReadAll(f, magic, 8) || std::memcmp(magic, kMagic, 8) != 0) return false;
    if (!ReadAll(f, &version, 4) || version != kVersion) return false;
    if (!ReadAll(f, &dl, 4)) return false;
    if (!ReadAll(f, &vs, 8)) return false;
    if (!ReadAll(f, &jid, 8)) return false;
    if (!ReadAll(f, &nusn, 8)) return false;
    if (!ReadAll(f, &ec, 8)) return false;
    if (!ReadAll(f, &nc, 8)) return false;
    if (!ReadAll(f, &mc, 8)) return false;

    // Plausibilitaets-Grenzen um beliebige Files nicht Speicher-fluten zu lassen.
    if (ec > 100ULL * 1000 * 1000) return false;
    if (nc > 4ULL  * 1024 * 1024 * 1024ULL / 2) return false;
    if (mc > 200ULL * 1000 * 1000) return false;

    entries_.assign(ec, {});
    name_pool_.assign(nc, 0);
    mft_to_idx_.assign(mc, UINT32_MAX);

    if (ec > 0 && !ReadAll(f, entries_.data(), ec * sizeof(Entry))) return false;
    if (nc > 0 && !ReadAll(f, name_pool_.data(), nc * sizeof(wchar_t))) return false;
    if (mc > 0 && !ReadAll(f, mft_to_idx_.data(), mc * sizeof(uint32_t))) return false;

    if (!ReadAll(f, &geometry_, sizeof(geometry_))) return false;
    uint64_t run_count = 0;
    if (!ReadAll(f, &run_count, 8)) return false;
    if (run_count > 1000000) return false;   // Plausibilitaet
    mft_runs_.assign(run_count, {});
    if (run_count > 0 && !ReadAll(f, mft_runs_.data(), run_count * sizeof(ntfs::DataRun)))
        return false;

    drive_letter_   = wchar_t(dl);
    volume_serial_  = vs;
    usn_journal_id_ = jid;
    next_usn_       = nusn;
    return true;
}

// ================== On-demand MFT record read ==========================

MftDetails ReadMftDetails(const Index& idx, uint32_t mft_id) {
    MftDetails d;
    d.mft_id = mft_id;
    const auto& g = idx.geometry();
    if (g.bytes_per_mft_record == 0 || idx.mft_runs().empty()) return d;

    wchar_t path[16];
    swprintf(path, 16, L"\\\\.\\%c:", idx.drive_letter());
    HANDLE h = CreateFileW(path, GENERIC_READ,
                          FILE_SHARE_READ | FILE_SHARE_WRITE,
                          nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return d;
    struct Closer { HANDLE h; ~Closer(){ CloseHandle(h); } } closer{h};

    // VCN + Offset innerhalb des Clusters ausrechnen.
    const uint64_t record_offset = uint64_t(mft_id) * g.bytes_per_mft_record;
    const uint64_t vcn           = record_offset / g.bytes_per_cluster;
    const uint32_t off_in_cl     = uint32_t(record_offset % g.bytes_per_cluster);
    const uint64_t byte_off      = ntfs::VcnToByteOffset(idx.mft_runs(), vcn, g.bytes_per_cluster);
    if (byte_off == UINT64_MAX) return d;

    std::vector<uint8_t> rec(g.bytes_per_mft_record);
    LARGE_INTEGER li; li.QuadPart = LONGLONG(byte_off + off_in_cl);
    if (!SetFilePointerEx(h, li, nullptr, FILE_BEGIN)) return d;
    DWORD got = 0;
    if (!ReadFile(h, rec.data(), (DWORD)rec.size(), &got, nullptr) || got != rec.size()) return d;
    if (!ntfs::ApplyFixup(rec.data(), rec.size(), g.bytes_per_sector)) return d;

    const auto* hdr = reinterpret_cast<const ntfs::MftRecordHeader*>(rec.data());
    d.sequence         = hdr->sequence_number;
    d.flags            = hdr->flags;
    d.hard_link_count  = hdr->hard_link_count;

    ntfs::WalkAttributes(rec.data(), rec.size(), [&](const ntfs::AttributeHeader* a) {
        if (a->type == ntfs::kAttrFileName && !a->non_resident) {
            if (auto fn = ntfs::ReadFileName(a)) {
                MftDetails::Name n;
                n.name       = std::move(fn->name);
                n.parent_mft = fn->parent_record;
                n.ns         = fn->ns;
                d.names.push_back(std::move(n));
            }
        } else if (a->type == ntfs::kAttrData) {
            MftDetails::Stream s;
            if (a->name_length > 0) {
                const wchar_t* nptr = reinterpret_cast<const wchar_t*>(
                    reinterpret_cast<const uint8_t*>(a) + a->name_offset);
                s.name.assign(nptr, a->name_length);
            }
            s.resident = !a->non_resident;
            if (a->non_resident) {
                const auto* nr = reinterpret_cast<const ntfs::NonResidentAttribute*>(a);
                s.size = nr->data_size;
            } else {
                const auto* r = reinterpret_cast<const ntfs::ResidentAttribute*>(a);
                s.size = r->value_length;
            }
            d.streams.push_back(std::move(s));
        }
        return true;
    });

    d.ok = true;
    return d;
}

} // namespace argus
