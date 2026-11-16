#!/usr/bin/env python3
"""Where libnvencvt (the vendored NVENC / NVDEC codecs) registers: its decoders in every process, its encoders by selection.

Ada had no hardware encoder and no VP9 or AV1 decoder because the library registered only when a user switched it on in
nvrm610.conf; and Safari, QuickTime and FaceTime, which decode inside VTDecoderXPCService, never saw it because that
process registered only the built-in decoders. Later a video still decoded on NVDEC only in the processes a list named.
The encoders' selection function is compiled out of the plugin source and run on a table; the decoders' wiring (the
decode service, every other process at load, device initialisation, WebKit's GPU process) is checked in the text.

usage: test_nvencvt_selection.py [repo root]"""
import pathlib, platform, shutil, subprocess, sys, tempfile

root = pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else pathlib.Path(__file__).resolve().parents[1])
src = (root / 'plugin/NVMTLDevice.m').read_text()
fails = 0


def check(name, ok, detail=''):
    global fails
    print(('ok   ' if ok else 'FAIL ') + name + (f' ({detail})' if detail else ''))
    fails += 0 if ok else 1


def function(signature):
    i = src.find(signature)
    if i < 0:
        return None
    return src[i:src.index('\n}\n', i) + 3]


sel = function('static bool nvmtl_nvencvt_selected(')
gen = function('static bool nvmtl_gen_is(')
check('the plugin has a selection function for libnvencvt', sel is not None)

if sel and shutil.which('clang') and platform.system() == 'Darwin':
    harness = ('#include <stdbool.h>\n#include <stdio.h>\n#include <string.h>\n' + gen + sel +
               'int main(void) {\n'
               '  const char *sels[] = { NULL, "", "0", "1", "obs,Parsec", "VTDecoderXPCService" };\n'
               '  const char *gens[] = { "Ada", "Ampere", "Blackwell", "Turing GTX 16", "Turing", NULL };\n'
               '  const char *procs[] = { "Parsec", "VTDecoderXPCService", "Safari" };\n'
               '  for (int s = 0; s < 6; s++) for (int g = 0; g < 6; g++) for (int p = 0; p < 3; p++)\n'
               '    printf("%d %d %d %d\\n", s, g, p, nvmtl_nvencvt_selected(sels[s], procs[p], gens[g]));\n'
               '  return 0; }\n')
    with tempfile.TemporaryDirectory() as d:
        d = pathlib.Path(d)
        (d / 'h.c').write_text(harness)
        r = subprocess.run(['clang', '-o', str(d / 'h'), str(d / 'h.c')], capture_output=True, text=True)
        check('the selection function compiles on its own', r.returncode == 0, r.stderr.strip()[-300:])
        if r.returncode == 0:
            out = subprocess.run([str(d / 'h')], capture_output=True, text=True).stdout.split('\n')
            got = {tuple(map(int, l.split()[:3])): bool(int(l.split()[3])) for l in out if l.strip()}
            # sels: 0 unset, 1 empty, 2 "0", 3 "1", 4 list, 5 decode service named
            # gens: 0 Ada, 1 Ampere, 2 Blackwell, 3 Turing GTX 16, 4 Turing, 5 none published
            # procs: 0 Parsec, 1 VTDecoderXPCService, 2 Safari
            check('unset: Ada and Ampere get the codecs in every process', all(got[(0, g, p)] for g in (0, 1) for p in range(3)))
            check('unset: Blackwell, Turing and an unknown card get nothing', not any(got[(0, g, p)] for g in (2, 3, 4, 5) for p in range(3)))
            check('an empty value is the same as unset', all(got[(1, g, p)] == got[(0, g, p)] for g in range(6) for p in range(3)))
            check('0 turns it off everywhere, Ada included', not any(got[(2, g, p)] for g in range(6) for p in range(3)))
            check('1 turns it on for every card and process, GTX 16 included', all(got[(3, g, p)] for g in range(6) for p in range(3)))
            check('a list registers only in the processes it names', got[(4, 0, 0)] and not got[(4, 0, 1)] and not got[(4, 0, 2)])
            check('naming the decode service registers there and nowhere else', got[(5, 5, 1)] and not got[(5, 0, 0)])

check('the boot-line switch for the wallpaper is gone: the decoders need no list',
      'nvmtl_nvencvt_wallpaper' not in src and 'nvwallpaper' not in src)

opener = function('static void *nvmtl_nvencvt_open(')
dec = function('static void nvmtl_nvencvt_register_decoders(')
cod = function('static void nvmtl_nvencvt_register_codecs(')
check('one opener loads the library, and only from a root-owned path',
      opener is not None and 'nvmtl_loadable_root_only(path)' in opener
      and opener.index('nvmtl_loadable_root_only(path)') < opener.index('dlopen(path')
      and src.count('dlopen(path, RTLD_NOW | RTLD_LOCAL)') >= 1 and 'libnvencvt.dylib' not in (dec or '') + (cod or ''))
