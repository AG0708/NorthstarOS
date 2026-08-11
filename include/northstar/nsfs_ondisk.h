#ifndef NORTHSTAR_NSFS_ONDISK_H
#define NORTHSTAR_NSFS_ONDISK_H

/*
 * NorthstarFS v1 disk ABI.
 *
 * Every integer in an on-disk structure is little endian.  The packed
 * structures describe the byte layout only: disk buffers must be copied into
 * aligned objects before fields are accessed on architectures that forbid
 * unaligned loads.  Block and inode number zero are reserved as null values.
 */

#include <northstar/base.h>

#define NSFS_MAGIC_BYTES             "NSTARFS\0"
#define NSFS_JOURNAL_MAGIC_BYTES     "NSJNL1\0\0"
#define NSFS_VERSION                 1u
#define NSFS_JOURNAL_VERSION         1u
#define NSFS_BLOCK_SIZE              4096u
#define NSFS_SUPERBLOCK_SIZE         256u
#define NSFS_INODE_SIZE              128u
#define NSFS_SUPERBLOCK_COPIES       2u
#define NSFS_ROOT_INODE              1u
#define NSFS_DIRECT_BLOCKS           12u
#define NSFS_INDIRECT_BLOCKS         (NSFS_BLOCK_SIZE / sizeof(uint32_t))
#define NSFS_MAX_FILE_BLOCKS         (NSFS_DIRECT_BLOCKS + NSFS_INDIRECT_BLOCKS)
#define NSFS_MAX_FILE_SIZE           ((uint64_t)NSFS_MAX_FILE_BLOCKS * NSFS_BLOCK_SIZE)
#define NSFS_NAME_MAX                255u
#define NSFS_DIRENT_HEADER_SIZE      8u
#define NSFS_JOURNAL_HEADER_SIZE     64u
#define NSFS_JOURNAL_DESCRIPTOR_SIZE 16u
#define NSFS_JOURNAL_MAX_ENTRIES     128u
#define NSFS_DEFAULT_JOURNAL_BLOCKS  65u
#define NSFS_MIN_JOURNAL_BLOCKS      9u

typedef uint16_t nsfs_le16_t;
typedef uint32_t nsfs_le32_t;
typedef uint64_t nsfs_le64_t;

static inline uint16_t nsfs_bswap16(uint16_t value) {
    return (uint16_t)((value << 8) | (value >> 8));
}

static inline uint32_t nsfs_bswap32(uint32_t value) {
    return ((value & UINT32_C(0x000000ff)) << 24) |
           ((value & UINT32_C(0x0000ff00)) << 8) |
           ((value & UINT32_C(0x00ff0000)) >> 8) |
           ((value & UINT32_C(0xff000000)) >> 24);
}

static inline uint64_t nsfs_bswap64(uint64_t value) {
    return ((uint64_t)nsfs_bswap32((uint32_t)value) << 32) |
           nsfs_bswap32((uint32_t)(value >> 32));
}

#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
#define nsfs_cpu_to_le16(v) nsfs_bswap16((uint16_t)(v))
#define nsfs_cpu_to_le32(v) nsfs_bswap32((uint32_t)(v))
#define nsfs_cpu_to_le64(v) nsfs_bswap64((uint64_t)(v))
#define nsfs_le16_to_cpu(v) nsfs_bswap16((uint16_t)(v))
#define nsfs_le32_to_cpu(v) nsfs_bswap32((uint32_t)(v))
#define nsfs_le64_to_cpu(v) nsfs_bswap64((uint64_t)(v))
#else
#define nsfs_cpu_to_le16(v) ((uint16_t)(v))
#define nsfs_cpu_to_le32(v) ((uint32_t)(v))
#define nsfs_cpu_to_le64(v) ((uint64_t)(v))
#define nsfs_le16_to_cpu(v) ((uint16_t)(v))
#define nsfs_le32_to_cpu(v) ((uint32_t)(v))
#define nsfs_le64_to_cpu(v) ((uint64_t)(v))
#endif

enum nsfs_super_state {
    NSFS_STATE_CLEAN = 1,
    NSFS_STATE_DIRTY = 2
};

enum nsfs_feature_flags {
    NSFS_FEATURE_NONE = 0
};

enum nsfs_inode_type {
    NSFS_INODE_REGULAR = 1,
    NSFS_INODE_DIRECTORY = 2,
    NSFS_INODE_SYMLINK = 3
};

enum nsfs_inode_flags {
    NSFS_INODE_FLAG_NONE = 0
};

