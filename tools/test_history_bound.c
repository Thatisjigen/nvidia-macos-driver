/* test_history_bound.c - the fault-report histories stay bounded and keep every live record, proven on the host
   without a GPU. Build and run anywhere:  cc -I plugin tools/test_history_bound.c -o /tmp/test_history_bound && /tmp/test_history_bound */
#include "nvmtl_slots.h"

static int fails;
#define ARM(name, cond) do { const int ok = !!(cond); printf("%s  %s\n", ok ? "ok  " : "FAIL", name); if (!ok) fails++; } while (0)

/* Handles are distinct and non-NULL, as the driver's are. The live ones are made first and never die. */
static const void *h(uintptr_t base, uint32_t i) { return (const void *)(base + (uintptr_t)i * 16); }
/* The budget is stated here, not read from the header: this test says what the histories must fit in, so it also
   builds against a revision that has no cap at all and fails there at runtime. Each history is driven past its own
   cap, so every one of them actually sweeps. */
enum { IM_CAP = 262144, VW_CAP = 524288, MM_CAP = 65536 };
enum { LIVE = 64, CHURN = IM_CAP + IM_CAP / 2, VW_CHURN = VW_CAP + VW_CAP / 2, MM_CHURN = MM_CAP + MM_CAP / 2 };

