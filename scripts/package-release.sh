#!/usr/bin/env bash
# package-release.sh — baut die Portable-Zip fuer ein Argus-Release.
set -euo pipefail

version="${1:-v0.1.0}"
here="$(cd "$(dirname "$0")/.." && pwd)"
build="$here/build"
name="argus-$version-windows-x64"
staging="$here/dist/$name"

if [[ ! -f "$build/argus.exe" ]]; then
    echo "Fehler: build/argus.exe nicht gefunden. Erst bauen."
    exit 1
fi

echo "Staging -> $staging"
rm -rf "$here/dist"
mkdir -p "$staging"

# App + CLI + alle DLLs
cp "$build/argus.exe"                             "$staging/"
cp "$build/mftdump.exe"                           "$staging/"
cp "$build/"*.dll                                 "$staging/"

# Qt-Plugin-Ordner (platforms/, styles/, etc.)
for d in "$build"/*/; do
    dn="$(basename "$d")"
    case "$dn" in
        CMakeFiles|_deps|argus_autogen|mftdump_autogen) continue ;;
    esac
    if compgen -G "$d"*.dll > /dev/null; then
        cp -r "$d" "$staging/"
    fi
done

# Kurz-Anleitung fuer Endnutzer
cat > "$staging/USE.txt" <<EOF
Argus — Instant File Search

Doppelklick auf "argus.exe" -> UAC-Prompt bestaetigen (braucht Admin fuer rohen
NTFS-Zugriff) -> Fenster oeffnet und indexiert die erste NTFS-Platte.

Nach der Indexierung im Suchfeld tippen. Live-Filter, Doppelklick oeffnet die
Datei, Rechtsklick zeigt Kontextmenue.

CLI-Diagnose:  "mftdump.exe <drive-letter>" fuer Boot-Sektor + MFT-Header.
Auch mit --all: iteriert die komplette MFT und zeigt Statistik.

Projekt: https://github.com/0x4Devs/argus
EOF

# Zippen
cd "$here/dist"
zipfile="$name.zip"
echo "Zipping -> $zipfile"
if command -v powershell.exe > /dev/null; then
    powershell.exe -NoProfile -Command "Compress-Archive -Path '$name/*' -DestinationPath '$zipfile' -Force" > /dev/null
else
    zip -qr "$zipfile" "$name"
fi

size_mb=$(du -m "$zipfile" | cut -f1)
echo ""
echo "Fertig:  $here/dist/$zipfile   (${size_mb} MB)"
