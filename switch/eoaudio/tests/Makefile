# Host build of the portable parts of eoaudio (gcc/clang, not devkitPro).
CC      ?= cc
CFLAGS  ?= -O1 -g -Wall -Wextra -Werror -std=c11 -D_DEFAULT_SOURCE -fsanitize=address,undefined
LDLIBS  := -lm
FIXTURES := ../../../fixtures
SRC     := ../source

all: test_pcm test_config test_policy

test_pcm: test_pcm.c $(SRC)/pcm.c $(SRC)/pcm.h
	$(CC) $(CFLAGS) -o $@ test_pcm.c $(SRC)/pcm.c $(LDLIBS)

test_config: test_config.c $(SRC)/config.c $(SRC)/config.h $(SRC)/usbdesc.c $(SRC)/usbdesc.h
	$(CC) $(CFLAGS) -o $@ test_config.c $(SRC)/config.c $(SRC)/usbdesc.c $(LDLIBS)

test_policy: test_policy.c $(SRC)/policy.c $(SRC)/policy.h $(SRC)/state.c $(SRC)/state.h
	$(CC) $(CFLAGS) -o $@ test_policy.c $(SRC)/policy.c $(SRC)/state.c $(LDLIBS)

check: all
	./test_pcm
	./test_config $(FIXTURES)
	./test_policy

clean:
	rm -f test_pcm test_config test_policy

.PHONY: all check clean
