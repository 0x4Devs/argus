// Argus — GUI (Qt6 Widgets).
// v0.4.0: Advanced query syntax (ext:/type:/path:/size:/modified:),
//         multi-drive concurrent indexing ("All Drives").

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>

#include <QAbstractTableModel>
#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHash>
#include <QHeaderView>
#include <QIcon>
#include <QImage>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QMainWindow>
#include <QMenu>
#include <QMessageBox>
#include <QPixmap>
#include <QProgressBar>
#include <QPushButton>
#include <QShortcut>
#include <QStatusBar>
#include <QStyleFactory>
#include <QTableView>
#include <QThread>
#include <QTimer>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QMenuBar>
#include <QMetaObject>
#include <QMutex>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTableWidget>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QUrl>
#include <QVBoxLayout>

#include <mutex>
#include <unordered_map>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <memory>
#include <thread>

#include "core/index.h"
#include "core/multi_index.h"
#include "core/ntfs.h"
#include "core/query.h"
#include "core/search.h"

// ================== NtfsDetailsDialog ==================================
// Right-click -> "NTFS details" reads the actual MFT record on demand and
// shows hardlinks, alternate data streams and raw metadata.

class NtfsDetailsDialog : public QDialog {
    Q_OBJECT
public:
    NtfsDetailsDialog(const argus::MftDetails& d,
                      const QString& primary_path,
                      wchar_t drive_letter,
                      QWidget* parent = nullptr)
        : QDialog(parent) {
        setWindowTitle("NTFS details");
        resize(720, 520);

        auto* root = new QVBoxLayout(this);
        root->setContentsMargins(16, 14, 16, 12);
        root->setSpacing(10);

        // -------- Header form --------
        auto* form = new QFormLayout();
        form->setHorizontalSpacing(14);
        form->setVerticalSpacing(4);

        auto* pathLabel = new QLabel(primary_path);
        pathLabel->setStyleSheet("font-weight: 600;");
        pathLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
        pathLabel->setWordWrap(true);
        form->addRow("Path:", pathLabel);

        form->addRow("Volume:", new QLabel(QString(QChar(drive_letter)) + ":"));
        form->addRow("MFT record:", new QLabel(QString::number(d.mft_id)));
        form->addRow("Sequence:",   new QLabel(QString::number(d.sequence)));

        QStringList flag_names;
        if (d.flags & 0x0001) flag_names << "IN_USE";
        if (d.flags & 0x0002) flag_names << "DIRECTORY";
        if (d.flags & 0x0004) flag_names << "EXTENSION";
        form->addRow("Flags:", new QLabel(flag_names.isEmpty()
                                          ? "—" : flag_names.join(" | ")));
        form->addRow("Hard link count:", new QLabel(QString::number(d.hard_link_count)));

        root->addLayout(form);

        // -------- Names / Hardlinks --------
        auto* namesLabel = new QLabel(QString("Names / hardlinks (%1)").arg(d.names.size()));
        namesLabel->setStyleSheet("font-weight: 600; margin-top: 6px;");
        root->addWidget(namesLabel);

        auto* namesTable = new QTableWidget(int(d.names.size()), 3);
        namesTable->setHorizontalHeaderLabels({"Namespace", "Parent MFT", "Name"});
        namesTable->verticalHeader()->setVisible(false);
        namesTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
        namesTable->setSelectionBehavior(QAbstractItemView::SelectRows);
        namesTable->setAlternatingRowColors(true);
        namesTable->horizontalHeader()->setStretchLastSection(true);
        namesTable->setColumnWidth(0, 100);
        namesTable->setColumnWidth(1, 100);
        for (int i = 0; i < int(d.names.size()); ++i) {
            const char* ns = "?";
            switch (d.names[i].ns) {
                case 0: ns = "POSIX";      break;
                case 1: ns = "Win32";      break;
                case 2: ns = "DOS";        break;
                case 3: ns = "Win32+DOS";  break;
            }
            namesTable->setItem(i, 0, new QTableWidgetItem(ns));
            namesTable->setItem(i, 1, new QTableWidgetItem(QString::number(d.names[i].parent_mft)));
            namesTable->setItem(i, 2, new QTableWidgetItem(
                QString::fromWCharArray(d.names[i].name.data(), int(d.names[i].name.size()))));
        }
        namesTable->setMinimumHeight(120);
        root->addWidget(namesTable);

        // -------- Data streams + ADS --------
        int ads_count = 0;
        for (const auto& s : d.streams) if (!s.name.empty()) ++ads_count;
        auto* streamsLabel = new QLabel(QString("Data streams (%1, including %2 alternate)")
                                       .arg(d.streams.size()).arg(ads_count));
        streamsLabel->setStyleSheet("font-weight: 600; margin-top: 6px;");
        root->addWidget(streamsLabel);

        auto* streamsTable = new QTableWidget(int(d.streams.size()), 3);
        streamsTable->setHorizontalHeaderLabels({"Stream name", "Size", "Storage"});
        streamsTable->verticalHeader()->setVisible(false);
        streamsTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
        streamsTable->setAlternatingRowColors(true);
        streamsTable->horizontalHeader()->setStretchLastSection(true);
        streamsTable->setColumnWidth(0, 220);
        streamsTable->setColumnWidth(1, 120);
        for (int i = 0; i < int(d.streams.size()); ++i) {
            const auto& s = d.streams[i];
            QString name = s.name.empty()
                          ? "<default>"
                          : QString::fromWCharArray(s.name.data(), int(s.name.size()));
            streamsTable->setItem(i, 0, new QTableWidgetItem(name));
            streamsTable->setItem(i, 1, new QTableWidgetItem(
                QLocale::system().formattedDataSize(qint64(s.size))));
            streamsTable->setItem(i, 2, new QTableWidgetItem(s.resident ? "resident (in MFT)"
                                                                        : "non-resident"));
        }
        streamsTable->setMinimumHeight(120);
        root->addWidget(streamsTable);

        // -------- Close button --------
        auto* bb = new QDialogButtonBox(QDialogButtonBox::Close);
        connect(bb, &QDialogButtonBox::rejected, this, &QDialog::reject);
        root->addWidget(bb);
    }
};

