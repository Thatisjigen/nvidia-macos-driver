/* nvmtl_translator_tag.h - the shader cache's build tag: which translator made an entry.
   The tag was the SHA-256 of the whole libnvmtl_translate.dylib file, code signature included. Signing the same
   translator again (codesign --deep over the bundle after one library in it was replaced) gave the file another hash:
   every process took its cache for another build's, swept it, and at the next start WindowServer's main thread waited
   on 450 translations for a minute, the watchdog killed it and the screen stayed black for two minutes (RTX 5060 Ti,
   macOS 26.7.2, 2026-11-14).
   The tag is now a digest of what makes the translator translate the way it does and that signing does not touch: its
   LC_UUID and the file bytes of every segment except __LINKEDIT (where the signature lives), without the Mach-O header,
   the load commands and the padding after them at the start of __TEXT (LC_CODE_SIGNATURE is added there and
   __LINKEDIT's sizes change there with a signature).
   A file this cannot read as a thin 64-bit Mach-O is hashed whole, as before.
   Pure C over a memory image, so tools/test_translator_tag.py proves it without the plugin. */
#ifndef NVMTL_TRANSLATOR_TAG_H
#define NVMTL_TRANSLATOR_TAG_H
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <CommonCrypto/CommonDigest.h>

#define NVMTL_TT_MH_MAGIC_64 0xfeedfacfu
#define NVMTL_TT_LC_SEGMENT_64 0x19u
#define NVMTL_TT_LC_UUID 0x1bu

/* Feeds the signature-independent parts of a thin 64-bit Mach-O image to `ctx`. Returns 1, or 0 (nothing fed that the
   caller may keep: start a fresh context) when the image is not one or a command points outside it. */
static inline int nvmtl_tt_feed_content(const uint8_t *img, size_t size, CC_SHA256_CTX *ctx)
{
    uint32_t magic, ncmds, sizeofcmds;
    if (!img || size < 32) return 0;
    memcpy(&magic, img, 4); memcpy(&ncmds, img + 16, 4); memcpy(&sizeofcmds, img + 20, 4);
    if (magic != NVMTL_TT_MH_MAGIC_64 || (size_t)sizeofcmds > size - 32) return 0;
    const size_t hdr_end = 32 + (size_t)sizeofcmds;
    size_t at = 32; unsigned segments = 0;
    for (uint32_t i = 0; i < ncmds; i++) {
        uint32_t cmd, cmdsize;
        if (at + 8 > hdr_end) return 0;
        memcpy(&cmd, img + at, 4); memcpy(&cmdsize, img + at + 4, 4);
        if (cmdsize < 8 || (size_t)cmdsize > hdr_end - at) return 0;
        if (cmd == NVMTL_TT_LC_UUID && cmdsize >= 24) {
            CC_SHA256_Update(ctx, "uuid", 4); CC_SHA256_Update(ctx, img + at + 8, 16);
        } else if (cmd == NVMTL_TT_LC_SEGMENT_64 && cmdsize >= 72) {
            char name[17]; uint64_t fileoff, filesize;
            memcpy(name, img + at + 8, 16); name[16] = 0;
            memcpy(&fileoff, img + at + 40, 8); memcpy(&filesize, img + at + 48, 8);
            if (strcmp(name, "__LINKEDIT") && filesize) {
                if (fileoff > size || filesize > size - fileoff) return 0;
                uint64_t from = fileoff, to = fileoff + filesize;
                if (from < hdr_end) {
                    /* The header and load commands open this segment (__TEXT), followed by padding up to its first
                       section. Signing an unsigned file adds a load command there, so the padding is not content
                       either: start at the first section. */
                    uint32_t nsects; memcpy(&nsects, img + at + 64, 4);
                    uint64_t first = to;
                    if ((size_t)nsects > (cmdsize - 72) / 80) return 0;
                    for (uint32_t k = 0; k < nsects; k++) {
                        uint32_t off; memcpy(&off, img + at + 72 + (size_t)k * 80 + 48, 4);
                        if (off && off >= hdr_end && off < first) first = off;
                    }
                    from = first;
                }
                CC_SHA256_Update(ctx, name, 16);
                /* CC_LONG is 32 bits: a segment is fed in pieces */
                for (uint64_t p = from; p < to; ) {
                    const uint64_t n = to - p > (1u << 30) ? (1u << 30) : to - p;
                    CC_SHA256_Update(ctx, img + p, (CC_LONG)n); p += n;
                }
                segments++;
            }
        }
        at += cmdsize;
    }
    return segments > 0;
}

/* Writes "sha256-<64 hex>" for the translator image into tag[cap] (cap >= 72). Returns 2 when the digest is of the
   signature-independent content, 1 when the image was hashed whole (not a thin 64-bit Mach-O), 0 when cap is short. */
static inline int nvmtl_translator_tag_of_image(const uint8_t *img, size_t size, char *tag, size_t cap)
{
    if (!tag || cap < 72) return 0;
    CC_SHA256_CTX ctx; CC_SHA256_Init(&ctx);
    int how = 2;
    if (!nvmtl_tt_feed_content(img, size, &ctx)) {
        how = 1; CC_SHA256_Init(&ctx);
        for (size_t p = 0; p < size; ) { const size_t n = size - p > (1u << 30) ? (1u << 30) : size - p; CC_SHA256_Update(&ctx, img + p, (CC_LONG)n); p += n; }
    }
    uint8_t d[CC_SHA256_DIGEST_LENGTH]; CC_SHA256_Final(d, &ctx);
    memcpy(tag, "sha256-", 7);
    for (size_t i = 0; i < sizeof d; i++) snprintf(tag + 7 + 2 * i, 3, "%02x", d[i]);
    return how;
}
#endif
