# T32 ioctl ABI table (#287): host asserts, trace-reference check and the
# optional cross-check against open-tx-isp driver/t32/tx_isp_t32_abi.h
#   make -C tests/t32 check
#   make -C tests/t32 check-drv [OPEN_TX_ISP=<checkout>] [DRV_REF=<ref>] | [DRV_T32_DIR=<dir>]
CC ?= cc
CFLAGS ?= -std=c99 -O2 -Wall -Wextra -Werror
PROJECT_DIR := ../..
BUILD := $(PROJECT_DIR)/build/tests
TEST_BIN := $(BUILD)/t32-ioctl-abi-test
DRV_BIN := $(BUILD)/t32-ioctl-abi-drv-test
INC := -DPLATFORM_T32 -I$(PROJECT_DIR)/include -I$(PROJECT_DIR)/src
DEPS := ioctl_abi_test.c $(PROJECT_DIR)/src/t32/openimp_t32_ioctl.h
OPEN_TX_ISP ?= $(PROJECT_DIR)/../open-tx-isp
DRV_REF ?= origin/claude/t32-ds-batch
DRV_EXTRACT := $(BUILD)/t32-drv-abi

.PHONY: check check-drv
check: $(TEST_BIN)
	$(TEST_BIN)
	python3 $(PROJECT_DIR)/tools/t32/t32_ioctl_seq.py check vendor-2.2.0-ioctl-seq-*.txt

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
