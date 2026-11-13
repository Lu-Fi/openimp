# T33 host tests (#391): HERA v1.6 H.264 command lists vs vendor libimp 2.0.2.1
#   make -C tests/t33 check
CC ?= cc
CFLAGS ?= -std=c99 -O2 -Wall -Wextra -Werror
PROJECT_DIR := ../..
BUILD := $(PROJECT_DIR)/build/tests
INC := -DPLATFORM_T32 -DPLATFORM_T33 -I$(PROJECT_DIR)/include -I$(PROJECT_DIR)/src
HERA_BIN := $(BUILD)/t33-hera-list-test
HERA_SRC := hera_list_test.c $(PROJECT_DIR)/src/t32/openimp_t33_hera.c $(PROJECT_DIR)/src/t32/openimp_t32_hera.c \
	$(PROJECT_DIR)/src/t30/h264enc/cabac.c $(PROJECT_DIR)/src/t30/h264enc/common.c
HERA_DEPS := $(HERA_SRC) $(PROJECT_DIR)/src/t32/openimp_t33_hera.h $(PROJECT_DIR)/src/t32/openimp_t33_hera_tab.h \
	$(PROJECT_DIR)/src/t32/openimp_t32_hera.h

.PHONY: check
check: $(HERA_BIN)
	gzip -dc vendor-2.0.2.1-hera-h264.txt.gz > $(BUILD)/t33-hera-lists.txt
	$(HERA_BIN) $(BUILD)/t33-hera-lists.txt

$(HERA_BIN): $(HERA_DEPS)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $(INC) -o $@ $(HERA_SRC) -lm
