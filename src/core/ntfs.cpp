#include "core/ntfs.h"

#include <cstring>

namespace argus::ntfs {

VolumeGeometry ParseBootSector(const BootSector& bs) {
    VolumeGeometry g{};
    g.bytes_per_sector    = bs.bytes_per_sector;
    g.sectors_per_cluster = bs.sectors_per_cluster;
    g.bytes_per_cluster   = uint32_t(bs.bytes_per_sector) * bs.sectors_per_cluster;
    // "clusters_per_mft_record" ist entweder positiv (in Clustern) oder negativ:
    // dann bezeichnet er 2^abs(x) BYTES pro Record.
    if (bs.clusters_per_mft_record > 0) {
        g.bytes_per_mft_record = uint32_t(bs.clusters_per_mft_record) * g.bytes_per_cluster;
    } else {
        g.bytes_per_mft_record = 1u << static_cast<uint32_t>(-bs.clusters_per_mft_record);
    }
    g.mft_lcn         = bs.mft_lcn;
    g.mft_byte_offset = bs.mft_lcn * g.bytes_per_cluster;
    g.total_sectors   = bs.total_sectors;
    g.volume_serial   = bs.volume_serial;
    return g;
}

bool ApplyFixup(uint8_t* record, size_t record_size, size_t sector_size) {
    if (record_size < sizeof(MftRecordHeader)) return false;
    auto* h = reinterpret_cast<MftRecordHeader*>(record);
    if (std::memcmp(h->signature, "FILE", 4) != 0) return false;
    if (h->update_sequence_offset + h->update_sequence_size * 2u > record_size) return false;

    const uint16_t* usa = reinterpret_cast<uint16_t*>(record + h->update_sequence_offset);
    const uint16_t  usn = usa[0];      // update-sequence-number
    // usa[1..] enthalten die urspruenglichen letzten 2 Bytes jedes Sektors.

    const size_t sectors = h->update_sequence_size - 1u;
    if (sectors * sector_size != record_size) {
        // Manche exotischen Geometrien; fuer NTFS-Standard (1024 Byte Record, 512 Byte Sektor)
        // ist sectors == 2. Wir sind streng.
        return false;
    }
    for (size_t i = 0; i < sectors; ++i) {
        uint8_t* end_of_sector = record + (i + 1) * sector_size - 2;
        // Signatur-Check: die letzten 2 Bytes jedes Sektors muessen == usn sein.
        uint16_t on_disk;
        std::memcpy(&on_disk, end_of_sector, 2);
        if (on_disk != usn) return false;
        std::memcpy(end_of_sector, &usa[i + 1], 2);
    }
    return true;
}

std::vector<DataRun> DecodeRunlist(const uint8_t* p, size_t max_bytes) {
    std::vector<DataRun> out;
    uint64_t vcn = 0;
    int64_t  prev_lcn = 0;
    size_t   off = 0;
    while (off < max_bytes) {
        const uint8_t hdr = p[off++];
        if (hdr == 0) break;              // Runlist-Ende
        const uint8_t len_size = hdr & 0x0F;
        const uint8_t off_size = (hdr >> 4) & 0x0F;
        if (len_size == 0 || len_size > 8 || off_size > 8) break;
        if (off + len_size + off_size > max_bytes) break;

        uint64_t length = 0;
        for (int i = 0; i < len_size; ++i) length |= uint64_t(p[off + i]) << (8 * i);
        off += len_size;

        int64_t delta_lcn = 0;
        if (off_size > 0) {
            for (int i = 0; i < off_size; ++i)
                delta_lcn |= int64_t(p[off + i]) << (8 * i);
            // Vorzeichen erweitern falls off_size < 8.
            if (off_size < 8 && (p[off + off_size - 1] & 0x80))
                delta_lcn |= (int64_t(-1) << (off_size * 8));
        }
        off += off_size;

        DataRun r{};
        r.vcn_start = vcn;
        r.length    = length;
        if (off_size == 0) {
            r.lcn = -1;                   // sparse
        } else {
            prev_lcn += delta_lcn;
            r.lcn = prev_lcn;
        }
        out.push_back(r);
        vcn += length;
    }
    return out;
}

uint64_t VcnToByteOffset(const std::vector<DataRun>& runs, uint64_t vcn,
                        uint32_t bytes_per_cluster) {
    for (const auto& r : runs) {
        if (vcn >= r.vcn_start && vcn < r.vcn_start + r.length) {
            if (r.lcn < 0) return UINT64_MAX;   // sparse
            return (static_cast<uint64_t>(r.lcn) + (vcn - r.vcn_start)) *
                   bytes_per_cluster;
        }
    }
    return UINT64_MAX;
}

std::optional<FileNameInfo> ReadFileName(const AttributeHeader* a) {
    if (a->type != kAttrFileName) return std::nullopt;
    if (a->non_resident) return std::nullopt; // FILE_NAME ist immer resident.

    const auto* r = reinterpret_cast<const ResidentAttribute*>(a);
    const uint8_t* body = reinterpret_cast<const uint8_t*>(a) + r->value_offset;
    if (r->value_length < sizeof(FileNameAttribute)) return std::nullopt;

    const auto* fn = reinterpret_cast<const FileNameAttribute*>(body);
    // Bounds-Check: name muss innerhalb des attribute value liegen.
    const uint32_t name_bytes = uint32_t(fn->name_length) * 2u;
    if (sizeof(FileNameAttribute) + name_bytes > r->value_length) return std::nullopt;

    FileNameInfo info{};
    info.parent_record = fn->parent_ref & 0x0000FFFFFFFFFFFFULL;
    info.ns            = fn->namespace_code;
    info.is_directory  = (fn->file_attributes & 0x10000000u) != 0; // FILE_ATTRIBUTE_DIRECTORY-Flag im MFT-Kontext

    const wchar_t* name_ptr = reinterpret_cast<const wchar_t*>(body + sizeof(FileNameAttribute));
    info.name.assign(name_ptr, fn->name_length);
    return info;
}

} // namespace argus::ntfs
