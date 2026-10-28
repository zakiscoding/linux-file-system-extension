CC ?= cc
CFLAGS ?= -O2 -g -std=c11 -Wall -Wextra -Werror -pedantic

.PHONY: all test kernel guest run debug test-qemu test-gdb
all: build/edufs

build/edufs: tools/edufs.c include/edufs_disk.h
	mkdir -p build
	$(CC) $(CFLAGS) -Iinclude tools/edufs.c -o $@

test: build/edufs
	python3 tests/test_images.py

kernel:
	bash scripts/build-kernel.sh
guest: build/edufs
	bash scripts/build-guest.sh
run:
	bash scripts/run-qemu.sh
debug:
	bash scripts/run-qemu.sh --debug
test-qemu:
	bash scripts/test-qemu.sh
test-gdb:
	bash scripts/test-gdb.sh
