CC ?= gcc
CFLAGS := -std=c11 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-omit-frame-pointer

check: test_host
	./test_host

test_host: test_host.c ../source/decode.c ../source/config.c ../source/rewrite.c ../source/decode.h ../source/config.h ../source/rewrite.h
	$(CC) $(CFLAGS) -o $@ test_host.c ../source/decode.c ../source/config.c ../source/rewrite.c

clean:
	rm -f test_host

.PHONY: check clean
