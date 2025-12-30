# kbd-backlight-daemon

A lightweight C daemon that automatically dims the keyboard backlight on Framework Laptop 13 based on input inactivity.

## Features

- Monitors keyboard, mouse, and touchpad input events
- Smooth fade transitions when dimming/brightening
- Dynamic brightness: automatically adapts to manual changes (Fn+Space, GNOME extension)
- Respects user choice: if you turn off the backlight manually, it stays off
- Configurable timeout, brightness levels, and fade speed
- Low resource footprint (C, no dependencies)
- systemd integration

## Building

```bash
make
```

## Installation

```bash
sudo make install
sudo systemctl daemon-reload
sudo systemctl enable kbd-backlight-daemon
sudo systemctl start kbd-backlight-daemon
```

## Configuration

Edit `/etc/kbd-backlight-daemon.conf`:

```ini
# Timeout in seconds before dimming
timeout=5

# Initial brightness when active (0-100, or -1 to use current)
target_brightness=-1

# Brightness when dimmed (0 = off)
dim_brightness=0

# Fade animation settings
fade_steps=10
fade_interval_ms=50
```

Restart the service after changing configuration:
```bash
sudo systemctl restart kbd-backlight-daemon
```

## Usage

The daemon runs automatically as a systemd service. It:

1. Keeps keyboard backlight on while you're typing, using the mouse, or touchpad
2. Dims to `dim_brightness` after `timeout` seconds of inactivity
3. Automatically detects manual brightness changes (Fn+Space) and uses the new value as the target

### Dynamic brightness

When you change the keyboard brightness manually (using Fn+Space or GNOME keyboard backlight extension), the daemon detects this and updates its target brightness:

- Set brightness to 30% with Fn+Space → daemon will restore to 30% after activity
- Set brightness to 80% via GNOME → daemon will restore to 80% after activity
- Set brightness to 0% (off) → daemon stops controlling the backlight until you turn it back on

### Manual testing

Run in foreground mode for testing:
```bash
sudo ./kbd-backlight-daemon -f
```

### Command-line options

- `-f, --foreground` - Run in foreground (don't daemonize)
- `-h, --help` - Show help message

## How it works

The daemon uses the Linux input event subsystem to monitor:
- `/dev/input/event*` devices for keyboard, mouse, and touchpad activity

For brightness control, it writes to:
- `/sys/class/leds/chromeos::kbd_backlight/brightness`

External brightness changes (Fn+Space) are detected by polling the sysfs file every 200ms when active, or every 2 seconds when idle. This adaptive polling minimizes CPU usage while maintaining responsiveness.

### State machine

| State | Description | Behavior |
|-------|-------------|----------|
| Active | User is present | Backlight on, dims after timeout |
| Dimmed | Inactive timeout | Backlight off, turns on with activity |
| User disabled | User set brightness to 0 | Backlight stays off until user turns it back on |

## Uninstallation

```bash
sudo systemctl stop kbd-backlight-daemon
sudo systemctl disable kbd-backlight-daemon
sudo make uninstall
sudo systemctl daemon-reload
```

## Requirements

- Linux with input event subsystem
- Framework Laptop 13 (or any laptop with ChromeOS EC keyboard backlight)
- Root access (for reading input devices and writing to sysfs)

## License

MIT
