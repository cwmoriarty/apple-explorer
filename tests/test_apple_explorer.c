#define _DARWIN_C_SOURCE
#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "../src/aex_internal.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define APFS_NX_MAGIC  0x4253584EU
#define APFS_VOL_MAGIC 0x42535041U

#ifdef _WIN32
#include <direct.h>
#define MKDIR_P(p) _mkdir(p)
#else
#include <unistd.h>
#define MKDIR_P(p) mkdir(p, 0755)
#endif

static void put_be16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static void put_be32(uint8_t *p, uint32_t v) { p[0]=(uint8_t)(v>>24); p[1]=(uint8_t)(v>>16); p[2]=(uint8_t)(v>>8); p[3]=(uint8_t)v; }
static void put_be64(uint8_t *p, uint64_t v) { put_be32(p, (uint32_t)(v >> 32)); put_be32(p + 4, (uint32_t)v); }
static void put_le16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put_le32(uint8_t *p, uint32_t v) { p[0]=(uint8_t)v; p[1]=(uint8_t)(v>>8); p[2]=(uint8_t)(v>>16); p[3]=(uint8_t)(v>>24); }
static void put_le64(uint8_t *p, uint64_t v) { put_le32(p, (uint32_t)v); put_le32(p + 4, (uint32_t)(v >> 32)); }

/* Build a minimal valid Classic HFS ('BD') image in memory */
static void build_classic_hfs_image(uint8_t *img, size_t img_len) {
    memset(img, 0, img_len);
    uint8_t *mdb = img + 1024U;
    put_be16(mdb + 0U, 0x4244U);  /* 'BD' */
    put_be16(mdb + 18U, 64U);     /* drNmAlBlks */
    put_be32(mdb + 20U, 512U);    /* drAlBlkSiz = 512 */
    put_be16(mdb + 28U, 4U);      /* drAlBlSt = sector 4 (offset 2048) */
    put_be16(mdb + 34U, 40U);     /* drFreeBks */
    mdb[36] = 9U;
    memcpy(mdb + 37U, "RetroMac9", 9U);

    /* Catalog file: 2 nodes of 512B = 1024B at alloc block 0 (offset 2048) */
    put_be32(mdb + 146U, 1024U);
    put_be16(mdb + 150U, 0U);
    put_be16(mdb + 152U, 2U);

    /* Node 0 (offset 2048): BTHeaderNode (nodeSize=512, firstLeaf=1) */
    uint8_t *n0 = img + 2048U;
    n0[8] = 1; /* header node */
    put_be16(n0 + 10U, 1U);
    put_be32(n0 + 14U + 10U, 1U); /* firstLeafNode = 1 */
    put_be16(n0 + 14U + 18U, 512U);
    put_be16(n0 + 510U, 14U);

    /* Node 1 (offset 2560): Leaf node with root folder (CNID 2) and /ReadMe.txt (CNID 16, alloc blk 2 = offset 3072) */
    uint8_t *n1 = img + 2560U;
    n1[8] = (uint8_t)-1; /* leaf */
    put_be16(n1 + 10U, 2U);

    /* Record 0 at +14: root folder (parent 1, name "RetroMac9") */
    size_t r0 = 14U;
    n1[r0 + 0U] = 6U + 9U;
    put_be32(n1 + r0 + 2U, 1U);
    n1[r0 + 6U] = 9U;
    memcpy(n1 + r0 + 7U, "RetroMac9", 9U);
    size_t d0 = r0 + ((1U + 15U + 1U) & ~1U); /* 30 */
    n1[d0 + 0U] = 1U; /* folder */
    put_be32(n1 + d0 + 6U, 2U); /* CNID 2 */

    /* Record 1 at +110: /ReadMe.txt (parent 2, CNID 16, data "Welcome to Classic HFS!" at alloc blk 2, rsrc "RSRC!" at alloc blk 3) */
    size_t r1 = 110U;
    n1[r1 + 0U] = 6U + 10U;
    put_be32(n1 + r1 + 2U, 2U);
    n1[r1 + 6U] = 10U;
    memcpy(n1 + r1 + 7U, "ReadMe.txt", 10U);
    size_t d1 = r1 + 18U;
    n1[d1 + 0U] = 2U; /* file */
    put_be32(n1 + d1 + 20U, 16U); /* fileID */
    const char *msg = "Welcome to Classic HFS!";
    const char *rsrc = "RSRC_FORK_BYTES";
    put_be32(n1 + d1 + 26U, (uint32_t)strlen(msg));
    put_be32(n1 + d1 + 36U, (uint32_t)strlen(rsrc));
    put_be16(n1 + d1 + 74U, 2U); put_be16(n1 + d1 + 76U, 1U); /* data extent: blk 2, cnt 1 */
    put_be16(n1 + d1 + 86U, 3U); put_be16(n1 + d1 + 88U, 1U); /* rsrc extent: blk 3, cnt 1 */

    put_be16(n1 + 510U, (uint16_t)r0);
    put_be16(n1 + 508U, (uint16_t)r1);

    memcpy(img + 2048U + 2U * 512U, msg, strlen(msg));
    memcpy(img + 2048U + 3U * 512U, rsrc, strlen(rsrc));
}

