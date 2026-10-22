CC ?= gcc
CFLAGS := -std=gnu11 -g -Wall -Wextra -Wno-unused-parameter -Werror -fsanitize=address,undefined -fno-omit-frame-pointer
FONT_KO ?= /mnt/c/Windows/Fonts/malgun.ttf
FONT_KO_BOLD ?= /mnt/c/Windows/Fonts/malgunbd.ttf

check: test_rds
	./test_rds

test_rds: test_rds.c ../source/rds.c ../source/rds.h
	$(CC) $(CFLAGS) -o $@ test_rds.c ../source/rds.c

# Screen preview on the PC: the real draw() with a Windows Korean font standing in for the
# console's shared font. Writes preview_*.ppm (python3 ppm2png.py converts them).
preview: preview.c ../source/main.c ../source/gfx.c ../source/rds.c
	$(CC) -std=gnu11 -g -Wall -Wno-unused-parameter -Wno-unused-function -DEOFM_HOST -Ihost -I/usr/include/freetype2 \
		-o preview_bin preview.c ../source/gfx.c ../source/rds.c -lfreetype -lm
	./preview_bin $(FONT_KO)

clean:
	rm -f test_rds preview_bin preview_*.ppm preview_*.png

.PHONY: check preview clean
