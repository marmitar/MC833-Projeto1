ifeq ($(origin CC), default)
	CC := clang
endif
ifeq ($(strip $(CC)), gcc)
	IS_GCC = 1
endif

, := ,

# undefined, address, memory or thread
SANITIZER ?= undefined

DEBUG_FLAGS := -D_FORTIFY_SOURCE=1
DEBUGGER := -ggdb3 -DDEBUG -D_FORTIFY_SOURCE=3 $(if $(SANITIZER), -fsanitize=$(SANITIZER)) \
	-fstack-clash-protection -fcf-protection -ftrapv -fno-omit-frame-pointer -mno-omit-leaf-frame-pointer \
	$(if $(IS_GCC),,-Rpass=.*)

LDFLAGS := -O1 --gc-sections --sort-common --as-needed -z,relro -z,now -z,pack-relative-relocs

CFLAGS = -std=gnu23 -Wall -Werror -Wpedantic \
	-Winline -Wunused-result -Wconversion -Wsign-conversion -Wformat=2 -Wformat-signedness \
	-Wformat-truncation$(if $(IS_GCC),=2) -Wformat-overflow$(if $(IS_GCC),=2) $(if $(IS_GCC), -Wstringop-overflow=4) \
	-O3 -march=native -mtune=native -fno-plt -fno-exceptions -ffast-math $(if $(IS_GCC), -fallow-store-data-races) \
	-pipe -flto=$(if $(IS_GCC),auto,full) $(if $(IS_GCC), -fwhole-program, -fvisibility=hidden) \
	-ffunction-sections -fdata-sections $(addprefix -Wl$(,), $(LDFLAGS)) $(DEBUG_FLAGS)

VGFLAGS = --leak-check=full --show-leak-kinds=all --track-origins=yes --verbose --error-exitcode=10


.PHONY: all clean debug run .clangd

all: clean build/main

debug: DEBUG_FLAGS := $(DEBUGGER)
debug: all

build/main: src/main.c
	$(CC) $(CFLAGS) $< -o $@

run: build/main
	valgrind $(VGFLAGS) ./$<

.clangd: DEBUG_FLAGS := $(DEBUGGER)
.clangd:
	echo "CompileFlags:"          > $@
	echo "  Compiler: $(CC)"     >> $@
	echo "  Add:"                >> $@
	printf "    - %s\n" $(CFLAGS)  >> $@

clean:
	rm -f build/main *.valgrind vgcore.*
