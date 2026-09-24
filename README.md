# Argus

*"Der Riese mit hundert Augen — er sieht alles."*

Instant file search for Windows. Reads NTFS Master File Table directly, no
background indexer required, no external services. Written in C++20 with a
Qt6 GUI planned.

**Status:** Early development. Currently ships a diagnostic CLI (`mftdump`)
that opens a raw NTFS volume and dumps boot sector and MFT record 0
information. GUI comes next.

## Why?

- [Everything](https://voidtools.com/) is the gold standard but closed source.
- [fsearch](https://github.com/cboxdoerfer/fsearch) is great but Linux/GTK only.
- Open source alternatives for Windows are either dated or Python-based
  wrappers that can't match a native C++ implementation for indexing millions
  of files.

Argus aims to be a modern, native, Qt6-based file search for Windows —
portable, fast, MIT-licensed.

## Roadmap

| Milestone | Status |
|---|---|
| v0.0 `mftdump`: raw NTFS read, boot sector + MFT record 0 parse | ⏳ in progress |
| v0.1 CLI dumps all file names on a volume | planned |
| v0.2 Qt6 GUI with live search (single drive) | planned |
| v0.3 USN Journal live updates | planned |
| v0.4 Multi-drive, regex, size/date filters | planned |
| v0.5 Portable distribution + first stable | planned |

## Build (Windows, MSYS2 + MinGW-w64)

Prereqs:
```
pacman -S mingw-w64-x86_64-gcc \
          mingw-w64-x86_64-cmake \
          mingw-w64-x86_64-ninja
```

Build:
```
export PATH=/c/msys64/mingw64/bin:$PATH
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

Run (requires Administrator, because raw disk access on Windows requires
elevated privileges):
```
build\mftdump.exe C
```

## Design notes

Argus opens `\\.\<drive>:` with `FILE_SHARE_READ | FILE_SHARE_WRITE`, reads
the NTFS boot sector to locate `$MFT`, follows the MFT's DATA attribute
runlist to enumerate all MFT records, and extracts `FILE_NAME` attributes.
Parent references form a compact tree so full paths are reconstructed on
demand rather than stored.

## License

MIT. See [LICENSE](LICENSE).
