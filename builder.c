// gcc -O2 -std=c17 -Wall -Wextra builder.c -o mkfs_builder
#define _FILE_OFFSET_BITS 64
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <inttypes.h>
#include <errno.h>
#include <time.h>
#include <assert.h>

#define BS 4096u
#define INODE_SIZE 128u
#define ROOT_INO 1u

#pragma pack(push, 1)
typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t block_size;
    uint64_t total_blocks;
    uint64_t inode_count;
    uint64_t inode_bitmap_start;
    uint64_t inode_bitmap_blocks;
    uint64_t data_bitmap_start;
    uint64_t data_bitmap_blocks;
    uint64_t inode_table_start;
    uint64_t inode_table_blocks;
    uint64_t data_region_start;
    uint64_t data_region_blocks;
    uint64_t root_inode;
    uint64_t mtime_epoch;
    uint32_t flags;
    uint32_t checksum; // crc32(superblock block [0..4091]), checksum at the tail (last 4)
} superblock_t;
#pragma pack(pop)
_Static_assert(sizeof(superblock_t) == 116, "superblock must be 116 bytes");

#pragma pack(push, 1)
typedef struct {
    uint16_t mode;
    uint16_t links;
    uint32_t uid;
    uint32_t gid;
    uint64_t size_bytes;
    uint64_t atime;
    uint64_t mtime;
    uint64_t ctime;
    uint32_t direct[12];
    uint32_t reserved_0;
    uint32_t reserved_1;
    uint32_t reserved_2;
    uint32_t proj_id;
    uint32_t uid16_gid16;
    uint64_t xattr_ptr;
    uint64_t inode_crc;
} inode_t;
#pragma pack(pop)
_Static_assert(sizeof(inode_t) == INODE_SIZE, "inode size mismatch");

#pragma pack(push, 1)
typedef struct {
    uint32_t inode_no;
    uint8_t  type;       // 1=file, 2=dir
    char     name[58];
    uint8_t  checksum;   // XOR of bytes 0..62
} dirent64_t;
#pragma pack(pop)
_Static_assert(sizeof(dirent64_t) == 64, "dirent size mismatch");

// ========================== helpers: crc32 ===================================
static uint32_t CRC32_TAB[256];
static void crc32_init(void){
    for (uint32_t i=0;i<256;i++){
        uint32_t c=i;
        for(int j=0;j<8;j++) c = (c&1)?(0xEDB88320u^(c>>1)):(c>>1);
        CRC32_TAB[i]=c;
    }
}
static uint32_t crc32(const void* data, size_t n){
    const uint8_t* p=(const uint8_t*)data; uint32_t c=0xFFFFFFFFu;
    for(size_t i=0;i<n;i++) c = CRC32_TAB[(c^p[i])&0xFF] ^ (c>>8);
    return c ^ 0xFFFFFFFFu;
}

// Call this on the 4 KiB superblock memory (block 0 in image)
static uint32_t superblock_crc_finalize(superblock_t *sb_block_aligned) {
    sb_block_aligned->checksum = 0;
    uint32_t s = crc32((void *) sb_block_aligned, BS - 4);
    sb_block_aligned->checksum = s;
    return s;
}
static void inode_crc_finalize(inode_t* ino){
    uint8_t tmp[INODE_SIZE]; memcpy(tmp, ino, INODE_SIZE);
    memset(&tmp[120], 0, 8);
    uint32_t c = crc32(tmp, 120);
    ino->inode_crc = (uint64_t)c;
}
static void dirent_checksum_finalize(dirent64_t* de) {
    const uint8_t* p = (const uint8_t*)de;
    uint8_t x = 0;
    for (int i = 0; i < 63; i++) x ^= p[i];
    de->checksum = x;
}

static inline void set_bitmap_bit(uint8_t* bmp, uint64_t idx){
    bmp[idx >> 3] |= (uint8_t)(1u << (idx & 7u));
}

