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
    CFLAGS = -Wall -Wextra -std=c11 -O3 -Isrc -D_CRT_SECURE_NO_WARNINGS
    LDFLAGS = -lws2_32
    TARGET = magnesium.exe
    LIB_TARGET = libmagnesium.dll
    MT_TARGET = mt.exe
    RUNTIME_API_TEST = tests/test_runtime_api.exe
    ASAN_RUNTIME_API_TEST = tests/test_runtime_api_asan.exe
    UBSAN_RUNTIME_API_TEST = tests/test_runtime_api_ubsan.exe
    LIB_CFLAGS = -DMG_BUILD_SHARED
    LIB_LDFLAGS = -shared $(LDFLAGS)
else
    UNAME_S ?= $(shell uname -s)
    CC = gcc
    CFLAGS = -Wall -Wextra -std=c11 -O3 -Isrc -D_GNU_SOURCE -pthread
    LDFLAGS = -lm -ldl -pthread
    TARGET = magnesium
    ifeq ($(UNAME_S),Darwin)
        CFLAGS = -Wall -Wextra -std=c11 -O3 -Isrc -pthread
        LDFLAGS = -lm -pthread
        LIB_TARGET = libmagnesium.dylib
        LIB_LDFLAGS = -dynamiclib -lm -pthread -install_name @rpath/$(LIB_TARGET)
    else
        LIB_TARGET = libmagnesium.so
        LIB_LDFLAGS = -shared -lm -ldl -pthread -Wl,-soname,$(LIB_TARGET)
    endif
    MT_TARGET = mt
    RUNTIME_API_TEST = tests/test_runtime_api
    ASAN_RUNTIME_API_TEST = tests/test_runtime_api_asan
    UBSAN_RUNTIME_API_TEST = tests/test_runtime_api_ubsan
    LIB_CFLAGS = -fPIC -DMG_BUILD_SHARED
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

ifeq ($(OS),Windows_NT)
    TEST_RUNNER = powershell.exe -NoProfile -ExecutionPolicy Bypass -File scripts/run_tests.ps1
    TEST_RUNNER_VERBOSE = powershell.exe -NoProfile -ExecutionPolicy Bypass -File scripts/run_tests.ps1 -v
    BYTECODE_TEST_RUNNER = powershell.exe -NoProfile -ExecutionPolicy Bypass -File scripts/run_bytecode_tests.ps1
    NATIVE_API_RUNNER = .\tests\test_runtime_api.exe
else
    TEST_RUNNER = ./scripts/run_tests.sh
    TEST_RUNNER_VERBOSE = ./scripts/run_tests.sh -v
    BYTECODE_TEST_RUNNER = bash ./scripts/run_bytecode_tests.sh
    NATIVE_API_RUNNER = ./$(RUNTIME_API_TEST)
endif

SAN_CFLAGS = $(CFLAGS) -g -O1 -fno-omit-frame-pointer -fsanitize=address,undefined
SAN_LDFLAGS = $(LDFLAGS) -fsanitize=address,undefined
UBSAN_CFLAGS = $(CFLAGS) -g -O1 -fno-omit-frame-pointer -fsanitize=undefined
UBSAN_LDFLAGS = $(LDFLAGS) -fsanitize=undefined

all: $(TARGET) $(MT_TARGET)

$(TARGET): $(SRC) Makefile
	$(CC) $(CFLAGS) $(SRC) -o $(TARGET) $(LDFLAGS)

$(MT_TARGET): src/mt.c Makefile
	$(CC) $(CFLAGS) src/mt.c -o $(MT_TARGET)

ifeq ($(OS),Windows_NT)
mt: $(MT_TARGET)
endif

debug: $(SRC) Makefile
	$(CC) $(CFLAGS) -g -O0 $(SRC) -o $(TARGET) $(LDFLAGS)

asan: $(SRC) Makefile
	$(CC) $(SAN_CFLAGS) $(SRC) -o $(TARGET) $(SAN_LDFLAGS)

ubsan: $(SRC) Makefile
	$(CC) $(UBSAN_CFLAGS) $(SRC) -o $(TARGET) $(UBSAN_LDFLAGS)

$(RUNTIME_API_TEST): tests/test_runtime_api.c $(LIB_SRC) Makefile
	$(CC) $(CFLAGS) tests/test_runtime_api.c $(LIB_SRC) -o $(RUNTIME_API_TEST) $(LDFLAGS)

$(ASAN_RUNTIME_API_TEST): tests/test_runtime_api.c $(LIB_SRC) Makefile
	$(CC) $(SAN_CFLAGS) tests/test_runtime_api.c $(LIB_SRC) -o $(ASAN_RUNTIME_API_TEST) $(SAN_LDFLAGS)

$(UBSAN_RUNTIME_API_TEST): tests/test_runtime_api.c $(LIB_SRC) Makefile
	$(CC) $(UBSAN_CFLAGS) tests/test_runtime_api.c $(LIB_SRC) -o $(UBSAN_RUNTIME_API_TEST) $(UBSAN_LDFLAGS)

test-native-api: $(RUNTIME_API_TEST)
	@$(NATIVE_API_RUNNER)

test: $(SRC) $(RUNTIME_API_TEST) Makefile
	$(CC) $(CFLAGS) $(SRC) -o $(TARGET) $(LDFLAGS)
	@$(NATIVE_API_RUNNER)
	@$(TEST_RUNNER)
	@$(BYTECODE_TEST_RUNNER)