/* Helper to write an HFS+ Catalog key (UTF-16BE ASCII) and return data start offset */
static size_t write_hfsp_cat_key(uint8_t *node, size_t roff, uint32_t parent_id, const uint16_t *u16, size_t nchars) {
    uint16_t klen = (uint16_t)(6U + nchars * 2U);
    put_be16(node + roff, klen);
    put_be32(node + roff + 2U, parent_id);
    put_be16(node + roff + 6U, (uint16_t)nchars);
    for (size_t i = 0; i < nchars; i++) put_be16(node + roff + 8U + i * 2U, u16[i]);
    return roff + 2U + klen;
}

static size_t ascii_to_u16(const char *s, uint16_t *out) {
    size_t n = strlen(s);
    for (size_t i = 0; i < n; i++) out[i] = (uint8_t)s[i];
    return n;
}

/* Build an HFS+ image containing Backups.backupdb directory hard links AND decmpfs compressed file */
static void build_hfsplus_time_machine_image(uint8_t *img, size_t img_len) {
    memset(img, 0, img_len);
    const uint32_t bsz = 4096U;
    uint8_t *vh = img + 1024U;
    put_be16(vh + 0U, 0x482BU); /* 'H+' */
    put_be16(vh + 2U, 4U);
    put_be32(vh + 40U, bsz);
    put_be32(vh + 44U, (uint32_t)(img_len / bsz));
    put_be32(vh + 48U, 10U);

    /* Catalog file at blocks 2..3 (nodeSize = 4096, node 0 = blk 2, node 1 = blk 3) */
    put_be64(vh + 272U + 0U, 8192ULL);
    put_be32(vh + 272U + 12U, 2U);
    put_be32(vh + 272U + 16U, 2U);
    put_be32(vh + 272U + 20U, 2U);

    /* Attributes file at blocks 4..5 (nodeSize = 4096, node 0 = blk 4, node 1 = blk 5) */
    put_be64(vh + 352U + 0U, 8192ULL);
    put_be32(vh + 352U + 12U, 2U);
    put_be32(vh + 352U + 16U, 4U);
    put_be32(vh + 352U + 20U, 2U);

    /* Catalog Node 0 (Header) */
    uint8_t *cn0 = img + 2U * bsz;
    cn0[8] = 1;
    put_be16(cn0 + 10U, 1U);
    put_be32(cn0 + 14U + 10U, 1U);
    put_be16(cn0 + 14U + 18U, 4096U);
    put_be16(cn0 + 4094U, 14U);

    /* Catalog Node 1 (Leaf) */
    uint8_t *cn1 = img + 3U * bsz;
    cn1[8] = (uint8_t)-1;
    uint16_t rec_offs[16];
    uint16_t rc = 0;
    size_t cur = 14U;
    uint16_t u16[64];
    size_t ulen = 0;

    /* 0: Root folder CNID 2 ("TimeMachineHD") */
    rec_offs[rc++] = (uint16_t)cur;
    ulen = ascii_to_u16("TimeMachineHD", u16);
    cur = write_hfsp_cat_key(cn1, cur, 1U, u16, ulen);
    put_be16(cn1 + cur, 1U); put_be32(cn1 + cur + 8U, 2U);
    cur += 88U;

    /* 1: "\0\0\0\0HFS+ Private Directory Data\r" (CNID 20, parent 2) */
    rec_offs[rc++] = (uint16_t)cur;
    static const char pdd_suf[] = "HFS+ Private Directory Data\r";
    memset(u16, 0, sizeof(u16));
    for (size_t i = 0; i < sizeof(pdd_suf) - 1U; i++) u16[4U + i] = (uint8_t)pdd_suf[i];
    cur = write_hfsp_cat_key(cn1, cur, 2U, u16, 4U + sizeof(pdd_suf) - 1U);
    put_be16(cn1 + cur, 1U); put_be32(cn1 + cur + 8U, 20U);
    cur += 88U;

    /* 2: "Backups.backupdb" (CNID 30, parent 2) */
    rec_offs[rc++] = (uint16_t)cur;
    ulen = ascii_to_u16("Backups.backupdb", u16);
    cur = write_hfsp_cat_key(cn1, cur, 2U, u16, ulen);
    put_be16(cn1 + cur, 1U); put_be32(cn1 + cur + 8U, 30U);
    cur += 88U;

    /* 3: "MacBookPro" (CNID 31, parent 30) */
    rec_offs[rc++] = (uint16_t)cur;
    ulen = ascii_to_u16("MacBookPro", u16);
    cur = write_hfsp_cat_key(cn1, cur, 30U, u16, ulen);
    put_be16(cn1 + cur, 1U); put_be32(cn1 + cur + 8U, 31U);
    cur += 88U;

    /* 4: "2026-11-24-090000" (CNID 32, parent 31) */
    rec_offs[rc++] = (uint16_t)cur;
    ulen = ascii_to_u16("2026-11-24-090000", u16);
    cur = write_hfsp_cat_key(cn1, cur, 31U, u16, ulen);
    put_be16(cn1 + cur, 1U); put_be32(cn1 + cur + 8U, 32U);
    cur += 88U;

    /* 5: Directory hard-link stub "Documents" inside 2026-11-24-090000 (CNID 33, parent 32 -> special_inum=777, 'fldr'/'hlnk') */
    rec_offs[rc++] = (uint16_t)cur;
    ulen = ascii_to_u16("Documents", u16);
    cur = write_hfsp_cat_key(cn1, cur, 32U, u16, ulen);
    put_be16(cn1 + cur, 2U); /* File record acting as directory hard link */
    put_be32(cn1 + cur + 8U, 33U);
    put_be16(cn1 + cur + 42U, 0100444);
    put_be32(cn1 + cur + 44U, 777U);        /* special.iNodeNum = 777 */
    put_be32(cn1 + cur + 48U, 0x666C6472U); /* 'fldr' */
    put_be32(cn1 + cur + 52U, 0x686C6E6BU); /* 'hlnk' */
    cur += 248U;

    /* 6: Target shared directory "dir_777" inside Private Directory Data (CNID 50, parent 20) */
    rec_offs[rc++] = (uint16_t)cur;
    ulen = ascii_to_u16("dir_777", u16);
    cur = write_hfsp_cat_key(cn1, cur, 20U, u16, ulen);
    put_be16(cn1 + cur, 1U); put_be32(cn1 + cur + 8U, 50U);
    cur += 88U;

    /* 7: File "budget.txt" inside dir_777 (CNID 51, parent 50, UF_COMPRESSED inline decmpfs!) */
    rec_offs[rc++] = (uint16_t)cur;
    ulen = ascii_to_u16("budget.txt", u16);
    cur = write_hfsp_cat_key(cn1, cur, 50U, u16, ulen);
    put_be16(cn1 + cur, 2U);
    put_be32(cn1 + cur + 8U, 51U);
    cn1[cur + 41U] = 0x20U; /* UF_COMPRESSED */
    put_be16(cn1 + cur + 42U, 0100644);
    cur += 248U;

    put_be16(cn1 + 10U, rc);
    for (uint16_t i = 0; i < rc; i++) {
        put_be16(cn1 + 4096U - 2U * (i + 1U), rec_offs[i]);
    }

    /* Attributes B-tree Node 0 & Node 1: inline com.apple.decmpfs for CNID 51 */
    uint8_t *an0 = img + 4U * bsz;
    an0[8] = 1;
    put_be16(an0 + 10U, 1U);
    put_be32(an0 + 14U + 10U, 1U);
    put_be16(an0 + 14U + 18U, 4096U);
    put_be16(an0 + 4094U, 14U);

    uint8_t *an1 = img + 5U * bsz;
    an1[8] = (uint8_t)-1;
    put_be16(an1 + 10U, 1U);
    size_t aoff = 14U;
    ulen = ascii_to_u16("com.apple.decmpfs", u16);
    uint16_t aklen = (uint16_t)(12U + ulen * 2U);
    put_be16(an1 + aoff, aklen);
    put_be32(an1 + aoff + 4U, 51U); /* fileID = 51 */
    put_be16(an1 + aoff + 12U, (uint16_t)ulen);
    for (size_t i = 0; i < ulen; i++) put_be16(an1 + aoff + 14U + i * 2U, u16[i]);

    const char *plain_budget = "Recovered Time Machine Q4 Budget: $42,000";
    size_t pblen = strlen(plain_budget);
    uint8_t *aval = an1 + aoff + 2U + aklen;
    put_be32(aval + 0U, 0x00000010U); /* kHFSPlusAttrInlineData */
    put_be32(aval + 12U, (uint32_t)(16U + 1U + pblen));
    put_le32(aval + 16U, 0x636D7066U); /* 'cmpf' */
    put_le32(aval + 20U, 3U);          /* inline zlib / 0xFF uncompressed */
    put_le64(aval + 24U, pblen);
    aval[32] = 0xFFU;                  /* uncompressed inline marker */
    memcpy(aval + 33U, plain_budget, pblen);
    put_be16(an1 + 4094U, (uint16_t)aoff);
}

