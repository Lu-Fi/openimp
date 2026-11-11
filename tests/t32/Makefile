# T32 ioctl ABI table (#287): host asserts, trace-reference check and the
# optional cross-check against open-tx-isp driver/t32/tx_isp_t32_abi.h
#   make -C tests/t32 check
#   make -C tests/t32 check-drv [OPEN_TX_ISP=<checkout>] [DRV_REF=<ref>] | [DRV_T32_DIR=<dir>]
CC ?= cc
CFLAGS ?= -std=c99 -O2 -Wall -Wextra -Werror
PROJECT_DIR := ../..
BUILD := $(PROJECT_DIR)/build/tests
TEST_BIN := $(BUILD)/t32-ioctl-abi-test
HERA_BIN := $(BUILD)/t32-hera-list-test
HERA_SRC := hera_list_test.c $(PROJECT_DIR)/src/t32/openimp_t32_hera.c $(PROJECT_DIR)/src/t32/openimp_t32_h264.c \
	$(PROJECT_DIR)/src/t30/h264enc/cabac.c $(PROJECT_DIR)/src/t30/h264enc/common.c \
	$(PROJECT_DIR)/src/t32/openimp_t32_hera_h265.c $(PROJECT_DIR)/src/t32/openimp_t32_h265.c \
	$(PROJECT_DIR)/src/t30/radix/t30_radix_hevc.c $(PROJECT_DIR)/src/t30/radix/t30_radix_cabac.c
HERA_DEPS := $(HERA_SRC) $(PROJECT_DIR)/src/t32/openimp_t32_hera.h $(PROJECT_DIR)/src/t32/openimp_t32_hera_tab.h \
	$(PROJECT_DIR)/src/t32/openimp_t32_h264.h $(PROJECT_DIR)/src/t32/openimp_t32_h265.h \
	$(PROJECT_DIR)/src/t32/openimp_t32_hera_h265.h $(PROJECT_DIR)/src/t32/openimp_t32_hera_h265_tab.h
# rate control (#295): attribute mapping, closed loop, vendor 2.2.0 sequences
RC_BIN := $(BUILD)/t32-rc-test
RC_SRC := rc_test.c $(PROJECT_DIR)/src/t32/openimp_t32_rc.c $(PROJECT_DIR)/src/eprc/eprc.c \
	$(PROJECT_DIR)/src/eprc/eprc_mbrc.c
RC_DEPS := $(RC_SRC) $(PROJECT_DIR)/src/t32/openimp_t32_rc.h $(PROJECT_DIR)/src/eprc/eprc.h \
	$(PROJECT_DIR)/src/eprc/eprc_internal.h
DRV_BIN := $(BUILD)/t32-ioctl-abi-drv-test
INC := -DPLATFORM_T32 -I$(PROJECT_DIR)/include -I$(PROJECT_DIR)/src
DEPS := ioctl_abi_test.c $(PROJECT_DIR)/src/t32/openimp_t32_ioctl.h
OPEN_TX_ISP ?= $(PROJECT_DIR)/../open-tx-isp
DRV_REF ?= origin/claude/t32-ds-batch
DRV_EXTRACT := $(BUILD)/t32-drv-abi

# OpenIMP ISP lifecycle (#288) incl. the IQ-bin loader (#289) = vendor plus the
# shared allocator's rmem cache write-back when the ISP memory is freed; the
# day/night bin run also ignores vendor EnableTuning's isp-m0 request (later issue)
# #302: compile-time IMPEncoderProfile check, 32-bit layout (MIPS o32 aligns int64 to 8)
PROFILE_FLAGS := -m32 -malign-double -ffreestanding -fsyntax-only -std=gnu99 -Wall -Wextra -Werror
OI_ISP_PENDING := rmem
OI_BIN_PENDING := rmem,DEFAULT_CMD_ISP_TUNING
# FrameSource (#290): + the /dev/rmem cache invalidate before each depth copy
OI_FS_PENDING := rmem,DEFAULT_CMD_ISP_TUNING
# soc_vpu channel layer (#291) + JPEG encoder (#292): the VPU part equals the vendor's;
# + the /dev/rmem write-back of the new stream buffers; JPEG job register image
OI_ENC_PENDING := rmem,DEFAULT_CMD_ISP_TUNING

