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
# The http cases fetch /index.txt from a fixture server on 127.0.0.1:18080.
# This target starts that server itself when nothing is already listening
# (running the test binary directly still needs the server from the header
# comment in tests/test_main.c -- without it the http cases fail, which is
# a missing fixture, not a code failure). An already-running server on the
# port is reused and left alone, so tools/build.sh's server keeps working.
# No -D_DEFAULT_SOURCE on the command line: the sources self-define it, and
# a command-line define collides with that under -Werror (macro redefined).
test:
	gcc -std=c99 -Wall -Wextra -Werror -Isrc -Ithird_party/cJSON -Ithird_party/fbink \
	    -o /tmp/aiquota_test tests/test_main.c src/config.c src/quota.c src/http.c \
	    src/input.c src/net.c src/light.c src/watchdog.c src/mono.c third_party/cJSON/cJSON.c -lpthread -lrt
	@command -v python3 >/dev/null 2>&1 || { echo "make test: python3 not found (fixture server)"; exit 1; }; \
	if python3 -c "import socket; socket.create_connection(('127.0.0.1', 18080), 1)" 2>/dev/null; then \
	    /tmp/aiquota_test; \
	else \
	    python3 -m http.server 18080 --bind 127.0.0.1 --directory tests >/dev/null 2>&1 & \
	    srv=$$!; trap 'kill $$srv 2>/dev/null' EXIT; sleep 1; /tmp/aiquota_test; \
	fi

clean:
	rm -f $(BIN)
# Shell-level launcher/watchdog simulations (session isolation, traps).
test-scripts:
	bash tests/wrapper_sim.sh
	bash tests/watchdog_sim.sh

.PHONY: test test-scripts clean
