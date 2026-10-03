CC ?= cc
CFLAGS ?= -std=gnu99 -O2 -Wall -Wextra -Werror
PROJECT_DIR := ../..
BUILD := $(PROJECT_DIR)/build/tests
SRC := $(PROJECT_DIR)/src/isp/openimp_caps.c
HDR := $(PROJECT_DIR)/include/openimp/openimp_caps.h
PLATFORMS := t20 t21 t23 t30 t31 t40 t41
DEF_t20 := -DPLATFORM_T20 -DPLATFORM_T21
DEF_t21 := -DPLATFORM_T21
DEF_t23 := -DPLATFORM_T23
DEF_t30 := -DPLATFORM_T30
DEF_t31 := -DPLATFORM_T31
DEF_t40 := -DPLATFORM_T40
DEF_t41 := -DPLATFORM_T41
BINS := $(foreach p,$(PLATFORMS),$(BUILD)/caps-test-$(p))

.PHONY: check clean

check: $(BINS)
	@for p in $(PLATFORMS); do $(BUILD)/caps-test-$$p $$p || exit 1; done

$(BUILD)/caps-test-%: caps_test.c $(SRC) $(HDR)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $(DEF_$*) -DOPENIMP_CAPS_TEST -I$(PROJECT_DIR)/include \
		-o $@ caps_test.c $(SRC)

clean:
	$(RM) $(BINS)
