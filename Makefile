CC       ?= cc
AR       ?= ar
PREFIX   ?= /usr/local
DESTDIR  ?=
BUILD    ?= build

CFLAGS   ?= -O2 -Wall -Wextra -std=c11
CPPFLAGS += -Iinclude -Isrc

UNAME_S  := $(shell uname -s)
FUSE_PREFIX ?= /usr/local

WITH_FUSE ?= auto
ifeq ($(WITH_FUSE),auto)
  ifneq ($(wildcard $(FUSE_PREFIX)/include/fuse3/fuse.h),)
    ENABLE_FUSE := 1
    FUSE_CFLAGS := -I$(FUSE_PREFIX)/include/fuse3
    ifeq ($(UNAME_S),Darwin)
      FUSE_LIBS := -L$(FUSE_PREFIX)/lib -lfuse3 -Wl,-rpath,/usr/local/lib -lpthread
    else
      FUSE_LIBS := -L$(FUSE_PREFIX)/lib -lfuse3 -lpthread
    endif
  else ifneq ($(wildcard /usr/include/fuse3/fuse.h),)
    ENABLE_FUSE := 1
    FUSE_CFLAGS := -I/usr/include/fuse3
    FUSE_LIBS   := -lfuse3 -lpthread
  else
    ENABLE_FUSE := 0
  endif
else ifeq ($(WITH_FUSE),1)
  ENABLE_FUSE := 1
  FUSE_CFLAGS ?= -I$(FUSE_PREFIX)/include/fuse3
  ifeq ($(UNAME_S),Darwin)
    FUSE_LIBS ?= -L$(FUSE_PREFIX)/lib -lfuse3 -Wl,-rpath,/usr/local/lib -lpthread
  else
    FUSE_LIBS ?= -L$(FUSE_PREFIX)/lib -lfuse3 -lpthread
  endif
else
  ENABLE_FUSE := 0
endif

LIB_SRCS := \
	src/crypto_compress.c \
	src/container.c \
	src/fs_hfs.c \
	src/fs_apfs.c

LIB_OBJS   := $(patsubst src/%.c,$(BUILD)/%.o,$(LIB_SRCS))
STATIC_LIB := $(BUILD)/libappleexplorer.a
CLI_BIN    := $(BUILD)/apple-explorer
FUSE_BIN   := $(BUILD)/apple-explorer-mount
TEST_BIN   := $(BUILD)/test_apple_explorer

TARGETS := $(STATIC_LIB) $(CLI_BIN)
ifeq ($(ENABLE_FUSE),1)
  TARGETS += $(FUSE_BIN)
endif

.PHONY: all clean check check-asan check-fuzz win32-portable install dist

all: $(TARGETS)

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/%.o: src/%.c include/apple_explorer.h src/aex_internal.h | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(STATIC_LIB): $(LIB_OBJS)
	rm -f $@
	$(AR) rcs $@ $(LIB_OBJS)

$(CLI_BIN): tools/apple_explorer_cli.c $(STATIC_LIB)
	$(CC) $(CPPFLAGS) $(CFLAGS) $< $(STATIC_LIB) -o $@

$(FUSE_BIN): fuse/apple_explorer_fuse.c $(STATIC_LIB)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(FUSE_CFLAGS) $< $(STATIC_LIB) $(FUSE_LIBS) -o $@

$(TEST_BIN): tests/test_apple_explorer.c $(STATIC_LIB)
	$(CC) $(CPPFLAGS) $(CFLAGS) $< $(STATIC_LIB) -o $@

check: all $(TEST_BIN)
	./$(TEST_BIN)
	./$(CLI_BIN) --version >/dev/null

check-asan: | $(BUILD)
	$(CC) $(CPPFLAGS) -O1 -g -Wall -Wextra -std=c11 -fsanitize=address,undefined \
		$(LIB_SRCS) tests/test_apple_explorer.c -o $(BUILD)/test_apple_explorer_asan
	./$(BUILD)/test_apple_explorer_asan

check-fuzz: | $(BUILD)
	$(CC) $(CPPFLAGS) -O1 -g -Wall -Wextra -std=c11 -fsanitize=address,undefined \
		$(LIB_SRCS) tests/fuzz_apple_explorer.c -o $(BUILD)/fuzz_apple_explorer
	./$(BUILD)/fuzz_apple_explorer 2500

# Cross-compile standalone zero-install Windows x64 portable binaries via MinGW-w64
MINGW_CC ?= x86_64-w64-mingw32-gcc
win32-portable:
	mkdir -p $(BUILD)/win32-portable
	$(MINGW_CC) $(CPPFLAGS) -O2 -Wall -Wextra -std=c11 -static \
		$(LIB_SRCS) win32/apple_explorer_win_mount.c tools/apple_explorer_cli.c \
		-lole32 -luuid -o $(BUILD)/win32-portable/apple-explorer.exe
	$(MINGW_CC) $(CPPFLAGS) -O2 -Wall -Wextra -std=c11 -static -mwindows \
		$(LIB_SRCS) win32/apple_explorer_gui.c \
		-lcomctl32 -lcomdlg32 -lshell32 -lole32 -lgdi32 -luuid \
		-o $(BUILD)/win32-portable/AppleExplorer.exe
	cp -p README.md LICENSE $(BUILD)/win32-portable/

install: all
	mkdir -p "$(DESTDIR)$(PREFIX)/include" "$(DESTDIR)$(PREFIX)/lib" "$(DESTDIR)$(PREFIX)/bin"
	cp -p include/apple_explorer.h "$(DESTDIR)$(PREFIX)/include/apple_explorer.h"
	cp -p $(STATIC_LIB) "$(DESTDIR)$(PREFIX)/lib/libappleexplorer.a"
	cp -p $(CLI_BIN) "$(DESTDIR)$(PREFIX)/bin/apple-explorer"
	@if [ "$(ENABLE_FUSE)" = "1" ]; then \
		cp -p $(FUSE_BIN) "$(DESTDIR)$(PREFIX)/bin/apple-explorer-mount"; \
	fi

clean:
	rm -rf $(BUILD)
