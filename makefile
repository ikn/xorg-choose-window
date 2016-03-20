PROG := xorg-choose-window
PKGCONFIG_LIBS := xcb xcb-keysyms xcb-icccm xcb-ewmh
CFLAGS += -Wall `pkg-config --cflags ${PKGCONFIG_LIBS}`
LDLIBS += -lm `pkg-config --libs ${PKGCONFIG_LIBS}`
INSTALL_PROGRAM := install

prefix := /usr/local
exec_prefix := $(prefix)
bindir := $(exec_prefix)/bin

.PHONY: all clean distclean install uninstall

all: $(PROG)

clean:
	- $(RM) $(PROG)

distclean: clean

install:
	mkdir -p "$(DESTDIR)$(bindir)"
	$(INSTALL_PROGRAM) $(PROG) "$(DESTDIR)$(bindir)"

uninstall:
	$(RM) -r "$(DESTDIR)$(bindir)/$(PROG)"
