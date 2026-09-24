// Argus — GUI (Qt6 Widgets).
// v0.2.0: Sortierung, Shell-Icons, Regex/Wildcard-Modes, Files/Folders-Filter,
//         Keyboard-Shortcuts (Ctrl+F, F5, Esc, Ctrl+C).

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
#include <QToolBar>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <memory>
#include <thread>

#include "core/index.h"
#include "core/search.h"

// ================== IconCache ==========================================
// Windows shell icons per extension via SHGetFileInfoW + SHGFI_USEFILEATTRIBUTES.
// Cached: an extension only ever hits the shell once.

class IconCache {
public:
    IconCache() {
        folder_ = shellIconForPath(L"", true);
        generic_ = shellIconForPath(L"file", false);
    }

    QIcon iconFor(std::wstring_view name, bool isDir) {
        if (isDir) return folder_;
        // Extension aus name extrahieren.
        int dot = -1;
        for (int i = int(name.size()) - 1; i >= 0; --i) {
            if (name[i] == L'.') { dot = i; break; }
            if (name[i] == L'\\' || name[i] == L'/') break;
        }
        if (dot < 0 || dot == int(name.size()) - 1) return generic_;

        // Lowercase key.
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

    void setIndex(const argus::Index* idx) { index_ = idx; }
    void setIconCache(IconCache* c)        { icons_ = c; }

    void setResults(std::vector<uint32_t>&& ids) {
        beginResetModel();
        results_ = std::move(ids);
        applySort();
        endResetModel();
    }

    int rowCount(const QModelIndex& = {}) const override {
        return int(results_.size());
    }
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
        if (!index_ || !mi.isValid()) return {};
        const uint32_t id = results_[mi.row()];
        const auto& e = index_->entry(id);

        if (role == Qt::DecorationRole && mi.column() == ColName && icons_) {
            return icons_->iconFor(index_->name(id), index_->is_directory(id));
        }
        if (role == Qt::DisplayRole) {
            switch (mi.column()) {
                case ColName: {
                    auto sv = index_->name(id);
                    return QString::fromWCharArray(sv.data(), int(sv.size()));
                }
                case ColPath: {
                    auto p = index_->full_path(id);
                    int slash = int(p.size()) - 1;
                    while (slash >= 0 && p[slash] != L'\\') --slash;
                    if (slash > 0)
                        return QString::fromWCharArray(p.data(), slash);
                    return QString::fromWCharArray(p.data(), int(p.size()));
                }
                case ColSize: {
                    if (e.flags & argus::kFlagDirectory) return QVariant();
                    return QLocale::system().formattedDataSize(qint64(e.size));
                }
                case ColModified: {
                    if (e.modified_time == 0) return QVariant();
                    const qint64 filetime_epoch_ms = -11644473600000LL;
                    qint64 ms = qint64(e.modified_time / 10000ULL) + filetime_epoch_ms;
                    return QDateTime::fromMSecsSinceEpoch(ms).toString("yyyy-MM-dd HH:mm");
                }
            }
        }
        if (role == Qt::TextAlignmentRole && mi.column() == ColSize) {
            return int(Qt::AlignRight | Qt::AlignVCenter);
        }
        if (role == Qt::UserRole) return id;
        return {};
    }

    void sort(int column, Qt::SortOrder order) override {
        sort_column_ = column;
        sort_order_  = order;
        beginResetModel();
        applySort();
        endResetModel();
    }

    uint32_t entryIdFor(int row) const {
        if (row < 0 || row >= int(results_.size())) return UINT32_MAX;
        return results_[row];
    }

private:
    void applySort() {
        if (!index_ || results_.empty()) return;
        const auto& idx = *index_;
        const int col   = sort_column_;
        const bool asc  = (sort_order_ == Qt::AscendingOrder);

        auto cmp = [&](uint32_t a, uint32_t b) -> bool {
            const auto& ea = idx.entry(a);
            const auto& eb = idx.entry(b);
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
                    auto pa = idx.full_path(a);
                    auto pb = idx.full_path(b);
                    int c = _wcsicmp(pa.c_str(), pb.c_str());
                    if (c != 0) return asc ? c < 0 : c > 0;
                    break;
                }
                case ColName:
                default: {
                    auto na = idx.name(a);
                    auto nb = idx.name(b);
                    std::wstring wa(na.data(), na.size());
                    std::wstring wb(nb.data(), nb.size());
                    int c = _wcsicmp(wa.c_str(), wb.c_str());
                    if (c != 0) return asc ? c < 0 : c > 0;
                    break;
                }
            }
            return a < b;
        };
        std::sort(results_.begin(), results_.end(), cmp);
    }

    const argus::Index* index_ = nullptr;
    IconCache*          icons_ = nullptr;
    std::vector<uint32_t> results_;
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

