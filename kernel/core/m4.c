#include <northstar/arch_cpu.h>
#include <northstar/arch_io.h>
#include <northstar/ata.h>
#include <northstar/block_slice.h>
#include <northstar/boot_info.h>
#include <northstar/errno.h>
#include <northstar/kernel.h>
#include <northstar/mm_heap.h>
#include <northstar/nsfs.h>
#include <northstar/sha256.h>

enum {
    M4_SCRATCH_SECTORS = 8192,
    M4_MIN_PRIMARY_SECTORS = 32768,
    M4_PAYLOAD_BYTES = 15 * NSFS_BLOCK_SIZE + 137,
    /* Cache-flush completion is asynchronous in QEMU and can be delayed by a
     * loaded host.  Keep a finite protocol bound, but leave enough I/O polls
     * for the documented Ubuntu/QEMU 8.2 CI lane. */
    M4_ATA_POLL_LIMIT = 10000000,
};

struct m4_control_sector {
    uint8_t magic[8];
    uint32_t checkpoint;
    uint32_t checkpoint_complement;
    uint8_t reserved[512 - 16];
} NS_PACKED;

struct m4_runtime_context {
    enum nsfs_journal_checkpoint cutpoint;
};

NS_STATIC_ASSERT(sizeof(struct m4_control_sector) == 512,
                 "M4 control sector size changed");

static const uint8_t m4_control_magic[8] = {
    'N', 'S', 'C', 'U', 'T', '0', '1', '\0',
};

static const uint8_t expected_payload_sha256[NS_SHA256_DIGEST_BYTES] = {
    0x29, 0xfe, 0x68, 0x0b, 0xb7, 0x28, 0x55, 0x1e,
    0x86, 0x7f, 0xff, 0x00, 0x11, 0xdc, 0xfc, 0xdc,
    0x09, 0x8c, 0xf5, 0x5b, 0xc7, 0x4d, 0xbf, 0x53,
    0xe4, 0x28, 0x87, 0xca, 0x6a, 0x43, 0x30, 0x6e,
};

#if NORTHSTAR_ENABLE_G5
extern void northstar_g5_run(struct nsfs *filesystem,
                             const struct nsfs_runtime *runtime) NS_NORETURN;
#endif

static NS_NORETURN void m4_fail(const char *reason)
{
    klog("m4", reason);
    serial_write("# NS_GATE G4 FAIL\n");
    serial_write("NS:RUN:COMPLETE\n");
    kernel_debug_exit(0x14);
}

static void write_decimal(uint64_t value)
{
    char digits[20];
    size_t count = 0;
    if (value == 0) {
        serial_putc('0');
        return;
    }
    while (value != 0) {
        digits[count++] = (char)('0' + value % 10u);
        value /= 10u;
    }
    while (count != 0)
        serial_putc(digits[--count]);
}

static void write_digest(const uint8_t digest[NS_SHA256_DIGEST_BYTES])
{
    static const char digits[] = "0123456789abcdef";
    for (size_t index = 0; index < NS_SHA256_DIGEST_BYTES; ++index) {
        serial_putc(digits[digest[index] >> 4]);
        serial_putc(digits[digest[index] & 15u]);
    }
}

static uint8_t ata_read8(void *context, uint16_t port)
{
    (void)context;
    return arch_in8(port);
}

static uint16_t ata_read16(void *context, uint16_t port)
{
    (void)context;
    return arch_in16(port);
}

static void ata_write8(void *context, uint16_t port, uint8_t value)
{
    (void)context;
    arch_out8(port, value);
}

static void ata_write16(void *context, uint16_t port, uint16_t value)
{
    (void)context;
    arch_out16(port, value);
}

static void ata_relax(void *context)
{
    (void)context;
    arch_cpu_relax();
}

static void *fs_allocate(void *context, size_t size)
{
    (void)context;
    return kmalloc(size);
}

static void fs_deallocate(void *context, void *pointer)
{
    (void)context;
    kfree(pointer);
}

static const char *checkpoint_name(enum nsfs_journal_checkpoint checkpoint)
{
    switch (checkpoint) {
    case NSFS_JOURNAL_CHECKPOINT_REDO_DURABLE:
        return "redo-durable";
    case NSFS_JOURNAL_CHECKPOINT_COMMIT_DURABLE:
        return "commit-durable";
    case NSFS_JOURNAL_CHECKPOINT_HOME_DURABLE:
        return "home-durable";
    case NSFS_JOURNAL_CHECKPOINT_CLEARED:
        return "journal-cleared";
    default:
        return "invalid";
    }
}

