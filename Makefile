PROJECT := kitty-framebuffer
BUILD_DIR ?= build
PREFIX ?= /usr/local
DESTDIR ?=

CC ?= cc
AR ?= ar
INSTALL ?= install

CPPFLAGS += -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE -Iinclude
WARNINGS := \
	-Wall -Wextra -Wpedantic -Wconversion -Wshadow \
	-Wstrict-prototypes -Wmissing-prototypes -Wformat=2
CFLAGS ?= -O2 -g
override CFLAGS += -std=c11 -fPIC -pthread $(WARNINGS)
LDLIBS := -lz

LIB_OBJS := \
	$(BUILD_DIR)/kitty_framebuffer.o
STATIC_LIB := $(BUILD_DIR)/lib$(PROJECT).a
SHARED_LIB := $(BUILD_DIR)/lib$(PROJECT).so
TEST_BIN := $(BUILD_DIR)/test-framebuffer
EXAMPLE_BIN := $(BUILD_DIR)/bounce

.PHONY: all clean install sanitize test

all: $(STATIC_LIB) $(SHARED_LIB) $(EXAMPLE_BIN)

$(BUILD_DIR):
	mkdir -p $@

$(BUILD_DIR)/kitty_framebuffer.o: src/kitty_framebuffer.c include/kitty_framebuffer.h src/kitty_framebuffer_internal.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(STATIC_LIB): $(LIB_OBJS)
	$(AR) rcs $@ $^

$(SHARED_LIB): $(LIB_OBJS)
	$(CC) -shared -pthread $(LDFLAGS) $^ $(LDLIBS) -o $@

$(TEST_BIN): tests/test_framebuffer.c $(STATIC_LIB) | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) -Isrc $(CFLAGS) $< $(STATIC_LIB) $(LDFLAGS) $(LDLIBS) -lutil -o $@

$(EXAMPLE_BIN): examples/bounce.c $(STATIC_LIB) | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $< $(STATIC_LIB) $(LDFLAGS) $(LDLIBS) -o $@

test: $(TEST_BIN)
	$(TEST_BIN)

sanitize: | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) -Isrc -std=c11 -O1 -g3 -pthread $(WARNINGS) \
		-fno-omit-frame-pointer -fsanitize=address,undefined \
		src/kitty_framebuffer.c tests/test_framebuffer.c \
		$(LDLIBS) -lutil -fsanitize=address,undefined \
		-o $(BUILD_DIR)/test-framebuffer-sanitize
	ASAN_OPTIONS=detect_leaks=1:allocator_may_return_null=1 \
		$(BUILD_DIR)/test-framebuffer-sanitize

install: all
	$(INSTALL) -d $(DESTDIR)$(PREFIX)/include $(DESTDIR)$(PREFIX)/lib
	$(INSTALL) -m 0644 include/kitty_framebuffer.h $(DESTDIR)$(PREFIX)/include/
	$(INSTALL) -m 0644 $(STATIC_LIB) $(SHARED_LIB) $(DESTDIR)$(PREFIX)/lib/

clean:
	rm -rf $(BUILD_DIR)
