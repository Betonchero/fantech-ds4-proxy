#!/bin/sh
# fantech-unbind-audio.sh <usb-interface-id>
#
# Unbind the fake USB-Audio interface of the Fantech dongle in DS4 mode
# (0f0d:0162). Its audio descriptors are broken: snd-usb-audio probes them,
# the dongle STALLs, and EHCI/companion controllers respond by resetting the
# whole device every ~2s. We only need the HID interface (gyro), so tear the
# audio interface away as soon as it binds.
#
# Retries because udev "add" fires before the driver has bound.

IFACE="$1"
[ -n "$IFACE" ] || exit 0

i=0
while [ $i -lt 20 ]; do
    if [ -e "/sys/bus/usb/drivers/snd-usb-audio/$IFACE" ]; then
        echo "$IFACE" > /sys/bus/usb/drivers/snd-usb-audio/unbind 2>/dev/null \
            && logger -t fantech "unbound snd-usb-audio from $IFACE"
        exit 0
    fi
    sleep 0.2
    i=$((i + 1))
done

exit 0
