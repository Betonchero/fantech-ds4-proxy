#!/usr/bin/env bash
# install.sh - installer for the Fantech Shooter III -> DualShock 4 uhid proxy

set -e

if [ "$EUID" -ne 0 ]; then
    echo "[-] Please run as root: sudo ./install.sh"
    exit 1
fi

echo "========================================================="
echo "  Installing Fantech Shooter III -> DualShock 4 proxy    "
echo "========================================================="

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

echo "[1/6] Compiling proxy..."
make -C "$DIR" clean
make -C "$DIR"

echo "[2/6] Installing binary to /usr/local/bin/fantech-ds4-proxy..."
install -m 755 "$DIR/fantech-ds4-proxy" /usr/local/bin/fantech-ds4-proxy

echo "[3/6] Installing udev rules and helpers..."
install -m 644 "$DIR/99-fantech-ds4-proxy.rules" /etc/udev/rules.d/99-fantech-ds4-proxy.rules
install -m 644 "$DIR/98-fantech-ds4-unbind-audio.rules" /etc/udev/rules.d/98-fantech-ds4-unbind-audio.rules
install -m 755 "$DIR/fantech-unbind-audio.sh" /usr/local/bin/fantech-unbind-audio.sh
udevadm control --reload-rules
udevadm trigger

echo "[4/6] Loading uhid / hid-playstation kernel modules..."
modprobe uhid || true
modprobe hid_playstation || true
if ! grep -q "uhid" /etc/modules-load.d/uhid.conf 2>/dev/null; then
    echo "uhid" > /etc/modules-load.d/uhid.conf
fi

echo "[5/6] Installing and starting systemd service..."
install -m 644 "$DIR/fantech-ds4-proxy.service" /etc/systemd/system/fantech-ds4-proxy.service
systemctl daemon-reload
systemctl enable --now fantech-ds4-proxy.service

echo "[6/6] Done."

echo ""
echo "========================================================="
echo "  Installation Successful!                               "
echo "========================================================="
echo "  Status:"
systemctl status fantech-ds4-proxy.service --no-pager -l | head -n 12
echo ""
echo "  Your controller should now appear as a genuine"
echo "  DualShock 4 (054c:05c4) with working gyro/accel and rumble."
echo "========================================================="
