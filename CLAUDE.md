# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Build Commands

```bash
make              # Build the daemon
make clean        # Remove compiled binary
sudo make install # Install binary, config, and systemd service
sudo make uninstall
```

## Testing

Run in foreground mode for testing (requires root for input device access):
```bash
sudo ./kbd-backlight-daemon -f
```

## Architecture

Single-file C daemon (`src/kbd-backlight-daemon.c`) with no external dependencies.

**Input monitoring**: Uses epoll to monitor `/dev/input/event*` devices. Implements a debounce mechanism that removes file descriptors from epoll during the debounce period to prevent busy-looping during continuous mouse movement.

**Brightness control**: Writes to `/sys/class/leds/chromeos::kbd_backlight/brightness`. Uses a persistent file descriptor for fast polling reads (avoids open/close overhead).

**State machine**: Three states - Active (backlight on), Dimmed (timeout reached), User disabled (user manually set brightness to 0).

**External change detection**: Polls brightness file to detect Fn+Space changes (EC-handled, no input events generated). Polling intervals: 1s when active, 5s when idle.
