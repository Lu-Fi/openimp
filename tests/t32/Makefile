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
RC33_SRC := $(PROJECT_DIR)/src/t32/openimp_t32_rc33.c $(PROJECT_DIR)/src/eprc/eprc_t33_h264.c \
	$(PROJECT_DIR)/src/eprc/eprc_t33_h265.c
RC33_HDR := $(PROJECT_DIR)/src/t32/openimp_t32_rc33.h $(PROJECT_DIR)/src/eprc/eprc_t33.h \
	$(PROJECT_DIR)/src/eprc/eprc_t33_mem.h
RC_SRC := rc_test.c $(PROJECT_DIR)/src/t32/openimp_t32_rc.c $(PROJECT_DIR)/src/eprc/eprc.c \
	$(PROJECT_DIR)/src/eprc/eprc_mbrc.c $(RC33_SRC)
RC_DEPS := $(RC_SRC) $(PROJECT_DIR)/src/t32/openimp_t32_rc.h $(PROJECT_DIR)/src/eprc/eprc.h \
	$(PROJECT_DIR)/src/eprc/eprc_internal.h $(RC33_HDR)
# eprc T33 H.264 port (#295): QP of every picture vs the vendor 2.2.0 runs
RC33_BIN := $(BUILD)/t32-rc33-test
# ... and the HERA list words made from its slice block (-l)
RC33_HERA := $(PROJECT_DIR)/src/t32/openimp_t32_hera.c $(PROJECT_DIR)/src/t30/h264enc/cabac.c \
	$(PROJECT_DIR)/src/t30/h264enc/common.c
# IMP attributes -> i264e parameters and SetDefaultParam (#295) vs vendor 2.2.0
RC_IMP_BIN := $(BUILD)/t32-rc-imp-test
# eprc T33 H.265 port + i265e glue (#295): parameters and QP of every picture vs vendor 2.2.0
RC65_BIN := $(BUILD)/t32-rc65-test
# IMP frame rate / GOP glue (#295) vs vendor 2.2.0 encoder_get_fps_mask
GOP33_BIN := $(BUILD)/t32-gop33-test
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

# IMP_Decoder (#301): header parse + engine job of the vendor 2.0.0 decoder
DEC_BIN := $(BUILD)/t32-dec-test
DEC_SRC := dec_test.c $(PROJECT_DIR)/src/t32/openimp_t32_jpegd.c
DEC_DEPS := $(DEC_SRC) $(PROJECT_DIR)/src/t32/openimp_t32_jpegd.h $(PROJECT_DIR)/src/t32/openimp_t32_ioctl.h

ACODEC_BIN := $(BUILD)/t32-acodec-test
ACODEC_SRC := acodec_test.c $(PROJECT_DIR)/src/audio/openimp_audio_enc_dec.c \
	$(PROJECT_DIR)/src/audio/openimp_audio_codec.c

AUDIO_BIN := $(BUILD)/t32-audio2x-test
AUDIO_DEPS := audio2x_test.c $(PROJECT_DIR)/src/t40/openimp_p3_audio.c \
	$(PROJECT_DIR)/src/t32/openimp_t32_audio2x.inc

