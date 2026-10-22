# Host build of the portable descriptor parser test (gcc/clang, not devkitPro).
CC      ?= cc
CFLAGS  ?= -O1 -g -Wall -Wextra -Werror -std=c11 -fsanitize=address,undefined
FIXTURES := ../../../fixtures

test_usbdesc: test_usbdesc.c ../source/usbdesc.c ../source/usbdesc.h
	$(CC) $(CFLAGS) -o $@ test_usbdesc.c ../source/usbdesc.c

check: test_usbdesc
	./test_usbdesc $(FIXTURES)

clean:
	rm -f test_usbdesc

.PHONY: check clean
