#!/usr/bin/env bash
# Bitflash GUI installer for Linux (via Wine)

INSTALL_DIR="$HOME/.btf-wine"
WINE_DATA="$HOME/.wine/drive_c/users/$USER/AppData/Roaming/Bitflash"
LINUX_DATA="$HOME/.bitflash"
BIN_DIR="$HOME/.local/bin"
REPO="Bitflash-sh/bitflash"

echo "==> Bitflash GUI installer"
echo

# ---- Wine
echo "==> Checking Wine..."
if ! command -v wine &>/dev/null; then
    echo "    Installing Wine..."
    sudo apt-get update -qq && sudo apt-get install -y wine
fi
echo "    Wine: $(wine --version)"

# ---- db-util
echo "==> Checking db-util..."
if ! command -v db_dump &>/dev/null; then
    echo "    Installing db-util..."
    sudo apt-get install -y db-util
fi
echo "    db_dump: $(which db_dump)"

# ---- Download
echo "==> Fetching latest release from GitHub..."
RELEASE_URL=$(curl -s "https://api.github.com/repos/$REPO/releases/latest" \
    | grep "browser_download_url" \
    | grep -i "win\|windows\|gui" \
    | head -1 | cut -d '"' -f 4)

if [ -z "$RELEASE_URL" ]; then
    RELEASE_URL=$(curl -s "https://api.github.com/repos/$REPO/releases/latest" \
        | grep "browser_download_url" | head -1 | cut -d '"' -f 4)
fi

if [ -z "$RELEASE_URL" ]; then
    echo "ERROR: No release found at https://github.com/$REPO/releases"
    exit 1
fi

echo "    URL: $RELEASE_URL"
TMPZIP=$(mktemp /tmp/bitflash-XXXXXX.zip)
echo "    Downloading..."
curl -L -o "$TMPZIP" "$RELEASE_URL"
echo "    Downloaded: $(du -h "$TMPZIP" | cut -f1)"

# ---- Extract
echo "==> Extracting to $INSTALL_DIR..."
rm -rf "$INSTALL_DIR"
mkdir -p "$INSTALL_DIR"
echo "    Running unzip -j..."
unzip -j "$TMPZIP" -d "$INSTALL_DIR" 2>&1 | grep -v "^warning" || true
echo "    Files after extract: $(ls "$INSTALL_DIR" | wc -l)"

# ---- Rename backslash files
echo "==> Renaming files..."
count=0
for f in "$INSTALL_DIR"/*; do
    [ -f "$f" ] || continue
    base=$(basename "$f")
    newname=$(echo "$base" | sed 's/.*\\//')
    if [ "$newname" != "$base" ]; then
        mv "$f" "$INSTALL_DIR/$newname"
        count=$((count+1))
    fi
done
echo "    Renamed $count files"
echo "    Files now: $(ls "$INSTALL_DIR" | wc -l)"

rm -f "$TMPZIP"

# ---- Verify
if [ ! -f "$INSTALL_DIR/bitflash.exe" ]; then
    echo "ERROR: bitflash.exe not found. Files present:"
    ls "$INSTALL_DIR"
    exit 1
fi
echo "    bitflash.exe found ✓"

# ---- Init Wine data dir
echo "==> Initializing Wine Bitflash data directory..."
echo "    Starting Wine to create data dir (may take a moment)..."
WINEDLLOVERRIDES="libdb_cxx-6.2=n" wine "$INSTALL_DIR/bitflash.exe" &
WINE_PID=$!
# Wait until wallet.dat is created, max 30 seconds
for i in $(seq 1 30); do
    sleep 1
    if [ -f "$WINE_DATA/wallet.dat" ]; then
        echo "    wallet.dat created after ${i}s"
        break
    fi
    echo "    Waiting for Wine to initialize... ${i}s"
done
kill $WINE_PID 2>/dev/null || true
wait $WINE_PID 2>/dev/null || true
sleep 1
mkdir -p "$WINE_DATA"
echo "    Wine data dir ready"

# ---- Wallet
echo
if [ -f "$LINUX_DATA/wallet.dat" ]; then
    echo "==> Found existing Linux wallet at $LINUX_DATA/wallet.dat"
    read -p "    Import it into the GUI wallet? [Y/n] " ans
    ans=${ans:-Y}
    if [[ "$ans" =~ ^[Yy] ]]; then
        echo "    Dumping Linux wallet..."
        DUMP=$(mktemp)
        CONVERTED=$(mktemp)
        rm "$CONVERTED"  # db_load needs a non-existing file
        db_dump -p "$LINUX_DATA/wallet.dat" > "$DUMP"
        echo "    Converting format..."
        db_load -f "$DUMP" "$CONVERTED"
        echo "    Installing converted wallet..."
        cp "$CONVERTED" "$WINE_DATA/wallet.dat"
        rm "$DUMP" "$CONVERTED"
        echo "    Wallet imported ✓"
    else
        echo "    Skipping. Re-run script to import later."
    fi
else
    echo "==> No Linux wallet found, starting fresh."
fi

# ---- Launcher
echo "==> Creating bitflash command..."
mkdir -p "$BIN_DIR"
LAUNCHER_FILE="$BIN_DIR/bitflash"
printf '#!/usr/bin/env bash\n' > "$LAUNCHER_FILE"
printf 'RELAY="${BTF_RELAY:-}"\n' >> "$LAUNCHER_FILE"
printf 'ANNOUNCE=""\n' >> "$LAUNCHER_FILE"
printf '[ -n "$RELAY" ] && ANNOUNCE="-announcerelay=$RELAY"\n' >> "$LAUNCHER_FILE"
printf 'WINEDLLOVERRIDES="libdb_cxx-6.2=n" wine "%s/bitflash.exe" $ANNOUNCE "$@"\n' "$INSTALL_DIR" >> "$LAUNCHER_FILE"
chmod +x "$LAUNCHER_FILE"
echo "    Launcher created: $(cat "$LAUNCHER_FILE" | wc -l) lines"

if [[ ":$PATH:" != *":$HOME/.local/bin:"* ]]; then
    echo "==> Adding ~/.local/bin to PATH..."
    echo 'export PATH="$HOME/.local/bin:$PATH"' >> "$HOME/.bashrc"
fi
export PATH="$HOME/.local/bin:$PATH"

echo
echo "=================================================================="
echo " DONE."
echo " Launch:    bitflash"
echo " Mine:      bitflash -gen"
echo " Relay:     bitflash -announcerelay=YOUR_HOST:8434"
echo " Data:      $WINE_DATA"
echo "=================================================================="
