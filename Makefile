CC ?= cc
PKG_CONFIG ?= pkg-config
PYTHON ?= python3
CFLAGS ?= -O2
DEP_CPPFLAGS = $(shell $(PKG_CONFIG) --cflags libcurl json-c)
DEP_LDLIBS = $(shell $(PKG_CONFIG) --libs libcurl json-c)
WARNINGS = -Wall -Wextra -Wpedantic -Wconversion -Wshadow

.PHONY: all test clean

all: nvc

nvc: nvc.c source.inc state.inc
	$(CC) $(CPPFLAGS) $(DEP_CPPFLAGS) $(CFLAGS) -std=c17 $(WARNINGS) $(LDFLAGS) -o $@ $< $(DEP_LDLIBS) $(LDLIBS)

nvc-test: nvc.c source.inc state.inc
	$(CC) $(CPPFLAGS) $(DEP_CPPFLAGS) $(CFLAGS) -std=c17 $(WARNINGS) -DNVC_TEST $(LDFLAGS) -o $@ $< $(DEP_LDLIBS) $(LDLIBS)

test: nvc nvc-test
	$(PYTHON) -I tests/test.py ./nvc-test

clean:
	rm -f nvc nvc-test
