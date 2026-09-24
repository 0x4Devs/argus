// Argus — NTFS on-disk structures + parsing primitives.
//
// Referenz: Brian Carrier, "File System Forensic Analysis", ch. 12/13,
//           https://flatcap.github.io/linux-ntfs/ntfs/, Microsoft NTFS.pdf.
//
// Alle Structs sind gepackt und entsprechen 1:1 dem was auf der Platte liegt.
// Little-endian: NTFS ist ausschliesslich x86, wir setzen little-endian voraus.

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace argus::ntfs {

// ---------------- Konstanten ----------------

// Attribute-Typen die uns interessieren.
constexpr uint32_t kAttrStandardInformation = 0x10;
constexpr uint32_t kAttrAttributeList       = 0x20;
constexpr uint32_t kAttrFileName            = 0x30;
constexpr uint32_t kAttrData                = 0x80;
constexpr uint32_t kAttrEnd                 = 0xFFFFFFFF;

// FILE_NAME namespace codes.
constexpr uint8_t  kNsPosix                 = 0;
constexpr uint8_t  kNsWin32                 = 1;
constexpr uint8_t  kNsDos                   = 2;   // 8.3-Kurzname
constexpr uint8_t  kNsWin32AndDos           = 3;

// MFT record header flags.
constexpr uint16_t kMftFlagInUse            = 0x0001;
constexpr uint16_t kMftFlagDirectory        = 0x0002;

// ---------------- On-disk Structs ----------------

#pragma pack(push, 1)

// NTFS Boot Sector — die ersten 512 Bytes eines NTFS-Volumes.
struct BootSector {
    uint8_t  jmp[3];                    // 0x00 EB 52 90
    char     oem_id[8];                 // "NTFS    "
    uint16_t bytes_per_sector;          // 0x0B
    uint8_t  sectors_per_cluster;       // 0x0D
    uint8_t  _reserved0[7];
    uint8_t  media_descriptor;          // 0x15 (0xF8 = HDD)
    uint8_t  _reserved1[2];
    uint16_t sectors_per_track;         // 0x18
    uint16_t number_of_heads;           // 0x1A
    uint32_t hidden_sectors;            // 0x1C
    uint8_t  _reserved2[8];
    uint64_t total_sectors;             // 0x28
    uint64_t mft_lcn;                   // 0x30  MFT start (in clusters)
    uint64_t mft_mirror_lcn;            // 0x38
    int8_t   clusters_per_mft_record;   // 0x40  (negativ = 2^abs Bytes)
    uint8_t  _reserved3[3];
    int8_t   clusters_per_index_record; // 0x44
    uint8_t  _reserved4[3];
    uint64_t volume_serial;             // 0x48
    uint32_t checksum;                  // 0x50
    uint8_t  boot_code[426];            // 0x54
    uint16_t signature;                 // 0x1FE  0xAA55
};
static_assert(sizeof(BootSector) == 512, "BootSector must be 512 bytes");

// Header eines MFT-Records ("FILE" oder "BAAD" bei Fehler).
struct MftRecordHeader {
    char     signature[4];              // "FILE"
    uint16_t update_sequence_offset;    // Offset zum USA (Fixup-Array)
    uint16_t update_sequence_size;      // Anzahl 16-bit Eintraege im USA
    uint64_t log_file_sequence;
    uint16_t sequence_number;
    uint16_t hard_link_count;
    uint16_t first_attribute_offset;    // Offset zum ersten Attribut
    uint16_t flags;                     // kMftFlagInUse / kMftFlagDirectory
    uint32_t used_size;
    uint32_t allocated_size;
    uint64_t base_record;               // Parent-MFT ref bei extension records
    uint16_t next_attribute_id;
    uint16_t _padding;
    uint32_t record_number;             // (nur bei XP+)
};

// Gemeinsamer Kopf jedes Attributs.
struct AttributeHeader {
    uint32_t type;                      // 0x10 / 0x30 / 0x80 / 0xFFFFFFFF
    uint32_t length;                    // Gesamtlaenge inkl. Header
    uint8_t  non_resident;              // 0 = resident, 1 = non-resident
    uint8_t  name_length;               // in wchar_t
    uint16_t name_offset;
    uint16_t flags;
    uint16_t attribute_id;
};

// Zusatz-Fields fuer resident attributes.
struct ResidentAttribute {
    AttributeHeader hdr;
    uint32_t value_length;
    uint16_t value_offset;
    uint8_t  indexed_flag;
    uint8_t  _padding;
};

// Zusatz-Fields fuer non-resident attributes (mit runlist).
struct NonResidentAttribute {
    AttributeHeader hdr;
    uint64_t start_vcn;
    uint64_t last_vcn;
    uint16_t runlist_offset;            // Offset zum Data-Run
    uint16_t compression_unit_size;
    uint32_t _padding;
    uint64_t allocated_size;
    uint64_t data_size;
    uint64_t initialized_size;
};

// $FILE_NAME attribute body.
struct FileNameAttribute {
    uint64_t parent_ref;                // low 48 bit = MFT record #
    uint64_t creation_time;
    uint64_t modified_time;
    uint64_t mft_changed_time;
    uint64_t access_time;
    uint64_t allocated_size;
    uint64_t data_size;
    uint32_t file_attributes;
    uint32_t reparse_or_ea;
    uint8_t  name_length;               // in wchar_t
    uint8_t  namespace_code;            // kNsWin32 etc.
    // Danach: wchar_t name[name_length]
};

#pragma pack(pop)

// ---------------- High-level ----------------

struct VolumeGeometry {
    uint32_t bytes_per_sector;
    uint32_t sectors_per_cluster;
    uint32_t bytes_per_cluster;
    uint32_t bytes_per_mft_record;      // haeufig 1024
    uint64_t mft_lcn;
    uint64_t mft_byte_offset;           // mft_lcn * bytes_per_cluster
    uint64_t total_sectors;
    uint64_t volume_serial;
};

// Aus einem BootSector die praktisch nutzbaren Werte ableiten.
VolumeGeometry ParseBootSector(const BootSector& bs);

// Fixup-Array anwenden. In NTFS ueberschreibt der USA die letzten 2 Bytes jedes
// Sektors innerhalb eines Records; ohne Ruecktransformation sehen wir garbage.
// Gibt true zurueck wenn Signature stimmt und Fixup erfolgreich war.
bool ApplyFixup(uint8_t* record, size_t record_size, size_t sector_size);

// Parst alle Attribute in einem Record und ruft cb fuer jedes auf.
// cb erhaelt Zeiger auf AttributeHeader; kann false zurueckgeben um zu stoppen.
template <typename Callback>
void WalkAttributes(const uint8_t* record, size_t record_size, Callback cb) {
    const auto* h = reinterpret_cast<const MftRecordHeader*>(record);
    if (h->first_attribute_offset >= record_size) return;
    size_t off = h->first_attribute_offset;
    while (off + sizeof(AttributeHeader) <= record_size) {
        const auto* a = reinterpret_cast<const AttributeHeader*>(record + off);
        if (a->type == kAttrEnd) break;
        if (a->length == 0 || a->length > record_size - off) break;
        if (!cb(a)) return;
        off += a->length;
    }
}

// FILE_NAME aus einem resident $FILE_NAME-Attribut extrahieren.
struct FileNameInfo {
    uint64_t     parent_record;
    std::wstring name;
    uint8_t      ns;
    bool         is_directory;
};
std::optional<FileNameInfo> ReadFileName(const AttributeHeader* a);

// ---------------- Runlist ----------------

// Ein "Data Run" = zusammenhaengende Cluster-Sequenz.
// lcn < 0 markiert einen sparse-Run (keine physischen Cluster).
struct DataRun {
    uint64_t vcn_start;   // Virtual Cluster Number Anfang
    uint64_t length;      // Cluster-Anzahl
    int64_t  lcn;         // Logical Cluster Number Anfang, -1 = sparse
};

// Runlist der non-resident attributes: kompakt binaer-codierte Sequenz.
// Format je Run:  hdr = (offset_size<<4)|length_size, dann length_bytes,
// dann offset_bytes (signed, delta zum vorherigen LCN). Endet mit 0x00.
std::vector<DataRun> DecodeRunlist(const uint8_t* p, size_t max_bytes);

// Physical byte offset zu einer bestimmten Virtual Cluster Number.
// Gibt UINT64_MAX zurueck fuer sparse oder ausserhalb.
uint64_t VcnToByteOffset(const std::vector<DataRun>& runs,
                        uint64_t vcn,
                        uint32_t bytes_per_cluster);

} // namespace argus::ntfs