test-verbose: $(SRC) Makefile
	$(CC) $(CFLAGS) $(SRC) -o $(TARGET) $(LDFLAGS)
	@$(TEST_RUNNER_VERBOSE)

test-ubsan: ubsan $(UBSAN_RUNTIME_API_TEST)
ifeq ($(OS),Windows_NT)
	@set UBSAN_OPTIONS=halt_on_error=1&& .\tests\test_runtime_api_ubsan.exe
	@set UBSAN_OPTIONS=halt_on_error=1&& $(TEST_RUNNER)
	@set UBSAN_OPTIONS=halt_on_error=1&& $(BYTECODE_TEST_RUNNER)
else
	@UBSAN_OPTIONS=halt_on_error=1 ./$(UBSAN_RUNTIME_API_TEST)
	@UBSAN_OPTIONS=halt_on_error=1 $(TEST_RUNNER)
	@UBSAN_OPTIONS=halt_on_error=1 $(BYTECODE_TEST_RUNNER)
endif

test-bytecode: $(SRC) Makefile
	$(CC) $(CFLAGS) $(SRC) -o $(TARGET) $(LDFLAGS)
	@$(BYTECODE_TEST_RUNNER)

test-asan: asan $(ASAN_RUNTIME_API_TEST)
ifeq ($(OS),Windows_NT)
	@set ASAN_OPTIONS=abort_on_error=1:print_stacktrace=1&& set UBSAN_OPTIONS=halt_on_error=1&& .\tests\test_runtime_api_asan.exe
	@set ASAN_OPTIONS=abort_on_error=1:print_stacktrace=1&& set UBSAN_OPTIONS=halt_on_error=1&& .\magnesium.exe tests/test_asan_smoke.mg >NUL
else
	@env ASAN_OPTIONS=detect_leaks=1:abort_on_error=1:print_stacktrace=1 UBSAN_OPTIONS=halt_on_error=1 ./$(ASAN_RUNTIME_API_TEST)
	@env ASAN_OPTIONS=detect_leaks=1:abort_on_error=1:print_stacktrace=1 UBSAN_OPTIONS=halt_on_error=1 ./magnesium tests/test_asan_smoke.mg >/dev/null
endif

test-sanitize:
	$(MAKE) test-ubsan
	$(MAKE) test-asan

test-asan-full: asan $(ASAN_RUNTIME_API_TEST)
ifeq ($(OS),Windows_NT)
	@set ASAN_OPTIONS=abort_on_error=1:print_stacktrace=1&& set UBSAN_OPTIONS=halt_on_error=1&& .\tests\test_runtime_api_asan.exe
	@set ASAN_OPTIONS=abort_on_error=1:print_stacktrace=1&& set UBSAN_OPTIONS=halt_on_error=1&& $(TEST_RUNNER)
else
	@env ASAN_OPTIONS=detect_leaks=1:abort_on_error=1:print_stacktrace=1 UBSAN_OPTIONS=halt_on_error=1 ./$(ASAN_RUNTIME_API_TEST)
	@env ASAN_OPTIONS=detect_leaks=1:abort_on_error=1:print_stacktrace=1 UBSAN_OPTIONS=halt_on_error=1 $(TEST_RUNNER)
endif

bench: $(SRC) Makefile
	$(CC) $(CFLAGS) $(SRC) -o $(TARGET) $(LDFLAGS)
	@./scripts/run_bench.sh

lib: $(LIB_TARGET)

$(LIB_TARGET): $(LIB_SRC) Makefile
	$(CC) $(CFLAGS) $(LIB_CFLAGS) $(LIB_SRC) -o $(LIB_TARGET) $(LIB_LDFLAGS)

PREFIX ?= /usr/local

install: $(TARGET) $(LIB_TARGET) $(MT_TARGET)
	install -d $(PREFIX)/lib $(PREFIX)/include $(PREFIX)/lib/pkgconfig $(PREFIX)/bin
	install -m 755 $(LIB_TARGET) $(PREFIX)/lib/$(LIB_TARGET)
	install -m 644 src/magnesium.h $(PREFIX)/include/magnesium.h
	install -m 755 $(MT_TARGET) $(PREFIX)/bin/$(MT_TARGET)
	install -m 755 $(TARGET) $(PREFIX)/bin/$(TARGET)
	sed 's|@PREFIX@|$(PREFIX)|' magnesium.pc > $(PREFIX)/lib/pkgconfig/magnesium.pc

uninstall:
	rm -f $(PREFIX)/lib/$(LIB_TARGET) $(PREFIX)/include/magnesium.h $(PREFIX)/lib/pkgconfig/magnesium.pc $(PREFIX)/bin/$(MT_TARGET) $(PREFIX)/bin/$(TARGET)

clean:
	rm -f $(TARGET) $(LIB_TARGET) $(MT_TARGET) $(RUNTIME_API_TEST) $(ASAN_RUNTIME_API_TEST) $(UBSAN_RUNTIME_API_TEST)

.PHONY: all clean debug asan ubsan test test-native-api test-verbose test-ubsan test-bytecode test-asan test-sanitize test-asan-full bench lib install uninstall
ifeq ($(OS),Windows_NT)
.PHONY: mt
endif
