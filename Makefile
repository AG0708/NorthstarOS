SHELL := /bin/sh

BUILD_DIR ?= build
ARTIFACT_DIR ?= artifacts
CROSS ?= x86_64-elf-
CC := $(CROSS)gcc
LD := $(CROSS)ld
OBJCOPY := $(CROSS)objcopy
NM := $(CROSS)nm
NASM ?= nasm
PYTHON ?= python3
QEMU ?= qemu-system-x86_64
NORTHSTAR_ENABLE_M3 ?= 1
NORTHSTAR_ENABLE_M4 ?= 1
NORTHSTAR_ENABLE_G5 ?= 1
NORTHSTAR_ENABLE_M5 ?= 1
NORTHSTAR_INTERACTIVE ?= 0

CPPFLAGS := -Iinclude -I$(BUILD_DIR)/generated \
	-DNORTHSTAR_ENABLE_M3=$(NORTHSTAR_ENABLE_M3) \
	-DNORTHSTAR_ENABLE_M4=$(NORTHSTAR_ENABLE_M4) \
	-DNORTHSTAR_ENABLE_G5=$(NORTHSTAR_ENABLE_G5) \
	-DNORTHSTAR_ENABLE_M5=$(NORTHSTAR_ENABLE_M5) \
	-DNORTHSTAR_INTERACTIVE=$(NORTHSTAR_INTERACTIVE)
CFLAGS := -std=c11 -O2 -g -ffreestanding -fno-builtin -fno-pie \
	-fno-stack-protector -fno-asynchronous-unwind-tables -fno-unwind-tables \
	-mno-red-zone -mcmodel=kernel -mgeneral-regs-only \
	-Wall -Wextra -Wpedantic -Werror -MMD -MP
LDFLAGS := -nostdlib --build-id=none -z max-page-size=0x1000 -z noexecstack \
	-T kernel/linker.ld

KERNEL_C_SRCS := \
	kernel/core/main.c \
	kernel/core/m2.c \
	kernel/core/m3.c \
	kernel/core/m4.c \
	kernel/core/console.c \
	kernel/core/sha256.c \
	kernel/core/string.c \
	kernel/proc/scheduler.c
KERNEL_ASM_SRCS := kernel/core/entry.asm kernel/core/m2_guard.asm
HOST_TEST_SRCS := tests/host/test_sha256.c
HOST_SCRIPT_TESTS :=

BOOT_LAYOUT_DIR := $(BUILD_DIR)/generated
BOOT_LAYOUT_INC := $(BOOT_LAYOUT_DIR)/boot_layout.inc
BOOT_LAYOUT_JSON := $(BOOT_LAYOUT_DIR)/boot_layout.json
BOOT_LAYOUT_STAMP := $(BOOT_LAYOUT_DIR)/.layout.stamp

# M1 boot and architecture sources are part of the bootable vertical slice.
-include build/boot_memory.mk
-include build/storage_fs.mk
-include build/process_userland.mk
-include build/networking.mk
-include build/verification.mk

RUN_REPRODUCIBILITY = $(PYTHON) tools/check_reproducible.py \
	--source . \
	--command "$(MAKE) all CROSS=$(CROSS)" \
	--source-date-epoch "$(SOURCE_DATE_EPOCH)" \
	--artifacts-dir $(ARTIFACT_DIR)/reproducibility

KERNEL_OBJ_DIR := $(BUILD_DIR)/obj/m3-$(NORTHSTAR_ENABLE_M3)-m4-$(NORTHSTAR_ENABLE_M4)-g5-$(NORTHSTAR_ENABLE_G5)-m5-$(NORTHSTAR_ENABLE_M5)-interactive-$(NORTHSTAR_INTERACTIVE)
KERNEL_C_OBJS := $(sort $(patsubst %.c,$(KERNEL_OBJ_DIR)/%.o,$(filter %.c,$(KERNEL_C_SRCS))))
KERNEL_ASM_OBJS := $(sort $(patsubst %.asm,$(KERNEL_OBJ_DIR)/%.o,$(filter %.asm,$(KERNEL_ASM_SRCS))))
KERNEL_S_OBJS := $(sort $(patsubst %.S,$(KERNEL_OBJ_DIR)/%.o,$(filter %.S,$(KERNEL_ASM_SRCS))))
KERNEL_OBJS := $(KERNEL_ASM_OBJS) $(KERNEL_S_OBJS) $(KERNEL_C_OBJS)
KERNEL_DEPS := $(KERNEL_C_OBJS:.o=.d)

.PHONY: all image interactive-image run test-interactive test-host test-boot test-network test-journal test-repeat test release release-evidence reproducibility clean toolchain-check

all: image

image: $(BUILD_DIR)/northstar.img

interactive-image:
	$(MAKE) BUILD_DIR=build-interactive ARTIFACT_DIR=$(ARTIFACT_DIR) \
		NORTHSTAR_INTERACTIVE=1 NORTHSTAR_ENABLE_M5=0 image

