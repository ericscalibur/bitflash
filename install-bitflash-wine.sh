#!/usr/bin/env bash
# Bitflash GUI installer for Linux (via Wine) — hardened
set -euo pipefail

INSTALL_DIR="$HOME/.btf-wine"
WINE_DATA="$HOME/.wine/drive_c/users/$USER/AppData/Roaming/Bitflash"
LINUX_DATA="$HOME/.bitflash"
BIN_DIR="$HOME/.local/bin"
REPO="Bitflash-sh/bitflash"

echo "==> Bitflash GUI installer"
echo

# ---- Safety: don't run as root (breaks Wine + pollutes /root)
if [ "$(id -u)" -eq 0 ]; then
    echo "ERROR: do not run this as root. Run as your normal user (it will call sudo only when needed)." >&2
    exit 1
fi

need() { command -v "$1" >/dev/null 2>&1; }

# ---- Wine
echo "==> Checking Wine..."
if ! need wine; then
    echo "    Installing Wine..."
    if ! need apt-get; then
        echo "ERROR: apt-get not found. Install 'wine' manually, then re-run." >&2
        exit 1
    fi
    sudo apt-get update -qq
    sudo apt-get install -y wine
fi
echo "    Wine: $(wine --version)"

# ---- db-util (db_dump / db_load for wallet conversion)
echo "==> Checking db-util..."
if ! need db_dump; then
    echo "    Installing db-util..."
    sudo apt-get install -y db-util
fi
echo "    db_dump: $(command -v db_dump)"

# ---- unzip + sha256sum must exist
for tool in unzip sha256sum curl; do
    if ! need "$tool"; then
        echo "    Installing $tool..."
        sudo apt-get install -y "$tool"
    fi
done

# ---- Resolve latest release (single API call)
echo "==> Fetching latest release metadata from GitHub..."
API_JSON=$(curl -fsSL "https://api.github.com/repos/$REPO/releases/latest")

RELEASE_URL=$(printf '%s\n' "$API_JSON" | grep '"browser_download_url"' \
    | grep -iE 'win|windows|gui' | head -1 | cut -d '"' -f 4 || true)
[ -z "$RELEASE_URL" ] && RELEASE_URL=$(printf '%s\n' "$API_JSON" | grep '"browser_download_url"' \
    | grep -iE '\.zip"?$' | head -1 | cut -d '"' -f 4 || true)

if [ -z "$RELEASE_URL" ]; then
    echo "ERROR: no downloadable release asset found at https://github.com/$REPO/releases" >&2
    exit 1
fi
ASSET_NAME=$(basename "$RELEASE_URL")
echo "    Asset: $ASSET_NAME"

# checksum manifest published alongside the release (SHA256SUMS)
SUMS_URL=$(printf '%s\n' "$API_JSON" | grep '"browser_download_url"' \
    | grep -iE 'sha256sums|checksums|\.sha256"?$' | head -1 | cut -d '"' -f 4 || true)

# ---- Download the release
TMPZIP=$(mktemp /tmp/bitflash-XXXXXX.zip)
cleanup() { rm -f "$TMPZIP" "${TMPSUMS:-}"; }
trap cleanup EXIT
echo "==> Downloading release..."
curl -fL --retry 3 --retry-delay 2 -o "$TMPZIP" "$RELEASE_URL"
echo "    Downloaded: $(du -h "$TMPZIP" | cut -f1)"

# ---- MANDATORY integrity check
echo "==> Verifying integrity..."
GOT=$(sha256sum "$TMPZIP" | awk '{print $1}')
if [ -n "${EXPECTED_SHA256:-}" ]; then
    # Strongest: user pinned a known-good hash out-of-band.
    if [ "$GOT" != "$EXPECTED_SHA256" ]; then
        echo "ERROR: checksum mismatch. expected $EXPECTED_SHA256, got $GOT" >&2
        exit 1
    fi
    echo "    OK (matches pinned EXPECTED_SHA256)"
elif [ -n "$SUMS_URL" ]; then
    TMPSUMS=$(mktemp /tmp/bitflash-sums-XXXXXX)
    curl -fsSL --retry 3 -o "$TMPSUMS" "$SUMS_URL"
    WANT=$(grep -iF "$ASSET_NAME" "$TMPSUMS" | awk '{print $1}' | head -1 || true)
    if [ -z "$WANT" ]; then
        echo "ERROR: no checksum for $ASSET_NAME in published SHA256SUMS." >&2
        exit 1
    fi
    if [ "$GOT" != "$WANT" ]; then
        echo "ERROR: checksum mismatch. published $WANT, got $GOT" >&2
        exit 1
    fi
    echo "    OK (matches published SHA256SUMS)"