.PHONY: check check-drv
check: $(TEST_BIN) $(HERA_BIN) $(RC_BIN)
	$(TEST_BIN)
	$(RC_BIN) vendor-2.2.0-rc-h264.txt
	# H.264 (#293) and H.265 (#294) on HERA: command lists + parameter sets / slice
	# headers vs vendor 2.2.0
	gzip -dc vendor-2.2.0-hera-h264.txt.gz vendor-2.2.0-hera-h265.txt.gz > $(BUILD)/t32-hera-lists.txt
	$(HERA_BIN) $(BUILD)/t32-hera-lists.txt vendor-2.2.0-h264-headers.txt vendor-2.2.0-h265-headers.txt
	for p in T32 T31 T41 T23; do $(CC) $(PROFILE_FLAGS) -DPLATFORM_$$p -I$(PROJECT_DIR)/include \
		encoder_profile_test.c || exit 1; done; echo "encoder profile numbering (T32 2.x, T31/T41/T23): ok"
	python3 $(PROJECT_DIR)/tools/t32/t32_ioctl_seq.py check vendor-2.2.0-ioctl-seq-*.txt
	python3 $(PROJECT_DIR)/tools/t32/t32_ioctl_seq.py diff vendor-2.2.0-ioctl-seq-isp.txt \
		openimp-ioctl-seq-isp.txt --ignore $(OI_ISP_PENDING)
	python3 $(PROJECT_DIR)/tools/t32/t32_ioctl_seq.py diff vendor-2.2.0-ioctl-seq-bin.txt \
		openimp-ioctl-seq-bin.txt --ignore $(OI_BIN_PENDING)
	for s in fs fs3; do python3 $(PROJECT_DIR)/tools/t32/t32_ioctl_seq.py diff vendor-2.2.0-ioctl-seq-$$s.txt \
		openimp-ioctl-seq-$$s.txt --ignore $(OI_FS_PENDING) || exit 1; done
	for s in vpu jpeg h264 h265; do python3 $(PROJECT_DIR)/tools/t32/t32_ioctl_seq.py diff vendor-2.2.0-ioctl-seq-$$s.txt \
		openimp-ioctl-seq-$$s.txt --ignore $(OI_ENC_PENDING) || exit 1; done
	grep -v '^#' vendor-2.2.0-jpeg-job.txt > $(BUILD)/t32-jpeg-job-vendor.txt
	grep -v '^#' openimp-jpeg-job.txt > $(BUILD)/t32-jpeg-job-openimp.txt
	diff $(BUILD)/t32-jpeg-job-vendor.txt $(BUILD)/t32-jpeg-job-openimp.txt && \
		echo "JPEG job register image (jpegQp -1/30/80): identical"
	# ISP tuning (#296): id, direction, VI and payload of every IMP_ISP_Tuning_*
	# request and the return codes, vendor 2.2.0 vs OpenIMP
	python3 $(PROJECT_DIR)/tools/t32/t32_tuning_table.py diff vendor-2.2.0-tuning.txt openimp-tuning.txt --rc

$(HERA_BIN): $(HERA_DEPS)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $(INC) -o $@ $(HERA_SRC) -lm

# host build: the 32-bit stream ABI asserts of imp_encoder.h do not hold on
# a 64-bit host (the rate-control attributes hold no pointers)
$(RC_BIN): $(RC_DEPS)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -std=gnu99 -ffp-contract=off -DPLATFORM_T32 '-D_Static_assert(c,m)=' -I$(PROJECT_DIR)/include \
		-I$(PROJECT_DIR)/src -o $@ $(RC_SRC) -lm

$(TEST_BIN): $(DEPS)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $(INC) -o $@ ioctl_abi_test.c

check-drv:
	@mkdir -p $(BUILD)
	@if [ -n "$(DRV_T32_DIR)" ]; then dir="$(DRV_T32_DIR)"; \
	elif git -C "$(OPEN_TX_ISP)" rev-parse -q --verify "$(DRV_REF)" >/dev/null 2>&1; then \
		dir="$(DRV_EXTRACT)"; mkdir -p "$$dir"; \
		for f in tx_isp_t32_abi.h tx_isp_t32_abi_attr.h; do \
			git -C "$(OPEN_TX_ISP)" show "$(DRV_REF):driver/t32/$$f" > "$$dir/$$f" || exit 1; done; \
	else echo "check-drv: skipped (no DRV_T32_DIR, no $(DRV_REF) in $(OPEN_TX_ISP))"; exit 0; fi; \
	$(CC) $(CFLAGS) $(INC) -DOI_T32_DRV_CROSSCHECK -I"$$dir" -o $(DRV_BIN) ioctl_abi_test.c && $(DRV_BIN)
