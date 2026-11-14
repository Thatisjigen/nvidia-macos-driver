#!/usr/bin/env python3
"""A kernel that writes a BGR10A2Unorm texture reaches it through a twin in the other channel order.

VK_FORMAT_A2R10G10B10_UNORM_PACK32 (58) cannot be a storage image in Vulkan, so the storage twin of a BGR10A2Unorm
texture was refused and every write of the kernel was lost. ImageIO writes its HDR output that way: an iPhone photo
with a gain map (4032x3024, iPhone 14 Pro) decoded with kCGImageSourceDecodeToHDR came back all zero on an RTX 5060 Ti
(driver 3.0, macOS 26.7.2; the log said "storage twin: VkFormat 58 4032x3024 cannot be a storage image in optimal
tiling either - shader writes to it are LOST"), while the same program without a Metal device drew the photo (channel
means 0.4229 0.3230 0.2596). The twin is now A2B10G10R10 (64), the same layout in the order Vulkan requires as a
storage format, filled and written back by blits, which convert by channel. With it the photo's means are
0.4263 0.3231 0.2619.

This compiles nvmtl_storage_view_for and nvmtl_storage_twins_out out of NVMTLObjects.m over a model of the Vulkan
calls (a format is a storage format or not; every create, blit and copy is written down) and asks:

  bgr10   format 58: a twin of format 64, a view, filled by a blit and written back by a blit
  same    a storage format (RGBA8, 37): a twin of the same format, filled and written back by copies
  srgb    an sRGB format (43): the half-float twin and blits, as before
  none    a format with no storage twin and no sibling (A2R10G10B10 UINT, 62): no view, and the log says the writes are lost
  control v3.0's function gives format 58 no view (the control that must fire)"""
import pathlib
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[1]
REL = "plugin/NVMTLObjects.m"
fails = 0


def arm(name, ok, detail=""):
    global fails
    fails += not ok
    print(("ok   " if ok else "FAIL ") + name + ("" if ok else f" ({str(detail).strip()[-600:]})"))


def cut(src, start, end):
    i = src.index(start)
    return src[i:src.index(end, i)]


HARNESS = r'''
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <stdarg.h>
typedef struct { void *img, *view; uint32_t w, h, mips, layers, samples, mtl_type, bw, fmt, bpp; int storage; } nvk_image;
typedef struct { int in_rp; } nvk_cmdbuf;
static void nvlog(const char *f, ...) { va_list a; va_start(a, f); printf("LOG "); vprintf(f, a); printf("\n"); va_end(a); }
static int storage_ok(uint32_t f) { return f == 37 || f == 64 || f == 97; }
static int nvmtl_tile_on(void) { return 0; }
static int nvmtl_vk_ms_storage_ok(void) { return 0; }
static int nvmtl_vk_srgb_format(uint32_t f) { return f == 43 || f == 50; }
static int nvmtl_vk_image_create_ms_storage(uint32_t w, uint32_t h, uint32_t f, uint32_t bpp, uint32_t s, nvk_image *o) { return -1; }
static int g_live;
static int nvmtl_vk_image_create_typed(uint32_t w, uint32_t h, uint32_t f, uint32_t bpp, int a8, uint32_t mips, int storage, uint32_t type, uint32_t layers, nvk_image *o) {
    memset(o, 0, sizeof *o); o->img = malloc(1); o->view = o->img; o->w = w; o->h = h; o->fmt = f; o->bpp = bpp; o->mtl_type = type;
    o->storage = storage && storage_ok(f); g_live++;
    printf("CREATE %u bpp %u storage %d\n", f, bpp, o->storage); return 0; }
static void nvmtl_vk_image_destroy(nvk_image *i) { if (i->img) { free(i->img); g_live--; printf("DESTROY %u\n", i->fmt); } }
static int nvmtl_vk_cmd_blit_image_2d(nvk_cmdbuf *c, nvk_image *s, nvk_image *d, uint32_t w, uint32_t h) { printf("BLIT %u->%u\n", s->fmt, d->fmt); return 0; }
static int nvmtl_vk_cmd_copy_image_sub(nvk_cmdbuf *c, nvk_image *s, uint32_t a, uint32_t b, uint32_t e, uint32_t f, nvk_image *d, uint32_t g, uint32_t h, uint32_t i, uint32_t j, uint32_t w, uint32_t hh) { printf("COPY %u->%u\n", s->fmt, d->fmt); return 0; }
static int nvmtl_vk_cmd_prepare_sampled_image(nvk_cmdbuf *c, nvk_image *i, int storage) { return 0; }
@interface NVMTLTexture : NSObject { @public nvk_image _im; id _stex, _surf; NVMTLTexture *_parent; NSUInteger _usage; nvk_image _stTwin; BOOL _stTwinTried; }
- (nvk_image *)nvi; - (void *)nvview;
@end
@implementation NVMTLTexture
- (nvk_image *)nvi { return &_im; }
- (void *)nvview { return _im.view; }
@end
@interface NVMTLCommandBuffer : NSObject { @public nvk_cmdbuf _c; NSMutableSet *_stTwins, *_surfDirty; }
@end
@implementation NVMTLCommandBuffer
@end
static void nvmtl_retain_resource(NVMTLCommandBuffer *cb, id r) {}
static void nvmtl_surf_dirty(NVMTLCommandBuffer *cb, NVMTLTexture *t) { printf("DIRTY %u\n", t->_im.fmt); }
@@VIEW@@
@@OUT@@
int main(int argc, char **argv) { @autoreleasepool {
    const uint32_t fmt = (uint32_t)atoi(argv[1]);
    NVMTLTexture *t = [NVMTLTexture new]; t->_im = (nvk_image){ .img = &t, .view = &t, .w = 64, .h = 48, .mtl_type = 2, .fmt = fmt, .bpp = 4 };
    NVMTLCommandBuffer *cb = [NVMTLCommandBuffer new];
    void *v = nvmtl_storage_view_for(cb, t, YES);
    printf("VIEW %s twin %u\n", v ? "yes" : "no", t->_stTwin.fmt);
    void *v2 = nvmtl_storage_view_for(cb, t, YES);
    printf("AGAIN %s\n", v2 == v ? "same" : "other");
    if (cb->_stTwins.count) nvmtl_storage_twins_out(cb);
    printf("LIVE %d\n", g_live);
} return 0; }
'''


