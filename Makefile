CC ?= gcc
CFLAGS ?= -O2 -Wall -Wextra
TARGET = fantech-ds4-proxy
SRC = fantech-ds4-proxy.c

PREFIX ?= /usr/local
BINDIR ?= $(PREFIX)/bin
SYSTEMDDIR ?= /etc/systemd/system
UDEVDIR ?= /etc/udev/rules.d

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) $(SRC) -o $(TARGET)

clean:
	rm -f $(TARGET)

install: $(TARGET)
	install -d $(DESTDIR)$(BINDIR)
	install -m 755 $(TARGET) $(DESTDIR)$(BINDIR)/$(TARGET)
	install -m 755 fantech-unbind-audio.sh $(DESTDIR)$(BINDIR)/fantech-unbind-audio.sh
	install -d $(DESTDIR)$(UDEVDIR)
	install -m 644 99-fantech-ds4-proxy.rules $(DESTDIR)$(UDEVDIR)/99-fantech-ds4-proxy.rules
	install -m 644 98-fantech-ds4-unbind-audio.rules $(DESTDIR)$(UDEVDIR)/98-fantech-ds4-unbind-audio.rules
	install -d $(DESTDIR)$(SYSTEMDDIR)
	install -m 644 fantech-ds4-proxy.service $(DESTDIR)$(SYSTEMDDIR)/fantech-ds4-proxy.service
	udevadm control --reload-rules && udevadm trigger
	systemctl daemon-reload
	systemctl enable --now fantech-ds4-proxy.service

uninstall:
	systemctl stop fantech-ds4-proxy.service || true
	systemctl disable fantech-ds4-proxy.service || true
	rm -f $(DESTDIR)$(SYSTEMDDIR)/fantech-ds4-proxy.service
	rm -f $(DESTDIR)$(UDEVDIR)/99-fantech-ds4-proxy.rules
	rm -f $(DESTDIR)$(UDEVDIR)/98-fantech-ds4-unbind-audio.rules
	rm -f $(DESTDIR)$(BINDIR)/$(TARGET)
	rm -f $(DESTDIR)$(BINDIR)/fantech-unbind-audio.sh
	udevadm control --reload-rules
	systemctl daemon-reload

.PHONY: all clean install uninstall
