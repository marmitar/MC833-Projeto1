CC = clang

# undefined, address, memory or thread
SANITIZER = undefined

DEBUG_FLAGS = -D_FORTIFY_SOURCE=1

CFLAGS = -std=gnu23 -Wall -Werror -Wpedantic -Wunused-result \
	-Wformat=2 -Wformat-truncation -Wformat-overflow \
	-Wconversion -Wsign-conversion -Wformat-signedness \
	-march=native -mtune=native -pipe  -O3 -flto=full \
	-fno-plt -fno-exceptions -Winline \
	$(DEBUG_FLAGS)

VGFLAGS = --leak-check=full --show-leak-kinds=all --track-origins=yes --verbose --error-exitcode=10


.PHONY: all clean debug ubsan run

all: clean build/main

debug: DEBUG_FLAGS = -ggdb3 -DDEBUG -D_FORTIFY_SOURCE=3 \
	-fstack-clash-protection -fcf-protection \
	-fno-omit-frame-pointer -mno-omit-leaf-frame-pointer \
	$(if $(SANITIZER), -fsanitize=$(SANITIZER))
debug: all

build/main: src/main.c
	$(CC) $(CFLAGS) $< -o $@

run: build/main
	valgrind $(VGFLAGS) ./$<

clean:
	rm -f build/main *.valgrind vgcore.*
