# Boot and early-memory source inventory.  The root Makefile owns toolchain and
# global flag policy; this fragment only contributes sources and boot targets.

KERNEL_C_SRCS += $(sort $(wildcard \
	kernel/arch/x86_64/*.c \
	kernel/mm/*.c))

KERNEL_ASM_SRCS += $(sort $(wildcard \
	kernel/arch/x86_64/*.asm \
	kernel/arch/x86_64/*.S))

HOST_TEST_SRCS += $(sort $(wildcard \
	tests/host/test_boot_*.c \
	tests/host/test_mm_*.c))

BOOT_STAGE1_SRC ?= boot/stage1.asm
BOOT_STAGE2_SRC ?= boot/stage2.asm
BOOT_STAGE1_BIN ?= $(BUILD_DIR)/stage1.bin
BOOT_STAGE2_BIN ?= $(BUILD_DIR)/stage2.bin

# The image build must assemble against metadata generated from the final
# kernel binary.  `boot/boot_layout.inc` remains a direct-NASM bring-up
# fallback, but Make never uses it for a disk image.
BOOT_LAYOUT_INC ?= $(BUILD_DIR)/generated/boot_layout.inc

.PHONY: boot-binaries
boot-binaries: $(BOOT_STAGE1_BIN) $(BOOT_STAGE2_BIN)

$(BOOT_STAGE1_BIN): $(BOOT_STAGE1_SRC) $(BOOT_LAYOUT_STAMP)
	@mkdir -p $(dir $@)
	$(NASM) -f bin -I$(BUILD_DIR)/generated/ -Iboot/ -o $@ $<
	@test "$$(wc -c < $@ | tr -d ' ')" -eq 512

$(BOOT_STAGE2_BIN): $(BOOT_STAGE2_SRC) $(BOOT_LAYOUT_STAMP)
	@mkdir -p $(dir $@)
	$(NASM) -f bin -I$(BUILD_DIR)/generated/ -Iboot/ -o $@ $<
	@test "$$(wc -c < $@ | tr -d ' ')" -le $$((32 * 512))
