# Changelog

All notable changes to Argus are documented here. This project follows
[Semantic Versioning](https://semver.org/spec/v2.0.0.html).

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
