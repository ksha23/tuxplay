# tuxplayd - AirPlay 2 sender with a PipeWire sink clocked by the receivers' PTP time.

PKGS     = libpipewire-0.3 libevent libevent_pthreads libsodium libplist-2.0 avahi-client openssl json-c
CC      ?= cc
CFLAGS  ?= -O2 -g
CFLAGS  += -std=gnu11 -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers \
           -D_GNU_SOURCE -DHAVE_ENDIAN_H -DCONFIG_OPENSSL=1 \
           -Isrc -Ivendor/pair_ap -Ivendor/airptp $(shell pkg-config --cflags $(PKGS))
LDLIBS  += $(shell pkg-config --libs $(PKGS)) -lpthread -lm

SRC      = $(wildcard src/*.c)
VENDOR   = $(wildcard vendor/pair_ap/*.c) $(wildcard vendor/airptp/*.c)
OBJ      = $(SRC:%.c=build/%.o) $(VENDOR:%.c=build/%.o)

PREFIX  ?= /usr/local

all: tuxplayd

tuxplayd: $(OBJ)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

build/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -MMD -MP -c -o $@ $<

# The vendored code is not ours to restyle; keep its warnings out of the way.
build/vendor/%.o: CFLAGS += -w

install: tuxplayd
	install -D -m 0755 tuxplayd $(DESTDIR)$(PREFIX)/bin/tuxplayd
	# PTP needs ports 319 and 320, which are privileged. A file capability lets
	# the daemon bind them while running as the desktop user, next to PipeWire.
	setcap cap_net_bind_service=+ep $(DESTDIR)$(PREFIX)/bin/tuxplayd

clean:
	rm -rf build tuxplayd

-include $(OBJ:.o=.d)

.PHONY: all install clean