static void journal_checkpoint(void *context,
                               enum nsfs_journal_checkpoint checkpoint)
{
    struct m4_runtime_context *runtime = context;
    if (runtime == NULL || runtime->cutpoint != checkpoint)
        return;
    serial_write("NS:NSFS:CUT phase=");
    serial_write(checkpoint_name(checkpoint));
    serial_putc('\n');
    serial_flush();
    arch_irq_disable();
    for (;;)
        arch_cpu_halt();
}

static enum nsfs_journal_checkpoint
read_cutpoint(struct ns_block_device *scratch)
{
    struct m4_control_sector control;
    if (ns_block_read(scratch, M4_SCRATCH_SECTORS - 1u, 1u, &control) != 0)
        m4_fail("could not read journal-matrix control sector");
    if (memcmp(control.magic, m4_control_magic, sizeof(control.magic)) != 0)
        return 0;
    if (control.checkpoint < NSFS_JOURNAL_CHECKPOINT_REDO_DURABLE ||
        control.checkpoint > NSFS_JOURNAL_CHECKPOINT_CLEARED ||
        control.checkpoint_complement != ~control.checkpoint)
        m4_fail("journal-matrix control sector is invalid");
    return (enum nsfs_journal_checkpoint)control.checkpoint;
}

static void fill_payload(uint8_t *payload, size_t length)
{
    for (size_t index = 0; index < length; ++index) {
        payload[index] = (uint8_t)(index * 131u + (index >> 3u) * 17u + 0x5au);
    }
}

static bool block_is_zero(const uint8_t *block, size_t length)
{
    for (size_t index = 0; index < length; ++index) {
        if (block[index] != 0)
            return false;
    }
    return true;
}

static void persist_payload(struct nsfs *filesystem)
{
    static const char boot_proof[] = "PERSISTED_FROM_BOOT_ONE\n";
    uint8_t *payload = kmalloc(M4_PAYLOAD_BYTES);
    uint32_t directory;
    uint32_t nested;
    uint32_t file;
    uint32_t proof;
    int64_t written;

    if (payload == NULL)
        m4_fail("could not allocate persistence payload");
    fill_payload(payload, M4_PAYLOAD_BYTES);
    if (nsfs_mkdir(filesystem, NSFS_ROOT_INODE, "evidence", 8u, 0755u,
                   &directory) != 0 ||
        nsfs_mkdir(filesystem, directory, "nested", 6u, 0755u, &nested) != 0 ||
        nsfs_create(filesystem, nested, "payload.bin", 11u, 0644u, &file) !=
            0)
        m4_fail("could not create persistent namespace");
    written = nsfs_write(filesystem, file, 0, payload, M4_PAYLOAD_BYTES);
    if (written != M4_PAYLOAD_BYTES ||
        nsfs_create(filesystem, nested, "boot-proof.txt", 14u, 0644u,
                    &proof) != 0 ||
        nsfs_write(filesystem, proof, 0, boot_proof,
                   sizeof(boot_proof) - 1u) !=
            (int64_t)(sizeof(boot_proof) - 1u) ||
        nsfs_sync(filesystem) != 0)
        m4_fail("could not durably write persistence payload");
    kfree(payload);
}

static void readback_payload(struct nsfs *filesystem,
                             uint8_t digest[NS_SHA256_DIGEST_BYTES])
{
    uint8_t *actual = kmalloc(M4_PAYLOAD_BYTES);
    uint8_t *expected = kmalloc(M4_PAYLOAD_BYTES);
    struct ns_sha256 hash;
    uint32_t directory;
    uint32_t nested;
    uint32_t file;
    struct nsfs_stat stat;
    int64_t count;

    if (actual == NULL || expected == NULL)
        m4_fail("could not allocate readback buffers");
    if (nsfs_lookup(filesystem, NSFS_ROOT_INODE, "evidence", 8u,
                    &directory) != 0 ||
        nsfs_lookup(filesystem, directory, "nested", 6u, &nested) != 0 ||
        nsfs_lookup(filesystem, nested, "payload.bin", 11u, &file) != 0 ||
        nsfs_stat_inode(filesystem, file, &stat) != 0 ||
        stat.type != NSFS_INODE_REGULAR || stat.size != M4_PAYLOAD_BYTES ||
        stat.allocated_blocks <= NSFS_DIRECT_BLOCKS)
        m4_fail("persistent namespace or indirect extent is invalid");
    count = nsfs_read(filesystem, file, 0, actual, M4_PAYLOAD_BYTES);
    fill_payload(expected, M4_PAYLOAD_BYTES);
    if (count != M4_PAYLOAD_BYTES ||
        memcmp(actual, expected, M4_PAYLOAD_BYTES) != 0)
        m4_fail("cold-reboot payload bytes differ");
    ns_sha256_init(&hash);
    ns_sha256_update(&hash, actual, M4_PAYLOAD_BYTES);
    ns_sha256_final(&hash, digest);
    if (memcmp(digest, expected_payload_sha256, sizeof(expected_payload_sha256)) !=
        0)
        m4_fail("cold-reboot payload hash differs");
    kfree(expected);
    kfree(actual);
}

