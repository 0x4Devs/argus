#include "core/multi_index.h"

#include <thread>

namespace argus {

void MultiIndex::SetDrives(const std::vector<wchar_t>& drives) {
    drives_ = drives;
    indexes_.clear();
    indexes_.reserve(drives.size());
    for (size_t i = 0; i < drives.size(); ++i)
        indexes_.push_back(std::make_unique<Index>());
}

void MultiIndex::ScanAll(AggregateStats* agg) {
    if (agg) agg->drives_total.store(uint32_t(drives_.size()));

    std::vector<Index::ScanStats> per_drive(drives_.size());
    std::vector<std::thread>       threads;
    threads.reserve(drives_.size());

    for (size_t i = 0; i < drives_.size(); ++i) {
        threads.emplace_back([this, i, &per_drive]{
            indexes_[i]->ScanDrive(drives_[i], &per_drive[i]);
        });
    }

    if (agg) {
        // Aggregieren waehrend die Threads laufen.
        bool all_done = false;
        while (!all_done) {
            uint64_t r=0, t=0, e=0, b=0;
            uint32_t done_cnt = 0;
            for (auto& s : per_drive) {
                r += s.records_seen.load();
                t += s.total_records.load();
                e += s.entries.load();
                b += s.bytes_read.load();
                if (s.done.load()) ++done_cnt;
            }
            agg->records_seen.store(r);
            agg->total_records.store(t);
            agg->entries.store(e);
            agg->bytes_read.store(b);
            agg->drives_done.store(done_cnt);
            all_done = (done_cnt == drives_.size());
            if (!all_done) std::this_thread::sleep_for(std::chrono::milliseconds(60));
        }
    }
    for (auto& th : threads) th.join();
    if (agg) agg->done.store(true);
}

MultiIndex::UsnAggregate MultiIndex::ApplyUsnChanges() {
    UsnAggregate agg;
    for (auto& idx : indexes_) {
        auto s = idx->ApplyUsnChanges();
        agg.added   += s.added;
        agg.renamed += s.renamed;
        agg.deleted += s.deleted;
        if (s.rolled_over) agg.any_rolled_over = true;
    }
    return agg;
}

size_t MultiIndex::total_entries() const {
    size_t n = 0;
    for (auto& idx : indexes_) n += idx->entry_count();
    return n;
}

} // namespace argus
