# Changelog

All notable changes to Argus are documented here. This project follows
[Semantic Versioning](https://semver.org/spec/v2.0.0.html).

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