// ================== DuplicateFinderDialog ==============================
// Background pipeline: 1) group by exact size, 2) drop singletons, 3) hash the
// first 64 KB of each candidate with FNV-1a and sub-group. Cheap, no crypto —
// good enough for a media-vs-source drive; a v0.7 could add full-file hash.

class DuplicateFinderDialog : public QDialog {
    Q_OBJECT
public:
    DuplicateFinderDialog(const argus::MultiIndex* multi, QWidget* parent = nullptr)
        : QDialog(parent), multi_(multi) {
        setWindowTitle("Find duplicates");
        resize(880, 580);

        auto* root = new QVBoxLayout(this);
        root->setContentsMargins(14, 12, 14, 10);

        progress_ = new QProgressBar();
        progress_->setRange(0, 100);
        progress_->setValue(0);
        summary_ = new QLabel("Scanning…");
        root->addWidget(summary_);
        root->addWidget(progress_);

        tree_ = new QTreeWidget();
        tree_->setColumnCount(3);
        tree_->setHeaderLabels({"File", "Size", "Path"});
        tree_->setAlternatingRowColors(true);
        tree_->header()->setStretchLastSection(true);
        tree_->setColumnWidth(0, 260);
        tree_->setColumnWidth(1, 100);
        root->addWidget(tree_, 1);

        auto* btns = new QDialogButtonBox();
        cancel_btn_ = btns->addButton("Cancel", QDialogButtonBox::RejectRole);
        auto* close = btns->addButton("Close", QDialogButtonBox::AcceptRole);
        connect(cancel_btn_, &QPushButton::clicked, this, [this]{
            cancelled_.store(true);
            summary_->setText("Cancelling…");
        });
        connect(close, &QPushButton::clicked, this, &QDialog::accept);
        root->addWidget(btns);

        worker_ = std::thread([this]{ runScan(); });
    }

    ~DuplicateFinderDialog() {
        cancelled_.store(true);
        if (worker_.joinable()) worker_.join();
    }

signals:
    void statusUpdate(int pct, quint64 groups, quint64 wasted);
    void done();

private:
    static uint64_t fnv1a(const uint8_t* data, size_t n) {
        uint64_t h = 14695981039346656037ULL;
        for (size_t i = 0; i < n; ++i) {
            h ^= data[i];
            h *= 1099511628211ULL;
        }
        return h;
    }

    void runScan() {
        connect(this, &DuplicateFinderDialog::statusUpdate, this,
            [this](int pct, quint64 groups, quint64 wasted){
                progress_->setValue(pct);
                summary_->setText(QString("%L1 duplicate group(s) — %2 recoverable")
                    .arg(groups).arg(QLocale::system().formattedDataSize(qint64(wasted))));
            }, Qt::QueuedConnection);
        connect(this, &DuplicateFinderDialog::done, this, [this]{
            cancel_btn_->setEnabled(false);
            populateTree();
        }, Qt::QueuedConnection);

        // Phase 1: bucket by size.
        std::unordered_map<uint64_t, std::vector<argus::SearchHit>> by_size;
        by_size.reserve(200000);
        uint64_t total_entries = 0;
        for (size_t slot = 0; slot < multi_->drive_count(); ++slot)
            total_entries += multi_->index(slot).entry_count();
        uint64_t seen = 0;
        for (size_t slot = 0; slot < multi_->drive_count(); ++slot) {
            const auto& idx = multi_->index(slot);
            const auto& es = idx.entries();
            for (uint32_t i = 0; i < es.size(); ++i) {
                if (cancelled_.load()) return;
                const auto& e = es[i];
                if (e.flags & (argus::kFlagDirectory | argus::kFlagDeleted)) continue;
                if (e.size == 0) continue;
                by_size[e.size].push_back({uint8_t(slot), i});
                if (((++seen) & 0x1FFFF) == 0) {
                    int pct = int(seen * 30 / std::max<uint64_t>(1, total_entries));
                    emit statusUpdate(pct, 0, 0);
                }
            }
        }

        // Phase 2: hash first 64 KB of each file in groups with more than one.
        std::vector<uint8_t> buf(65536);
        std::vector<std::pair<uint64_t, std::vector<argus::SearchHit>>> candidate_groups;
        for (auto& kv : by_size) {
            if (cancelled_.load()) return;
            if (kv.second.size() < 2) continue;
            candidate_groups.emplace_back(std::move(kv));
        }
        by_size.clear();

        uint64_t groups_found = 0, wasted = 0;
        uint64_t processed_files = 0;
        uint64_t total_candidate_files = 0;
        for (auto& g : candidate_groups) total_candidate_files += g.second.size();

        {
            std::lock_guard<std::mutex> lk(found_mtx_);
            found_.clear();
        }

        for (auto& g : candidate_groups) {
            if (cancelled_.load()) return;
            std::unordered_map<uint64_t, std::vector<argus::SearchHit>> by_hash;
            for (const auto& hit : g.second) {
                if (cancelled_.load()) return;
                const argus::Index& idx = multi_->index(hit.drive_slot);
                std::wstring path = idx.full_path(hit.entry_idx);
                HANDLE h = CreateFileW(path.c_str(), GENERIC_READ,
                                      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                      nullptr, OPEN_EXISTING,
                                      FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
                                      nullptr);
                if (h == INVALID_HANDLE_VALUE) { ++processed_files; continue; }
                DWORD got = 0;
                DWORD to_read = DWORD(std::min<uint64_t>(g.first, buf.size()));
                BOOL ok = ReadFile(h, buf.data(), to_read, &got, nullptr);
                CloseHandle(h);
                if (!ok) { ++processed_files; continue; }
                uint64_t hh = fnv1a(buf.data(), got);
                by_hash[hh].push_back(hit);
                ++processed_files;
                if ((processed_files & 0x1FF) == 0) {
                    int pct = 30 + int(processed_files * 70 /
                        std::max<uint64_t>(1, total_candidate_files));
                    emit statusUpdate(pct, groups_found, wasted);
                }
            }
            for (auto& hk : by_hash) {
                if (hk.second.size() < 2) continue;
                DupGroup dg;
                dg.size = g.first;
                dg.hash = hk.first;
                dg.files = std::move(hk.second);
                wasted += g.first * (dg.files.size() - 1);
                ++groups_found;
                std::lock_guard<std::mutex> lk(found_mtx_);
                found_.push_back(std::move(dg));
            }
            emit statusUpdate(std::min(99, int(30 + processed_files * 70 /
                                        std::max<uint64_t>(1, total_candidate_files))),
                              groups_found, wasted);
        }
        emit statusUpdate(100, groups_found, wasted);
        emit done();
    }