.PHONY: check check-drv
check: $(TEST_BIN) $(HERA_BIN) $(RC_BIN) $(RC33_BIN) $(RC_IMP_BIN) $(RC65_BIN) $(GOP33_BIN) $(AUDIO_BIN) $(ACODEC_BIN) $(DEC_BIN)
	$(TEST_BIN)
	$(DEC_BIN)
	gzip -dc vendor-2.2.0-gop33.txt.gz | $(GOP33_BIN) /dev/stdin
	$(RC_BIN) vendor-2.2.0-rc-imp-h264.txt
	$(RC33_BIN) vendor-2.2.0-rc-h264.txt vendor-2.2.0-rc33-h264.txt -m 2 vendor-2.2.0-rc33-motion2-h264.txt \
		-r vendor-2.2.0-rc33-rows-h264.txt
	# ... and the HERA list words of the controller's slice block (vendor H264E_T32V_SliceInit)
	gzip -dc vendor-2.2.0-rc33-slice-h264.txt.gz | $(RC33_BIN) -m 2 -r -l /dev/stdin
	# ... and the run-time setters (i264e_set_param 0x11..0x14, taken at the next IDR)
	gzip -dc vendor-2.2.0-rc33-setters-h264.txt.gz | $(RC33_BIN) -m 2 -r /dev/stdin
	gzip -dc vendor-2.2.0-rc-imp.txt.gz | $(RC_IMP_BIN) vendor-2.2.0-rc-imp-defaults.txt /dev/stdin
	# H.265: IMP attribute -> i265e parameters, then the QP of every picture (+ IDR row sizes)
	gzip -dc vendor-2.2.0-rc65-params.txt.gz | $(RC65_BIN) /dev/stdin
	gzip -dc vendor-2.2.0-rc65-h265.txt.gz | $(RC65_BIN) /dev/stdin
	# audio (#329): vendor 2.x software volume, AO block queue, extras
	$(AUDIO_BIN)
	# AENC/ADEC (#298): G.711A/U, G.726, ADPCM bytes == vendor 2.2.0, frame time stamps
	$(ACODEC_BIN)
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
	for s in vpu jpeg h264 h265 ivdc; do python3 $(PROJECT_DIR)/tools/t32/t32_ioctl_seq.py diff vendor-2.2.0-ioctl-seq-$$s.txt \
		openimp-ioctl-seq-$$s.txt --ignore $(OI_ENC_PENDING) || exit 1; done
	# IMP_Decoder (#301): vendor libimp 2.0.0 (QEMU, 2.0.0 helix decode path) vs OpenIMP
	python3 $(PROJECT_DIR)/tools/t32/t32_ioctl_seq.py diff vendor-2.0.0-ioctl-seq-dec.txt \
		openimp-ioctl-seq-dec.txt --ignore rmem
	grep -v '^#' vendor-2.0.0-dec-job.txt > $(BUILD)/t32-dec-job-vendor.txt
	grep -v '^#' openimp-dec-job.txt > $(BUILD)/t32-dec-job-openimp.txt
	diff $(BUILD)/t32-dec-job-vendor.txt $(BUILD)/t32-dec-job-openimp.txt && \
		echo "JPEG decode job register image (3 pictures): identical"
	grep -v '^#' vendor-2.2.0-jpeg-job.txt > $(BUILD)/t32-jpeg-job-vendor.txt
	grep -v '^#' openimp-jpeg-job.txt > $(BUILD)/t32-jpeg-job-openimp.txt
	diff $(BUILD)/t32-jpeg-job-vendor.txt $(BUILD)/t32-jpeg-job-openimp.txt && \
		echo "JPEG job register image (jpegQp -1/30/80): identical"
	# ISP tuning (#296): id, direction, VI and payload of every IMP_ISP_Tuning_*
	# request and the return codes, vendor 2.2.0 vs OpenIMP
	python3 $(PROJECT_DIR)/tools/t32/t32_tuning_table.py diff vendor-2.2.0-tuning.txt openimp-tuning.txt --rc

$(DEC_BIN): $(DEC_DEPS)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $(INC) -o $@ $(DEC_SRC)

$(HERA_BIN): $(HERA_DEPS)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $(INC) -o $@ $(HERA_SRC) -lm

# host build: the 32-bit stream ABI asserts of imp_encoder.h do not hold on
# a 64-bit host (the rate-control attributes hold no pointers)
$(RC_BIN): $(RC_DEPS)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -std=gnu99 -ffp-contract=off -DPLATFORM_T32 '-D_Static_assert(c,m)=' -I$(PROJECT_DIR)/include \
		-I$(PROJECT_DIR)/src -o $@ $(RC_SRC) -lm

$(AUDIO_BIN): $(AUDIO_DEPS)
	@mkdir -p $(BUILD)
	$(CC) -std=gnu99 -O2 -Wall -Wextra -Werror -Wno-unused-function $(INC) -o $@ audio2x_test.c \
		$(PROJECT_DIR)/src/core/imp_log_emit.c -pthread -ldl -lm

$(ACODEC_BIN): $(ACODEC_SRC) $(PROJECT_DIR)/src/audio/openimp_audio_codec.h
	@mkdir -p $(BUILD)
	$(CC) -std=gnu99 -O2 -Wall -Wextra -Werror $(INC) -o $@ $(ACODEC_SRC) \
		$(PROJECT_DIR)/src/core/imp_log_emit.c -pthread -ldl -lm

$(RC_IMP_BIN): rc_imp_test.c $(RC_DEPS)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -std=gnu99 -ffp-contract=off -DPLATFORM_T32 '-D_Static_assert(c,m)=' -I$(PROJECT_DIR)/include \
		-I$(PROJECT_DIR)/src -o $@ rc_imp_test.c $(filter-out rc_test.c,$(RC_SRC)) -lm

$(RC65_BIN): rc65_test.c $(RC_DEPS)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -std=gnu99 -ffp-contract=off -DPLATFORM_T32 '-D_Static_assert(c,m)=' -I$(PROJECT_DIR)/include \
		-I$(PROJECT_DIR)/src -o $@ rc65_test.c $(filter-out rc_test.c,$(RC_SRC)) -lm

$(GOP33_BIN): gop33_test.c $(PROJECT_DIR)/src/t32/t32_gop33.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -I$(PROJECT_DIR)/src -o $@ gop33_test.c

$(RC33_BIN): rc33_test.c $(RC33_SRC) $(RC33_HDR) $(RC33_HERA)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -std=gnu99 -ffp-contract=off -DPLATFORM_T32 -I$(PROJECT_DIR)/include -I$(PROJECT_DIR)/src \
		-o $@ rc33_test.c $(RC33_SRC) $(RC33_HERA) -lm

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