def build(src, work, tag):
    view = cut(src, "static void *nvmtl_storage_view_for(", "NSDictionary *nvmtl_function_reflection(")
    out = cut(src, "static void nvmtl_storage_twins_out(", "static int g_trace_lines;")
    m = work / f"{tag}.m"
    m.write_text(HARNESS.replace("@@VIEW@@", view).replace("@@OUT@@", out))
    cc = subprocess.run(["xcrun", "clang", "-fobjc-arc", "-O1", "-Wno-unused-value", "-o", str(work / tag), str(m),
                         "-framework", "Foundation", "-framework", "Metal"], capture_output=True, text=True)
    return cc.returncode == 0, cc.stderr


def run(work, tag, fmt):
    return subprocess.run([str(work / tag), str(fmt)], capture_output=True, text=True).stdout.splitlines()


with tempfile.TemporaryDirectory() as d:
    work = pathlib.Path(d)
    ok, err = build((ROOT / REL).read_text(), work, "now")
    arm("the production functions compile out of NVMTLObjects.m", ok, err)
    if not ok:
        sys.exit(1)

    o = run(work, "now", 58)
    arm("bgr10: format 58 gets a view", "VIEW yes twin 64" in o, o)
    arm("  its twin is format 64 and a storage image", "CREATE 64 bpp 4 storage 1" in o, o)
    arm("  the refused twin of format 58 is destroyed, one image left", "DESTROY 58" in o and "LIVE 1" in o, o)
    arm("  it is filled by a blit, which converts by channel", "BLIT 58->64" in o and not any(l.startswith("COPY") for l in o), o)
    arm("  and written back by a blit, the texture marked written", "BLIT 64->58" in o and "DIRTY 58" in o, o)
    arm("  asked again in the same command buffer: the same view, no second fill", "AGAIN same" in o and o.count("BLIT 58->64") == 1, o)
    arm("  nothing is reported lost", not any("LOST" in l for l in o), o)

    o = run(work, "now", 37)
    arm("same: a storage format keeps a twin of its own format", "VIEW yes twin 37" in o and "CREATE 37 bpp 4 storage 1" in o, o)
    arm("  filled and written back by copies", "COPY 37->37" in o and o.count("COPY 37->37") == 2 and not any(l.startswith("BLIT") for l in o), o)

    o = run(work, "now", 43)
    arm("srgb: an sRGB format keeps its half-float twin and blits", "VIEW yes twin 97" in o and "BLIT 43->97" in o and "BLIT 97->43" in o, o)

    o = run(work, "now", 62)
    arm("none: a format with no storage twin and no sibling gets no view", "VIEW no twin 0" in o, o)
    arm("  the log says its writes are lost, and no image is left", any("LOST" in l for l in o) and "LIVE 0" in o, o)

    old = subprocess.run(["git", "-C", str(ROOT), "show", f"v3.0:{REL}"], capture_output=True, text=True)
    arm("v3.0's source can be read for the control (full clone needed)", old.returncode == 0, old.stderr)
    if old.returncode == 0:
        ok, err = build(old.stdout, work, "v30")
        arm("control: v3.0's functions compile in the same harness", ok, err)
        if ok:
            o = run(work, "v30", 58)
            arm("control: v3.0 gives format 58 no view and reports its writes lost",
                "VIEW no twin 0" in o and any("LOST" in l for l in o), o)
            o = run(work, "v30", 37)
            arm("control: v3.0 treats a storage format the same way", "VIEW yes twin 37" in o and o.count("COPY 37->37") == 2, o)

print(f"{fails} failed")
sys.exit(1 if fails else 0)