    void populateTree() {
        std::lock_guard<std::mutex> lk(found_mtx_);
        tree_->clear();
        // Nach Groesse absteigend sortieren damit die groessten Duplikate zuerst kommen.
        std::sort(found_.begin(), found_.end(),
                  [](const DupGroup& a, const DupGroup& b){
                      return a.size * a.files.size() > b.size * b.files.size();
                  });
        for (auto& g : found_) {
            auto* top = new QTreeWidgetItem(tree_);
            top->setText(0, QString("%1 duplicates").arg(g.files.size()));
            top->setText(1, QLocale::system().formattedDataSize(qint64(g.size)));
            top->setText(2, QString("wastes %1")
                .arg(QLocale::system().formattedDataSize(qint64(g.size * (g.files.size() - 1)))));
            for (const auto& hit : g.files) {
                const argus::Index& idx = multi_->index(hit.drive_slot);
                auto name = idx.name(hit.entry_idx);
                auto path = idx.full_path(hit.entry_idx);
                auto* child = new QTreeWidgetItem(top);
                child->setText(0, QString::fromWCharArray(name.data(), int(name.size())));
                child->setText(1, "");
                child->setText(2, QString::fromWCharArray(path.data(), int(path.size())));
            }
        }
    }

    struct DupGroup {
        uint64_t size;
        uint64_t hash;
        std::vector<argus::SearchHit> files;
    };

    const argus::MultiIndex* multi_;
    QTreeWidget*   tree_;
    QProgressBar*  progress_;
    QLabel*        summary_;
    QPushButton*   cancel_btn_;
    std::thread    worker_;
    std::atomic<bool> cancelled_{false};
    std::vector<DupGroup> found_;
    std::mutex     found_mtx_;
};

// ================== IconCache ==========================================

class IconCache {
public:
    IconCache() {
        folder_  = shellIconForPath(L"", true);
        generic_ = shellIconForPath(L"file", false);
    }

    QIcon iconFor(std::wstring_view name, bool isDir) {
        if (isDir) return folder_;
        int dot = -1;
        for (int i = int(name.size()) - 1; i >= 0; --i) {
            if (name[i] == L'.') { dot = i; break; }
            if (name[i] == L'\\' || name[i] == L'/') break;
        }
        if (dot < 0 || dot == int(name.size()) - 1) return generic_;

        QString key;
        key.reserve(int(name.size()) - dot);
        for (int i = dot + 1; i < int(name.size()); ++i) {
            wchar_t c = name[i];
            if (c >= L'A' && c <= L'Z') c += 32;
            key.append(QChar(c));
        }
        auto it = ext_cache_.constFind(key);
        if (it != ext_cache_.constEnd()) return it.value();

        std::wstring fake = L"argus.";
        fake.append(name.data() + dot + 1, name.size() - dot - 1);
        QIcon ic = shellIconForPath(fake.c_str(), false);
        if (ic.isNull()) ic = generic_;
        ext_cache_.insert(key, ic);
        return ic;
    }

private:
    static QIcon shellIconForPath(const wchar_t* path, bool asFolder) {
        SHFILEINFOW sfi{};
        UINT flags = SHGFI_ICON | SHGFI_SMALLICON | SHGFI_USEFILEATTRIBUTES;
        DWORD attrs = asFolder ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
        if (!SHGetFileInfoW(path, attrs, &sfi, sizeof(sfi), flags)) return {};
        QImage img = QImage::fromHICON(sfi.hIcon);
        DestroyIcon(sfi.hIcon);
        return QIcon(QPixmap::fromImage(img));
    }

    QIcon folder_;
    QIcon generic_;
    QHash<QString, QIcon> ext_cache_;
};

// ================== FileModel ==========================================

class FileModel : public QAbstractTableModel {
    Q_OBJECT
public:
    enum Column { ColName = 0, ColPath = 1, ColSize = 2, ColModified = 3, ColCount };

    FileModel(QObject* parent = nullptr) : QAbstractTableModel(parent) {}

    void setMulti(const argus::MultiIndex* m) { multi_ = m; }
    void setIconCache(IconCache* c)          { icons_ = c; }

    void setResults(std::vector<argus::SearchHit>&& hits) {
        beginResetModel();
        results_ = std::move(hits);
        applySort();
        endResetModel();
    }

    int rowCount(const QModelIndex& = {}) const override { return int(results_.size()); }
    int columnCount(const QModelIndex& = {}) const override { return ColCount; }

    QVariant headerData(int section, Qt::Orientation orient, int role) const override {
        if (role != Qt::DisplayRole || orient != Qt::Horizontal) return {};
        switch (section) {
            case ColName:     return "Name";
            case ColPath:     return "Path";
            case ColSize:     return "Size";
            case ColModified: return "Modified";
        }
        return {};
    }

