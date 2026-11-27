# Compile-only layout check: the public headers hold pointer structs whose
# asserts need a 32-bit target, so use a mipsel (or any 32-bit) compiler:
#   make -C tests/t20 check T20_CC=/path/to/mipsel-linux-gcc
# (make presets CC=cc, so a ?= on CC never took effect)
T20_CC ?= mipsel-linux-gcc
CFLAGS ?= -std=gnu99 -O2 -Wall -Wextra -Werror -mabi=32 -march=mips32r2
PROJECT_DIR := ../..
BUILD := $(PROJECT_DIR)/build/tests
OBJ := $(BUILD)/t20-abi-test.o

BYPASS_TEST := $(BUILD)/t20-bypass-test

FLIP_TEST := $(BUILD)/t20-flip-test

DECODER_TEST := $(BUILD)/t20-decoder-test

.PHONY: check clean bypass-check flip-check flip-guard-check flip-guard-check decoder-check onejob-check

ifeq ($(shell command -v $(T20_CC) 2>/dev/null),)
check: bypass-check flip-check flip-guard-check decoder-check onejob-check
	@echo "t20_abi_test: skipped ($(T20_CC) not found; set T20_CC to a 32-bit compiler)"
else
check: $(OBJ) bypass-check flip-check flip-guard-check decoder-check onejob-check
	@echo t20_abi_test: ok
endif

# host test of IMP_ISP_Tuning_SetISPBypass (isp_tseries.c with the 32-bit ABI asserts off, fake ioctl)
bypass-check: $(BYPASS_TEST)
	$(BYPASS_TEST)

$(BYPASS_TEST): bypass_test.c $(PROJECT_DIR)/src/isp/isp_tseries.c
	@mkdir -p $(BUILD)
	$(CC) -std=gnu11 -O2 -Wall -Wextra -Werror -Wno-unused-function -fno-pie -no-pie \
		"-D_Static_assert(a,b)=" -DPLATFORM_T20 -DPLATFORM_T21 -I$(PROJECT_DIR)/src -I$(PROJECT_DIR)/include \
		bypass_test.c $(PROJECT_DIR)/src/isp/isp_tseries.c \
		-Wl,--wrap=ioctl,--unresolved-symbols=ignore-all -lpthread -o $@

# #954 guard (replaces the #939 copy default): T10/T20 Helix JPEG runs one VPU job by default
# (HELIX_ONE_JOB_DEFAULT 1 only on PLATFORM_T20), the encoder lends the capture frame when
# OpenIMP_HelixJpeg_OneJobNext() says one job and copies otherwise; SRC_COPY=1 is opt-in again
onejob-check:
	@mkdir -p $(BUILD)
	@sed -n '/^static int p2_jpeg_lend_wanted/,/^}/p' $(PROJECT_DIR)/src/t40/openimp_p2_encoder.c > $(BUILD)/onejob.txt
	@sed -n '/^static int p2_jpeg_src_copy_forced/,/^}/p' $(PROJECT_DIR)/src/t40/openimp_p2_encoder.c > $(BUILD)/srccopy.txt
	@grep -q 'p2_jpeg_lend_size_ok(width, height)' $(BUILD)/onejob.txt && \
	 grep -q "forced = value && value\[0\] == '1'" $(BUILD)/srccopy.txt && \
	 ! grep -q 'PLATFORM_T20' $(BUILD)/srccopy.txt && \
	 grep -q 'OpenIMP_HelixJpeg_OneJobNext(width, height)' $(PROJECT_DIR)/src/t40/openimp_p2_encoder.c && \
	 grep -A1 '^#if defined(PLATFORM_T20) && !defined(PLATFORM_T23)' $(PROJECT_DIR)/src/t30/helix_jpeg.c | grep -q 'HELIX_ONE_JOB_DEFAULT   1u' && \
	 echo "onejob-check: ok"


# host test of the T10/T20 flips (vendor Temper guard + 120 ms wait, openimp #944)
flip-check: $(FLIP_TEST)
	$(FLIP_TEST)

$(FLIP_TEST): flip_test.c $(PROJECT_DIR)/src/isp/isp_tseries.c
	@mkdir -p $(BUILD)
	$(CC) -std=gnu11 -O2 -Wall -Wextra -Werror -Wno-unused-function -fno-pie -no-pie \
		"-D_Static_assert(a,b)=" -DPLATFORM_T20 -DPLATFORM_T21 -I$(PROJECT_DIR)/src -I$(PROJECT_DIR)/include \
		flip_test.c $(PROJECT_DIR)/src/isp/isp_tseries.c \
		-Wl,--wrap=ioctl,--unresolved-symbols=ignore-all -lpthread -o $@

# host test of IMP_Decoder_* on T10/T20/T21 (src/t23/openimp_t23_decoder.c as OPENIMP_DECODER_SW_ONLY + the software JPEG decoder)
decoder-check: $(DECODER_TEST)
	$(DECODER_TEST)

$(DECODER_TEST): decoder_test.c $(PROJECT_DIR)/src/t23/openimp_t23_decoder.c $(PROJECT_DIR)/src/t23/openimp_t23_jpegdec.c
	@mkdir -p $(BUILD)
	$(CC) -std=gnu11 -O2 -Wall -Wextra -Werror -fno-pie -no-pie "-D_Static_assert(a,b)=" -DPLATFORM_T21 \
		-DOPENIMP_DECODER_SW_ONLY -I$(PROJECT_DIR)/src -I$(PROJECT_DIR)/include \
		decoder_test.c $(PROJECT_DIR)/src/t23/openimp_t23_decoder.c $(PROJECT_DIR)/src/t23/openimp_t23_jpegdec.c \
		-lpthread -o $@

$(OBJ): t20_abi_test.c $(PROJECT_DIR)/include/imp/imp_encoder.h \
		$(PROJECT_DIR)/include/imp/imp_isp.h
	@mkdir -p $(BUILD)
	$(T20_CC) $(CFLAGS) -c -DPLATFORM_T20 -DPLATFORM_T21 -I$(PROJECT_DIR)/include -o $@ t20_abi_test.c

clean:
	$(RM) $(OBJ) $(BYPASS_TEST) $(DECODER_TEST)

# host test of the T21/T30 flips (vendor Temper guard + 120 ms wait, openimp #948)
flip-guard-check: $(BUILD)/flip-guard-t21 $(BUILD)/flip-guard-t30
	$(BUILD)/flip-guard-t21
	$(BUILD)/flip-guard-t30

$(BUILD)/flip-guard-t21 $(BUILD)/flip-guard-t30: $(BUILD)/flip-guard-%: flip_guard_test.c $(PROJECT_DIR)/src/isp/isp_tseries.c
	@mkdir -p $(BUILD)
	$(CC) -std=gnu11 -O2 -Wall -Wextra -Werror -Wno-unused-function -fno-pie -no-pie \
		"-D_Static_assert(a,b)=" -DPLATFORM_$(shell echo $* | tr a-z A-Z) -I$(PROJECT_DIR)/src -I$(PROJECT_DIR)/include \
		flip_guard_test.c $(PROJECT_DIR)/src/isp/isp_tseries.c \
		-Wl,--wrap=ioctl,--unresolved-symbols=ignore-all -lpthread -o $@