int main(void)
{
    /* the image history: a compositor's churn, with a few images that live through all of it */
    {   nvmtl_im_state im; memset(&im, 0, sizeof im);
        for (uint32_t k = 0; k < LIVE; k++) nvmtl_im_add(&im, h(0x8000000, k), NULL, 0, 4096, h(0x8100000, k), 8, 8, 37, 0, 0, 0, 1);
        for (uint32_t i = 0; i < CHURN; i++) { nvmtl_im_add(&im, h(0x100000, i), NULL, 0, 4096, h(0x110000, i), 8, 8, 37, 0, 0, 0, 100 + i); nvmtl_im_died(&im, h(0x100000, i), 200 + i); }
        ARM("the image history stops at its cap instead of doubling forever", im.cap <= IM_CAP && im.n <= IM_CAP);
        ARM("  so its bytes stay within the budget", (size_t)im.cap * sizeof(nvmtl_im_entry) <= 30u * 1024 * 1024);
        int all_live = 1;
        for (uint32_t k = 0; k < LIVE; k++) { const nvmtl_im_entry *e = nvmtl_im_by_img(&im, h(0x8000000, k)); if (!e || e->died) all_live = 0; }
        ARM("every image that is still alive is still resolvable after the sweeps", all_live && im.live == LIVE);
        ARM("the newest death is kept: a fault report can still name it", nvmtl_im_by_img(&im, h(0x100000, CHURN - 1)) != NULL);
        ARM("the oldest deaths are gone (negative control: a raised cap alone would keep them)", nvmtl_im_by_img(&im, h(0x100000, 0)) == NULL);
        /* a handle the driver reissues must resolve to its newest record, not to a swept or stale one */
        nvmtl_im_add(&im, h(0x8000000, 0), NULL, 0, 8192, h(0x8100000, 0), 16, 16, 37, 0, 0, 0, 999);
        const nvmtl_im_entry *re = nvmtl_im_by_img(&im, h(0x8000000, 0));
        ARM("a reissued image handle resolves to its newest record after a sweep", re && re->w == 16 && re->seq == 999 && !re->died);
        free(im.e); }

    /* a live image is never dropped to bound the table: with nothing dead to sweep, the history grows instead */
    {   nvmtl_im_state im; memset(&im, 0, sizeof im);
        for (uint32_t i = 0; i <= IM_CAP; i++) nvmtl_im_add(&im, h(0x200000, i), NULL, 0, 4096, NULL, 8, 8, 37, 0, 0, 0, i);
        ARM("with every record live the history grows rather than dropping one", im.n == IM_CAP + 1 && im.live == IM_CAP + 1);
        ARM("  the first live image is still resolvable", nvmtl_im_by_img(&im, h(0x200000, 0)) != NULL);
        free(im.e); }

    /* the view history */
    {   nvmtl_vw_state vw; memset(&vw, 0, sizeof vw);
        for (uint32_t k = 0; k < LIVE; k++) nvmtl_vw_add(&vw, h(0x9000000, k), h(0x8000000, k));
        for (uint32_t i = 0; i < VW_CHURN; i++) { nvmtl_vw_add(&vw, h(0x120000, i), h(0x100000, i)); nvmtl_vw_died(&vw, h(0x120000, i), 200 + i); }
        ARM("the view history stops at its cap", vw.cap <= VW_CAP && vw.n <= VW_CAP);
        ARM("  so its bytes stay within the budget", (size_t)vw.cap * sizeof(nvmtl_vw_entry) <= 44u * 1024 * 1024);
        int all_live = 1;
        for (uint32_t k = 0; k < LIVE; k++) if (!nvmtl_vw_find_live(&vw, h(0x9000000, k))) all_live = 0;
        ARM("every live view is still found through the rebuilt live index", all_live);
        ARM("the newest dead view record is kept with its death", nvmtl_vw_find(&vw, h(0x120000, VW_CHURN - 1)) != NULL);
        ARM("the oldest dead view records are gone (negative control)", nvmtl_vw_find(&vw, h(0x120000, 0)) == NULL);
        /* the recorder must still be able to fill the newest live record after a sweep */
        nvmtl_vw_set_tic(&vw, h(0x9000000, 0), 7, 0x4c3200000ull, 10263, 0x4c3200000ull, 4000);
        const nvmtl_vw_entry *ve = nvmtl_vw_find_live(&vw, h(0x9000000, 0));
        ARM("the tic recorder still reaches a live view after a sweep", ve && ve->t_idx == 10263 && ve->seq == 4000);
        free(vw.e); }

    /* the memory history, including the unbased count the report prints */
    {   nvmtl_mm_state mm; memset(&mm, 0, sizeof mm);
        for (uint32_t k = 0; k < LIVE; k++) nvmtl_mm_add(&mm, h(0xa000000, k), 0x300000000ull + k * 4096, 4096, 0, "live", 1);
        for (uint32_t i = 0; i < MM_CHURN; i++) {
            nvmtl_mm_add(&mm, h(0x130000, i), (i % 4) ? 0x400000000ull + i * 4096 : 0, 4096, 0, "churn", 100 + i);
            nvmtl_mm_died(&mm, h(0x130000, i), 200 + i);
        }
        ARM("the memory history stops at its cap", mm.cap <= MM_CAP && mm.n <= MM_CAP);
        ARM("  so its bytes stay within the budget", (size_t)mm.cap * sizeof(nvmtl_mm_entry) <= 6u * 1024 * 1024);
        int all_live = 1; uint64_t unbased = 0;
        for (uint32_t k = 0; k < LIVE; k++) { const nvmtl_mm_entry *e = nvmtl_mm_by_mem(&mm, h(0xa000000, k)); if (!e || e->died) all_live = 0; }
        for (uint32_t i = 0; i < mm.n; i++) if (!mm.e[i].base) unbased++;
        ARM("every live memory is still resolvable after the sweeps", all_live && mm.live == LIVE);
        ARM("the unbased count describes the records that remain, not ones already swept", mm.unbased == unbased && unbased > 0);
        ARM("the newest death is kept", nvmtl_mm_by_mem(&mm, h(0x130000, MM_CHURN - 1)) != NULL);
        ARM("the oldest deaths are gone (negative control)", nvmtl_mm_by_mem(&mm, h(0x130000, 0)) == NULL);
        /* an address inside a live memory still resolves to it, with the offset the report prints */
        char line[300]; nvmtl_mm_describe(&mm, 0x300000000ull + 2048, line, sizeof line);
        ARM("an address in a live memory still resolves to it with its offset", strstr(line, "memory 0xa000000") != NULL && strstr(line, "+ offset 2048") != NULL);
        free(mm.e); }

    printf("test_history_bound: %d failed\n", fails);
    return fails ? 1 : 0;
}