// Argus — In-Memory Datei-Index einer NTFS-Volume.
// Liest die MFT komplett ein und baut eine kompakte Liste aller in-use
// Datei-/Verzeichnis-Eintraege plus einen gepoolten Namens-Buffer auf.

#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "core/ntfs.h"

namespace argus {

// 32 Bytes pro Eintrag. Namen liegen im separaten Pool.
struct Entry {
    uint32_t parent_mft;     // MFT-Record-Nr. des Elterns
    uint32_t name_offset;    // wchar_t-Offset in name_pool_
    uint16_t name_length;    // wchar_t-Anzahl
    uint16_t flags;          // bit 0 = directory
    uint64_t size;
    uint64_t modified_time;  // FILETIME (100ns seit 1601)
};
static_assert(sizeof(Entry) == 32, "Entry must stay 32 bytes");

constexpr uint16_t kFlagDirectory = 1u << 0;
constexpr uint16_t kFlagDeleted   = 1u << 15;  // via USN als geloescht markiert

class Index {
public:
    struct ScanStats {
        std::atomic<uint64_t> records_seen{0};
        std::atomic<uint64_t> total_records{0};
        std::atomic<uint64_t> entries{0};
        std::atomic<uint64_t> bytes_read{0};
        std::atomic<uint64_t> skipped{0};     // records with bad signature or fixup
        std::atomic<bool>     done{false};
        std::atomic<bool>     ok{false};
        // Fehlerklasse damit die GUI eine konkrete Meldung zeigen kann.
        enum class Error {
            None = 0,
            AccessDenied,       // requires admin
            NotNtfs,            // volume is FAT32/exFAT/ReFS
            BootReadFailed,     // could not read sector 0
            MftReadFailed,
            NoDataAttr,         // MFT record 0 missing $DATA
        };
        std::atomic<int> error{int(Error::None)};
    };

    // Fuellt das Index-Objekt durch rohes Lesen des NTFS-MFT.
    // stats (optional) wird waehrend des Laufs live aktualisiert — brauchen wir
    // fuer die GUI-Progress-Anzeige aus einem anderen Thread.
    // Braucht Administrator-Rechte (rohes \\.\<drive>: oeffnen).
    bool ScanDrive(wchar_t drive, ScanStats* stats = nullptr);

    // ---------- read-only Access ----------
    size_t              entry_count() const { return entries_.size(); }
    const Entry&        entry(uint32_t i) const { return entries_[i]; }
    std::wstring_view   name(uint32_t i) const;
    bool                is_directory(uint32_t i) const { return entries_[i].flags & kFlagDirectory; }
    wchar_t             drive_letter() const { return drive_letter_; }

    // Voller Pfad, absolut, mit Laufwerk (z.B. L"C:\\Users\\andre\\...").
    std::wstring        full_path(uint32_t i) const;

    // Reverse lookup: entry_idx -> MFT record number, O(1) via parallel table.
    uint32_t            mft_id_of(uint32_t entry_idx) const {
        return (entry_idx < entry_mft_id_.size()) ? entry_mft_id_[entry_idx] : UINT32_MAX;
    }

    // Fuer die Search-Funktion: read-only Zugriff auf Rohdaten.
    const std::vector<Entry>&   entries() const { return entries_; }
    const std::vector<wchar_t>& name_pool() const { return name_pool_; }

    // ---------- Persistenz ----------
    // Binaerformat: Magic "ARGIDX01", Version, drive_letter, volume_serial,
    // usn_journal_id, next_usn, entries, name_pool, mft_to_idx.
    bool SaveTo(const std::wstring& path) const;
    bool LoadFrom(const std::wstring& path);

    // ---------- USN Journal ----------
    struct UsnStats {
        uint64_t added   = 0;
        uint64_t renamed = 0;
        uint64_t deleted = 0;
        bool     rolled_over = false;  // Journal ist zu weit fortgeschritten
    };
    // Liest neue USN-Records seit next_usn_ und wendet Aenderungen im Speicher
    // an. Blockiert bis kein weiterer Record da ist. Braucht offenes \\.\<drive>:.
    UsnStats ApplyUsnChanges();

    uint64_t volume_serial() const   { return volume_serial_; }
    uint64_t usn_journal_id() const  { return usn_journal_id_; }
    uint64_t next_usn() const        { return next_usn_; }

    // Fuer On-demand-Lesen von MFT-Records: die Runlist des $MFT-$DATA und
    // die Cluster-/Record-Geometrie.
    struct Geometry {
        uint32_t bytes_per_sector = 0;
        uint32_t bytes_per_cluster = 0;
        uint32_t bytes_per_mft_record = 0;
    };
    const Geometry& geometry() const { return geometry_; }
    const std::vector<ntfs::DataRun>& mft_runs() const { return mft_runs_; }

private:
    std::vector<Entry>    entries_;
    std::vector<wchar_t>  name_pool_;
    // MFT-Record-Nr. -> Index in entries_. UINT32_MAX = kein Eintrag.
    std::vector<uint32_t> mft_to_idx_;
    // Entry-Idx -> MFT-Record-Nr. (parallel zu entries_).
    std::vector<uint32_t> entry_mft_id_;
    wchar_t               drive_letter_    = 0;
    uint64_t              volume_serial_   = 0;
    uint64_t              usn_journal_id_  = 0;
    uint64_t              next_usn_        = 0;

    // Geometrie + Runlist des $MFT $DATA — nur nach ScanDrive() gueltig, wird
    // aber auch aus dem Cache wiederhergestellt.
    Geometry                    geometry_{};
    std::vector<ntfs::DataRun>  mft_runs_;
};

// Wo Argus seinen Cache ablegt: %LOCALAPPDATA%\Argus\<drive>.aix
std::wstring CacheFilePath(wchar_t drive_letter);

// --------------- On-demand MFT record read ---------------

struct MftDetails {
    uint32_t mft_id       = 0;
    uint16_t sequence     = 0;
    uint16_t flags        = 0;
    uint16_t hard_link_count = 0;
    bool     ok           = false;
    struct Name {
        std::wstring name;
        uint64_t     parent_mft;
        uint8_t      ns;
    };
    std::vector<Name> names;
    struct Stream {
        std::wstring name;      // empty = default stream
        uint64_t     size;
        bool         resident;
    };
    std::vector<Stream> streams;
};

MftDetails ReadMftDetails(const Index& idx, uint32_t mft_id);

} // namespace argus
