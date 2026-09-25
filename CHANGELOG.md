# Changelog

All notable changes to Argus are documented here. This project follows
[Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [v0.10.0] — 2026-09-25

### Added
- **Content search inside text files** — new query predicate
  `content:api_key` (also `contains:...`). Reads the first 10 MB of each
  candidate file and does a case-insensitive substring search.
- Extension whitelist covers common text formats: source code
  (`cpp`, `h`, `py`, `js`, `ts`, `go`, `rs`, `java`, `cs`, `rb`, `php`,
  `swift`, `kt`, `dart`, `vue`, `svelte`, ...), docs (`txt`, `md`, `rst`,
  `log`), config (`ini`, `conf`, `cfg`, `yaml`, `toml`, `json`, `xml`,
  `env`), web (`html`, `css`, `scss`), scripts (`sh`, `bat`, `ps1`,
  `cmake`), data (`csv`, `tsv`, `sql`), and diffs (`diff`, `patch`).
- Files larger than 100 MB are skipped entirely; only the first 10 MB
  are read for smaller ones.
- F1 help dialog documents the new predicate and warns to combine it
  with a narrowing predicate (`ext:json content:api_key`) to avoid
  scanning millions of files.

### Query engine
- Predicates evaluate in query order and short-circuit on first miss,
  so users who put cheap filters (`ext:`, `size:`) before `content:`
  keep interactive latency.

## [v0.9.0] — 2026-09-25

### Added
- **Global hotkey `Ctrl+Alt+Space`** — pops Argus to the front from any
  application, focuses the search box and pre-selects any existing text.
  Registered via `RegisterHotKey`; handled in `nativeEvent()`.
- **System tray icon** with a right-click menu (Show / Quit) and
  left-click to toggle the window. The **X button now minimises to
  tray** instead of quitting, so the global hotkey stays live in the
  background. *Tools → Quit* or *tray → Quit* actually quits.
- Extended **Unicode case-folding** — the fast search path now handles
  Latin-1 Supplement, Latin Extended-A, Cyrillic (basic + supplement)
  and Greek uppercase pairs, not just ASCII + three German umlauts.

### Changed
- `Index::mft_id_of()` is now **O(1)** — a parallel `entry_mft_id_`
  vector is populated during scan (and updated when USN adds entries).
  Was O(N) linear scan of `mft_to_idx_`. Opening the NTFS details
  dialog on drives with millions of entries is now instant.
- Cache format bumped to `ARGIDX03` to store the reverse table. Older
  v0.7/v0.8 caches are ignored on load and a full scan runs once.

## [v0.8.0] — 2026-09-25

### Added
- **F1 query syntax help dialog** — full reference with examples for
  every predicate, mode and shortcut. Also linked from Help menu.
- **Persistent window state** — position, size, column widths,
  last-selected drive, search mode, filter, sort column and order are
  all restored on the next launch (via `QSettings` under
  `HKCU\Software\0x4Devs\Argus`).
- **Search history** — the last 20 queries appear as an autocomplete
  dropdown as soon as you start typing (populated on Enter).
- **Right-click a folder → "Search only inside this folder"** — inserts
  a `path:<full folder path>` predicate into the current query without
  losing what you already typed.
- The query-hint line under the search field is now Rich-Text (bolded
  keywords, slightly larger) and points to F1 for the full syntax.

### Robustness
- Try/catch guard around every MFT record processed during a scan.
  Corrupt or truncated records are counted as *skipped* and no longer
  bring down the whole indexing pass.
- Concrete `ScanStats::Error` codes returned when a scan fails (access
  denied, not NTFS, boot-sector read failed, MFT read failed, MFT
  record 0 has no `$DATA`) — enables clearer error messages in the GUI.

## [v0.7.1] — 2026-09-25

### Fixed
- About dialog was still hardcoded to "Version 0.6.0". The version string
  is now derived from `PROJECT_VERSION` via a compile-time
  `ARGUS_VERSION` macro so it always matches the release.

## [v0.7.0] — 2026-09-25

### Added
- **App icon** — a shield-and-eye motif embedded in `argus.exe`,
  `argus-cli.exe` and `mftdump.exe`. Multi-resolution `.ico` (16 → 256 px)
  used by Windows Explorer, and the same graphic loaded as a Qt resource
  (`:/argus.png`) so it also appears in the window title bar and taskbar.
- **README hero screenshot** — the running app with 1.5 M entries across
  two NTFS volumes visible at the top of the README.
- CMake option `ARGUS_ABI_FIX_LIBSTDCXX` (default ON) gates the vendor
  DLL overlay so downstream builds against a matching Qt6 can turn it off.

## [v0.6.0] — 2026-09-25

### Added
- **Fuzzy search mode** — new "Fuzzy" entry in the mode dropdown. Matches
  characters in order regardless of gaps, scores them fzf-style
  (consecutive bonus, word-boundary bonus, gap penalty), returns the top
  results sorted by score. Example: `argsrc` matches `Argus source`.
- **Duplicate finder** — *Tools → Find duplicates…* runs a background pipeline:
  1. Bucket every non-empty file by exact byte size
  2. For groups with more than one file, hash the first 64 KB with FNV-1a
  3. Report groups that agree on both size and partial hash
  Results appear in a tree grouped by size (largest wasted space on top).
  Progress bar, cancel button, live "N duplicate groups — X GB recoverable".
- **Menu bar** — Tools (Find duplicates, Re-scan, Quit) and Help (About).
- **Extended keyboard shortcuts**:
  - `Ctrl+Enter` — open the containing folder of the selected result
  - `Alt+Enter`  — open the Windows shell Properties dialog
  - `Delete`     — move selected items to the Recycle Bin (with prompt,
    goes through `SHFileOperation` with `FOF_ALLOWUNDO`)

## [v0.5.0] — 2026-09-25

### Added
- **NTFS Details dialog** (right-click a result → *NTFS details…*):
  - MFT record number, sequence, flags, hardlink count
  - Table of every `$FILE_NAME` attribute on the record (POSIX / Win32 /
    DOS / Win32+DOS namespaces, parent MFT ref, raw name)
  - Table of every `$DATA` stream — the default stream and every
    **Alternate Data Stream** (name, size, resident vs non-resident)
- **`argus-cli.exe`** — console interface bundled next to the GUI:
  - `argus-cli find "report ext:pdf size:>10MB"` — search from PowerShell
  - `argus-cli list-drives` — list known NTFS volumes
  - `--drive C`, `--limit N` flags
  - Reads the same persistent cache as the GUI, never needs elevation
- Persistent cache now stores the MFT runlist + volume geometry so
  arbitrary MFT records can be read back on demand (bumped format version
  to `ARGIDX02`)
- Reverse-lookup `Index::mft_id_of(entry_idx)` for details lookups

## [v0.4.0] — 2026-09-25

### Added
- **Advanced query syntax** — combine name terms with structured filters:
  - `ext:pdf` — file extension
  - `type:image|video|audio|document|archive|code|exe` — semantic categories
  - `path:downloads` — path substring
  - `size:>100MB`, `size:<1GB`, `size:>=500K` — size comparisons
  - `modified:<7d`, `modified:<24h`, `modified:<30d` — recency
  - `!term` — negation
  - Mix freely: `report ext:pdf size:>10MB modified:<30d`
- **Multi-drive indexing** — "All Drives" option scans every NTFS volume
  concurrently (one thread per volume) and returns aggregated hits with
  drive-tagged results
- Status bar shows per-drive progress during multi-drive indexing
- Inline hint under the search bar shows query-syntax examples

### Changed
- Search engine accepts an optional pre-parsed `Query` object so name search
  and structured filters run in one pass
- Persistent index cache now works per drive (`%LOCALAPPDATA%\Argus\<drive>.aix`)

## [v0.3.0] — 2026-09-25

### Added
- **USN Journal live updates** — file creates, deletes and renames appear in
  results within about a second, no manual refresh
- **Persistent index cache** — subsequent launches load in under a second and
  catch up via the USN Journal instead of doing a full scan
- Automatic USN Journal creation on volumes that don't have one yet

### Changed
- Deleted entries are marked with a flag and skipped by the search, no
  reshuffling of the entries vector

## [v0.2.0] — 2026-09-25

### Added
- **Column sorting** — click any header (Name / Path / Size / Modified)
- **Windows shell icons** in the Name column, per-extension cached
- **Wildcard mode** (`*.mp4`) and **Regex mode** (ECMAScript, case-insensitive)
- **Filter dropdown**: All / Files / Folders
- **Keyboard shortcuts**:
  - `Ctrl+F` focus & select the search box
  - `F5` rescan the current drive
  - `Esc` clear the search box
  - `Ctrl+C` copy full path(s) of the selected rows
- English UI throughout

## [v0.1.0] — 2026-09-25

### Added
- First public release
- Reads NTFS Master File Table directly via raw disk IO
- Live substring search on all filenames on the drive (150 ms debounce)
- Drive selector for every NTFS volume on the system
- Double-click a result to open the file / folder
- Right-click: Open, Reveal in Explorer, Copy path
- Diagnostic CLI `mftdump.exe` bundled
