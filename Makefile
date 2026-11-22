CXX ?= c++
CXXFLAGS ?= -O2
CPPFLAGS += -Iinclude
WARNINGS = -std=c++98 -Wall -Wextra -Wpedantic
PREFIX ?= /usr/local
BINDIR ?= $(PREFIX)/bin
MANDIR ?= $(PREFIX)/share/man
LICENSEDIR ?= $(PREFIX)/share/licenses/pgn-eco
INSTALL ?= install
THREAD_FLAGS ?= -pthread

OBJECTS = build/main.o build/chess.o build/pgn.o

.PHONY: all clean test install uninstall

all: pgn-eco

pgn-eco: $(OBJECTS)
	$(CXX) $(LDFLAGS) $(THREAD_FLAGS) -o $@ $(OBJECTS)

build:
	mkdir -p build

build/%.o: src/%.cpp | build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(THREAD_FLAGS) $(WARNINGS) -c -o $@ $<

build/pgn-eco-position-key: tests/test_position_key.cpp build/chess.o | build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(THREAD_FLAGS) $(WARNINGS) -o $@ $^

test: pgn-eco build/pgn-eco-position-key
	build/pgn-eco-position-key
	sh tests/test_public_smoke.sh

install: pgn-eco
	$(INSTALL) -d '$(DESTDIR)$(BINDIR)' '$(DESTDIR)$(MANDIR)/man1' '$(DESTDIR)$(LICENSEDIR)'
	$(INSTALL) -m 755 pgn-eco '$(DESTDIR)$(BINDIR)/pgn-eco'
	$(INSTALL) -m 644 man/pgn-eco.1 '$(DESTDIR)$(MANDIR)/man1/pgn-eco.1'
	$(INSTALL) -m 644 LICENSE COPYRIGHT THIRD_PARTY_NOTICES.md '$(DESTDIR)$(LICENSEDIR)/'

uninstall:
	rm -f '$(DESTDIR)$(BINDIR)/pgn-eco' '$(DESTDIR)$(MANDIR)/man1/pgn-eco.1'
	rm -f '$(DESTDIR)$(LICENSEDIR)/LICENSE' '$(DESTDIR)$(LICENSEDIR)/COPYRIGHT' '$(DESTDIR)$(LICENSEDIR)/THIRD_PARTY_NOTICES.md'

clean:
	rm -rf build pgn-eco