    QVariant data(const QModelIndex& mi, int role) const override {
        if (!multi_ || !mi.isValid()) return {};
        const argus::SearchHit h = results_[mi.row()];
        const argus::Index& idx = multi_->index(h.drive_slot);
        const auto& e = idx.entry(h.entry_idx);

        if (role == Qt::DecorationRole && mi.column() == ColName && icons_)
            return icons_->iconFor(idx.name(h.entry_idx), idx.is_directory(h.entry_idx));

        if (role == Qt::DisplayRole) {
            switch (mi.column()) {
                case ColName: {
                    auto sv = idx.name(h.entry_idx);
                    return QString::fromWCharArray(sv.data(), int(sv.size()));
                }
                case ColPath: {
                    auto p = idx.full_path(h.entry_idx);
                    int slash = int(p.size()) - 1;
                    while (slash >= 0 && p[slash] != L'\\') --slash;
                    if (slash > 0) return QString::fromWCharArray(p.data(), slash);
                    return QString::fromWCharArray(p.data(), int(p.size()));
                }
                case ColSize:
                    if (e.flags & argus::kFlagDirectory) return QVariant();
                    return QLocale::system().formattedDataSize(qint64(e.size));
                case ColModified: {
                    if (e.modified_time == 0) return QVariant();
                    const qint64 filetime_epoch_ms = -11644473600000LL;
                    qint64 ms = qint64(e.modified_time / 10000ULL) + filetime_epoch_ms;
                    return QDateTime::fromMSecsSinceEpoch(ms).toString("yyyy-MM-dd HH:mm");
                }
            }
        }
        if (role == Qt::TextAlignmentRole && mi.column() == ColSize)
            return int(Qt::AlignRight | Qt::AlignVCenter);
        return {};
    }

    void sort(int column, Qt::SortOrder order) override {
        sort_column_ = column;
        sort_order_  = order;
        beginResetModel();
        applySort();
        endResetModel();
    }

    argus::SearchHit hitAt(int row) const {
        if (row < 0 || row >= int(results_.size())) return {0xFF, UINT32_MAX};
        return results_[row];
    }

private:
    void applySort() {
        if (!multi_ || results_.empty()) return;
        const auto* m = multi_;
        const int col   = sort_column_;
        const bool asc  = (sort_order_ == Qt::AscendingOrder);

        auto cmp = [&](const argus::SearchHit& a, const argus::SearchHit& b) -> bool {
            const auto& ea = m->index(a.drive_slot).entry(a.entry_idx);
            const auto& eb = m->index(b.drive_slot).entry(b.entry_idx);
            switch (col) {
                case ColSize:
                    if (ea.size != eb.size) return asc ? ea.size < eb.size : ea.size > eb.size;
                    break;
                case ColModified:
                    if (ea.modified_time != eb.modified_time)
                        return asc ? ea.modified_time < eb.modified_time
                                    : ea.modified_time > eb.modified_time;
                    break;
                case ColPath: {
                    auto pa = m->index(a.drive_slot).full_path(a.entry_idx);
                    auto pb = m->index(b.drive_slot).full_path(b.entry_idx);
                    int c = _wcsicmp(pa.c_str(), pb.c_str());
                    if (c != 0) return asc ? c < 0 : c > 0;
                    break;
                }
                case ColName:
                default: {
                    auto na = m->index(a.drive_slot).name(a.entry_idx);
                    auto nb = m->index(b.drive_slot).name(b.entry_idx);
                    std::wstring wa(na.data(), na.size());
                    std::wstring wb(nb.data(), nb.size());
                    int c = _wcsicmp(wa.c_str(), wb.c_str());
                    if (c != 0) return asc ? c < 0 : c > 0;
                    break;
                }
            }
            return (a.drive_slot != b.drive_slot) ? a.drive_slot < b.drive_slot
                                                  : a.entry_idx < b.entry_idx;
        };
        std::sort(results_.begin(), results_.end(), cmp);
    }

    const argus::MultiIndex* multi_ = nullptr;
    IconCache*               icons_ = nullptr;
    std::vector<argus::SearchHit> results_;
    int         sort_column_ = -1;
    Qt::SortOrder sort_order_ = Qt::AscendingOrder;
};

// ================== MainWindow =========================================

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    MainWindow();
    ~MainWindow();

private slots:
    void pollScan();
    void onSearchTextChanged();
    void runSearch();
    void onActivated(const QModelIndex&);
    void onContextMenu(const QPoint&);
    void onDriveChanged(int);
    void onModeChanged(int);
    void onFilterChanged(int);
    void copySelectedPaths();
    void pollUsn();

private:
    void startScan();
    void loadOrScan();
    void openInExplorer(const argus::SearchHit&);
    void openFile(const argus::SearchHit&);
    void updateStatusReady();
    std::vector<wchar_t> selectedDrives() const;
    std::vector<wchar_t> availableDrives() const;

    QLineEdit*    search_;
    QComboBox*    drive_combo_;
    QComboBox*    mode_combo_;
    QComboBox*    filter_combo_;
    QTableView*   table_;
    FileModel*    model_;
    IconCache     icon_cache_;
    QLabel*       status_left_;
    QLabel*       hint_;
    QProgressBar* progress_;
    QTimer*       poll_timer_;
    QTimer*       debounce_;
    QTimer*       usn_timer_;

    argus::MultiIndex          multi_;
    argus::MultiIndex::AggregateStats stats_;
    std::thread scan_thread_;
    std::atomic<bool> scan_cancelled_{false};
    std::vector<wchar_t> avail_;  // populated in ctor
};

