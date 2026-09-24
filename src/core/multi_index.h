// Argus — Multi-Drive Index Aggregation.
// Groups several per-volume Index objects and exposes a unified search that
// combines their results.

#pragma once

#include <memory>
#include <string>
#include <vector>

#include "core/index.h"

namespace argus {

// A hit refers to an entry inside a specific drive slot.
struct SearchHit {
    uint8_t  drive_slot;
    uint32_t entry_idx;
};

class MultiIndex {
public:
    // Aggregate progress across all drives being scanned.
    struct AggregateStats {
        std::atomic<uint64_t> records_seen{0};
        std::atomic<uint64_t> total_records{0};
        std::atomic<uint64_t> entries{0};
        std::atomic<uint64_t> bytes_read{0};
        std::atomic<uint32_t> drives_done{0};
        std::atomic<uint32_t> drives_total{0};
        std::atomic<bool>     done{false};
    };

    // Adds a drive slot. Its Index is populated by ScanAll().
    void SetDrives(const std::vector<wchar_t>& drives);
    size_t drive_count() const { return indexes_.size(); }
    wchar_t drive_letter(size_t slot) const { return drives_[slot]; }
    Index&       index(size_t slot)       { return *indexes_[slot]; }
    const Index& index(size_t slot) const { return *indexes_[slot]; }

    // Blocking, but internally parallelizes: one std::thread per drive.
    void ScanAll(AggregateStats* stats = nullptr);

    // Applies USN changes on all drives (call from a UI timer).
    struct UsnAggregate {
        uint64_t added = 0, renamed = 0, deleted = 0;
        bool     any_rolled_over = false;
    };
    UsnAggregate ApplyUsnChanges();

    size_t total_entries() const;

private:
    std::vector<wchar_t>                drives_;
    std::vector<std::unique_ptr<Index>> indexes_;
};

} // namespace argus
