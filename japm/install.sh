#!/usr/bin/env sh
# japm — JAPlus Manager setup (Linux).
# Needs: Python 3, 32-bit libs (linuxjampded + japlusproxy are i386),
# screen, and a JA+ GameData install somewhere (see japm.conf / JAPLUS_HOME).

set -e

PY="python3"
SCRIPTPATH="$(cd "$(dirname "$0")" && pwd)"

echo "========================================"
echo "  japm — JAPlus Manager"
echo "========================================"
echo ""

# ── Python ──
if ! command -v $PY >/dev/null 2>&1; then
    echo "ERROR: Python 3 not found."
    if command -v apt-get >/dev/null 2>&1; then
        echo "  sudo apt-get install python3"
    elif command -v dnf >/dev/null 2>&1; then
        echo "  sudo dnf install python3"
    elif command -v pacman >/dev/null 2>&1; then
        echo "  sudo pacman -S python"
    fi
    exit 1
fi
echo "  [OK] $($PY --version 2>&1)"

# ── Distro detection ──
if command -v apt-get >/dev/null 2>&1; then
    PKG="apt"
elif command -v dnf >/dev/null 2>&1; then
    PKG="dnf"
elif command -v pacman >/dev/null 2>&1; then
    PKG="pacman"
else
    PKG=""
fi

# ── 32-bit libs (engine + proxy are i386) ──
case "$PKG" in
    apt)
        echo "  [....] Debian/Ubuntu — installing 32-bit libs..."
        sudo dpkg --add-architecture i386 2>/dev/null || true
        sudo apt-get update -qq
        sudo apt-get install -y libc6:i386 libstdc++6:i386 screen python3 gcc-multilib libc6-dev-i386
        echo "  [OK] 32-bit libs + screen + build tools installed"
        ;;
    dnf)
        echo "  [....] Fedora/RHEL — installing 32-bit libs..."
        sudo dnf install -y glibc.i686 libstdc++.i686 screen python3 gcc
        echo "  [OK] 32-bit libs + screen installed"
        ;;
    pacman)
        echo "  [....] Arch — installing 32-bit libs..."
        sudo pacman -S --noconfirm lib32-glibc lib32-gcc-libs screen python
        echo "  [OK] 32-bit libs + screen installed"
        ;;
    *)
        echo "  [SKIP] Unknown distro — install 32-bit libs + screen manually"
        ;;
esac

# ── CLI shortcut ──
chmod +x "$SCRIPTPATH/japm.py"
sudo ln -sf "$SCRIPTPATH/japm.py" "/usr/bin/japm"
echo "  [OK] Shortcut: japm"

echo ""
echo "========================================"
echo "  Setup complete"
echo "========================================"
echo ""
echo "  1. Build the proxy:  cd ../proxy && make"
echo "  2. Copy a config:    cp configs/example.json configs/my_server.json"
echo "  3. Edit it:          ja_path, host_name, rcon_password, maps"
echo "  4. Check the proxy:  japm my_server proxy"
echo "  5. Start:            japm my_server start"
