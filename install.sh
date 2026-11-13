#!/bin/bash
# clean-cam installer for Arch-based systems (Asahi Linux / Omarchy etc.).
#   curl -fsSL https://raw.githubusercontent.com/maxberggren/clean-cam/main/install.sh | bash
set -euo pipefail

REPO=https://github.com/maxberggren/clean-cam
SRC="$HOME/.local/src/clean-cam"
say() { printf '\033[1m==> %s\033[0m\n' "$*"; }

# Running from curl: fetch the source and re-run from there.
HERE="$(cd "$(dirname "${BASH_SOURCE[0]:-$0}")" 2>/dev/null && pwd || true)"
if [ ! -f "$HERE/clean-cam.c" ]; then
  command -v git >/dev/null || sudo pacman -S --needed --noconfirm git
  if [ -d "$SRC/.git" ]; then git -C "$SRC" pull --ff-only -q; else git clone -q "$REPO" "$SRC"; fi
  exec bash "$SRC/install.sh"
fi
cd "$HERE"

command -v pacman >/dev/null || { echo "This installer needs pacman (Arch). See README for manual steps." >&2; exit 1; }

KVER=$(uname -r)
KPKG=$(pacman -Qqo "/usr/lib/modules/$KVER/vmlinuz" 2>/dev/null || true)
PKGS=(base-devel v4l2loopback-dkms)
[ -n "$KPKG" ] && pacman -Si "$KPKG-headers" >/dev/null 2>&1 && PKGS+=("$KPKG-headers")

say "Installing packages: ${PKGS[*]}"
sudo pacman -S --needed --noconfirm "${PKGS[@]}"

# Kernels built with clang (e.g. Asahi) need clang to build DKMS modules.
if grep -q '^CONFIG_CC_IS_CLANG=y' "/usr/lib/modules/$KVER/build/.config" 2>/dev/null; then
  say "Kernel was built with clang, installing clang/lld/llvm"
  sudo pacman -S --needed --noconfirm clang lld llvm
fi

say "Building v4l2loopback for $KVER"
modinfo v4l2loopback >/dev/null 2>&1 || sudo dkms autoinstall -k "$KVER"

say "Configuring the virtual camera (/dev/video42)"
sudo install -Dm644 v4l2loopback.conf /etc/modprobe.d/clean-cam.conf
echo v4l2loopback | sudo tee /etc/modules-load.d/clean-cam.conf >/dev/null
if [ ! -e /dev/video42 ]; then
  if lsmod | grep -q '^v4l2loopback'; then
    sudo modprobe -r v4l2loopback || { echo "v4l2loopback is in use with other settings; reboot to finish." >&2; }
  fi
  sudo modprobe v4l2loopback || true
fi

say "Building clean-cam"
make -s clean-cam
make -s install PREFIX="$HOME/.local"

say "Starting the service"
install -Dm644 clean-cam.service "$HOME/.config/systemd/user/clean-cam.service"
systemctl --user daemon-reload
systemctl --user enable clean-cam.service >/dev/null 2>&1
systemctl --user restart clean-cam.service
# Let PipeWire rescan so it lists the new camera right away.
systemctl --user try-restart wireplumber.service 2>/dev/null || true

if [ -e /dev/video42 ] && systemctl --user is-active -q clean-cam.service; then
  say "Done. Pick \"FaceTime HD (Clean)\" as your camera."
else
  say "Installed, but the virtual camera isn't up yet: reboot, then check 'systemctl --user status clean-cam'."
fi