run: interactive-image
	@mkdir -p "$(ARTIFACT_DIR)"
	@test -f "$(ARTIFACT_DIR)/interactive.img" || \
		cp build-interactive/northstar.img "$(ARTIFACT_DIR)/interactive.img"
	$(QEMU) -machine pc-i440fx-7.2 -accel tcg,thread=single -cpu qemu64 \
		-m 256 -smp 1 -display none -monitor none -serial stdio \
		-no-reboot -rtc base=2000-01-01T00:00:00,clock=vm -boot c \
		-drive file=$(ARTIFACT_DIR)/interactive.img,format=raw,if=ide,index=0,media=disk,snapshot=off,cache=writeback \
		-nic none

test-interactive: interactive-image image
	$(PYTHON) tools/test_interactive.py \
		--image build-interactive/northstar.img \
		--canonical-image $(OS_IMAGE) \
		--artifacts-dir $(ARTIFACT_DIR)/interactive

toolchain-check:
	@command -v "$(CC)" >/dev/null || { echo "missing cross compiler: $(CC)" >&2; exit 2; }
	@command -v "$(LD)" >/dev/null || { echo "missing cross linker: $(LD)" >&2; exit 2; }
	@command -v "$(OBJCOPY)" >/dev/null || { echo "missing cross objcopy: $(OBJCOPY)" >&2; exit 2; }
	@command -v "$(NASM)" >/dev/null || { echo "missing assembler: $(NASM)" >&2; exit 2; }
	@command -v "$(PYTHON)" >/dev/null || { echo "missing Python: $(PYTHON)" >&2; exit 2; }

$(KERNEL_OBJ_DIR)/%.o: %.c
	@mkdir -p "$(dir $@)"
	$(CC) $(CPPFLAGS) $(CFLAGS) -c "$<" -o "$@"

$(KERNEL_OBJ_DIR)/%.o: %.asm
	@mkdir -p "$(dir $@)"
	$(NASM) -f elf64 -I$(BOOT_LAYOUT_DIR)/ -Iboot/ -o "$@" "$<"

$(KERNEL_OBJ_DIR)/%.o: %.S
	@mkdir -p "$(dir $@)"
	$(CC) $(CPPFLAGS) $(CFLAGS) -c "$<" -o "$@"

$(BUILD_DIR)/kernel.elf: $(KERNEL_OBJS) kernel/linker.ld
	@mkdir -p "$(dir $@)"
	$(LD) $(LDFLAGS) -Map=$(BUILD_DIR)/kernel.map -o "$@" $(KERNEL_OBJS)

$(BUILD_DIR)/kernel.bin: $(BUILD_DIR)/kernel.elf
	$(OBJCOPY) -O binary "$<" "$@"
	@test -s "$@"

$(BOOT_LAYOUT_STAMP): $(BUILD_DIR)/kernel.bin $(BUILD_DIR)/kernel.elf \
		$(USER_INITRAMFS) tools/gen_image_layout.py
	@mkdir -p "$(BOOT_LAYOUT_DIR)"
	$(GEN_BOOT_LAYOUT)
	@touch "$@"

$(BOOT_LAYOUT_INC) $(BOOT_LAYOUT_JSON): $(BOOT_LAYOUT_STAMP)

$(BUILD_DIR)/northstar.img: $(BUILD_DIR)/stage1.bin $(BUILD_DIR)/stage2.bin \
		$(BUILD_DIR)/kernel.bin $(USER_INITRAMFS) $(BOOT_LAYOUT_JSON) tools/build_image.py
	$(BUILD_RAW_IMAGE)

test-host:
	$(RUN_PYTHON_HOST_VERIFICATION)
	$(RUN_NATIVE_HOST_VERIFICATION)

test-boot: image
	$(RUN_BOOT_VERIFICATION)

test-network: image
	$(PYTHON) tools/run_integration.py --image $(OS_IMAGE) \
		--scenario m5_network_interop \
		--artifacts-dir $(ARTIFACT_DIR)/network

test-journal: image
	$(PYTHON) tools/run_journal_matrix.py --image $(OS_IMAGE) \
		--artifacts-dir $(ARTIFACT_DIR)/journal-matrix

test-repeat: image
	$(PYTHON) tools/run_integration.py --image $(OS_IMAGE) \
		--milestone M5 --repeat 100 --keep-going \
		--artifacts-dir $(ARTIFACT_DIR)/repetition

test: test-host test-boot

release: image interactive-image
	$(PYTHON) tools/run_release_gate.py \
		--image $(OS_IMAGE) \
		--interactive-image build-interactive/northstar.img \
		--artifacts-dir $(ARTIFACT_DIR)/release \
		--cross $(CROSS) \
		--qemu $(QEMU)

release-evidence: release

reproducibility:
	$(RUN_REPRODUCIBILITY)

clean:
	@if test -d "$(BUILD_DIR)"; then \
		find "$(BUILD_DIR)" -mindepth 1 ! -name '*.mk' -exec rm -rf {} +; \
	fi
	@rm -rf "$(ARTIFACT_DIR)"

-include $(KERNEL_DEPS)
