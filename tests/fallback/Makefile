# Host test of the fallback frame logic: make -C tests/fallback check
CC ?= cc
PROJECT_DIR := ../..
BUILD := $(PROJECT_DIR)/build/tests

.PHONY: check clean

check: $(BUILD)/fb_test
	$(BUILD)/fb_test

$(BUILD)/fb_test: fb_test.c $(PROJECT_DIR)/src/openimp_fb_logic.h $(PROJECT_DIR)/include/openimp_fallback.h
	@mkdir -p $(BUILD)
	$(CC) -std=gnu99 -O2 -g -Wall -Wextra -Werror -I$(PROJECT_DIR)/include -I$(PROJECT_DIR)/src -o $@ fb_test.c

clean:
	$(RM) $(BUILD)/fb_test
