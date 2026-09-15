CXX      ?= g++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -Wpedantic
PREFIX   ?= /usr/local

edit: edit.cpp
→   $(CXX) $(CXXFLAGS) -o $@ $<

install: edit
→   install -Dm755 edit $(DESTDIR)$(PREFIX)/bin/edit

uninstall:
→   rm -f $(DESTDIR)$(PREFIX)/bin/edit

clean:
→   rm -f edit

.PHONY: install uninstall clean