# Compile-only layout check: the public headers hold pointer structs whose
# asserts need a 32-bit target, so use a mipsel (or any 32-bit) compiler:
#   make -C tests/t20 check T20_CC=/path/to/mipsel-linux-gcc
# (make presets CC=cc, so a ?= on CC never took effect)
T20_CC ?= mipsel-linux-gcc
CFLAGS ?= -std=gnu99 -O2 -Wall -Wextra -Werror -mabi=32 -march=mips32r2
PROJECT_DIR := ../..
BUILD := $(PROJECT_DIR)/build/tests
OBJ := $(BUILD)/t20-abi-test.o

.PHONY: check clean

ifeq ($(shell command -v $(T20_CC) 2>/dev/null),)
check:
	@echo "t20_abi_test: skipped ($(T20_CC) not found; set T20_CC to a 32-bit compiler)"
else
check: $(OBJ)
	@echo t20_abi_test: ok
endif

$(OBJ): t20_abi_test.c $(PROJECT_DIR)/include/imp/imp_encoder.h \
		$(PROJECT_DIR)/include/imp/imp_isp.h
	@mkdir -p $(BUILD)
	$(T20_CC) $(CFLAGS) -c -DPLATFORM_T20 -DPLATFORM_T21 -I$(PROJECT_DIR)/include -o $@ t20_abi_test.c

clean:
	$(RM) $(OBJ)
