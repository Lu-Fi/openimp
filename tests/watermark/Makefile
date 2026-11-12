CC ?= cc
CFLAGS ?= -std=gnu99 -O2 -Wall -Wextra -Werror
PROJECT_DIR := ../..
BUILD := $(PROJECT_DIR)/build/tests
WATERMARK_TEST := $(BUILD)/watermark-test
SRC := $(PROJECT_DIR)/src/watermark/watermark.c $(PROJECT_DIR)/src/watermark/watermark.h

.PHONY: check clean

check: $(WATERMARK_TEST)
	$(WATERMARK_TEST) watermark_vectors.txt

$(WATERMARK_TEST): watermark_test.c $(SRC)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -I$(PROJECT_DIR)/src -o $@ watermark_test.c $(PROJECT_DIR)/src/watermark/watermark.c

clean:
	$(RM) $(WATERMARK_TEST)