/* Seal an APFS 4096-byte block with Fletcher-64 checksum */
static void seal_apfs_block(uint8_t *blk, uint64_t oid, uint64_t xid, uint32_t otype, uint32_t osubtype) {
    put_le64(blk + 8U, oid);
    put_le64(blk + 16U, xid);
    put_le32(blk + 24U, otype);
    put_le32(blk + 28U, osubtype);
    put_le64(blk + 0U, aex_fletcher64(blk, 4096U));
}

/* Write a variable-KV APFS leaf B-tree record into a 4096-byte root+leaf node */
typedef struct {
    uint8_t *blk;
    uint16_t count;
    uint16_t key_cur;
    uint16_t val_cur; /* offset from val_end (4096 - 40 = 4056) growing downward */
} apfs_leaf_builder_t;

static void apfs_leaf_init(apfs_leaf_builder_t *b, uint8_t *blk) {
    memset(blk, 0, 4096U);
    b->blk = blk;
    b->count = 0;
    put_le16(blk + 32U, 0x0001U | 0x0002U); /* BTNODE_ROOT | BTNODE_LEAF */
    put_le16(blk + 40U, 0U);
    put_le16(blk + 42U, 256U); /* 32 variable KV TOC entries * 8B */
    b->key_cur = 0U;
    b->val_cur = 0U;
}

