# Process, scheduler, syscall, and freestanding userland inventory.
# The root Makefile owns the toolchain and kernel-wide policy.

KERNEL_C_SRCS += \
	kernel/proc/elf_loader.c \
	kernel/proc/fd.c \
	kernel/proc/image.c \
	kernel/proc/pipe.c \
	kernel/proc/process.c \
	kernel/proc/runtime.c \
	kernel/proc/scheduler.c \
	kernel/proc/syscall.c \
	kernel/proc/usercopy.c

HOST_TEST_SRCS += \
	tests/host/test_proc_elf.c \
	tests/host/test_proc_process.c \
	tests/host/test_proc_scheduler.c \
	tests/host/test_proc_syscall.c \
	tests/host/test_proc_usercopy.c

USER_ROOT_PROGRAMS := init sh
USER_BIN_PROGRAMS := \
	cat \
	cpu_spin \
	echo \
	false \
	fault \
	g5check \
	hexdump \
	ls \
	m3test \
	netcheck \
	privilege_fault \
	pwd \
	sleep \
	sysinfo \
	true \
	wc
USER_PROGRAMS += $(USER_ROOT_PROGRAMS) $(USER_BIN_PROGRAMS)

USER_CPPFLAGS := -Iinclude -Iuser/include
USER_CFLAGS := -std=c11 -O2 -g -ffreestanding -fno-builtin -fno-pie \
	-fno-stack-protector -fno-asynchronous-unwind-tables -fno-unwind-tables \
	-mno-red-zone -mcmodel=small -mgeneral-regs-only \
	-Wall -Wextra -Wpedantic -Werror -MMD -MP
USER_LDFLAGS := -nostdlib --build-id=none -z max-page-size=0x1000 \
	-z noexecstack -T user/linker.ld

USER_OBJ_DIR := $(BUILD_DIR)/user/obj
USER_BIN_DIR := $(BUILD_DIR)/user/bin
USER_COMMON_OBJS := \
	$(USER_OBJ_DIR)/crt0.o \
	$(USER_OBJ_DIR)/lib/libc.o \
	$(USER_OBJ_DIR)/lib/syscalls.o \
	$(USER_OBJ_DIR)/lib/syscall.o
USER_ELFS := $(addprefix $(USER_BIN_DIR)/,$(USER_PROGRAMS))
USER_INITRAMFS := $(BUILD_DIR)/initramfs.cpio

$(USER_OBJ_DIR)/crt0.o: user/crt0.S
	@mkdir -p "$(dir $@)"
	$(CC) $(USER_CPPFLAGS) $(USER_CFLAGS) -c "$<" -o "$@"

$(USER_OBJ_DIR)/lib/%.o: user/lib/%.c
	@mkdir -p "$(dir $@)"
	$(CC) $(USER_CPPFLAGS) $(USER_CFLAGS) -c "$<" -o "$@"

$(USER_OBJ_DIR)/lib/%.o: user/lib/%.S
	@mkdir -p "$(dir $@)"
	$(CC) $(USER_CPPFLAGS) $(USER_CFLAGS) -c "$<" -o "$@"

$(USER_OBJ_DIR)/root/%.o: user/%.c
	@mkdir -p "$(dir $@)"
	$(CC) $(USER_CPPFLAGS) $(USER_CFLAGS) -c "$<" -o "$@"

$(USER_OBJ_DIR)/bin/%.o: user/bin/%.c
	@mkdir -p "$(dir $@)"
	$(CC) $(USER_CPPFLAGS) $(USER_CFLAGS) -c "$<" -o "$@"

$(USER_BIN_DIR)/init: $(USER_COMMON_OBJS) $(USER_OBJ_DIR)/root/init.o user/linker.ld build/process_userland.mk
	@mkdir -p "$(dir $@)"
	$(LD) $(USER_LDFLAGS) -Map="$@.map" -o "$@" \
		$(USER_COMMON_OBJS) $(USER_OBJ_DIR)/root/init.o
	$(OBJCOPY) --strip-debug "$@"

$(USER_BIN_DIR)/sh: $(USER_COMMON_OBJS) $(USER_OBJ_DIR)/root/shell.o user/linker.ld build/process_userland.mk
	@mkdir -p "$(dir $@)"
	$(LD) $(USER_LDFLAGS) -Map="$@.map" -o "$@" \
		$(USER_COMMON_OBJS) $(USER_OBJ_DIR)/root/shell.o
	$(OBJCOPY) --strip-debug "$@"

$(USER_BIN_DIR)/%: $(USER_COMMON_OBJS) $(USER_OBJ_DIR)/bin/%.o user/linker.ld build/process_userland.mk
	@mkdir -p "$(dir $@)"
	$(LD) $(USER_LDFLAGS) -Map="$@.map" -o "$@" \
		$(USER_COMMON_OBJS) $(USER_OBJ_DIR)/bin/$*.o
	$(OBJCOPY) --strip-debug "$@"

$(USER_INITRAMFS): $(USER_ELFS) user/g5.script user/mkinitramfs.py Makefile \
		build/process_userland.mk build/verification.mk
	$(PYTHON) user/mkinitramfs.py --output "$@" \
		--file init=$(USER_BIN_DIR)/init \
		--file sh=$(USER_BIN_DIR)/sh \
		--file shell=$(USER_BIN_DIR)/sh \
		--file g5.script=user/g5.script \
		$(foreach program,$(USER_BIN_PROGRAMS),--file $(program)=$(USER_BIN_DIR)/$(program))

.PHONY: user-programs user-initramfs
user-programs: $(USER_ELFS)
user-initramfs: $(USER_INITRAMFS)

-include $(USER_COMMON_OBJS:.o=.d)
-include $(USER_OBJ_DIR)/root/*.d
-include $(USER_OBJ_DIR)/bin/*.d
