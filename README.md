# fantech-ds4-proxy

A small userspace driver that makes a Fantech Shooter III wgp13s, and probably other
multi-mode controllers, work as a proper PS4 controller on Linux without patching the kernel.

## The problem

In DS4 / D-input mode the controller shows up as `0f0d:0162` (wireless dongle) or
`0f00:0570` (wired). Those IDs aren't in `hid-playstation`'s device table, so the
kernel's PlayStation driver never binds. `hid-generic` takes it instead and gives
you a badly mapped pad: sticks and triggers are wrong, there's no gyro or
accelerometer.

The frustrating part is that the controller actually speaks the real DualShock 4
protocol. It even hands out the genuine DS4 report descriptor. The only thing
missing is a matching VID/PID. The natural fix would be a small kernel patch that adds the
IDs to `hid-playstation` and corrects the calibration, which means building and
maintaining an out-of-tree module.

## How it works

The daemon reads the physical controller's HID report descriptor and creates a
`/dev/uhid` device with Sony IDs (`054c:05c4`, bus USB) and that same descriptor.
The stock `hid-playstation` driver binds to the virtual device and builds a real
DualShock 4: gamepad, motion sensors, touchpad, rumble.

After that the daemon stays in the middle and passes traffic between the two.
Input reports go from the physical hidraw into `UHID_INPUT2`. Feature reads and
writes and output reports go both ways between the virtual device
and the physical hidraw.

Along the way it corrects a few firmware bugs. The battery always reads around
5%, so it's set to a steady 100. Calibration report 0x02 has a broken
accelerometer block with negative and zero ranges, which is why the IMU looked
dead; the daemon replaces it with sane defaults. The controller has no touchpad,
but its touch region reports one active finger at (0,0), which makes viewers draw
a dot in the top left. The daemon clears it for both the kernel path and the
raw-report path SDL and Steam read. The dongle's fake USB-audio interface also
causes trouble: `snd-usb-audio` fails to probe it, and that resets the whole
device every couple of seconds, so a udev helper unbinds that interface.

Two more things. A udev rule hides the physical device from userspace by making
its hidraw, evdev, and js nodes unreachable, so only the virtual DS4 is visible.
The daemon removes the rule when it exits. And the virtual device is kept alive
across short disconnects, since the dongle soft-resets when the controller powers
off. If it comes back within the grace window, the same virtual device is reused,
which keeps Steam's gyro working. The window is 5 seconds by default, see
`--grace`.

## Requirements

- Linux with udev and systemd
- kernel modules `uhid` and `hid_playstation`
- root, since the daemon needs `/dev/uhid` and the physical hidraw

## Install

```bash
git clone <this repo> fantech-ds4-proxy
cd fantech-ds4-proxy
sudo ./install.sh
```

That builds the binary, installs the udev rules and the audio-unbind helper, and
enables the systemd service. To remove it all:

```bash
sudo ./uninstall.sh
```

## Usage

You normally don't run it by hand, the service starts at boot. For testing:

```bash
sudo systemctl stop fantech-ds4-proxy
sudo ./fantech-ds4-proxy            # foreground
sudo ./fantech-ds4-proxy -v         # verbose
sudo ./fantech-ds4-proxy --dump     # print raw physical HID reports and exit
sudo ./fantech-ds4-proxy --grace=15 # hold the virtual pad for 15s on disconnect
```

`--dump` is useful when something looks off. It decodes report ID 1 at the
standard DS4 offsets (sticks, triggers, buttons, gyro, accel, status, touch
count) straight from the hardware.

## Notes

- The controller has to be in DS4 / D-input mode (`0f0d:0162` / `0f00:0570`).
  Other modes, like native XInput, aren't proxied.
- The Snap/Fn button maps to a touchpad click. It's a real input and is left
  alone.
- If Steam was already running when you start the daemon, it may keep the old
  physical entry around until it re-enumerates. Restart Steam or replug.

## License

GPL-2.0-or-later.
