CC ?= cc
CFLAGS ?= -std=c99 -O2 -Wall -Wextra -Werror
PROJECT_DIR := ../..
STREAM_LAYOUT_TEST_BIN := $(PROJECT_DIR)/build/tests/t40-stream-layout-test

DENOISE_TEST_BIN := $(PROJECT_DIR)/build/tests/t40-denoise-test

.PHONY: check clean

check: $(STREAM_LAYOUT_TEST_BIN) $(DENOISE_TEST_BIN)
	$(STREAM_LAYOUT_TEST_BIN)
	$(DENOISE_TEST_BIN)

$(STREAM_LAYOUT_TEST_BIN): stream_layout_test.c \
	$(PROJECT_DIR)/src/t40/t40_stream_layout.c \
	$(PROJECT_DIR)/src/t40/t40_stream_layout.h
	mkdir -p "$(dir $@)"
	$(CC) $(CFLAGS) -I$(PROJECT_DIR)/src \
		stream_layout_test.c \
		$(PROJECT_DIR)/src/t40/t40_stream_layout.c \
		-o "$@"

$(DENOISE_TEST_BIN): denoise_test.c $(PROJECT_DIR)/src/t40/p2_denoise.h
	mkdir -p "$(dir $@)"
	$(CC) $(CFLAGS) -I$(PROJECT_DIR)/src denoise_test.c -o "$@"

clean:
	$(RM) $(STREAM_LAYOUT_TEST_BIN) $(DENOISE_TEST_BIN)
