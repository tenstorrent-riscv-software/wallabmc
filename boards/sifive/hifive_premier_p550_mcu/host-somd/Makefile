# SPDX-License-Identifier: Apache-2.0
#
# somd - HiFive Premier P550 SOM daemon
#
# Native build on RISC-V host:
#   make
#
# Cross-compile:
#   make CC=riscv64-linux-gnu-gcc

CC ?= gcc
CFLAGS ?= -O2 -Wall -Wextra -Wpedantic
PREFIX ?= /usr/local

somd: somd.c hfp_protocol.h
	$(CC) $(CFLAGS) -o $@ somd.c

test_protocol: test_protocol.c hfp_protocol.h
	$(CC) $(CFLAGS) -o $@ test_protocol.c

test: test_protocol
	./test_protocol

install: somd
	install -D -m 755 somd $(DESTDIR)$(PREFIX)/sbin/somd
	install -D -m 644 somd.service $(DESTDIR)/etc/systemd/system/somd.service

clean:
	rm -f somd test_protocol

.PHONY: install clean test
