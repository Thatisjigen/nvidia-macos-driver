/* Per-process GPU choice on hybrid laptops (plugin/nvmtl_gpu_policy.h), on the host.
 *   cc -Wall -I../plugin test_gpu_policy.c -o /tmp/t && /tmp/t     -> 0 failed
 *   cc -DALWAYS_NVIDIA ...   (the plugin before the policy: every process gets the device)  -> the "integrated GPU" arms fail */
#include <stdio.h>
#include <string.h>
#include "nvmtl_gpu_policy.h"
static int fails;
#define ARM(name, cond) do { if (cond) printf("ok   %s\n", name); else { printf("FAIL %s\n", name); fails++; } } while (0)
#ifdef ALWAYS_NVIDIA
#define DECIDE(h, m, p, a, o) ((void)(h), (void)(m), (void)(p), (void)(a), (void)(o), 1)
#else
#define DECIDE(h, m, p, a, o) nmp_decide(h, m, p, a, o, 0)
#endif
int main(void)
{
    const char *file = "# policy\nmode apple\nigpu Safari\nnvidia Blender  \nigpu Cyberpunk2077\nnvidia Cyberpunk2077\n\n";
    int apple;
    ARM("a process with no rule gets AUTO", nmp_parse(file, "Notes", &apple) == NMP_PREF_AUTO);
    ARM("mode apple is read", apple == 1);
    ARM("igpu rule", nmp_parse(file, "Safari", &apple) == NMP_PREF_IGPU);
    ARM("nvidia rule, trailing blanks ignored", nmp_parse(file, "Blender", &apple) == NMP_PREF_NVIDIA);
    ARM("the last matching rule wins", nmp_parse(file, "Cyberpunk2077", &apple) == NMP_PREF_NVIDIA);
    ARM("a prefix of a name does not match", nmp_parse(file, "Safar", &apple) == NMP_PREF_AUTO);
    ARM("no file means mode nvidia", (nmp_parse(NULL, "x", &apple), apple == 0));
    ARM("mode nvidia after mode apple switches back", (nmp_parse("mode apple\nmode nvidia\n", "x", &apple), apple == 0));

    ARM("switch off: every process gets the NVIDIA device", DECIDE(0, 1, NMP_PREF_IGPU, 1, 1) == 1);
    ARM("no other Metal GPU: the device is given even if the policy says integrated", DECIDE(1, 1, NMP_PREF_IGPU, 1, 0) == 1);
    ARM("a per-process igpu rule hides the device", DECIDE(1, 0, NMP_PREF_IGPU, -1, 1) == 0);
    ARM("a per-process nvidia rule gives it even in mode apple for an app that allows switching", DECIDE(1, 1, NMP_PREF_NVIDIA, 1, 1) == 1);
    ARM("default mode gives the device to everyone", DECIDE(1, 0, NMP_PREF_AUTO, 1, 1) == 1);
    ARM("mode apple: an app that allows automatic switching is sent to the integrated GPU", DECIDE(1, 1, NMP_PREF_AUTO, 1, 1) == 0);
    ARM("mode apple: an app without the key keeps the high-performance GPU", DECIDE(1, 1, NMP_PREF_AUTO, -1, 1) == 1);
    ARM("mode apple: an app that sets the key to false keeps it", DECIDE(1, 1, NMP_PREF_AUTO, 0, 1) == 1);
    const char *w = 0; nmp_decide(1, 1, NMP_PREF_AUTO, 1, 1, &w);
    ARM("the reason is named for the log", w && strstr(w, "automatic graphics switching"));

    /* Without the switch, the integrated GPU driving displays of its own (nmp_decide_two_gpus) */
    ARM("no mode line is mode auto", nmp_parse_mode(NULL) == NMP_MODE_AUTO && nmp_parse_mode("igpu Safari\n") == NMP_MODE_AUTO);
    ARM("the last mode line wins, blanks ignored", nmp_parse_mode("mode apple\nmode nvidia  \n") == NMP_MODE_NVIDIA
        && nmp_parse_mode("mode nvidia\nmode auto\n") == NMP_MODE_AUTO && nmp_parse_mode("mode apple\n") == NMP_MODE_APPLE);
    ARM("an unknown mode keeps the one before", nmp_parse_mode("mode apple\nmode turbo\n") == NMP_MODE_APPLE);
#define TWO(m, p, a, pl, sv, as, o) nmp_decide_two_gpus(m, p, a, pl, sv, as, o, 0)
    ARM("auto: an app gets the NVIDIA device", TWO(NMP_MODE_AUTO, NMP_PREF_AUTO, NMP_APP_NONE, 0, 0, 1, 1) == 1);
    ARM("auto: a macOS process does not", TWO(NMP_MODE_AUTO, NMP_PREF_AUTO, NMP_APP_NONE, 1, 0, -1, 1) == 0);
    ARM("auto: a macOS service that works for the apps does", TWO(NMP_MODE_AUTO, NMP_PREF_AUTO, NMP_APP_NONE, 1, 1, -1, 1) == 1);
    ARM("the app's avoidRemovable keeps the device from an app", TWO(NMP_MODE_AUTO, NMP_PREF_AUTO, NMP_APP_AVOID, 0, 0, -1, 1) == 0);
    ARM("the app's preferRemovable gives it to a macOS process", TWO(NMP_MODE_AUTO, NMP_PREF_AUTO, NMP_APP_PREFER, 1, 0, -1, 1) == 1);
    ARM("a policy-file line beats the app's choice", TWO(NMP_MODE_AUTO, NMP_PREF_IGPU, NMP_APP_PREFER, 0, 0, -1, 1) == 0
        && TWO(NMP_MODE_AUTO, NMP_PREF_NVIDIA, NMP_APP_AVOID, 1, 0, -1, 1) == 1);
    ARM("mode nvidia gives it to every process", TWO(NMP_MODE_NVIDIA, NMP_PREF_AUTO, NMP_APP_NONE, 1, 0, -1, 1) == 1);
    ARM("mode apple keeps Apple's rule", TWO(NMP_MODE_APPLE, NMP_PREF_AUTO, NMP_APP_NONE, 0, 0, 1, 1) == 0
        && TWO(NMP_MODE_APPLE, NMP_PREF_AUTO, NMP_APP_NONE, 1, 0, -1, 1) == 1);
    ARM("with no other GPU driving a display, every process gets it", TWO(NMP_MODE_AUTO, NMP_PREF_IGPU, NMP_APP_AVOID, 1, 0, 1, 0) == 1);
    ARM("WindowServer, WebKit's GPU process, the video services and the wallpaper are services",
        nmp_is_service("WindowServer") && nmp_is_service("com.apple.WebKit.GPU") && nmp_is_service("VTDecoderXPCService")
        && nmp_is_service("VTEncoderXPCService") && nmp_is_service("WallpaperAerialsExtension") && nmp_is_service("DisplaysExt"));
    ARM("System Information keeps it, so it can report the GPU's Metal family",
        nmp_is_service("system_profiler") && nmp_is_service("System Information"));
    ARM("near names and other processes are not", !nmp_is_service("Finder") && !nmp_is_service("WindowServe")
        && !nmp_is_service("com.apple.WebKit.GPUx") && !nmp_is_service("Wallpaper") && !nmp_is_service(0)
        && !nmp_is_service("System") && !nmp_is_service("system_profile"));
    w = 0; nmp_decide_two_gpus(NMP_MODE_AUTO, NMP_PREF_AUTO, NMP_APP_NONE, 1, 0, -1, 1, &w);
    ARM("the reason is named for the log", w && strstr(w, "macOS process"));
    printf("%d failed\n", fails);
    return fails != 0;
}
