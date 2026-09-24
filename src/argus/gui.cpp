// Argus — GUI (Qt6 Widgets).
// Startet als Admin (Manifest), indiziert eine Platte, bietet Live-Suche.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>

#include <QAbstractTableModel>
#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QFileIconProvider>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QIcon>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QMainWindow>
#include <QMenu>
#include <QMessageBox>
#include <QProgressBar>
#include <QPushButton>
#include <QStandardPaths>
#include <QStatusBar>
#include <QStyleFactory>
#include <QTableView>
#include <QThread>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

#include <atomic>
#include <chrono>
#include <memory>
#include <thread>

#include "core/index.h"
#include "core/search.h"

// ================== FileModel ==========================================

class FileModel : public QAbstractTableModel {
    Q_OBJECT
public:
    enum Column { ColName = 0, ColPath = 1, ColSize = 2, ColModified = 3, ColCount };

    FileModel(QObject* parent = nullptr) : QAbstractTableModel(parent) {}

    void setIndex(const argus::Index* idx) { index_ = idx; }

    void setResults(std::vector<uint32_t>&& ids) {
        beginResetModel();
        results_ = std::move(ids);
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
            case ColPath:     return "Pfad";
            case ColSize:     return "Groesse";
            case ColModified: return "Geaendert";
        }
        return {};
    }

    QVariant data(const QModelIndex& mi, int role) const override {
        if (!index_ || !mi.isValid()) return {};
        const uint32_t id = results_[mi.row()];

        if (role == Qt::DisplayRole) {
            const auto& e = index_->entry(id);
            switch (mi.column()) {
                case ColName: {
                    auto sv = index_->name(id);
                    return QString::fromWCharArray(sv.data(), int(sv.size()));
                }
                case ColPath: {
                    auto p = index_->full_path(id);
                    // Nur den Elter zeigen (ohne den Datei-Basename),
                    // sonst dupliziert es die Name-Spalte.
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
                    // FILETIME -> QDateTime (100ns since 1601, epoch offset)
                    const qint64 filetime_epoch_ms = -11644473600000LL;
                    qint64 ms = qint64(e.modified_time / 10000ULL) + filetime_epoch_ms;
                    return QDateTime::fromMSecsSinceEpoch(ms).toString("yyyy-MM-dd HH:mm");
                }
            }
        }
        if (role == Qt::TextAlignmentRole) {
            if (mi.column() == ColSize) return int(Qt::AlignRight | Qt::AlignVCenter);
        }
        if (role == Qt::UserRole) return id;
        return {};
    }

    uint32_t entryIdFor(int row) const {
        if (row < 0 || row >= int(results_.size())) return UINT32_MAX;
        return results_[row];
    }

private:
    const argus::Index* index_ = nullptr;
    std::vector<uint32_t> results_;
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

private:
    void startScan();
    void openInExplorer(uint32_t id);
    void openFile(uint32_t id);

    QLineEdit*    search_;
    QComboBox*    drive_combo_;
    QTableView*   table_;
    FileModel*    model_;
    QLabel*       status_left_;
    QProgressBar* progress_;
    QTimer*       poll_timer_;
    QTimer*       debounce_;

    argus::Index index_;
    argus::Index::ScanStats stats_;
    std::thread scan_thread_;
    std::atomic<bool> scan_cancelled_{false};

    std::atomic<bool> search_cancel_{false};
};

MainWindow::MainWindow() {
    setWindowTitle("Argus — Instant File Search");
    resize(1100, 680);

    // --- Header: Drive-Dropdown + Suchfeld ---
    auto* central = new QWidget();
    setCentralWidget(central);
    auto* v = new QVBoxLayout(central);
    v->setContentsMargins(10, 10, 10, 6);
    v->setSpacing(8);

    auto* head = new QHBoxLayout();
    head->setSpacing(8);
    drive_combo_ = new QComboBox();
    // Alle bereiten NTFS-Laufwerke (nur mounted drives).
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
    search_->setPlaceholderText("Suchen — tippe einen Namen (auch Teil-Wort)…");
    search_->setClearButtonEnabled(true);
    search_->setEnabled(false);

    head->addWidget(drive_combo_);
    head->addWidget(search_, 1);
    v->addLayout(head);

    // --- Table ---
    table_ = new QTableView();
    model_ = new FileModel(this);
    model_->setIndex(&index_);
    table_->setModel(model_);
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table_->setAlternatingRowColors(true);
    table_->setShowGrid(false);
    table_->verticalHeader()->setVisible(false);
    table_->verticalHeader()->setDefaultSectionSize(22);
    table_->horizontalHeader()->setStretchLastSection(false);
    table_->horizontalHeader()->setSectionResizeMode(FileModel::ColName,     QHeaderView::Interactive);
    table_->horizontalHeader()->setSectionResizeMode(FileModel::ColPath,     QHeaderView::Stretch);
    table_->horizontalHeader()->setSectionResizeMode(FileModel::ColSize,     QHeaderView::ResizeToContents);
    table_->horizontalHeader()->setSectionResizeMode(FileModel::ColModified, QHeaderView::ResizeToContents);
    table_->setColumnWidth(FileModel::ColName, 260);
    table_->setContextMenuPolicy(Qt::CustomContextMenu);
    v->addWidget(table_, 1);

    // --- Statusbar ---
    status_left_ = new QLabel("Bereit.");
    progress_ = new QProgressBar();
    progress_->setRange(0, 100);
    progress_->setValue(0);
    progress_->setFixedWidth(200);
    progress_->setTextVisible(true);
    progress_->hide();
    statusBar()->addWidget(status_left_, 1);
    statusBar()->addPermanentWidget(progress_);

    // Native Windows-11-Style.
    if (QStyleFactory::keys().contains("windows11", Qt::CaseInsensitive))
        QApplication::setStyle(QStyleFactory::create("windows11"));

    // Debounce fuer Live-Suche.
    debounce_ = new QTimer(this);
    debounce_->setSingleShot(true);
    debounce_->setInterval(150);
    connect(debounce_, &QTimer::timeout, this, &MainWindow::runSearch);

    // Scan-Progress Polling.
    poll_timer_ = new QTimer(this);
    poll_timer_->setInterval(80);
    connect(poll_timer_, &QTimer::timeout, this, &MainWindow::pollScan);

    // Connections.
    connect(search_, &QLineEdit::textChanged, this, &MainWindow::onSearchTextChanged);
    connect(table_,  &QTableView::activated,  this, &MainWindow::onActivated);
    connect(table_,  &QTableView::customContextMenuRequested,
            this, &MainWindow::onContextMenu);
    connect(drive_combo_, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &MainWindow::onDriveChanged);

    // Direkt scannen.
    startScan();
}

