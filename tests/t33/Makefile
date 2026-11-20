# T33 host tests (#391): HERA v1.6 H.264 and H.265 command lists vs vendor libimp 2.0.2.1; software OSD (#675)
#   make -C tests/t33 check
CC ?= cc
CFLAGS ?= -std=c99 -O2 -Wall -Wextra -Werror
PROJECT_DIR := ../..
BUILD := $(PROJECT_DIR)/build/tests
INC := -DPLATFORM_T32 -DPLATFORM_T33 -I$(PROJECT_DIR)/include -I$(PROJECT_DIR)/src
HERA_BIN := $(BUILD)/t33-hera-list-test
HERA_SRC := hera_list_test.c $(PROJECT_DIR)/src/t32/openimp_t33_hera.c $(PROJECT_DIR)/src/t32/openimp_t33_hera_h265.c \
	$(PROJECT_DIR)/src/t30/radix/t30_radix_cabac.c $(PROJECT_DIR)/src/t32/openimp_t32_hera.c \
	$(PROJECT_DIR)/src/t30/h264enc/cabac.c $(PROJECT_DIR)/src/t30/h264enc/common.c
HERA_DEPS := $(HERA_SRC) $(PROJECT_DIR)/src/t32/openimp_t33_hera.h $(PROJECT_DIR)/src/t32/openimp_t33_hera_h265.h \
	$(PROJECT_DIR)/src/t32/openimp_t33_hera_h265_tab.h $(PROJECT_DIR)/src/t32/openimp_t33_hera_tab.h \
	$(PROJECT_DIR)/src/t32/openimp_t32_hera.h

OSD_BIN := $(BUILD)/t33-osd-sw-test
OSD_SRC := osd_sw_test.c $(PROJECT_DIR)/src/t32/openimp_t33_osd_sw.c

.PHONY: check
check: $(HERA_BIN) $(OSD_BIN)
	gzip -dc vendor-2.0.2.1-hera-h264.txt.gz vendor-2.0.2.1-hera-h265.txt.gz > $(BUILD)/t33-hera-lists.txt
	$(HERA_BIN) $(BUILD)/t33-hera-lists.txt
	gzip -dc vendor-2.0.2.1-osd-sw.txt.gz > $(BUILD)/t33-osd-sw.txt
	$(OSD_BIN) $(BUILD)/t33-osd-sw.txt

# software OSD (#675) vs vendor libimp 2.0.2.1 (tools/t33_osd_oracle.py)
$(OSD_BIN): $(OSD_SRC) $(PROJECT_DIR)/src/t32/openimp_t33_osd_sw.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $(INC) -o $@ $(OSD_SRC)

$(HERA_BIN): $(HERA_DEPS)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $(INC) -o $@ $(HERA_SRC) -lm