enum nsfs_journal_state {
    NSFS_JOURNAL_EMPTY = 0,
    NSFS_JOURNAL_PREPARED = 1,
    NSFS_JOURNAL_COMMITTED = 2
};

/* The primary superblock is block 0 and the redundant copy is block 1. */
struct NS_PACKED nsfs_disk_superblock {
    uint8_t magic[8];
    nsfs_le32_t version;
    nsfs_le32_t header_size;
    nsfs_le32_t block_size;
    nsfs_le32_t inode_size;
    nsfs_le32_t state;
    nsfs_le32_t features;
    nsfs_le64_t generation;
    nsfs_le64_t total_blocks;
    nsfs_le32_t total_inodes;
    nsfs_le32_t root_inode;
    nsfs_le64_t journal_start;
    nsfs_le32_t journal_blocks; /* one header block plus redo-image blocks */
    nsfs_le32_t journal_entries;
    nsfs_le64_t inode_bitmap_start;
    nsfs_le64_t inode_bitmap_blocks;
    nsfs_le64_t block_bitmap_start;
    nsfs_le64_t block_bitmap_blocks;
    nsfs_le64_t inode_table_start;
    nsfs_le64_t inode_table_blocks;
    nsfs_le64_t data_start;
    nsfs_le64_t free_blocks;
    nsfs_le32_t free_inodes;
    nsfs_le32_t mount_count;
    nsfs_le64_t last_mount_ns;
    nsfs_le64_t last_write_ns;
    uint8_t uuid[16];
    nsfs_le64_t last_txid;
    nsfs_le32_t checksum;
    uint8_t reserved[68];
};

/* Exactly 128 bytes.  allocated_blocks includes the indirect pointer block. */
struct NS_PACKED nsfs_disk_inode {
    nsfs_le16_t mode;
    uint8_t type;
    uint8_t flags;
    nsfs_le32_t uid;
    nsfs_le32_t gid;
    nsfs_le32_t link_count;
    nsfs_le64_t size;
    nsfs_le64_t allocated_blocks;
    nsfs_le64_t generation;
    nsfs_le64_t atime_ns;
    nsfs_le64_t mtime_ns;
    nsfs_le64_t ctime_ns;
    nsfs_le32_t direct[NSFS_DIRECT_BLOCKS];
    nsfs_le32_t indirect;
    nsfs_le32_t reserved0;
    nsfs_le32_t checksum;
    nsfs_le32_t reserved1;
};

/* Variable-length entry.  rec_len is 4-byte aligned and never crosses a block. */
struct NS_PACKED nsfs_disk_dirent {
    nsfs_le32_t inode;
    nsfs_le16_t rec_len;
    uint8_t name_len;
    uint8_t type;
    uint8_t name[];
};

struct NS_PACKED nsfs_disk_journal_descriptor {
    nsfs_le64_t target_block;
    nsfs_le32_t data_checksum;
    nsfs_le32_t flags;
};

/* Occupies the first 64 bytes of the journal header block. */
struct NS_PACKED nsfs_disk_journal_header {
    uint8_t magic[8];
    nsfs_le32_t version;
    nsfs_le32_t header_size;
    nsfs_le32_t state;
    nsfs_le32_t entry_count;
    nsfs_le64_t txid;
    nsfs_le64_t filesystem_generation;
    nsfs_le32_t descriptor_size;
    nsfs_le32_t checksum;
    uint8_t reserved[16];
};

NS_STATIC_ASSERT(sizeof(struct nsfs_disk_superblock) == NSFS_SUPERBLOCK_SIZE,
                 "NorthstarFS superblock ABI drift");
NS_STATIC_ASSERT(sizeof(struct nsfs_disk_inode) == NSFS_INODE_SIZE,
                 "NorthstarFS inode ABI drift");
NS_STATIC_ASSERT(sizeof(struct nsfs_disk_dirent) == NSFS_DIRENT_HEADER_SIZE,
                 "NorthstarFS dirent ABI drift");
NS_STATIC_ASSERT(sizeof(struct nsfs_disk_journal_descriptor) ==
                     NSFS_JOURNAL_DESCRIPTOR_SIZE,
                 "NorthstarFS journal descriptor ABI drift");
NS_STATIC_ASSERT(sizeof(struct nsfs_disk_journal_header) ==
                     NSFS_JOURNAL_HEADER_SIZE,
                 "NorthstarFS journal header ABI drift");

/* Castagnoli CRC-32C, initial/final xor 0xffffffff. */
uint32_t nsfs_crc32c(const void *data, size_t length);

#endif
