CC ?= cc
PREFIX ?= /usr/local
DESTDIR ?=

PKGS = gtk+-3.0 gdk-pixbuf-2.0 gio-2.0
CPPFLAGS += $(shell pkg-config --cflags $(PKGS))
CFLAGS ?= -O2 -g
CFLAGS += -std=c11 -Wall -Wextra -Wpedantic
LDLIBS += $(shell pkg-config --libs $(PKGS))

SOURCES = main.c gui.c file.c
OBJECTS = $(SOURCES:.c=.o)
SYSTEMD_USER_DIR = $(DESTDIR)$(PREFIX)/lib/systemd/user

.PHONY: all clean install uninstall

all: cleaner

cleaner: $(OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $(OBJECTS) $(LDLIBS)

%.o: %.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

install: cleaner
	install -D -m 0755 cleaner "$(DESTDIR)$(PREFIX)/bin/cleaner"
	install -D -m 0644 cleaner.desktop \
		"$(DESTDIR)$(PREFIX)/share/applications/cleaner.desktop"
	sed 's|/usr/local|$(PREFIX)|g' cleaner.desktop \
		> "$(DESTDIR)$(PREFIX)/share/applications/cleaner.desktop"
	install -d "$(SYSTEMD_USER_DIR)"
	install -D -m 0644 systemd/cleaner.timer \
		"$(SYSTEMD_USER_DIR)/cleaner.timer"
	install -D -m 0644 systemd/cleaner.service \
		"$(SYSTEMD_USER_DIR)/cleaner.service"
	sed 's|/usr/local|$(PREFIX)|g' systemd/cleaner.service \
		> "$(SYSTEMD_USER_DIR)/cleaner.service"

uninstall:
	rm -f "$(DESTDIR)$(PREFIX)/bin/cleaner"
	rm -f "$(DESTDIR)$(PREFIX)/share/applications/cleaner.desktop"
	rm -f "$(SYSTEMD_USER_DIR)/cleaner.service"
	rm -f "$(SYSTEMD_USER_DIR)/cleaner.timer"

clean:
	rm -f $(OBJECTS) cleaner
