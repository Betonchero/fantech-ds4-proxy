#!/usr/bin/env bash
# uninstall.sh - clean uninstaller for the Fantech Shooter III DS4 proxy

set -e

if [ "$EUID" -ne 0 ]; then
    echo "[-] Please run as root: sudo ./uninstall.sh"
    exit 1
fi

echo "Stopping and disabling fantech-ds4-proxy service..."
systemctl stop fantech-ds4-proxy.service || true
systemctl disable fantech-ds4-proxy.service || true

echo "Removing files..."
rm -f /etc/systemd/system/fantech-ds4-proxy.service
rm -f /etc/udev/rules.d/99-fantech-ds4-proxy.rules
rm -f /etc/udev/rules.d/98-fantech-ds4-unbind-audio.rules
rm -f /usr/local/bin/fantech-ds4-proxy
rm -f /usr/local/bin/fantech-unbind-audio.sh
rm -f /run/udev/rules.d/96-fantech-ds4-proxy-hide.rules

udevadm control --reload-rules
systemctl daemon-reload

echo "Uninstallation complete."
