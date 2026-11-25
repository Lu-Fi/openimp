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

YUV_BIN := $(BUILD)/t33-yuv-rc-test

# #784: crop window and H.264 chroma QP offset in headers and HERA list vs vendor 2.0.2.1
HDR_BIN := $(BUILD)/t33-hdr784-test
HDR_SRC := hdr784_test.c $(PROJECT_DIR)/src/t32/openimp_t32_h264.c $(PROJECT_DIR)/src/t32/openimp_t32_h265.c \
	$(PROJECT_DIR)/src/t30/radix/t30_radix_hevc.c $(PROJECT_DIR)/src/t32/openimp_t33_hera.c \
	$(PROJECT_DIR)/src/t32/openimp_t32_hera.c $(PROJECT_DIR)/src/t30/radix/t30_radix_cabac.c \
	$(PROJECT_DIR)/src/t30/h264enc/cabac.c $(PROJECT_DIR)/src/t30/h264enc/common.c

# #784: intra refresh (GDR) picture types and the controller's MB flag map vs vendor 2.0.2.1 (QEMU)
GDR_BIN := $(BUILD)/t33-gdr784-test
GDR_SRC := gdr784_test.c $(PROJECT_DIR)/src/t32/openimp_t32_rc.c $(PROJECT_DIR)/src/eprc/eprc.c \
	$(PROJECT_DIR)/src/eprc/eprc_mbrc.c $(PROJECT_DIR)/src/t32/openimp_t32_rc33.c \
	$(PROJECT_DIR)/src/eprc/eprc_t33_h264.c $(PROJECT_DIR)/src/eprc/eprc_t33_h265.c

# #784: P-skip threshold and the request's slice block vs vendor 2.0.2.1 (QEMU vps2 / ops2)
PSK_BIN := $(BUILD)/t33-pskip784-test
RC_BIN := $(BUILD)/t33-rc784-test
PSK_SRC := pskip784_test.c $(PROJECT_DIR)/src/t32/openimp_t32_rc.c $(PROJECT_DIR)/src/eprc/eprc.c \
	$(PROJECT_DIR)/src/eprc/eprc_mbrc.c $(PROJECT_DIR)/src/t32/openimp_t32_rc33.c \
	$(PROJECT_DIR)/src/eprc/eprc_t33_h264.c $(PROJECT_DIR)/src/eprc/eprc_t33_h265.c
RC_SRC := rc784_test.c $(filter-out pskip784_test.c,$(PSK_SRC))

# #793: the H.265 controller's skipped picture and the list bits it moves vs vendor 2.0.2.1 (QEMU v5pss)
PSK5_BIN := $(BUILD)/t33-pskip793-test
PSK5_SRC := pskip793_test.c $(filter-out pskip784_test.c,$(PSK_SRC)) $(filter-out hera_list_test.c,$(HERA_SRC))

# #784 group 3: multi-section / bitstream pools and the lists without BSFULL vs vendor 2.0.2.1 (QEMU vms0 / vjm0)
MS_BIN := $(BUILD)/t33-ms784-test
YP_BIN := $(BUILD)/t33-yuvp904-test
YP_SRC := yuvp904_test.c $(filter-out hera_list_test.c,$(HERA_SRC))
MS_SRC := ms784_test.c $(filter-out hera_list_test.c,$(HERA_SRC))

.PHONY: check
# #805: the custom AE / AWB workers ask again on EAGAIN / EINTR (blocking open-tx-isp get), leave on other errors
ALGO_BIN := $(BUILD)/t33-algo805-test
ALGO_SRC := algo805_test.c $(PROJECT_DIR)/src/t40/openimp_p1_algo.c algo805_inc/imp/imp_isp.h

check: $(HERA_BIN) $(OSD_BIN) $(YUV_BIN) $(HDR_BIN) $(GDR_BIN) $(PSK_BIN) $(PSK5_BIN) $(RC_BIN) $(MS_BIN) $(YP_BIN) $(ALGO_BIN)
	$(ALGO_BIN)
	$(YUV_BIN)
	$(HDR_BIN)
	$(GDR_BIN)
	$(PSK_BIN)
	$(PSK5_BIN)
	$(RC_BIN)
	$(MS_BIN)
	$(YP_BIN)
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

$(HDR_BIN): $(HDR_SRC) $(PROJECT_DIR)/src/t32/openimp_t32_h264.h $(PROJECT_DIR)/src/t32/openimp_t32_h265.h \
	$(PROJECT_DIR)/src/t32/openimp_t32_hera.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $(INC) -o $@ $(HDR_SRC) -lm

$(PSK_BIN): $(PSK_SRC) $(PROJECT_DIR)/src/t32/openimp_t32_pskip.h $(PROJECT_DIR)/src/t32/openimp_t32_rc.h \
	$(PROJECT_DIR)/src/t32/openimp_t32_rc33.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -std=gnu99 -ffp-contract=off '-D_Static_assert(c,m)=' $(INC) -o $@ $(PSK_SRC) -lm

$(PSK5_BIN): $(PSK5_SRC) $(HERA_DEPS) $(PROJECT_DIR)/src/t32/openimp_t32_rc.h $(PROJECT_DIR)/src/t32/openimp_t32_rc33.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -std=gnu99 -ffp-contract=off '-D_Static_assert(c,m)=' $(INC) -o $@ $(PSK5_SRC) -lm

$(RC_BIN): $(RC_SRC) $(PROJECT_DIR)/src/t32/openimp_t32_rc.h $(PROJECT_DIR)/src/t32/openimp_t32_rc33.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -std=gnu99 -ffp-contract=off '-D_Static_assert(c,m)=' $(INC) -o $@ $(RC_SRC) -lm

$(GDR_BIN): $(GDR_SRC) $(PROJECT_DIR)/src/t32/openimp_t32_gdr.h $(PROJECT_DIR)/src/t32/openimp_t32_rc.h \
	$(PROJECT_DIR)/src/t32/openimp_t32_rc33.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -std=gnu99 -ffp-contract=off '-D_Static_assert(c,m)=' $(INC) -o $@ $(GDR_SRC) -lm

# stand-alone YUV / JPEG encoder (#752): vendor argument checks, return codes, parameter maps
$(YUV_BIN): yuv_rc_test.c $(PROJECT_DIR)/src/t32/openimp_t32_yuvrc.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $(INC) -o $@ yuv_rc_test.c

$(MS_BIN): $(MS_SRC) $(HERA_DEPS) $(PROJECT_DIR)/src/t32/openimp_t33_sections.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $(INC) -o $@ $(MS_SRC) -lm

$(YP_BIN): $(YP_SRC) $(HERA_DEPS) $(PROJECT_DIR)/src/t32/openimp_t33_sections.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $(INC) -o $@ $(YP_SRC) -lm

$(ALGO_BIN): $(ALGO_SRC)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -DPLATFORM_T32 -DPLATFORM_T33 -Ialgo805_inc -I$(PROJECT_DIR)/include -I$(PROJECT_DIR)/src algo805_test.c -lpthread -o $@