private:
    void startScan();
    void openInExplorer(uint32_t id);
    void openFile(uint32_t id);
    void updateStatusReady();

    // Toolbar
    QLineEdit*    search_;
    QComboBox*    drive_combo_;
    QComboBox*    mode_combo_;
    QComboBox*    filter_combo_;

    // Table
    QTableView*   table_;
    FileModel*    model_;
    IconCache     icon_cache_;

    // Statusbar
    QLabel*       status_left_;
    QProgressBar* progress_;

    QTimer*       poll_timer_;
    QTimer*       debounce_;

    argus::Index index_;
    argus::Index::ScanStats stats_;
    std::thread scan_thread_;
    std::atomic<bool> scan_cancelled_{false};
};

MainWindow::MainWindow() {
    setWindowTitle("Argus — Instant File Search");
    resize(1180, 720);

    if (QStyleFactory::keys().contains("windows11", Qt::CaseInsensitive))
        QApplication::setStyle(QStyleFactory::create("windows11"));

    // -------- Toolbar (drive + search + mode + filter) --------
    auto* central = new QWidget();
    setCentralWidget(central);
    auto* v = new QVBoxLayout(central);
    v->setContentsMargins(10, 10, 10, 6);
    v->setSpacing(8);

    auto* head = new QHBoxLayout();
    head->setSpacing(8);

    drive_combo_ = new QComboBox();
    DWORD mask = GetLogicalDrives();
    for (int i = 0; i < 26; ++i) {
        if (!(mask & (1u << i))) continue;
        wchar_t root[] = { wchar_t(L'A' + i), L':', L'\\', 0 };
        if (GetDriveTypeW(root) != DRIVE_FIXED) continue;
        wchar_t fs[16] = {0};
        if (!GetVolumeInformationW(root, nullptr, 0, nullptr, nullptr, nullptr, fs, 16)) continue;
        if (wcscmp(fs, L"NTFS") != 0) continue;
        drive_combo_->addItem(QString(QChar(L'A' + i)) + ":");
    }
    if (drive_combo_->count() == 0) drive_combo_->addItem("C:");
    drive_combo_->setFixedWidth(80);

    search_ = new QLineEdit();
    search_->setPlaceholderText("Search — type any part of a name…");
    search_->setClearButtonEnabled(true);
    search_->setEnabled(false);

    mode_combo_ = new QComboBox();
    mode_combo_->addItem("Text");       // Substring
    mode_combo_->addItem("Wildcard");   // *.mp4
    mode_combo_->addItem("Regex");
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

    // -------- Table --------
    table_ = new QTableView();
    model_ = new FileModel(this);
    model_->setIndex(&index_);
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
    progress_->setFixedWidth(200);
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

    // -------- Keyboard shortcuts --------
    // Ctrl+F: Focus + select-all search box.
    auto* sc_find = new QShortcut(QKeySequence("Ctrl+F"), this);
    connect(sc_find, &QShortcut::activated, this, [this]{
        search_->setFocus();
        search_->selectAll();
    });
    // F5: rescan current drive.
    auto* sc_rescan = new QShortcut(QKeySequence("F5"), this);
    connect(sc_rescan, &QShortcut::activated, this, &MainWindow::startScan);
    // Esc while search box has focus: clear.
    auto* sc_esc = new QShortcut(QKeySequence("Escape"), this);
    connect(sc_esc, &QShortcut::activated, this, [this]{
        if (search_->hasFocus() && !search_->text().isEmpty()) search_->clear();
    });
    // Ctrl+C on table selection: copy full path(s).
    auto* sc_copy = new QShortcut(QKeySequence::Copy, table_);
    sc_copy->setContext(Qt::WidgetShortcut);
    connect(sc_copy, &QShortcut::activated, this, &MainWindow::copySelectedPaths);

    startScan();
}

MainWindow::~MainWindow() {
    scan_cancelled_.store(true);
    if (scan_thread_.joinable()) scan_thread_.join();
}

void MainWindow::startScan() {
    scan_cancelled_.store(true);
    if (scan_thread_.joinable()) scan_thread_.join();

    scan_cancelled_.store(false);
    stats_.records_seen.store(0);
    stats_.total_records.store(0);
    stats_.entries.store(0);
    stats_.bytes_read.store(0);
    stats_.done.store(false);
    stats_.ok.store(false);
    model_->setResults({});

    wchar_t drive = drive_combo_->currentText().at(0).toUpper().unicode();

    search_->setEnabled(false);
    progress_->show();
    progress_->setValue(0);
    status_left_->setText(QString("Indexing %1: …").arg(drive_combo_->currentText()));

    scan_thread_ = std::thread([this, drive]{
        index_.ScanDrive(drive, &stats_);
    });
    poll_timer_->start();
}

