CC ?= cc
CFLAGS ?= -std=c99 -O2 -Wall -Wextra -Werror
PROJECT_DIR := ../..
STREAM_LAYOUT_TEST_BIN := $(PROJECT_DIR)/build/tests/t40-stream-layout-test

.PHONY: check clean

CAMERA_INPUT_TEST_BIN := $(PROJECT_DIR)/build/tests/t40-camera-input-test

check: $(STREAM_LAYOUT_TEST_BIN) $(CAMERA_INPUT_TEST_BIN)
	$(STREAM_LAYOUT_TEST_BIN)
	$(CAMERA_INPUT_TEST_BIN)

# host-only: private ioctl fixture, no camera
$(CAMERA_INPUT_TEST_BIN): camera_input_test.c $(PROJECT_DIR)/src/t40/openimp_p1.c
	mkdir -p "$(dir $@)"
	$(CC) -std=gnu99 -O2 -Wall -Wextra -Werror -DPLATFORM_T40 \
		-I$(PROJECT_DIR)/src -I$(PROJECT_DIR)/include \
		camera_input_test.c -o "$@"

$(STREAM_LAYOUT_TEST_BIN): stream_layout_test.c \
	$(PROJECT_DIR)/src/t40/t40_stream_layout.c \
	$(PROJECT_DIR)/src/t40/t40_stream_layout.h
	mkdir -p "$(dir $@)"
	$(CC) $(CFLAGS) -I$(PROJECT_DIR)/src \
		stream_layout_test.c \
		$(PROJECT_DIR)/src/t40/t40_stream_layout.c \
		-o "$@"

clean:
	$(RM) $(STREAM_LAYOUT_TEST_BIN) $(CAMERA_INPUT_TEST_BIN)