int main(int argc, char* argv[]) {
    crc32_init();

    const char* image_name = NULL;
    uint64_t size_kib = 0, inode_count = 0;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--image") && i+1 < argc) image_name = argv[++i];
        else if (!strcmp(argv[i], "--size-kib") && i+1 < argc) size_kib = strtoull(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--inodes") && i+1 < argc) inode_count = strtoull(argv[++i], NULL, 10);
        else {
            fprintf(stderr, "Unknown or incomplete flag near '%s'\n", argv[i]);
            return 2;
        }
    }

    if (!image_name || size_kib < 180 || size_kib > 4096 || (size_kib % 4) != 0 ||
        inode_count < 128 || inode_count > 512) {
        fprintf(stderr, "Usage: --image <out.img> --size-kib <180..4096, multiple of 4> --inodes <128..512>\n");
        return 2;
    }

    uint64_t total_blocks = (size_kib * 1024u) / BS;
    uint64_t inode_table_blocks = (inode_count * INODE_SIZE + BS - 1) / BS;

    const uint64_t inode_bitmap_start = 1;
    const uint64_t data_bitmap_start  = 2;
    const uint64_t inode_table_start  = 3;
    const uint64_t data_region_start  = inode_table_start + inode_table_blocks;
    if (data_region_start >= total_blocks) {
        fprintf(stderr, "Configuration leaves no data region.\n");
        return 2;
    }
    const uint64_t data_region_blocks = total_blocks - data_region_start;

    // Allocate and zero entire image
    uint8_t* image = (uint8_t*)calloc(total_blocks, BS);
    if (!image) { perror("calloc"); return 1; }

    // Build and place superblock into block 0
    time_t now = time(NULL);
    superblock_t sb = {
        .magic = 0x4D565346u,        // "MVFS"
        .version = 1u,
        .block_size = BS,
        .total_blocks = total_blocks,
        .inode_count = inode_count,
        .inode_bitmap_start = inode_bitmap_start,
        .inode_bitmap_blocks = 1u,
        .data_bitmap_start = data_bitmap_start,
        .data_bitmap_blocks = 1u,
        .inode_table_start = inode_table_start,
        .inode_table_blocks = inode_table_blocks,
        .data_region_start = data_region_start,
        .data_region_blocks = data_region_blocks,
        .root_inode = ROOT_INO,
        .mtime_epoch = (uint64_t)now,
        .flags = 0u,
        .checksum = 0u
    };
    // Copy struct into block 0; block tail stays zero
    memcpy(image + 0*BS, &sb, sizeof(sb));
    // Compute checksum over the entire 4 KiB block (not the stack struct)
    superblock_crc_finalize((superblock_t*)(image + 0*BS));

    // Mark root inode and its first data block in bitmaps
    uint8_t* inode_bmp = image + inode_bitmap_start*BS;
    uint8_t* data_bmp  = image + data_bitmap_start*BS;

    set_bitmap_bit(inode_bmp, 0); // inode #1
    set_bitmap_bit(data_bmp,  0); // first data block in data region

    // Create root inode in inode table (inode index 0 -> ino #1)
    inode_t root = {0};
    root.mode = 0040000; // directory
    root.links = 2;      // . and ..
    root.uid = 0;
    root.gid = 0;
    root.size_bytes = 2u * sizeof(dirent64_t);
    root.atime = root.mtime = root.ctime = (uint64_t)now;
    memset(root.direct, 0, sizeof(root.direct));
    root.direct[0] = (uint32_t)data_region_start;
    root.proj_id = 0;         // set to your group ID if required
    root.uid16_gid16 = 0;
    root.xattr_ptr = 0;
    inode_crc_finalize(&root);

    memcpy(image + inode_table_start*BS + 0*INODE_SIZE, &root, sizeof(root));

    // Write "." and ".." directory entries into the first data block
    dirent64_t dot = {0};
    dot.inode_no = ROOT_INO;
    dot.type = 2;
    strcpy(dot.name, ".");
    dirent_checksum_finalize(&dot);

    dirent64_t dotdot = {0};
    dotdot.inode_no = ROOT_INO;
    dotdot.type = 2;
    strcpy(dotdot.name, "..");
    dirent_checksum_finalize(&dotdot);

    uint8_t* root_block = image + data_region_start*BS;
    memcpy(root_block, &dot, sizeof(dot));
    memcpy(root_block + sizeof(dot), &dotdot, sizeof(dotdot));

    // Persist image
    FILE* f = fopen(image_name, "wb");
    if (!f) { perror("fopen"); free(image); return 1; }
    size_t wrote = fwrite(image, BS, total_blocks, f);
    if (wrote != total_blocks) { perror("fwrite"); fclose(f); free(image); return 1; }
    fclose(f);
    free(image);
    return 0;
}

