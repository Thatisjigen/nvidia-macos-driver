// SPDX-License-Identifier: MIT
// Copyright (c) 2026 kevineduardo
// translator_tag.c <libnvmtl_translate.dylib>...: for each file, the shader cache's build tag as the plugin computes it
// now (plugin/nvmtl_translator_tag.h: the content without the code signature) and as it did before (the whole file).
//   new sha256-<hex>  old sha256-<hex>  <path>
// Build: clang -O1 -I plugin -o translator_tag tools/translator_tag.c
#include <stdio.h>
#include <stdlib.h>
#include "nvmtl_translator_tag.h"
int main(int c, char **v)
{
    if (c < 2) { fprintf(stderr, "usage: %s <libnvmtl_translate.dylib>...\n", v[0]); return 2; }
    for (int i = 1; i < c; i++) {
        FILE *f = fopen(v[i], "rb"); if (!f) { perror(v[i]); return 1; }
        fseek(f, 0, SEEK_END); const long n = ftell(f); fseek(f, 0, SEEK_SET);
        unsigned char *b = malloc(n > 0 ? (size_t)n : 1);
        if (!b || fread(b, 1, (size_t)n, f) != (size_t)n) { fprintf(stderr, "%s: read failed\n", v[i]); return 1; }
        fclose(f);
        char tag[80], old[80] = "sha256-"; unsigned char d[CC_SHA256_DIGEST_LENGTH];
        if (!nvmtl_translator_tag_of_image(b, (size_t)n, tag, sizeof tag)) return 1;
        CC_SHA256_CTX x; CC_SHA256_Init(&x);
        for (size_t p = 0; p < (size_t)n; ) { const size_t k = (size_t)n - p > (1u << 30) ? (1u << 30) : (size_t)n - p; CC_SHA256_Update(&x, b + p, (CC_LONG)k); p += k; }
        CC_SHA256_Final(d, &x);
        for (size_t k = 0; k < sizeof d; k++) snprintf(old + 7 + 2 * k, 3, "%02x", d[k]);
        printf("new %s  old %s  %s\n", tag, old, v[i]); free(b);
    }
    return 0;
}
