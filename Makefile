CC ?= cc
PKG_CONFIG ?= pkg-config
CFLAGS ?= -O2
CPPFLAGS += $(shell $(PKG_CONFIG) --cflags libcurl json-c)
LDLIBS += $(shell $(PKG_CONFIG) --libs libcurl json-c)
WARNINGS = -Wall -Wextra -Wpedantic -Wconversion -Wshadow

.PHONY: all test clean

all: nvc

nvc: nvc.c source.inc state.inc
	$(CC) $(CPPFLAGS) $(CFLAGS) -std=c17 $(WARNINGS) $(LDFLAGS) -o $@ $< $(LDLIBS)

nvc-test: nvc.c source.inc state.inc
	$(CC) $(CPPFLAGS) $(CFLAGS) -std=c17 $(WARNINGS) -DNVC_TEST $(LDFLAGS) -o $@ $< $(LDLIBS)

test: nvc nvc-test
	python3 -I tests/test.py ./nvc-test

clean:
	rm -f nvc nvc-test