void MainWindow::pollScan() {
    const uint64_t total = stats_.total_records.load();
    const uint64_t seen  = stats_.records_seen.load();
    const uint64_t ents  = stats_.entries.load();

    if (total > 0) {
        int pct = int(seen * 100 / total);
        progress_->setValue(pct);
        status_left_->setText(QString("Indexing %1: … %L2 / %L3 records — %L4 entries")
                              .arg(drive_combo_->currentText())
                              .arg(qulonglong(seen))
                              .arg(qulonglong(total))
                              .arg(qulonglong(ents)));
    }

    if (stats_.done.load()) {
        poll_timer_->stop();
        if (scan_thread_.joinable()) scan_thread_.join();

        if (!stats_.ok.load()) {
            progress_->hide();
            status_left_->setText("MFT read failed — is Argus running as Administrator?");
            QMessageBox::warning(this, "Argus",
                "Could not read the MFT.\n\n"
                "Please make sure Argus was started as Administrator. "
                "Raw NTFS access requires elevated privileges.");
            return;
        }
        progress_->hide();
        search_->setEnabled(true);
        search_->setFocus();
        updateStatusReady();
        runSearch();  // update view if query already present
    }
}

void MainWindow::updateStatusReady() {
    status_left_->setText(QString("Ready — %L1 entries on %2")
        .arg(qulonglong(index_.entry_count()))
        .arg(drive_combo_->currentText()));
}

void MainWindow::onSearchTextChanged() {
    debounce_->start();
}

void MainWindow::onModeChanged(int)   { debounce_->start(); }
void MainWindow::onFilterChanged(int) { debounce_->start(); }

void MainWindow::runSearch() {
    const QString qtext = search_->text();
    if (qtext.isEmpty()) {
        model_->setResults({});
        updateStatusReady();
        return;
    }
    std::wstring q = qtext.toStdWString();
    argus::SearchOptions opt;
    opt.max_results = 10000;
    opt.mode = static_cast<argus::SearchMode>(mode_combo_->currentIndex());
    if (filter_combo_->currentIndex() == 1) opt.files_only = true;
    if (filter_combo_->currentIndex() == 2) opt.dirs_only  = true;

    auto t0 = std::chrono::steady_clock::now();
    auto ids = argus::Search(index_, q, opt);
    auto t1 = std::chrono::steady_clock::now();
    double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

    const size_t total_hits = ids.size();
    model_->setResults(std::move(ids));
    status_left_->setText(QString("%L1 hits  (%2 ms)")
        .arg(qulonglong(total_hits))
        .arg(QString::number(ms, 'f', 1)));
}

void MainWindow::onActivated(const QModelIndex& mi) {
    uint32_t id = model_->entryIdFor(mi.row());
    if (id == UINT32_MAX) return;
    if (index_.is_directory(id)) openInExplorer(id);
    else                         openFile(id);
}

void MainWindow::onContextMenu(const QPoint& pos) {
    QModelIndex mi = table_->indexAt(pos);
    if (!mi.isValid()) return;
    uint32_t id = model_->entryIdFor(mi.row());
    if (id == UINT32_MAX) return;

    QMenu menu(this);
    auto* aOpen = menu.addAction(index_.is_directory(id) ? "Open folder" : "Open file");
    auto* aReveal = menu.addAction("Reveal in Explorer");
    menu.addSeparator();
    auto* aCopyPath = menu.addAction("Copy path");
    QAction* chosen = menu.exec(table_->viewport()->mapToGlobal(pos));

    if (chosen == aOpen) {
        if (index_.is_directory(id)) openInExplorer(id);
        else                         openFile(id);
    } else if (chosen == aReveal) {
        auto path = index_.full_path(id);
        QString qpath = QString::fromWCharArray(path.data(), int(path.size()));
        QString param = "/select,\"" + QDir::toNativeSeparators(qpath) + "\"";
        ShellExecuteW(nullptr, L"open", L"explorer.exe",
                      reinterpret_cast<LPCWSTR>(param.utf16()),
                      nullptr, SW_SHOWNORMAL);
    } else if (chosen == aCopyPath) {
        copySelectedPaths();
    }
}

void MainWindow::openInExplorer(uint32_t id) {
    auto path = index_.full_path(id);
    QDesktopServices::openUrl(QUrl::fromLocalFile(
        QString::fromWCharArray(path.data(), int(path.size()))));
}

void MainWindow::openFile(uint32_t id) {
    auto path = index_.full_path(id);
    QDesktopServices::openUrl(QUrl::fromLocalFile(
        QString::fromWCharArray(path.data(), int(path.size()))));
}

void MainWindow::copySelectedPaths() {
    auto sel = table_->selectionModel()->selectedRows();
    if (sel.isEmpty()) return;
    QStringList lines;
    for (const auto& mi : sel) {
        uint32_t id = model_->entryIdFor(mi.row());
        if (id == UINT32_MAX) continue;
        auto p = index_.full_path(id);
        lines << QString::fromWCharArray(p.data(), int(p.size()));
    }
    QApplication::clipboard()->setText(lines.join("\n"));
}

void MainWindow::onDriveChanged(int) { startScan(); }

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