else
    echo "ERROR: no checksum available for this release." >&2
    echo "       Publish a SHA256SUMS asset with the release, or re-run with" >&2
    echo "       EXPECTED_SHA256=<hash> to verify against a known-good value." >&2
    echo "       (To bypass at your own risk: ALLOW_UNVERIFIED=1)" >&2
    if [ "${ALLOW_UNVERIFIED:-0}" != "1" ]; then
        exit 1
    fi
    echo "    WARNING: proceeding UNVERIFIED (ALLOW_UNVERIFIED=1). sha256=$GOT"
fi

# ---- Extract
echo "==> Extracting to $INSTALL_DIR..."
rm -rf "$INSTALL_DIR"
mkdir -p "$INSTALL_DIR"
unzip -j -o "$TMPZIP" -d "$INSTALL_DIR" >/dev/null

# ---- Rename backslash-mangled entries (Windows zip paths)
count=0
for f in "$INSTALL_DIR"/*; do
    [ -f "$f" ] || continue
    base=$(basename "$f")
    newname=$(printf '%s' "$base" | sed 's/.*\\//')
    if [ "$newname" != "$base" ]; then
        mv "$f" "$INSTALL_DIR/$newname"
        count=$((count+1))
    fi
done
echo "    Renamed $count files; total: $(ls "$INSTALL_DIR" | wc -l)"

# Locate the Bitflash exe case-insensitively (zip may ship Bitflash.exe or bitflash.exe)
EXE=$(find "$INSTALL_DIR" -maxdepth 1 -iname 'bitflash.exe' | head -1)
if [ -z "$EXE" ]; then
    echo "ERROR: bitflash exe not found after extract. Contents:" >&2
    ls "$INSTALL_DIR" >&2
    exit 1
fi
echo "    Found $(basename "$EXE")"

# ---- Init Wine data dir
echo "==> Initializing Wine data directory..."
WINEDLLOVERRIDES="libdb_cxx-6.2=n" wine "$EXE" &
WINE_PID=$!
created=0
for i in $(seq 1 30); do
    sleep 1
    if [ -f "$WINE_DATA/wallet.dat" ]; then
        echo "    wallet.dat created after ${i}s"
        created=1
        break
    fi
done
kill "$WINE_PID" 2>/dev/null || true
wait "$WINE_PID" 2>/dev/null || true
sleep 1
mkdir -p "$WINE_DATA"
[ "$created" -eq 1 ] || echo "    NOTE: Wine did not create wallet.dat in 30s; continuing."

# ---- Optional wallet import (with backup)
echo
if [ -f "$LINUX_DATA/wallet.dat" ]; then
    echo "==> Found existing Linux wallet at $LINUX_DATA/wallet.dat"
    read -r -p "    Import it into the GUI wallet? [Y/n] " ans
    ans=${ans:-Y}
    if [[ "$ans" =~ ^[Yy] ]]; then
        if [ -f "$WINE_DATA/wallet.dat" ]; then
            BK="$WINE_DATA/wallet.dat.bak.$(date +%Y%m%d%H%M%S)"
            cp "$WINE_DATA/wallet.dat" "$BK"
            echo "    Backed up existing GUI wallet -> $BK"
        fi
        DUMP=$(mktemp); CONVERTED=$(mktemp); rm -f "$CONVERTED"
        echo "    Converting Linux wallet format..."
        db_dump -p "$LINUX_DATA/wallet.dat" > "$DUMP"
        db_load -f "$DUMP" "$CONVERTED"
        cp "$CONVERTED" "$WINE_DATA/wallet.dat"
        rm -f "$DUMP" "$CONVERTED"
        echo "    Wallet imported."
    else
        echo "    Skipped. Re-run to import later."
    fi
else
    echo "==> No Linux wallet found, starting fresh."
fi

# ---- Launcher
echo "==> Creating 'bitflash' command..."
mkdir -p "$BIN_DIR"
LAUNCHER_FILE="$BIN_DIR/bitflash"
cat > "$LAUNCHER_FILE" <<EOF
#!/usr/bin/env bash
RELAY="\${BTF_RELAY:-}"
ANNOUNCE=""
[ -n "\$RELAY" ] && ANNOUNCE="-announcerelay=\$RELAY"
WINEDLLOVERRIDES="libdb_cxx-6.2=n" wine "$EXE" \$ANNOUNCE "\$@"
EOF
chmod +x "$LAUNCHER_FILE"

if [[ ":$PATH:" != *":$BIN_DIR:"* ]] && ! grep -qF 'export PATH="$HOME/.local/bin:$PATH"' "$HOME/.bashrc" 2>/dev/null; then
    echo 'export PATH="$HOME/.local/bin:$PATH"' >> "$HOME/.bashrc"
    echo "    Added ~/.local/bin to PATH (restart shell or: source ~/.bashrc)"
fi

echo
echo "=================================================================="
echo " DONE."
echo " Launch:   bitflash"
echo " Mine:     bitflash -gen"
echo " Relay:    bitflash -announcerelay=YOUR_HOST:8434"
echo " Data:     $WINE_DATA"
echo "=================================================================="
