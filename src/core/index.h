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

class Index {
public:
    struct ScanStats {
        std::atomic<uint64_t> records_seen{0};
        std::atomic<uint64_t> total_records{0};
        std::atomic<uint64_t> entries{0};
        std::atomic<uint64_t> bytes_read{0};
        std::atomic<bool>     done{false};
        std::atomic<bool>     ok{false};
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

    // Fuer die Search-Funktion: read-only Zugriff auf Rohdaten.
    const std::vector<Entry>&   entries() const { return entries_; }
    const std::vector<wchar_t>& name_pool() const { return name_pool_; }

private:
    std::vector<Entry>    entries_;
    std::vector<wchar_t>  name_pool_;
    // MFT-Record-Nr. -> Index in entries_. UINT32_MAX = kein Eintrag.
    std::vector<uint32_t> mft_to_idx_;
    wchar_t               drive_letter_ = 0;
};

} // namespace argus