check('the decoders register in every process but WindowServer, with no list; NVMTL_NVENCVT=0 keeps them out',
      dec is not None and 'NVEncVT_RegisterDecoders' in dec and 'nvmtl_nvencvt_selected' not in dec
      and 'strcmp(sel, "0")' in dec and 'strcmp(me, "WindowServer")' in dec and 'dispatch_once' in dec)
check('the encoders still follow the selection, and stay out of WindowServer',
      cod is not None and 'NVEncVT_Register"' in cod and 'nvmtl_nvencvt_selected(getenv("NVMTL_NVENCVT")' in cod
      and 'strcmp(me, "WindowServer")' in cod and 'dispatch_once' in cod)
svc = function('static void nvmtl_nvdec_in_decode_service(')
check('the decode service asks for libnvencvt after the built-in decoders',
      svc is not None and 'nvmtl_nvencvt_register_decoders()' in svc
      and svc.index('nvmtl_nvdec_hevc_register()') < svc.index('nvmtl_nvencvt_register_decoders()'))
check('the decode service reads nvrm610.conf before it reads any selector',
      svc is not None and 'nvmtl_conf_settle()' in svc and svc.index('nvmtl_conf_settle()') < svc.index('getenv("NVMTL_NO_NVDEC")'))
check('the decode service registers on a queue that is not starved under load',
      svc is not None and 'QOS_CLASS_USER_INITIATED' in svc and 'QOS_CLASS_UTILITY' not in svc)
every = function('static void nvmtl_nvencvt_in_every_process(')
onimg = function('static void nvmtl_nvencvt_on_image(')
isvt = function('static bool nvmtl_image_is_videotoolbox(')
check('every other process gets the decoders once VideoToolbox is in it, at load or later',
      every is not None and '__attribute__((constructor)) static void nvmtl_nvencvt_in_every_process(void)' in src
      and 'strcmp(me, "WindowServer")' in every and 'nvmtl_is_decode_service(me)' in every
      and every.index('nvmtl_conf_settle()') < every.index('_dyld_register_func_for_add_image(nvmtl_nvencvt_on_image)'))
check("the image callback reads VideoToolbox's install name from its load commands and registers once, off the callback",
      onimg is not None and isvt is not None and 'LC_ID_DYLIB' in isvt and '/System/Library/Frameworks/VideoToolbox.framework/' in isvt
      and 'dladdr' not in onimg and 'atomic_exchange(&queued, 1)' in onimg
      and onimg.index('dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0)') < onimg.index('nvmtl_nvencvt_register_decoders()'))
check("Apple's sandboxed apps leave it to the decode service, WebKit's GPU process apart",
      onimg is not None and 'if (nvmtl_decode_is_delegated(me) && !nvmtl_is_webkit_gpu(me)) return;' in onimg)
check('the decoders never load VideoToolbox themselves: no registration before it is there',
      dec is not None and dec.index('dlsym(RTLD_DEFAULT, "VTRegisterVideoDecoderWithInfo")') < dec.index('dispatch_once'))
check('the built-in encoders declare low-latency rate control only when NVMTL_ENCODER_LLRC asks',
      'NVMTL_ENCODER_LLRC' in src and src.count('nvmtl_encoder_registration_info(@"com.nvmtl.videoencoder') == 2
      and '"EnableLowLatencyRateControl"' in src or '@"EnableLowLatencyRateControl"' in src)
# WebKit's GPU process plays video in process and picks from its own registry: it gets the decoders alone, by its
# exact name, and is then asked to look for a VP9 hardware decoder again, because it keeps its first answer.
init = function('- (instancetype)initWithAcceleratorPort:(mach_port_t)port {')
wk = function('static bool nvmtl_is_webkit_gpu(')
again = function('static void nvmtl_webkit_ask_vp9_again(')
check("WebKit's GPU process is matched by its exact name", wk is not None and 'strcmp(me, "com.apple.WebKit.GPU")' in wk and 'strncmp' not in wk and 'strstr' not in wk)
check("device initialisation registers the decoders everywhere and the encoders everywhere but WebKit's GPU process",
      init is not None and 'nvmtl_nvencvt_register_decoders();' in init
      and 'if (!nvmtl_is_webkit_gpu(getprogname())) nvmtl_nvencvt_register_codecs();' in init
      and init.index('nvmtl_nvenc_hevc_register()') < init.index('nvmtl_nvencvt_register_decoders();')
      < init.index('nvmtl_nvencvt_register_codecs();'))
check('WebKit is asked about VP9 again only after a registration that worked, and only there',
      dec is not None and 'if (!st && nvmtl_is_webkit_gpu(me)) nvmtl_webkit_ask_vp9_again();' in dec
      and src.count('nvmtl_webkit_ask_vp9_again()') == 1)
check("the question is put from the main run loop's next turn, by a name that may be missing",
      again is not None and 'CFRunLoopPerformBlock(CFRunLoopGetMain(), kCFRunLoopCommonModes' in again
      and 'CFRunLoopWakeUp(CFRunLoopGetMain())' in again
      and 'dlsym(RTLD_DEFAULT, "_ZN7WebCore30registerSupplementalVP9DecoderEv")' in again and 'if (again) again();' in again)

print(f'{fails} failed' if fails else 'all passed')
sys.exit(1 if fails else 0)