static void apfs_leaf_add(apfs_leaf_builder_t *b, const void *k, uint16_t klen, const void *v, uint16_t vlen) {
    size_t key_base = 56U + 256U;
    size_t val_end = 4096U - 40U;
    b->val_cur = (uint16_t)(b->val_cur + vlen);
    memcpy(b->blk + key_base + b->key_cur, k, klen);
    memcpy(b->blk + (val_end - b->val_cur), v, vlen);
    size_t tpos = 56U + (size_t)b->count * 8U;
    put_le16(b->blk + tpos + 0U, b->key_cur);
    put_le16(b->blk + tpos + 2U, klen);
    put_le16(b->blk + tpos + 4U, b->val_cur);
    put_le16(b->blk + tpos + 6U, vlen);
    b->key_cur = (uint16_t)(b->key_cur + klen);
    b->count++;
    put_le32(b->blk + 36U, b->count);
}

/* Build an APFS image with Time Machine role, live FS tree (xid 20), and historical snapshot (xid 10) */
static void build_apfs_snapshot_image(uint8_t *img, size_t img_len) {
    memset(img, 0, img_len);
    const uint32_t bsz = 4096U;

    /* Block layout:
       0: NXSB (xid 20)
       1: Container OMAP phys -> tree at block 2
       2: Container OMAP root+leaf B-tree (oid 100 -> block 3 APSB live)
       3: Live APSB (role=0x0080 TimeMachine, xid 20, vol_omap=4, root_tree_oid=200, snap_meta_tree=8)
       4: Volume OMAP phys -> tree at block 5
       5: Volume OMAP root+leaf B-tree:
            (oid=200, xid=10) -> block 6 (snapshot FS root tree!)
            (oid=200, xid=20) -> block 7 (live FS root tree!)
       6: Snapshot FS B-tree (xid 10): / (ino 2) -> note.txt (ino 16) -> block 10 ("Snapshot v1 content")
       7: Live FS B-tree (xid 20):     / (ino 2) -> note.txt (ino 16) -> block 11 ("Live v2 content")
       8: Snapshot Metadata B-tree:    xid=10 -> sblock_oid=9, name="com.apple.TimeMachine.2026-11-24-103000.backup"
       9: Frozen Snapshot APSB (xid 10, vol_omap=4, root_tree_oid=200)
       10: Data block for snapshot v1
       11: Data block for live v2
    */
    uint8_t *nxsb = img + 0U * bsz;
    put_le32(nxsb + 32U, APFS_NX_MAGIC);
    put_le32(nxsb + 36U, bsz);
    put_le64(nxsb + 40U, img_len / bsz);
    put_le64(nxsb + 160U, 1ULL);   /* nx_omap_oid = block 1 */
    put_le64(nxsb + 184U, 100ULL); /* nx_fs_oid[0] = virtual oid 100 */
    seal_apfs_block(nxsb, 1ULL, 20ULL, 0x80000001U, 0U);

    /* Block 1: Container OMAP */
    uint8_t *comap = img + 1U * bsz;
    put_le64(comap + 48U, 2ULL);
    seal_apfs_block(comap, 1ULL, 20ULL, 0x0000000BU, 0U);

    /* Block 2: Container OMAP B-tree (fixed KV) */
    uint8_t *cot = img + 2U * bsz;
    put_le16(cot + 32U, 0x0001U | 0x0002U | 0x0004U);
    put_le32(cot + 36U, 1U);
    put_le16(cot + 42U, 64U);
    put_le16(cot + 56U, 0U);
    put_le16(cot + 58U, 16U);
    put_le64(cot + 56U + 64U + 0U, 100ULL);
    put_le64(cot + 56U + 64U + 8U, 20ULL);
    put_le64(cot + (4056U - 16U) + 8U, 3ULL); /* paddr = 3 */
    seal_apfs_block(cot, 2ULL, 20ULL, 0x40000002U, 0x0000000BU);

    /* Block 3 (Live APSB) and Block 9 (Snapshot APSB) */
    for (int is_snap = 0; is_snap < 2; is_snap++) {
        uint8_t *apsb = img + (is_snap ? 9U : 3U) * bsz;
        put_le32(apsb + 32U, APFS_VOL_MAGIC);
        put_le64(apsb + 56U, 0x01ULL); /* case-insensitive */
        put_le64(apsb + 88U, 12ULL);
        put_le64(apsb + 128U, 4ULL);   /* apfs_omap_oid = block 4 */
        put_le64(apsb + 136U, 200ULL); /* apfs_root_tree_oid = virtual 200 */
        if (!is_snap) {
            put_le64(apsb + 152U, 8ULL); /* snap_meta_tree_oid = block 8 */
            put_le64(apsb + 216U, 1ULL); /* 1 snapshot */
        }
        put_le64(apsb + 264U, 0x01ULL); /* APFS_FS_UNENCRYPTED */
        strcpy((char *)(apsb + 704U), "TMBackupAPFS");
        put_le16(apsb + 964U, 0x0080U); /* APFS_VOL_ROLE_BACKUP */
        seal_apfs_block(apsb, 100ULL, is_snap ? 10ULL : 20ULL, 0x4000000DU, 0U);
    }

    /* Block 4: Volume OMAP */
    uint8_t *vomap = img + 4U * bsz;
    put_le64(vomap + 48U, 5ULL);
    seal_apfs_block(vomap, 4ULL, 20ULL, 0x0000000BU, 0U);

    /* Block 5: Volume OMAP B-tree with 2 entries: (200, 10)->6 and (200, 20)->7 */
    uint8_t *vot = img + 5U * bsz;
    put_le16(vot + 32U, 0x0001U | 0x0002U | 0x0004U);
    put_le32(vot + 36U, 2U);
    put_le16(vot + 42U, 64U);
    /* Entry 0: (200, 10) -> block 6 */
    put_le16(vot + 56U + 0U, 0U);
    put_le16(vot + 56U + 2U, 16U);
    put_le64(vot + 120U + 0U, 200ULL);
    put_le64(vot + 120U + 8U, 10ULL);
    put_le64(vot + (4056U - 16U) + 8U, 6ULL);
    /* Entry 1: (200, 20) -> block 7 */
    put_le16(vot + 56U + 4U, 16U);
    put_le16(vot + 56U + 6U, 32U);
    put_le64(vot + 120U + 16U, 200ULL);
    put_le64(vot + 120U + 24U, 20ULL);
    put_le64(vot + (4056U - 32U) + 8U, 7ULL);
    seal_apfs_block(vot, 5ULL, 20ULL, 0x40000002U, 0x0000000BU);

    /* Blocks 6 & 7: Snapshot FS B-tree (xid 10) and Live FS B-tree (xid 20) */
    const char *v1_str = "Snapshot v1 content";
    const char *v2_str = "Live v2 updated content";
    memcpy(img + 10U * bsz, v1_str, strlen(v1_str));
    memcpy(img + 11U * bsz, v2_str, strlen(v2_str));

    for (int pass = 0; pass < 2; pass++) {
        uint8_t *fblk = img + (pass == 0 ? 6U : 7U) * bsz;
        const char *txt = (pass == 0) ? v1_str : v2_str;
        uint64_t dblk = (pass == 0) ? 10ULL : 11ULL;
        apfs_leaf_builder_t lb;
        apfs_leaf_init(&lb, fblk);

        /* Record 0: Root inode (oid=2, type=3 INODE) */
        uint8_t ik[8], iv[92];
        put_le64(ik, (3ULL << 60) | 2ULL);
        memset(iv, 0, sizeof(iv));
        put_le64(iv + 0U, 1ULL);
        put_le64(iv + 8U, 2ULL);
        put_le32(iv + 56U, 1U);
        put_le16(iv + 80U, 0040755);
        apfs_leaf_add(&lb, ik, 8U, iv, sizeof(iv));

        /* Record 1: Root dir_rec "note.txt" -> ino 16 (oid=2, type=9 DIR_REC) */
        uint8_t dk[21], dv[18];
        put_le64(dk, (9ULL << 60) | 2ULL);
        put_le32(dk + 8U, 9U); /* hashed drec key: nlen=9 including NUL */
        memcpy(dk + 12U, "note.txt", 9U);
        memset(dv, 0, sizeof(dv));
        put_le64(dv + 0U, 16ULL);
        put_le16(dv + 16U, 8U); /* DT_REG */
        apfs_leaf_add(&lb, dk, sizeof(dk), dv, sizeof(dv));

        /* Record 2: File inode 16 with INO_EXT_TYPE_DSTREAM (oid=16, type=3 INODE) */
        uint8_t fik[8], fiv[92 + 4 + 4 + 40];
        put_le64(fik, (3ULL << 60) | 16ULL);
        memset(fiv, 0, sizeof(fiv));
        put_le64(fiv + 0U, 2ULL);
        put_le64(fiv + 8U, 16ULL);
        put_le32(fiv + 56U, 1U);
        put_le16(fiv + 80U, 0100644);
        put_le16(fiv + 92U, 1U);  /* xf_num_exts = 1 */
        put_le16(fiv + 94U, 40U);
        fiv[96] = 8U;             /* INO_EXT_TYPE_DSTREAM */
        put_le16(fiv + 98U, 40U);
        put_le64(fiv + 100U, strlen(txt)); /* dstream.size */
        put_le64(fiv + 108U, 4096ULL);
        apfs_leaf_add(&lb, fik, 8U, fiv, sizeof(fiv));

        /* Record 3: File extent for private_id=16 (oid=16, type=8 FILE_EXTENT) */
        uint8_t ek[16], ev[24];
        put_le64(ek + 0U, (8ULL << 60) | 16ULL);
        put_le64(ek + 8U, 0ULL);
        put_le64(ev + 0U, 4096ULL);
        put_le64(ev + 8U, dblk);
        put_le64(ev + 16U, 0ULL);
        apfs_leaf_add(&lb, ek, sizeof(ek), ev, sizeof(ev));

        seal_apfs_block(fblk, 200ULL, pass == 0 ? 10ULL : 20ULL, 0x00000002U, 0x0000000EU);
    }

    /* Block 8: Snapshot Metadata B-tree (oid=10, type=1 SNAP_METADATA) */
    uint8_t *sm = img + 8U * bsz;
    apfs_leaf_builder_t slb;
    apfs_leaf_init(&slb, sm);
    const char *sname = "com.apple.TimeMachine.2026-11-24-103000.backup";
    uint16_t snlen = (uint16_t)(strlen(sname) + 1U);
    uint8_t sk[8];
    uint8_t sv[128];
    put_le64(sk, (1ULL << 60) | 10ULL);
    memset(sv, 0, sizeof(sv));
    put_le64(sv + 8U, 9ULL); /* sblock_oid = block 9 */
    put_le64(sv + 16U, 1732450000ULL * 1000000000ULL);
    put_le16(sv + 48U, snlen);
    memcpy(sv + 50U, sname, snlen);
    apfs_leaf_add(&slb, sk, 8U, sv, (uint16_t)(50U + snlen));
    seal_apfs_block(sm, 8ULL, 20ULL, 0x00000002U, 0x00000010U);
}