MainWindow::~MainWindow() {
    scan_cancelled_.store(true);
    if (scan_thread_.joinable()) scan_thread_.join();
}

void MainWindow::startScan() {
    // Vorherigen Scan abbrechen falls noch laeuft (drive-switch).
    scan_cancelled_.store(true);
    if (scan_thread_.joinable()) scan_thread_.join();

    // Reset.
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
    search_->clear();
    progress_->show();
    progress_->setValue(0);
    status_left_->setText(QString("Indiziere %1: …").arg(drive_combo_->currentText()));

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
        status_left_->setText(QString("Indiziere %1: … %L2 / %L3 Records — %L4 Eintraege")
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
            status_left_->setText("Fehler beim Lesen der MFT — braucht Argus als Administrator?");
            QMessageBox::warning(this, "Argus",
                "Konnte die MFT nicht lesen.\n\n"
                "Bitte pruefen: Wurde die Anwendung als Administrator gestartet? "
                "Rohen NTFS-Zugriff koennen wir nur mit erhoehten Rechten.");
            return;
        }
        progress_->hide();
        status_left_->setText(
            QString("Bereit — %L1 Eintraege auf %2").arg(qulonglong(ents)).arg(drive_combo_->currentText()));
        search_->setEnabled(true);
        search_->setFocus();
    }
}

void MainWindow::onSearchTextChanged() {
    debounce_->start();
}

void MainWindow::runSearch() {
    const QString qtext = search_->text();
    if (qtext.isEmpty()) {
        model_->setResults({});
        status_left_->setText(QString("Bereit — %L1 Eintraege auf %2")
                              .arg(qulonglong(index_.entry_count()))
                              .arg(drive_combo_->currentText()));
        return;
    }
    std::wstring q = qtext.toStdWString();
    argus::SearchOptions opt;
    opt.max_results = 5000;

    auto t0 = std::chrono::steady_clock::now();
    auto ids = argus::Search(index_, q, opt);
    auto t1 = std::chrono::steady_clock::now();
    double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

    const size_t total_hits = ids.size();
    model_->setResults(std::move(ids));
    status_left_->setText(QString("%L1 Treffer  (%2 ms)")
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
    auto* aOpen = menu.addAction(index_.is_directory(id) ? "Ordner oeffnen" : "Datei oeffnen");
    auto* aReveal = menu.addAction("Im Explorer zeigen");
    menu.addSeparator();
    auto* aCopyPath = menu.addAction("Pfad kopieren");
    QAction* chosen = menu.exec(table_->viewport()->mapToGlobal(pos));

    if (chosen == aOpen) {
        if (index_.is_directory(id)) openInExplorer(id);
        else                         openFile(id);
    } else if (chosen == aReveal) {
        auto path = index_.full_path(id);
        QString qpath = QString::fromWCharArray(path.data(), int(path.size()));
        // /select markiert die Datei im Explorer.
        QString param = "/select,\"" + QDir::toNativeSeparators(qpath) + "\"";
        ShellExecuteW(nullptr, L"open", L"explorer.exe",
                      reinterpret_cast<LPCWSTR>(param.utf16()),
                      nullptr, SW_SHOWNORMAL);
    } else if (chosen == aCopyPath) {
        auto path = index_.full_path(id);
        QApplication::clipboard()->setText(
            QString::fromWCharArray(path.data(), int(path.size())));
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

void MainWindow::onDriveChanged(int) {
    startScan();
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
