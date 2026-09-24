# Argus

*"The hundred-eyed giant — nothing escapes him."*

Instant file search for Windows. Reads the NTFS Master File Table directly, no
background indexer required, no external services. Written in C++20 with a
native Qt6 GUI.

**Status:** v0.2.0 released — indexes a full NTFS drive (millions of files) in
seconds and gives you live substring, wildcard or regex search in the GUI. See
[Releases](../../releases) for a portable Windows zip.

## Why?

- [Everything](https://voidtools.com/) is the gold standard but closed source.
- [fsearch](https://github.com/cboxdoerfer/fsearch) is great but Linux/GTK only.
- Open source alternatives for Windows are either dated or Python-based
  wrappers that can't match a native C++ implementation for indexing millions
  of files.

Argus is a modern, native, Qt6-based file search for Windows — portable,
fast, MIT-licensed.

## Download

Grab the latest portable zip from [Releases](../../releases). Extract, right-click
`argus.exe` → **Run as administrator** (UAC will prompt — raw NTFS access
requires elevated privileges).

## Features

- Reads NTFS MFT directly via raw disk IO — no background indexer
- Live search with three modes: **Text** (substring), **Wildcard** (`*.mp4`),
  **Regex** (full ECMAScript)
- Filter results by All / Files / Folders
- Sortable columns: Name, Path, Size, Modified
- Shell icons per file extension via the Windows shell
- Drive selector for every NTFS volume
- Double-click to open, right-click for Reveal in Explorer / Copy path
- Keyboard shortcuts: `Ctrl+F`, `F5`, `Esc`, `Ctrl+C`
- Bundled `mftdump.exe` CLI for MFT diagnostics
- Portable — no installer, just a folder with the exe and its DLLs

## Roadmap

| Milestone | Status |
|---|---|
| v0.1 GUI with live search on a single drive | ✅ released |
| v0.2 Sorting, shell icons, wildcard/regex, filters, hotkeys | ✅ released |
| v0.3 USN Journal live updates + persistent index cache | 🔨 next |
| v0.4 Content search inside text files, column configuration | planned |
| v0.5 Portable distribution polish + first stable | planned |

## Build from source (Windows, MSYS2 + MinGW-w64)

Prerequisites:
```
pacman -S mingw-w64-x86_64-gcc \
          mingw-w64-x86_64-cmake \
          mingw-w64-x86_64-ninja \
          mingw-w64-x86_64-qt6-base \
          mingw-w64-x86_64-qt6-tools
```

Build:
```
export PATH=/c/msys64/mingw64/bin:$PATH
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

The build produces `build/argus.exe` and `build/mftdump.exe`. `windeployqt` runs
automatically and copies the required Qt DLLs next to the exe, plus a helper
script bundles the transitive MinGW dependencies for a self-contained folder.

Run (requires Administrator):
```
build\argus.exe          # GUI
build\mftdump.exe C --all # CLI, iterates all MFT records on drive C
```

## Design notes

Argus opens `\\.\<drive>:` with `FILE_SHARE_READ | FILE_SHARE_WRITE`, reads
the NTFS boot sector to locate the `$MFT`, follows the MFT's `$DATA` attribute
runlist to enumerate all MFT records, and extracts the primary `$FILE_NAME`
attribute of each in-use record. Parent references form a compact tree so full
paths are reconstructed on demand rather than stored. All entries are held in a
single contiguous vector plus a pooled UTF-16 name buffer for cache-friendly
scanning.

The search engine has a fast path for substring queries that runs a manual
case-insensitive scan, and a general path built on `std::wregex` for wildcard
and regex modes.

## Contributing

Issues and pull requests welcome. Bug reports especially valued — testing
against varied NTFS configurations (fragmented volumes, unusual cluster sizes,
resized partitions, etc.) is exactly the sort of feedback this project needs.

## License

MIT. See [LICENSE](LICENSE).