static void verify_corruption_rejection(struct ns_block_device *scratch,
                                        const struct nsfs_runtime *runtime)
{
    uint8_t *superblocks = kmalloc(NSFS_BLOCK_SIZE * 2u);
    struct nsfs *unexpected = NULL;
    int status;

    if (superblocks == NULL)
        m4_fail("could not allocate corruption fixture");
    if (nsfs_format(scratch, runtime, NULL) != 0 ||
        ns_block_read(scratch, 0, (NSFS_BLOCK_SIZE * 2u) / 512u,
                      superblocks) != 0)
        m4_fail("could not prepare corruption fixture");
    superblocks[0] ^= UINT8_C(0xff);
    superblocks[NSFS_BLOCK_SIZE] ^= UINT8_C(0xff);
    if (ns_block_write(scratch, 0, NSFS_BLOCK_SIZE / 512u, superblocks) != 0 ||
        ns_block_write(scratch, NSFS_BLOCK_SIZE / 512u,
                       NSFS_BLOCK_SIZE / 512u,
                       superblocks + NSFS_BLOCK_SIZE) != 0 ||
        ns_block_flush(scratch) != 0)
        m4_fail("could not persist corrupt metadata fixture");
    status = nsfs_mount(scratch, runtime, NSFS_MOUNT_READ_ONLY, &unexpected);
    if (status == 0) {
        (void)nsfs_unmount(unexpected);
        m4_fail("filesystem accepted two corrupt superblocks");
    }
    kfree(superblocks);
}

