#define _FILE_OFFSET_BITS 64
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <libgen.h>
#define BS 4096u

#define INODE_SIZE 128u
#define ROOT_INO 1u
#define DIRECT_MAX 12
#pragma pack(push, 1)

typedef struct {
    // CREATE YOUR SUPERBLOCK HERE
    // ADD ALL FIELDS AS PROVIDED BY THE SPECIFICATION
    
    // THIS FIELD SHOULD STAY AT THE END
    // ALL OTHER FIELDS SHOULD BE ABOVE THIS
    uint32_t magic, version, block_size;
    uint64_t total_blocks, inode_count;
    uint64_t inode_bitmap_start, inode_bitmap_blocks;
    uint64_t data_bitmap_start, data_bitmap_blocks;
    uint64_t inode_table_start, inode_table_blocks;
    uint64_t data_region_start, data_region_blocks;
    uint64_t root_inode, mtime_epoch;
    uint32_t flags;
    uint32_t checksum;            // crc32(superblock[0..4091])
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

int main(int argc, char* argv[]) {
    crc32_init();

    if (argc != 7) {
        fprintf(stderr, "Usage: --input <in.img> --output <out.img> --file <filename>\n");
        return 1;
    }

    const char *input_img = NULL, *output_img = NULL, *filename = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--input")) input_img = argv[++i];
        else if (!strcmp(argv[i], "--output")) output_img = argv[++i];
        else if (!strcmp(argv[i], "--file")) filename = argv[++i];
    }

    if (!input_img || !output_img || !filename) {
        fprintf(stderr, "Missing required arguments.\n");
        return 1;
    }

    FILE *fin = fopen(input_img, "rb");
    if (!fin) {
        perror("Failed to open input image");
        return 1;
    }

    superblock_t sb;
    fread(&sb, sizeof(sb), 1, fin);

    uint64_t total_bytes = sb.total_blocks * BS;
    uint8_t *image = malloc(total_bytes);
    if (!image) {
        fprintf(stderr, "Memory allocation failed.\n");
        fclose(fin);
        return 1;
    }
    fseek(fin, 0, SEEK_SET);
    fread(image, 1, total_bytes, fin);
    fclose(fin);

    uint8_t *inode_bitmap = image + BS * sb.inode_bitmap_start;
    uint8_t *data_bitmap = image + BS * sb.data_bitmap_start;
    inode_t *inode_table = (inode_t *)(image + BS * sb.inode_table_start);

    FILE *fdata = fopen(filename, "rb");
    if (!fdata) {
        perror("Failed to open file to add");
        free(image);
        return 1;
    }

    fseek(fdata, 0, SEEK_END);
    uint64_t fsize = ftell(fdata);
    fseek(fdata, 0, SEEK_SET);

    uint32_t blocks_needed = (fsize + BS - 1) / BS;
    if (blocks_needed > DIRECT_MAX) {
        fprintf(stderr, "File too large for MiniVSFS (max %d blocks).\n", DIRECT_MAX);
        fclose(fdata);
        free(image);
        return 1;
    }

    int free_ino = -1;
    for (uint64_t i = 0; i < sb.inode_count; i++) { // Fix signed/unsigned comparison
        if (!(inode_bitmap[i / 8] & (1 << (i % 8)))) {
            free_ino = i;
            inode_bitmap[i / 8] |= (1 << (i % 8));
            break;
        }
    }
    if (free_ino == -1) {
        fprintf(stderr, "No free inode available.\n");
        fclose(fdata);
        free(image);
        return 1;
    }

    uint32_t data_blocks[DIRECT_MAX] = {0};
    uint64_t found = 0; // Fix signed/unsigned comparison
    for (uint64_t i = 0; i < sb.data_region_blocks && found < (uint64_t)blocks_needed; i++) {
        if (!(data_bitmap[i / 8] & (1 << (i % 8)))) {
            data_bitmap[i / 8] |= (1 << (i % 8));
            data_blocks[found++] = sb.data_region_start + i;
        }
    }
    if ((uint64_t)found < (uint64_t)blocks_needed) {
        fprintf(stderr, "Not enough free data blocks.\n");
        fclose(fdata);
        free(image);
        return 1;
    }

    inode_t *new_inode = &inode_table[free_ino];
    memset(new_inode, 0, sizeof(inode_t));
    new_inode->mode = 0100000;
    new_inode->links = 1;
    new_inode->uid = 0;
    new_inode->gid = 0;
    new_inode->size_bytes = fsize;
    new_inode->atime = new_inode->mtime = new_inode->ctime = time(NULL);
    memcpy(new_inode->direct, data_blocks, blocks_needed * sizeof(uint32_t));
    inode_crc_finalize(new_inode);

    uint64_t remaining = fsize;
    for (uint32_t i = 0; i < blocks_needed; i++) {
        uint8_t *block_ptr = image + BS * data_blocks[i];
        size_t to_read = (remaining > BS) ? BS : remaining;
        fread(block_ptr, 1, to_read, fdata);
        remaining -= to_read;
    }
    fclose(fdata);

    inode_t *root_inode = &inode_table[ROOT_INO - 1];
    if (root_inode->direct[0] == 0) {
        fprintf(stderr, "Root inode has no data block allocated.\n");
        free(image);
        return 1;
    }
    uint8_t *root_dir_block = image + BS * root_inode->direct[0];

    dirent64_t *entry = (dirent64_t *)(root_dir_block + root_inode->size_bytes);
    memset(entry, 0, sizeof(dirent64_t));
    entry->inode_no = free_ino + 1;
    entry->type = 1;
    char namebuf[58];
    strncpy(namebuf, basename((char *)filename), 57);
    namebuf[57] = '\0';
    strncpy(entry->name, namebuf, 58);
    dirent_checksum_finalize(entry);

    root_inode->size_bytes += sizeof(dirent64_t);
    root_inode->links += 1;
    inode_crc_finalize(root_inode);

    superblock_crc_finalize((superblock_t *)image);

    FILE *fout = fopen(output_img, "wb");
    if (!fout) {
        perror("Failed to open output image");
        free(image);
        return 1;
    }
    fwrite(image, 1, total_bytes, fout);
    fclose(fout);
    free(image);
    return 0;
}