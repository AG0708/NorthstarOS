# Canonical verification-tool interface for the root Makefile.
#
# This fragment intentionally owns no public targets.  The root Makefile owns
# `test-host`, `test-boot`, `test`, `reproducibility`, and release orchestration
# so target policy remains visible in one place.

VERIFY_PYTHON ?= python3
ARTIFACT_DIR ?= artifacts
CURRENT_MILESTONE ?= M5
SOURCE_DATE_EPOCH ?= 1700000000
export SOURCE_DATE_EPOCH

BOOT_LAYOUT_DIR ?= $(BUILD_DIR)/generated
BOOT_LAYOUT_JSON ?= $(BOOT_LAYOUT_DIR)/boot_layout.json
IMAGE_LAYOUT_JSON ?= $(BUILD_DIR)/image-layout.json
OS_IMAGE ?= $(BUILD_DIR)/northstar.img

GEN_BOOT_LAYOUT = $(VERIFY_PYTHON) tools/gen_image_layout.py \
	--kernel $(BUILD_DIR)/kernel.bin \
	--kernel-elf $(BUILD_DIR)/kernel.elf \
	--nm $(CROSS)nm \
	--initrd $(USER_INITRAMFS) \
	--output-dir $(BOOT_LAYOUT_DIR)

BUILD_RAW_IMAGE = $(VERIFY_PYTHON) tools/build_image.py \
	--stage1 $(BUILD_DIR)/stage1.bin \
	--stage2 $(BUILD_DIR)/stage2.bin \
	--kernel $(BUILD_DIR)/kernel.bin \
	--initrd $(USER_INITRAMFS) \
	--layout $(BOOT_LAYOUT_JSON) \
	--output $(OS_IMAGE) \
	--manifest $(IMAGE_LAYOUT_JSON)

RUN_PYTHON_HOST_VERIFICATION = $(VERIFY_PYTHON) -m unittest discover \
	-s tests/host -p 'test_*.py'

RUN_NATIVE_HOST_VERIFICATION = $(VERIFY_PYTHON) tools/run_host_tests.py \
	--artifacts-dir $(ARTIFACT_DIR)/host

# Backward-compatible alias used by the root Makefile for the Python lane.
RUN_HOST_VERIFICATION = $(RUN_PYTHON_HOST_VERIFICATION)

RUN_BOOT_VERIFICATION = $(VERIFY_PYTHON) tools/run_integration.py \
	--image $(OS_IMAGE) \
	--milestone $(CURRENT_MILESTONE) \
	--artifacts-dir $(ARTIFACT_DIR)/integration

RUN_PROGRESSIVE_VERIFICATION = $(VERIFY_PYTHON) tools/run_integration.py \
	--image $(OS_IMAGE) \
	--milestone $(CURRENT_MILESTONE) \
	--artifacts-dir $(ARTIFACT_DIR)/integration

RUN_REPRODUCIBILITY = $(VERIFY_PYTHON) tools/check_reproducible.py \
	--source . \
	--artifacts-dir $(ARTIFACT_DIR)/reproducibility

GENERATE_RELEASE_EVIDENCE = $(VERIFY_PYTHON) tools/gen_release_evidence.py \
	--source . \
	--image $(OS_IMAGE) \
	--image-manifest $(IMAGE_LAYOUT_JSON) \
	--boot-layout $(BOOT_LAYOUT_JSON) \
	--output $(ARTIFACT_DIR)/release/evidence.json