void northstar_m4_run(const struct northstar_boot_info *boot)
{
    const struct ns_ata_io ata_io = {
        .context = NULL,
        .read8 = ata_read8,
        .read16 = ata_read16,
        .write8 = ata_write8,
        .write16 = ata_write16,
        .relax = ata_relax,
    };
    const struct ns_ata_config ata_configuration = {
        .poll_limit = M4_ATA_POLL_LIMIT,
        .timeout_ns = 0,
        .disable_interrupts = true,
    };
    struct m4_runtime_context runtime_context = {0};
    const struct nsfs_runtime runtime = {
        .context = &runtime_context,
        .allocate = fs_allocate,
        .deallocate = fs_deallocate,
        .now_ns = NULL,
        .journal_checkpoint = journal_checkpoint,
    };
    struct ns_ata_bus ata_bus;
    struct ns_ata_device ata_disk;
    struct ns_ata_diagnostic diagnostic;
    struct ns_block_device ata_block;
    struct ns_block_slice primary;
    struct ns_block_slice scratch;
    struct nsfs *filesystem = NULL;
    uint8_t *probe;
    uint8_t digest[NS_SHA256_DIGEST_BYTES];
    uint64_t available;
    uint64_t primary_sectors;
    bool first_boot;

    if (boot == NULL || boot->filesystem_lba == 0)
        m4_fail("boot contract has no filesystem extent");
    if (ns_ata_bus_init(&ata_bus, &ata_io, &ata_configuration) != 0)
        m4_fail("could not initialize ATA bus");
    ns_ata_device_init(&ata_disk, &ata_bus, NS_ATA_PRIMARY, NS_ATA_MASTER);
    if (ns_ata_identify(&ata_disk, &diagnostic) != 0 ||
        ns_ata_block_device_init(&ata_block, &ata_disk) != 0)
        m4_fail("primary ATA disk identify failed");
    serial_write("NS:ATA:READY sectors=");
    write_decimal(ata_disk.sector_count);
    serial_putc('\n');

    if (boot->filesystem_lba >= ata_disk.sector_count)
        m4_fail("filesystem LBA is outside the ATA disk");
    available = boot->filesystem_sectors != 0
                    ? boot->filesystem_sectors
                    : ata_disk.sector_count - boot->filesystem_lba;
    if (available > ata_disk.sector_count - boot->filesystem_lba ||
        available <= M4_SCRATCH_SECTORS + M4_MIN_PRIMARY_SECTORS)
        m4_fail("filesystem extent is too small or out of range");
    primary_sectors = available - M4_SCRATCH_SECTORS;
    if (ns_block_slice_init(&primary, &ata_block, boot->filesystem_lba,
                            primary_sectors, 0) != 0 ||
        ns_block_slice_init(&scratch, &ata_block,
                            boot->filesystem_lba + primary_sectors,
                            M4_SCRATCH_SECTORS, 0) != 0)
        m4_fail("could not partition filesystem test extents");
    runtime_context.cutpoint = read_cutpoint(&scratch.device);

    probe = kmalloc(NSFS_BLOCK_SIZE);
    if (probe == NULL ||
        ns_block_read(&primary.device, 0, NSFS_BLOCK_SIZE / 512u, probe) != 0)
        m4_fail("could not inspect filesystem superblock");
    first_boot = block_is_zero(probe, NSFS_BLOCK_SIZE);
    kfree(probe);
    if (first_boot) {
        int format_status = nsfs_format(&primary.device, &runtime, NULL);
        if (format_status != 0) {
            klog_hex("m4", "format-status=",
                     (uint64_t)(int64_t)format_status);
            m4_fail("could not format NorthstarFS");
        }
    }
    if (first_boot && runtime_context.cutpoint != 0)
        m4_fail("journal cut requested before baseline initialization");
    {
        int mount_status =
            nsfs_mount(&primary.device, &runtime, 0, &filesystem);
        if (mount_status != 0) {
            klog_hex("m4", "mount-status=", (uint64_t)(int64_t)mount_status);
            m4_fail("could not mount NorthstarFS");
        }
    }
    serial_write("NS:NSFS:MOUNT mode=rw state=clean\n");

    if (first_boot) {
        persist_payload(filesystem);
#if !NORTHSTAR_INTERACTIVE
        if (nsfs_unmount(filesystem) != 0)
            m4_fail("could not cleanly unmount persistence image");
        serial_write("NS:NSFS:PHASE write-complete\n");
        serial_write("NS:RUN:COMPLETE\n");
        kernel_debug_exit(0x10);
#else
        serial_write("NS:NSFS:INTERACTIVE initialized\n");
#endif
    }

    if (runtime_context.cutpoint != 0) {
        uint32_t inode;
        serial_write("NS:NSFS:MATRIX phase=");
        serial_write(checkpoint_name(runtime_context.cutpoint));
        serial_putc('\n');
        if (nsfs_create(filesystem, NSFS_ROOT_INODE, "journal-cut", 11u,
                        0644u, &inode) != 0)
            m4_fail("journal matrix transaction did not reach its cutpoint");
        m4_fail("journal matrix cutpoint callback returned");
    }

    readback_payload(filesystem, digest);
    {
        uint32_t recovered_inode;
        int recovered = nsfs_lookup(filesystem, NSFS_ROOT_INODE,
                                    "journal-cut", 11u, &recovered_inode);
        if (recovered == 0) {
            struct nsfs_stat recovered_stat;
            if (nsfs_stat_inode(filesystem, recovered_inode,
                                &recovered_stat) != 0 ||
                recovered_stat.type != NSFS_INODE_REGULAR ||
                recovered_stat.size != 0)
                m4_fail("recovered journal transaction is malformed");
            serial_write("NS:NSFS:RECOVERY mutation=present\n");
        } else if (recovered == -NS_ENOENT) {
            serial_write("NS:NSFS:RECOVERY mutation=absent\n");
        } else {
            m4_fail("could not inspect recovered journal transaction");
        }
    }
    verify_corruption_rejection(&scratch.device, &runtime);
    serial_write("ok 1 - nsfs direct-indirect persistence\n");
    serial_write("ok 2 - nsfs nested directory persistence\n");
    serial_write("ok 3 - nsfs rejects corrupt metadata\n");
    serial_write("NS:GATE:G4:PASS image_sha256=");
    write_digest(digest);
    serial_putc('\n');
#if NORTHSTAR_ENABLE_G5
    northstar_g5_run(filesystem, &runtime);
#else
    if (nsfs_unmount(filesystem) != 0)
        m4_fail("could not cleanly unmount verified image");
    serial_write("NS:RUN:COMPLETE\n");
    kernel_debug_exit(0x10);
#endif
}