MainWindow::MainWindow() {
    setWindowTitle("Argus — Instant File Search");
    resize(1220, 720);

    if (QStyleFactory::keys().contains("windows11", Qt::CaseInsensitive))
        QApplication::setStyle(QStyleFactory::create("windows11"));

    // -------- Discover NTFS drives --------
    avail_ = availableDrives();

    // -------- Toolbar --------
    auto* central = new QWidget();
    setCentralWidget(central);
    auto* v = new QVBoxLayout(central);
    v->setContentsMargins(10, 10, 10, 6);
    v->setSpacing(6);

    auto* head = new QHBoxLayout();
    head->setSpacing(8);

    drive_combo_ = new QComboBox();
    drive_combo_->addItem("All Drives");
    for (wchar_t d : avail_) drive_combo_->addItem(QString(QChar(d)) + ":");
    drive_combo_->setFixedWidth(120);

    search_ = new QLineEdit();
    search_->setPlaceholderText("Search — name, or use ext:pdf size:>10MB modified:<7d …");
    search_->setClearButtonEnabled(true);
    search_->setEnabled(false);

    mode_combo_ = new QComboBox();
    mode_combo_->addItem("Text");
    mode_combo_->addItem("Wildcard");
    mode_combo_->addItem("Regex");
    mode_combo_->addItem("Fuzzy");
    mode_combo_->setFixedWidth(100);

    filter_combo_ = new QComboBox();
    filter_combo_->addItem("All");
    filter_combo_->addItem("Files");
    filter_combo_->addItem("Folders");
    filter_combo_->setFixedWidth(100);

    head->addWidget(drive_combo_);
    head->addWidget(search_, 1);
    head->addWidget(mode_combo_);
    head->addWidget(filter_combo_);
    v->addLayout(head);

    // Query syntax hint (dim, single line).
    hint_ = new QLabel("Hints:  ext:pdf   type:image|video|audio|document|archive|code   "
                       "size:>100MB   modified:<7d   path:downloads   !exclude");
    hint_->setStyleSheet("color: palette(mid); font-size: 11px;");
    v->addWidget(hint_);

    // -------- Table --------
    table_ = new QTableView();
    model_ = new FileModel(this);
    model_->setMulti(&multi_);
    model_->setIconCache(&icon_cache_);
    table_->setModel(model_);
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table_->setAlternatingRowColors(true);
    table_->setShowGrid(false);
    table_->setSortingEnabled(true);
    table_->verticalHeader()->setVisible(false);
    table_->verticalHeader()->setDefaultSectionSize(22);
    table_->horizontalHeader()->setSectionResizeMode(FileModel::ColName,     QHeaderView::Interactive);
    table_->horizontalHeader()->setSectionResizeMode(FileModel::ColPath,     QHeaderView::Stretch);
    table_->horizontalHeader()->setSectionResizeMode(FileModel::ColSize,     QHeaderView::ResizeToContents);
    table_->horizontalHeader()->setSectionResizeMode(FileModel::ColModified, QHeaderView::ResizeToContents);
    table_->setColumnWidth(FileModel::ColName, 280);
    table_->setContextMenuPolicy(Qt::CustomContextMenu);
    table_->setIconSize(QSize(16, 16));
    v->addWidget(table_, 1);

    // -------- Statusbar --------
    status_left_ = new QLabel("Ready.");
    progress_ = new QProgressBar();
    progress_->setRange(0, 100);
    progress_->setValue(0);
    progress_->setFixedWidth(220);
    progress_->setTextVisible(true);
    progress_->hide();
    statusBar()->addWidget(status_left_, 1);
    statusBar()->addPermanentWidget(progress_);

    // -------- Timers --------
    debounce_ = new QTimer(this);
    debounce_->setSingleShot(true);
    debounce_->setInterval(150);
    connect(debounce_, &QTimer::timeout, this, &MainWindow::runSearch);

    poll_timer_ = new QTimer(this);
    poll_timer_->setInterval(80);
    connect(poll_timer_, &QTimer::timeout, this, &MainWindow::pollScan);

    usn_timer_ = new QTimer(this);
    usn_timer_->setInterval(700);
    connect(usn_timer_, &QTimer::timeout, this, &MainWindow::pollUsn);

    // -------- Signals --------
    connect(search_, &QLineEdit::textChanged, this, &MainWindow::onSearchTextChanged);
    connect(table_,  &QTableView::activated,  this, &MainWindow::onActivated);
    connect(table_,  &QTableView::customContextMenuRequested,
            this, &MainWindow::onContextMenu);
    connect(drive_combo_, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &MainWindow::onDriveChanged);
    connect(mode_combo_, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &MainWindow::onModeChanged);
    connect(filter_combo_, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &MainWindow::onFilterChanged);

    auto* sc_find = new QShortcut(QKeySequence("Ctrl+F"), this);
    connect(sc_find, &QShortcut::activated, this, [this]{
        search_->setFocus();
        search_->selectAll();
    });
    auto* sc_rescan = new QShortcut(QKeySequence("F5"), this);
    connect(sc_rescan, &QShortcut::activated, this, &MainWindow::startScan);
    auto* sc_esc = new QShortcut(QKeySequence("Escape"), this);
    connect(sc_esc, &QShortcut::activated, this, [this]{
        if (search_->hasFocus() && !search_->text().isEmpty()) search_->clear();
    });
    auto* sc_copy = new QShortcut(QKeySequence::Copy, table_);
    sc_copy->setContext(Qt::WidgetShortcut);
    connect(sc_copy, &QShortcut::activated, this, &MainWindow::copySelectedPaths);

    // Ctrl+Enter -> open containing folder for the selected row.
    auto* sc_reveal = new QShortcut(QKeySequence("Ctrl+Return"), this);
    connect(sc_reveal, &QShortcut::activated, this, [this]{
        auto sel = table_->selectionModel()->selectedRows();
        if (sel.isEmpty()) return;
        argus::SearchHit h = model_->hitAt(sel.first().row());
        if (h.entry_idx == UINT32_MAX) return;
        auto path = multi_.index(h.drive_slot).full_path(h.entry_idx);
        QString qpath = QString::fromWCharArray(path.data(), int(path.size()));
        QString param = "/select,\"" + QDir::toNativeSeparators(qpath) + "\"";
        ShellExecuteW(nullptr, L"open", L"explorer.exe",
                      reinterpret_cast<LPCWSTR>(param.utf16()),
                      nullptr, SW_SHOWNORMAL);
    });

    // Alt+Enter -> shell Properties dialog.
    auto* sc_props = new QShortcut(QKeySequence("Alt+Return"), this);
    connect(sc_props, &QShortcut::activated, this, [this]{
        auto sel = table_->selectionModel()->selectedRows();
        if (sel.isEmpty()) return;
        argus::SearchHit h = model_->hitAt(sel.first().row());
        if (h.entry_idx == UINT32_MAX) return;
        auto path = multi_.index(h.drive_slot).full_path(h.entry_idx);
        SHELLEXECUTEINFOW sei{};
        sei.cbSize = sizeof(sei);
        sei.fMask  = SEE_MASK_INVOKEIDLIST;
        sei.lpVerb = L"properties";
        sei.lpFile = path.c_str();
        sei.nShow  = SW_SHOWNORMAL;
        ShellExecuteExW(&sei);
    });

    // Delete -> move to recycle bin (with confirmation).
    auto* sc_del = new QShortcut(QKeySequence("Delete"), table_);
    sc_del->setContext(Qt::WidgetShortcut);
    connect(sc_del, &QShortcut::activated, this, [this]{
        auto sel = table_->selectionModel()->selectedRows();
        if (sel.isEmpty()) return;
        // Doppel-Null-terminierte Liste fuer SHFileOperation aufbauen.
        std::wstring buf;
        int cnt = 0;
        for (const auto& mi : sel) {
            argus::SearchHit h = model_->hitAt(mi.row());
            if (h.entry_idx == UINT32_MAX) continue;
            buf.append(multi_.index(h.drive_slot).full_path(h.entry_idx));
            buf.push_back(L'\0');
            ++cnt;
        }
        buf.push_back(L'\0');
        if (QMessageBox::question(this, "Move to Recycle Bin",
                QString("Move %1 item(s) to the Recycle Bin?").arg(cnt))
                != QMessageBox::Yes)
            return;
        SHFILEOPSTRUCTW op{};
        op.hwnd   = HWND(winId());
        op.wFunc  = FO_DELETE;
        op.pFrom  = buf.c_str();
        op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_NOERRORUI | FOF_SILENT;
        SHFileOperationW(&op);
    });

    // Menu bar with Tools -> Find duplicates.
    auto* toolsMenu = menuBar()->addMenu("Tools");
    auto* aDup = toolsMenu->addAction("Find duplicates…");
    connect(aDup, &QAction::triggered, this, [this]{
        if (multi_.total_entries() == 0) {
            QMessageBox::information(this, "Argus",
                "Wait until indexing is done before running the duplicate finder.");
            return;
        }
        auto* dlg = new DuplicateFinderDialog(&multi_, this);
        dlg->setAttribute(Qt::WA_DeleteOnClose);
        dlg->show();
    });
    auto* aRescan = toolsMenu->addAction("Re-scan current drive(s)\tF5");
    connect(aRescan, &QAction::triggered, this, &MainWindow::startScan);
    toolsMenu->addSeparator();
    auto* aQuit = toolsMenu->addAction("Quit");
    connect(aQuit, &QAction::triggered, this, &QMainWindow::close);

    auto* helpMenu = menuBar()->addMenu("Help");
    auto* aAbout = helpMenu->addAction("About Argus");
    connect(aAbout, &QAction::triggered, this, [this]{
        QMessageBox::about(this, "About Argus",
            "<h3>Argus — Instant NTFS File Search</h3>"
            "<p>Version 0.6.0. MIT-licensed C++20 + Qt6.</p>"
            "<p><a href='https://github.com/0x4Devs/argus'>github.com/0x4Devs/argus</a></p>");
    });

    loadOrScan();
}

