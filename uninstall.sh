#!/bin/bash
# Remove clean-cam (leaves v4l2loopback-dkms and compilers installed).
set -u
systemctl --user disable --now clean-cam.service 2>/dev/null
rm -f "$HOME/.config/systemd/user/clean-cam.service" "$HOME/.local/bin/clean-cam"
systemctl --user daemon-reload
sudo rm -f /etc/modprobe.d/clean-cam.conf /etc/modules-load.d/clean-cam.conf
sudo modprobe -r v4l2loopback 2>/dev/null
rm -rf "$HOME/.local/src/clean-cam"
echo "clean-cam removed. 'sudo pacman -Rns v4l2loopback-dkms' to drop the module too."
