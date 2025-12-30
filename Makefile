CC = gcc
CFLAGS = -Wall -Wextra -O2
LDFLAGS =

PREFIX ?= /usr/local
BINDIR = $(PREFIX)/bin
SYSCONFDIR = /etc
SYSTEMDDIR = /etc/systemd/system

TARGET = kbd-backlight-daemon
SRC = src/kbd-backlight-daemon.c

.PHONY: all clean install uninstall

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

clean:
	rm -f $(TARGET)

install: $(TARGET)
	install -Dm755 $(TARGET) $(DESTDIR)$(BINDIR)/$(TARGET)
	install -Dm644 kbd-backlight-daemon.conf $(DESTDIR)$(SYSCONFDIR)/kbd-backlight-daemon.conf
	install -Dm644 kbd-backlight-daemon.service $(DESTDIR)$(SYSTEMDDIR)/kbd-backlight-daemon.service
	@echo ""
	@echo "Installation complete!"
	@echo "To enable and start the service:"
	@echo "  sudo systemctl daemon-reload"
	@echo "  sudo systemctl enable kbd-backlight-daemon"
	@echo "  sudo systemctl start kbd-backlight-daemon"

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/$(TARGET)
	rm -f $(DESTDIR)$(SYSCONFDIR)/kbd-backlight-daemon.conf
	rm -f $(DESTDIR)$(SYSTEMDDIR)/kbd-backlight-daemon.service
	@echo "Uninstall complete. Run 'sudo systemctl daemon-reload' to reload systemd."
