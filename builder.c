// Build: gcc -O2 -std=c17 -Wall -Wextra mkfs_minivsfs.c -o mkfs_builder
#define _FILE_OFFSET_BITS 64
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <inttypes.h>
#include <errno.h>
#include <time.h>
#include <assert.h>

#define BS 4096u               // block size
#define INODE_SIZE 128u
#define ROOT_INO 1u

uint64_t g_random_seed = 0; // This should be replaced by seed value from the CLI.

// below contains some basic structures you need for your project
// you are free to create more structures as you require

#pragma pack(push, 1)
typedef struct {
    // CREATE YOUR SUPERBLOCK HERE
    // ADD ALL FIELDS AS PROVIDED BY THE SPECIFICATION
    
    // THIS FIELD SHOULD STAY AT THE END
    // ALL OTHER FIELDS SHOULD BE ABOVE THIS
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
    uint32_t checksum;          // crc32(superblock[0..4091])
} superblock_t;
#pragma pack(pop)
_Static_assert(sizeof(superblock_t) == 116, "superblock must fit in one block");

#pragma pack(push,1)
typedef struct {
    // CREATE YOUR INODE HERE
    // IF CREATED CORRECTLY, THE STATIC_ASSERT ERROR SHOULD BE GONE

    // THIS FIELD SHOULD STAY AT THE END
    // ALL OTHER FIELDS SHOULD BE ABOVE THIS
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
    uint64_t inode_crc;   // low 4 bytes store crc32 of bytes [0..119]; high 4 bytes 0

} inode_t;
#pragma pack(pop)
_Static_assert(sizeof(inode_t)==INODE_SIZE, "inode size mismatch");

#pragma pack(push,1)
typedef struct {
    // CREATE YOUR DIRECTORY ENTRY STRUCTURE HERE
    // IF CREATED CORRECTLY, THE STATIC_ASSERT ERROR SHOULD BE GONE
    uint32_t inode_no;
    uint8_t type;
    char name[58];
    uint8_t  checksum; // XOR of bytes 0..62
} dirent64_t;
#pragma pack(pop)
_Static_assert(sizeof(dirent64_t)==64, "dirent size mismatch");


// ==========================DO NOT CHANGE THIS PORTION=========================
// These functions are there for your help. You should refer to the specifications to see how you can use them.
// ====================================CRC32====================================
uint32_t CRC32_TAB[256];
void crc32_init(void){
    for (uint32_t i=0;i<256;i++){
        uint32_t c=i;
        for(int j=0;j<8;j++) c = (c&1)?(0xEDB88320u^(c>>1)):(c>>1);
        CRC32_TAB[i]=c;
    }
}
uint32_t crc32(const void* data, size_t n){
    const uint8_t* p=(const uint8_t*)data; uint32_t c=0xFFFFFFFFu;
    for(size_t i=0;i<n;i++) c = CRC32_TAB[(c^p[i])&0xFF] ^ (c>>8);
    return c ^ 0xFFFFFFFFu;
}
// ====================================CRC32====================================

// WARNING: CALL THIS ONLY AFTER ALL OTHER SUPERBLOCK ELEMENTS HAVE BEEN FINALIZED
static uint32_t superblock_crc_finalize(superblock_t *sb) {
    sb->checksum = 0;
    uint32_t s = crc32((void *) sb, BS - 4);
    sb->checksum = s;
    return s;
}

// WARNING: CALL THIS ONLY AFTER ALL OTHER SUPERBLOCK ELEMENTS HAVE BEEN FINALIZED
void inode_crc_finalize(inode_t* ino){
    uint8_t tmp[INODE_SIZE]; memcpy(tmp, ino, INODE_SIZE);
    // zero crc area before computing
    memset(&tmp[120], 0, 8);
    uint32_t c = crc32(tmp, 120);
    ino->inode_crc = (uint64_t)c; // low 4 bytes carry the crc
}

// WARNING: CALL THIS ONLY AFTER ALL OTHER SUPERBLOCK ELEMENTS HAVE BEEN FINALIZED
void dirent_checksum_finalize(dirent64_t* de) {
    const uint8_t* p = (const uint8_t*)de;
    uint8_t x = 0;
    for (int i = 0; i < 63; i++) x ^= p[i];   // covers ino(4) + type(1) + name(58)
    de->checksum = x;
}

int main() {
    crc32_init();
    // WRITE YOUR DRIVER CODE HERE
    // PARSE YOUR CLI PARAMETERS
    // THEN CREATE YOUR FILE SYSTEM WITH A ROOT DIRECTORY
    // THEN SAVE THE DATA INSIDE THE OUTPUT IMAGE
    if (argc != 7) {
        fprintf(stderr, "Usage: --image <out.img> --size-kib <180..4096> --inodes <128..512>\n");
        return 1;
    }

    const char* image_name = NULL;
    uint64_t size_kib = 0, inode_count = 0;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--image")) image_name = argv[++i];
        else if (!strcmp(argv[i], "--size-kib")) size_kib = strtoull(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--inodes")) inode_count = strtoull(argv[++i], NULL, 10);
    }

    if (!image_name || size_kib < 180 || size_kib > 4096 || inode_count < 128 || inode_count > 512) {
        fprintf(stderr, "Invalid parameters.\n");
        return 1;
    }

    uint64_t total_blocks = size_kib * 1024 / BS;
    uint64_t inode_table_blocks = (inode_count * INODE_SIZE + BS - 1) / BS;
    uint64_t data_region_start = 3 + inode_table_blocks;
    uint64_t data_region_blocks = total_blocks - data_region_start;

    superblock_t sb = {
        .magic = 0x4D565346,
        .version = 1,
        .block_size = BS,
        .total_blocks = total_blocks,
        .inode_count = inode_count,
        .inode_bitmap_start = 1,
        .inode_bitmap_blocks = 1,
        .data_bitmap_start = 2,
        .data_bitmap_blocks = 1,
        .inode_table_start = 3,
        .inode_table_blocks = inode_table_blocks,
        .data_region_start = data_region_start,
        .data_region_blocks = data_region_blocks,
        .root_inode = ROOT_INO,
        .mtime_epoch = time(NULL),
        .flags = 0
    };
    superblock_crc_finalize(&sb);

    uint8_t* image = calloc(total_blocks, BS);
    memcpy(image, &sb, sizeof(sb));

    image[BS + 0] |= 1; // inode bitmap: mark inode 1
    image[2 * BS + 0] |= 1; // data bitmap: mark block 1

    inode_t root = {
        .mode = 040000,
        .links = 2,
        .uid = 0,
        .gid = 0,
        .size_bytes = 2 * sizeof(dirent64_t),
        .atime = sb.mtime_epoch,
        .mtime = sb.mtime_epoch,
        .ctime = sb.mtime_epoch,
        .direct = { data_region_start }
    };
    inode_crc_finalize(&root);
    memcpy(image + BS * sb.inode_table_start, &root, sizeof(root));

    dirent64_t dot = { .inode_no = ROOT_INO, .type = 2 };
    strcpy(dot.name, ".");
    dirent_checksum_finalize(&dot);

    dirent64_t dotdot = { .inode_no = ROOT_INO, .type = 2 };
    strcpy(dotdot.name, "..");
    dirent_checksum_finalize(&dotdot);

    memcpy(image + BS * data_region_start, &dot, sizeof(dot));
    memcpy(image + BS * data_region_start + sizeof(dot), &dotdot, sizeof(dotdot));

    FILE* f = fopen(image_name, "wb");
    fwrite(image, BS, total_blocks, f);
    fclose(f);
    free(image);
    return 0;
}

