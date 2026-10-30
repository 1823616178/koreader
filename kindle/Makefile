# Cross-build for Kindle (koxtoolchain kindlepw2 on PATH, or override CROSS).
# _BSD_SOURCE/_SVID_SOURCE: the kindlepw2 sysroot is glibc 2.12, which predates
# _DEFAULT_SOURCE (2.19); without them net.c's usleep() stays undeclared.
CROSS    ?= arm-kindlepw2-linux-gnueabi-
CC       := $(CROSS)gcc
CFLAGS   := -std=c99 -Os -ffunction-sections -fdata-sections -Wall -Wextra -D_BSD_SOURCE -D_SVID_SOURCE -Isrc -Ithird_party/cJSON -Ithird_party/fbink
LDFLAGS  := -Wl,--gc-sections
LIBS     := third_party/fbink/libfbink.a -lm -lpthread -lrt
SRCS     := $(wildcard src/*.c) third_party/cJSON/cJSON.c
BIN      := extension/bin/aiquota

$(BIN): $(SRCS) $(wildcard src/*.h) third_party/cJSON/cJSON.h third_party/fbink/fbink.h third_party/fbink/libfbink.a
	$(CC) $(CFLAGS) -o $@ $(SRCS) $(LIBS) $(LDFLAGS)

# Host-side unit tests (no libfbink dependency: excludes ui/keypad/main).
# The runner owns a fixture server on an OS-assigned loopback port and
# passes its URL to the binary. Parallel runs never share a server or binary.
# No -D_DEFAULT_SOURCE on the command line: the sources self-define it, and
# a command-line define collides with that under -Werror (macro redefined).
test:
	@set -e; test_bin=$$(mktemp /tmp/aiquota_test.XXXXXX); \
	trap 'rm -f "$$test_bin"' EXIT; \
	gcc -std=c99 -Wall -Wextra -Werror -Isrc -Ithird_party/cJSON -Ithird_party/fbink \
	    -o "$$test_bin" tests/test_main.c src/config.c src/quota.c src/http.c \
	    src/input.c src/net.c src/light.c src/watchdog.c src/mono.c third_party/cJSON/cJSON.c -lpthread -lrt; \
	python3 tests/run_host_tests.py "$$test_bin"

clean:
	rm -f $(BIN)
# Shell-level launcher/watchdog simulations (session isolation, traps).
test-scripts:
	bash tests/wrapper_sim.sh
	bash tests/watchdog_sim.sh

.PHONY: test test-scripts clean