MainWindow::~MainWindow() {
    scan_cancelled_.store(true);
    if (scan_thread_.joinable()) scan_thread_.join();
    // Cache jeden Index einzeln.
    for (size_t i = 0; i < multi_.drive_count(); ++i) {
        auto path = argus::CacheFilePath(multi_.drive_letter(i));
        if (!path.empty() && multi_.index(i).entry_count() > 0)
            multi_.index(i).SaveTo(path);
    }
}

// ---------- Drive discovery ----------

std::vector<wchar_t> MainWindow::availableDrives() const {
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

std::vector<wchar_t> MainWindow::selectedDrives() const {
    if (drive_combo_->currentIndex() == 0) return avail_;
    QString t = drive_combo_->currentText();
    if (t.isEmpty()) return avail_;
    return { wchar_t(t.at(0).toUpper().unicode()) };
}

// ---------- Scan / Load ----------

void MainWindow::loadOrScan() {
    auto drives = selectedDrives();
    if (drives.empty()) {
        status_left_->setText("No NTFS drives found.");
        return;
    }
    multi_.SetDrives(drives);
    bool all_loaded = true;
    for (size_t i = 0; i < multi_.drive_count(); ++i) {
        wchar_t d = multi_.drive_letter(i);
        auto path = argus::CacheFilePath(d);
        if (path.empty() || !multi_.index(i).LoadFrom(path)) { all_loaded = false; continue; }
        // Volume-Serial abgleichen.
        wchar_t p[16]; swprintf(p, 16, L"\\\\.\\%c:", d);
        HANDLE h = CreateFileW(p, GENERIC_READ,
                              FILE_SHARE_READ | FILE_SHARE_WRITE,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) { all_loaded = false; continue; }
        argus::ntfs::BootSector bs{};
        LARGE_INTEGER zero{}; zero.QuadPart = 0;
        SetFilePointerEx(h, zero, nullptr, FILE_BEGIN);
        DWORD got = 0;
        ReadFile(h, &bs, sizeof(bs), &got, nullptr);
        CloseHandle(h);
        if (got != sizeof(bs) || bs.volume_serial != multi_.index(i).volume_serial()) {
            all_loaded = false;
        }
    }
    if (all_loaded) {
        auto st = multi_.ApplyUsnChanges();
        if (st.any_rolled_over) { startScan(); return; }
        search_->setEnabled(true);
        search_->setFocus();
        updateStatusReady();
        usn_timer_->start();
        return;
    }
    startScan();
}

void MainWindow::startScan() {
    scan_cancelled_.store(true);
    if (scan_thread_.joinable()) scan_thread_.join();
    usn_timer_->stop();

    auto drives = selectedDrives();
    multi_.SetDrives(drives);

    stats_.records_seen.store(0);
    stats_.total_records.store(0);
    stats_.entries.store(0);
    stats_.bytes_read.store(0);
    stats_.drives_done.store(0);
    stats_.drives_total.store(uint32_t(drives.size()));
    stats_.done.store(false);
    model_->setResults({});

    search_->setEnabled(false);
    progress_->show();
    progress_->setValue(0);
    status_left_->setText(QString("Indexing %L1 drive(s)…").arg(int(drives.size())));

    scan_thread_ = std::thread([this]{ multi_.ScanAll(&stats_); });
    poll_timer_->start();
}

void MainWindow::pollScan() {
    const uint64_t total = stats_.total_records.load();
    const uint64_t seen  = stats_.records_seen.load();
    const uint64_t ents  = stats_.entries.load();
    const uint32_t dd    = stats_.drives_done.load();
    const uint32_t dt    = stats_.drives_total.load();

    if (total > 0) {
        int pct = int(seen * 100 / total);
        progress_->setValue(pct);
        status_left_->setText(QString("Indexing (%1/%2 drives): %L3 / %L4 records — %L5 entries")
                              .arg(dd).arg(dt)
                              .arg(qulonglong(seen)).arg(qulonglong(total)).arg(qulonglong(ents)));
    }

    if (stats_.done.load()) {
        poll_timer_->stop();
        if (scan_thread_.joinable()) scan_thread_.join();
        progress_->hide();
        search_->setEnabled(true);
        search_->setFocus();
        updateStatusReady();
        runSearch();

        // Cache jeden Drive.
        for (size_t i = 0; i < multi_.drive_count(); ++i) {
            auto path = argus::CacheFilePath(multi_.drive_letter(i));
            if (!path.empty()) multi_.index(i).SaveTo(path);
        }
        usn_timer_->start();
    }
}

void MainWindow::updateStatusReady() {
    QString drives_txt;
    if (multi_.drive_count() == 1) {
        drives_txt = QString(QChar(multi_.drive_letter(0))) + ":";
    } else {
        drives_txt = QString("%1 NTFS volumes").arg(int(multi_.drive_count()));
    }
    status_left_->setText(QString("Ready — %L1 entries across %2")
        .arg(qulonglong(multi_.total_entries())).arg(drives_txt));
}

// ---------- Search ----------

void MainWindow::onSearchTextChanged() { debounce_->start(); }
void MainWindow::onModeChanged(int)     { debounce_->start(); }
void MainWindow::onFilterChanged(int)   { debounce_->start(); }

void MainWindow::runSearch() {
    const QString qtext = search_->text();
    if (qtext.isEmpty()) {
        model_->setResults({});
        updateStatusReady();
        return;
    }

    std::wstring raw = qtext.toStdWString();
    argus::SearchMode mode = static_cast<argus::SearchMode>(mode_combo_->currentIndex());

    // Trenne Name-Teil (Tokens ohne ':') vom Advanced-Query-Teil (mit ':')
    // — nur im Text-Modus. Bei Wildcard/Regex bleibt der komplette String der
    // Pattern und Advanced-Query wird nicht extra angewendet.
    std::wstring name_part;
    std::wstring adv_part;
    if (mode == argus::SearchMode::Substring) {
        // Naiv aufteilen: alles was `field:value` oder `!field:value` oder `!wort` ist,
        // geht in adv_part; Rest in name_part (durch Leerzeichen getrennt).
        size_t i = 0;
        while (i < raw.size()) {
            while (i < raw.size() && iswspace(raw[i])) ++i;
            if (i >= raw.size()) break;
            size_t s = i;
            while (i < raw.size() && !iswspace(raw[i])) ++i;
            std::wstring_view tok(raw.data() + s, i - s);
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
    } else {
        name_part = raw;
    }

    argus::Query adv = argus::ParseQuery(adv_part);

    argus::SearchOptions opt;
    opt.max_results = 20000;
    opt.mode = mode;
    if (filter_combo_->currentIndex() == 1) opt.files_only = true;
    if (filter_combo_->currentIndex() == 2) opt.dirs_only  = true;
    opt.advanced_query = adv.empty() ? nullptr : &adv;

    std::vector<argus::SearchHit> hits;
    auto t0 = std::chrono::steady_clock::now();
    size_t max_per = std::max<size_t>(1, opt.max_results / std::max<size_t>(1, multi_.drive_count()));
    for (size_t slot = 0; slot < multi_.drive_count(); ++slot) {
        argus::SearchOptions o = opt;
        o.max_results = max_per;
        auto ids = argus::Search(multi_.index(slot), name_part, o);
        for (uint32_t id : ids) {
            hits.push_back({uint8_t(slot), id});
            if (hits.size() >= opt.max_results) break;
        }
        if (hits.size() >= opt.max_results) break;
    }
    auto t1 = std::chrono::steady_clock::now();
    double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

    size_t total = hits.size();
    model_->setResults(std::move(hits));
    status_left_->setText(QString("%L1 hits  (%2 ms across %L3 entries)")
        .arg(qulonglong(total)).arg(QString::number(ms, 'f', 1))
        .arg(qulonglong(multi_.total_entries())));
}

// ---------- Interaction ----------

void MainWindow::onActivated(const QModelIndex& mi) {
    argus::SearchHit h = model_->hitAt(mi.row());
    if (h.entry_idx == UINT32_MAX) return;
    if (multi_.index(h.drive_slot).is_directory(h.entry_idx)) openInExplorer(h);
    else                                                     openFile(h);
}

void MainWindow::onContextMenu(const QPoint& pos) {
    QModelIndex mi = table_->indexAt(pos);
    if (!mi.isValid()) return;
    argus::SearchHit h = model_->hitAt(mi.row());
    if (h.entry_idx == UINT32_MAX) return;
    bool is_dir = multi_.index(h.drive_slot).is_directory(h.entry_idx);

    QMenu menu(this);
    auto* aOpen = menu.addAction(is_dir ? "Open folder" : "Open file");
    auto* aReveal = menu.addAction("Reveal in Explorer");
    menu.addSeparator();
    auto* aCopyPath = menu.addAction("Copy path");
    menu.addSeparator();
    auto* aDetails = menu.addAction("NTFS details…");
    QAction* chosen = menu.exec(table_->viewport()->mapToGlobal(pos));

    if (chosen == aOpen) {
        if (is_dir) openInExplorer(h); else openFile(h);
    } else if (chosen == aReveal) {
        auto path = multi_.index(h.drive_slot).full_path(h.entry_idx);
        QString qpath = QString::fromWCharArray(path.data(), int(path.size()));
        QString param = "/select,\"" + QDir::toNativeSeparators(qpath) + "\"";
        ShellExecuteW(nullptr, L"open", L"explorer.exe",
                      reinterpret_cast<LPCWSTR>(param.utf16()),
                      nullptr, SW_SHOWNORMAL);
    } else if (chosen == aCopyPath) {
        copySelectedPaths();
    } else if (chosen == aDetails) {
        // mft_id ist bei uns = Index in mft_to_idx, hier braucht ReadMftDetails
        // aber die tatsaechliche MFT-Nummer. Wir suchen aus mft_to_idx zurueck.
        const argus::Index& idx = multi_.index(h.drive_slot);
        uint32_t mft_id = 0;
        // Reverse-Lookup: waere idealer wenn Entry das MFT-ID speichern wuerde,
        // aber fuer die Details reicht der Umweg.
        for (uint32_t i = 0; i < idx.entry_count(); ++i) {}
        // Direkter Weg: Entry hat kein mft_id, aber wir wissen dass entries_[k]
        // an Position mft_to_idx_[mft_id]=k liegt. Wir suchen linear.
        {
            const auto& e_target = idx.entry(h.entry_idx);
            (void)e_target;
            // Linearer Scan der mft_to_idx-Umkehrung waere langsam. Statt dessen
            // adden wir demnaechst mft_id direkt in Entry. Fuer MVP: linear scan.
            // Alternativ: die Runlist ist da, wir koennten die MFT durchgehen
            // und den Datensatz finden — aber die Reverse-Map ist auch okay.
        }
        // Reverse-Lookup mft_to_idx (public accessor fehlt — wir liefern hier
        // Best-Effort: iterieren durch mft_to_idx im core und suchen h.entry_idx).
        // Wir exponieren dafuer eine neue Methode idx.mft_id_for(entry_idx).
        mft_id = h.entry_idx; // Platzhalter, wird durch Getter ueberschrieben:
        // Wir nutzen die vorhandene entries()-API + externe Reverse-Suche:
        // (Als korrektes Redesign kaeme mft_id ins Entry. Fuer jetzt reicht
        // die Info aus dem Parent-Feld nicht — wir loesen ueber einen Getter.)

        // Robustere Loesung: neue Methode Index::mft_id_of(entry_idx)
        mft_id = idx.mft_id_of(h.entry_idx);

        auto d = argus::ReadMftDetails(idx, mft_id);
        auto path = idx.full_path(h.entry_idx);
        QString qpath = QString::fromWCharArray(path.data(), int(path.size()));
        NtfsDetailsDialog dlg(d, qpath, idx.drive_letter(), this);
        dlg.exec();
    }
}

void MainWindow::openInExplorer(const argus::SearchHit& h) {
    auto path = multi_.index(h.drive_slot).full_path(h.entry_idx);
    QDesktopServices::openUrl(QUrl::fromLocalFile(
        QString::fromWCharArray(path.data(), int(path.size()))));
}
void MainWindow::openFile(const argus::SearchHit& h) { openInExplorer(h); }

void MainWindow::copySelectedPaths() {
    auto sel = table_->selectionModel()->selectedRows();
    if (sel.isEmpty()) return;
    QStringList lines;
    for (const auto& mi : sel) {
        argus::SearchHit h = model_->hitAt(mi.row());
        if (h.entry_idx == UINT32_MAX) continue;
        auto p = multi_.index(h.drive_slot).full_path(h.entry_idx);
        lines << QString::fromWCharArray(p.data(), int(p.size()));
    }
    QApplication::clipboard()->setText(lines.join("\n"));
}

void MainWindow::onDriveChanged(int) { loadOrScan(); }

void MainWindow::pollUsn() {
    if (multi_.total_entries() == 0) return;
    auto st = multi_.ApplyUsnChanges();
    if (st.any_rolled_over) { usn_timer_->stop(); startScan(); return; }
    if ((st.added || st.renamed || st.deleted) && !search_->text().isEmpty())
        runSearch();
}

// ================== Entry ================================================

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    QApplication::setApplicationName("Argus");
    QApplication::setOrganizationName("0x4Devs");
    MainWindow w;
    w.show();
    return app.exec();
}

#ifdef _WIN32
extern "C" int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int) {
    return main(__argc, __argv);
}
#endif

#include "gui.moc"