static int mem_read_cb(void *ctx, uint64_t off, size_t len, void *buf) {
    const uint8_t *base = (const uint8_t *)ctx;
    memcpy(buf, base + off, len);
    return 0;
}

/* Base64 encoder for synthetic UDIF .dmg test */
static void b64_encode(const uint8_t *in, size_t len, char *out) {
    static const char t[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t p = 0;
    for (size_t i = 0; i < len; i += 3U) {
        uint32_t v = ((uint32_t)in[i]) << 16;
        if (i + 1U < len) v |= ((uint32_t)in[i + 1U]) << 8;
        if (i + 2U < len) v |= in[i + 2U];
        out[p++] = t[(v >> 18) & 63U];
        out[p++] = t[(v >> 12) & 63U];
        out[p++] = (i + 1U < len) ? t[(v >> 6) & 63U] : '=';
        out[p++] = (i + 2U < len) ? t[v & 63U] : '=';
    }
    out[p] = '\0';
}

int main(void) {
    printf("==> Running apple-explorer v%s test suite...\n", aex_version());

    /* 1. Classic HFS + Resource Fork */
    {
        size_t sz = 65536U;
        uint8_t *img = (uint8_t *)malloc(sz);
        build_classic_hfs_image(img, sz);

        aex_t *fs = NULL;
        assert(aex_open_io(mem_read_cb, img, sz, NULL, &fs) == 0);
        assert(aex_fs_kind(fs) == AEX_FS_HFS);
        assert(strcmp(aex_volume_name(fs), "RetroMac9") == 0);

        uint64_t ino = 0;
        assert(aex_lookup_path(fs, "/ReadMe.txt", &ino) == 0);
        char buf[64] = "";
        size_t got = 0;
        assert(aex_read(fs, ino, 0, sizeof(buf), buf, &got) == 0);
        assert(strcmp(buf, "Welcome to Classic HFS!") == 0);

        char rbuf[64] = "";
        assert(aex_read_rsrc(fs, ino, 0, sizeof(rbuf), rbuf, &got) == 0);
        assert(strcmp(rbuf, "RSRC_FORK_BYTES") == 0);
        aex_close(fs);

        /* Wrap Classic HFS inside a UDIF .dmg ('koly' + 'mish' blkx) and test container unwrapping */
        const char *dmg_path = "test_udif_tmp.dmg";
        uint8_t mish[204 + 80];
        memset(mish, 0, sizeof(mish));
        put_be32(mish + 0U, 0x6D697368U);
        put_be64(mish + 16U, sz / 512U);
        put_be32(mish + 200U, 2U);
        put_be32(mish + 204U, 0x00000001U); /* raw run */
        put_be64(mish + 204U + 16U, sz / 512U);
        put_be64(mish + 204U + 32U, sz);
        put_be32(mish + 244U, 0xFFFFFFFFU); /* end */

        char mish_b64[1024];
        b64_encode(mish, sizeof(mish), mish_b64);
        char xml[2048];
        int xlen = snprintf(xml, sizeof(xml),
            "<?xml version=\"1.0\"?><plist><dict><key>resource-fork</key><dict>"
            "<key>blkx</key><array><dict><key>Data</key><data>%s</data></dict></array>"
            "</dict></dict></plist>", mish_b64);

        uint8_t koly[512];
        memset(koly, 0, sizeof(koly));
        memcpy(koly, "koly", 4U);
        put_be64(koly + 216U, sz);
        put_be64(koly + 224U, (uint64_t)xlen);
        put_be64(koly + 492U, sz / 512U);

        FILE *dfp = fopen(dmg_path, "wb");
        assert(dfp != NULL);
        assert(fwrite(img, 1U, sz, dfp) == sz);
        assert(fwrite(xml, 1U, (size_t)xlen, dfp) == (size_t)xlen);
        assert(fwrite(koly, 1U, 512U, dfp) == 512U);
        fclose(dfp);

        aex_probe_info_t pinfo;
        assert(aex_probe_path(dmg_path, NULL, &pinfo) == 0);
        assert(pinfo.container_kind == AEX_CONTAINER_UDIF_DMG);
        assert(pinfo.primary_fs == AEX_FS_HFS);
        remove(dmg_path);
        free(img);
        printf("  [PASS] Classic HFS + Resource Fork + UDIF .dmg container\n");
    }

    /* 2. HFS+ Backups.backupdb Directory Hard Links + decmpfs + .sparsebundle Container */
    {
        size_t sz = 65536U;
        uint8_t *img = (uint8_t *)malloc(sz);
        build_hfsplus_time_machine_image(img, sz);

        /* Write as a .sparsebundle directory (band-size = 16384, bands/0 and bands/1, omitting bands/2..3) */
        const char *sb_dir = "test_tm_tmp.sparsebundle";
        const char *bands_dir = "test_tm_tmp.sparsebundle/bands";
        MKDIR_P(sb_dir);
        MKDIR_P(bands_dir);

        FILE *pf = fopen("test_tm_tmp.sparsebundle/Info.plist", "wb");
        assert(pf != NULL);
        fprintf(pf,
            "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
            "<plist version=\"1.0\"><dict>\n"
            "  <key>band-size</key><integer>16384</integer>\n"
            "  <key>size</key><integer>%zu</integer>\n"
            "</dict></plist>\n", sz);
        fclose(pf);

        FILE *b0 = fopen("test_tm_tmp.sparsebundle/bands/0", "wb");
        assert(b0 != NULL);
        assert(fwrite(img, 1U, 16384U, b0) == 16384U);
        fclose(b0);
        FILE *b1 = fopen("test_tm_tmp.sparsebundle/bands/1", "wb");
        assert(b1 != NULL);
        assert(fwrite(img + 16384U, 1U, 16384U, b1) == 16384U);
        fclose(b1);

        aex_probe_info_t pinfo;
        assert(aex_probe_path(sb_dir, NULL, &pinfo) == 0);
        assert(pinfo.container_kind == AEX_CONTAINER_SPARSEBUNDLE);
        assert(pinfo.primary_fs == AEX_FS_HFSPLUS);
        assert(pinfo.has_backups_db == 1);

        aex_t *fs = NULL;
        assert(aex_open_path(sb_dir, NULL, &fs) == 0);

        uint64_t bfile_ino = 0;
        assert(aex_lookup_path(fs, "/Backups.backupdb/MacBookPro/2026-11-24-090000/Documents/budget.txt", &bfile_ino) == 0);

        char out[128] = "";
        size_t got = 0;
        assert(aex_read(fs, bfile_ino, 0, sizeof(out), out, &got) == 0);
        assert(strcmp(out, "Recovered Time Machine Q4 Budget: $42,000") == 0);

        assert(aex_extract_path(fs, "/Backups.backupdb/MacBookPro/2026-11-24-090000/Documents/budget.txt", "extracted_budget_tmp.txt") == 0);
        FILE *ef = fopen("extracted_budget_tmp.txt", "rb");
        assert(ef != NULL);
        char ebuf[128] = "";
        size_t er = fread(ebuf, 1U, sizeof(ebuf) - 1U, ef);
        fclose(ef);
        assert(er == strlen("Recovered Time Machine Q4 Budget: $42,000"));
        assert(strcmp(ebuf, "Recovered Time Machine Q4 Budget: $42,000") == 0);

        aex_close(fs);
        remove("extracted_budget_tmp.txt");
        remove("test_tm_tmp.sparsebundle/bands/0");
        remove("test_tm_tmp.sparsebundle/bands/1");
        remove("test_tm_tmp.sparsebundle/Info.plist");
        rmdir(bands_dir);
        rmdir(sb_dir);
        free(img);
        printf("  [PASS] HFS+ Backups.backupdb hard-link resolution + decmpfs + .sparsebundle\n");
    }

    /* 3. APFS Container + Snapshot Metadata B-tree + Time Machine /Backups Timeline */
    {
        size_t sz = 16U * 4096U;
        uint8_t *img = (uint8_t *)malloc(sz);
        build_apfs_snapshot_image(img, sz);

        aex_t *fs = NULL;
        assert(aex_open_io(mem_read_cb, img, sz, NULL, &fs) == 0);
        assert(aex_fs_kind(fs) == AEX_FS_APFS);
        assert(strcmp(aex_volume_name(fs), "TMBackupAPFS") == 0);

        aex_snapshot_desc_t snaps[8];
        size_t scnt = 0;
        assert(aex_list_snapshots(fs, snaps, 8U, &scnt) == 0);
        assert(scnt == 1U);

        uint64_t live_ino = 0, snap_ino = 0;
        assert(aex_lookup_path(fs, "/note.txt", &live_ino) == 0);
        assert(aex_lookup_path(fs, "/Backups/2026-11-24-103000/note.txt", &snap_ino) == 0);

        char lbuf[64] = "", sbuf[64] = "";
        size_t got = 0;
        assert(aex_read(fs, live_ino, 0, sizeof(lbuf), lbuf, &got) == 0);
        assert(strcmp(lbuf, "Live v2 updated content") == 0);

        assert(aex_read(fs, snap_ino, 0, sizeof(sbuf), sbuf, &got) == 0);
        assert(strcmp(sbuf, "Snapshot v1 content") == 0);
        aex_close(fs);
        free(img);
        printf("  [PASS] APFS Fletcher-64 checkpoints + snap_meta_tree + /Backups timeline\n");
    }

#ifdef __APPLE__
    /* 4. Live Interoperability Check Against Real macOS hdiutil APFS & HFS+ Sparsebundle */
    {
        char cmd[1024];
        const char *apfs_dmg = "/tmp/aex_real_apfs_test.dmg";
        unlink(apfs_dmg);
        snprintf(cmd, sizeof(cmd),
                 "rm -rf /tmp/aex_src_dir && mkdir -p /tmp/aex_src_dir/sub && "
                 "printf 'Hello from real macOS APFS!' > /tmp/aex_src_dir/sub/real.txt && "
                 "hdiutil create -size 16m -fs APFS -volname RealAPFS -srcfolder /tmp/aex_src_dir -format UDRW -ov %s >/dev/null 2>&1 && "
                 "rm -rf /tmp/aex_src_dir",
                 apfs_dmg);
        if (system(cmd) == 0) {
            aex_t *fs = NULL;
            assert(aex_open_path(apfs_dmg, NULL, &fs) == 0);
            uint64_t rino = 0;
            assert(aex_lookup_path(fs, "/sub/real.txt", &rino) == 0);
            char rbuf[64] = "";
            size_t got = 0;
            assert(aex_read(fs, rino, 0, sizeof(rbuf), rbuf, &got) == 0);
            assert(strcmp(rbuf, "Hello from real macOS APFS!") == 0);
            aex_close(fs);
            unlink(apfs_dmg);
            printf("  [PASS] Real macOS hdiutil-formatted GPT + APFS image verification\n");
        }
    }
#endif

    printf("==> All apple-explorer tests PASSED.\n");
    return 0;
}
