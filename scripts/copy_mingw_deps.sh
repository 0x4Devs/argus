#!/usr/bin/env bash
# Kopiert rekursiv alle DLL-Abhaengigkeiten aus /mingw64/bin/ neben die Ziel-exe.
set -eu

target_exe="$1"
# Windows path -> MSYS path fuer POSIX-Tools
target_exe_msys="$(cygpath "$target_exe")"
target_dir="$(dirname "$target_exe_msys")"

MINGW_BIN="${MSYS2_MINGW_BIN:-/c/msys64/mingw64/bin}"
[[ -d "$MINGW_BIN" ]] || MINGW_BIN="C:/msys64/mingw64/bin"

declare -A resolved
queue=()

# Startpunkte: exe + alle DLLs im target_dir und in Unterordnern (Qt-Plugins)
shopt -s nullglob
queue+=( "$target_exe_msys" )
for f in "$target_dir"/*.dll; do queue+=( "$f" ); done
for d in "$target_dir"/*/; do
    for f in "$d"*.dll; do queue+=( "$f" ); done
done

copied=0
while [[ ${#queue[@]} -gt 0 ]]; do
    current="${queue[0]}"
    queue=( "${queue[@]:1}" )
    [[ -n "${resolved[$current]:-}" ]] && continue
    resolved[$current]=1

    # objdump listet 'DLL Name: xyz.dll' je Abhaengigkeit
    while IFS= read -r dep; do
        src="$MINGW_BIN/$dep"
        dst="$target_dir/$dep"
        if [[ -f "$src" && ! -f "$dst" ]]; then
            cp "$src" "$dst"
            copied=$((copied + 1))
            queue+=( "$dst" )
        fi
    done < <(objdump -p "$current" 2>/dev/null | awk '/DLL Name:/ {print $3}')
done

echo "MinGW deps: $copied DLLs kopiert."
