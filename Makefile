ifeq ($(OS),Windows_NT)
    # Windows builds require LLVM-MinGW (clang). MSVC (cl.exe) cannot build
    # Magnesium: the VM dispatch loop uses labels-as-values (computed goto),
    # a GNU C extension MSVC does not support. There is no switch-fallback.
    # make's $(shell) captures stdout only, so this is non-empty iff clang
    # runs: works under both cmd.exe (mingw32-make default) and sh.
    CLANG_OK := $(shell clang --version)
    ifeq ($(strip $(CLANG_OK)),)
        $(error LLVM-MinGW (clang) is required to build Magnesium on Windows. \
        Install: winget install MartinStorsjo.LLVM-MinGW.UCRT. \
        MSVC (cl.exe) is not supported: computed-goto dispatch requires labels-as-values.)
    endif
    CC = clang
    CFLAGS = -Wall -Wextra -std=c11 -O3 -Isrc -D_CRT_SECURE_NO_WARNINGS -fno-stack-protector
    LDFLAGS =
    TARGET = magnesium.exe
    LIB_TARGET = magnesium.dll
    LIB_LDFLAGS = -shared -lm -lws2_32
else
    CC = gcc
    CFLAGS = -Wall -Wextra -std=c11 -O3 -Isrc -D_GNU_SOURCE -pthread
    LDFLAGS = -lm -ldl -pthread
    TARGET = magnesium
    ifeq ($(shell uname -s),Darwin)
        LIB_TARGET = libmagnesium.dylib
        LIB_LDFLAGS = -dynamiclib -lm -ldl -pthread -install_name @rpath/$(LIB_TARGET)
    else
        LIB_TARGET = libmagnesium.so
        LIB_LDFLAGS = -shared -lm -ldl -pthread -Wl,-soname,$(LIB_TARGET)
    endif
endif

SRC = src/main.c \
      src/lexer.c \
      src/parser.c \
      src/compiler.c \
      src/typecheck.c \
      src/object.c \
      src/gc.c \
      src/vm.c \
      src/serialize.c \
      src/lsp.c

LIB_SRC = src/lexer.c \
          src/parser.c \
          src/compiler.c \
          src/typecheck.c \
          src/object.c \
          src/gc.c \
          src/vm.c \
          src/serialize.c \
          src/lsp.c \
          src/mg_ffi_glue.c

SAN_CFLAGS = $(CFLAGS) -g -O1 -fno-omit-frame-pointer -fsanitize=address,undefined
SAN_LDFLAGS = $(LDFLAGS) -fsanitize=address,undefined
UBSAN_CFLAGS = $(CFLAGS) -g -O1 -fno-omit-frame-pointer -fsanitize=undefined
UBSAN_LDFLAGS = $(LDFLAGS) -fsanitize=undefined

all: $(TARGET) mt

$(TARGET): $(SRC) Makefile
	$(CC) $(CFLAGS) $(SRC) -o $(TARGET) $(LDFLAGS)

mt: src/mt.c Makefile
	$(CC) $(CFLAGS) src/mt.c -o mt

debug: $(SRC) Makefile
	$(CC) -Wall -Wextra -std=c11 -g -O0 -Isrc $(SRC) -o $(TARGET) $(LDFLAGS)

asan: $(SRC) Makefile
	$(CC) $(SAN_CFLAGS) $(SRC) -o $(TARGET) $(SAN_LDFLAGS)

ubsan: $(SRC) Makefile
	$(CC) $(UBSAN_CFLAGS) $(SRC) -o $(TARGET) $(UBSAN_LDFLAGS)

test: $(SRC) Makefile
	$(CC) $(CFLAGS) $(SRC) -o $(TARGET) $(LDFLAGS)
	@./scripts/run_tests.sh
	@bash ./scripts/run_bytecode_tests.sh

test-verbose: $(SRC) Makefile
	$(CC) $(CFLAGS) $(SRC) -o $(TARGET) $(LDFLAGS)
	@./scripts/run_tests.sh -v

test-ubsan: ubsan
	@UBSAN_OPTIONS=halt_on_error=1 ./scripts/run_tests.sh
	@UBSAN_OPTIONS=halt_on_error=1 bash ./scripts/run_bytecode_tests.sh

test-bytecode: $(SRC) Makefile
	$(CC) $(CFLAGS) $(SRC) -o $(TARGET) $(LDFLAGS)
	@bash ./scripts/run_bytecode_tests.sh

test-asan: asan
	@env ASAN_OPTIONS=detect_leaks=1:abort_on_error=1:print_stacktrace=1 UBSAN_OPTIONS=halt_on_error=1 ./magnesium tests/test_asan_smoke.mg >/dev/null

test-sanitize: test-ubsan test-asan

test-asan-full: asan
	@env ASAN_OPTIONS=detect_leaks=1:abort_on_error=1:print_stacktrace=1 UBSAN_OPTIONS=halt_on_error=1 ./scripts/run_tests.sh

bench: $(SRC) Makefile
	$(CC) $(CFLAGS) $(SRC) -o $(TARGET) $(LDFLAGS)
	@./scripts/run_bench.sh

lib: $(LIB_TARGET)

$(LIB_TARGET): $(LIB_SRC) Makefile
	$(CC) $(CFLAGS) -fPIC $(LIB_SRC) -o $(LIB_TARGET) $(LIB_LDFLAGS)

PREFIX ?= /usr/local

install: $(LIB_TARGET) mt
	install -d $(PREFIX)/lib $(PREFIX)/include $(PREFIX)/lib/pkgconfig $(PREFIX)/bin
	install -m 755 $(LIB_TARGET) $(PREFIX)/lib/$(LIB_TARGET)
	install -m 644 src/magnesium.h $(PREFIX)/include/magnesium.h
	install -m 755 mt $(PREFIX)/bin/mt
	install -m 755 $(TARGET) $(PREFIX)/bin/magnesium
	sed 's|@PREFIX@|$(PREFIX)|' magnesium.pc > $(PREFIX)/lib/pkgconfig/magnesium.pc

uninstall:
	rm -f $(PREFIX)/lib/$(LIB_TARGET) $(PREFIX)/include/magnesium.h $(PREFIX)/lib/pkgconfig/magnesium.pc $(PREFIX)/bin/mt $(PREFIX)/bin/magnesium

clean:
	rm -f $(TARGET) $(LIB_TARGET) mt

.PHONY: all clean debug asan ubsan test test-verbose test-ubsan test-bytecode test-asan test-sanitize test-asan-full bench lib install uninstall mt
