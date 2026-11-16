/*
 * NullMoth NVIDIA driver for macOS
 * Copyright (c) 2026 NullMoth Systems.
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 */

/*
 * Which processes get the NVIDIA GPU as a Metal device on a hybrid laptop. Plain C so tools/test_gpu_policy.c runs the same
 * code on the host; NVMTLDevice.m supplies the inputs.
 *
 * nmp_decide applies when the hybrid switch (boot-arg nmhybrid) is on, and only matters when another GPU also offers Metal
 * (an integrated GPU macOS still drives, such as Coffee Lake). With no second Metal GPU the NVIDIA device is the only
 * choice and is always given. Without the switch, nmp_decide_two_gpus (below) applies.
 *
 * Policy file /Library/GPUBundles/nvmtl-gpu-policy.txt, one rule per line, '#' starts a comment:
 *     mode nvidia          (default) every process gets the NVIDIA device
 *     mode apple           follow Apple's rule: a process whose Info.plist sets NSSupportsAutomaticGraphicsSwitching to
 *                          true is content with the low-power GPU and does not get the NVIDIA device; every other process
 *                          keeps the high-performance GPU
 *     igpu <process>       this process never gets the NVIDIA device
 *     nvidia <process>     this process always gets it
 * A per-process rule beats the mode. The plugin can give a process the device or not; it cannot list a device without
 * making it the default, so a process the policy sends to the low-power GPU does not see the NVIDIA device at all.
 */
#ifndef NVMTL_GPU_POLICY_H
#define NVMTL_GPU_POLICY_H

#include <string.h>

enum { NMP_PREF_AUTO = 0, NMP_PREF_NVIDIA = 1, NMP_PREF_IGPU = 2 };

/* Reads the policy text. Returns the per-process preference; *mode_apple is 1 for "mode apple". */
static inline int nmp_parse(const char *text, const char *proc, int *mode_apple)
{
    int pref = NMP_PREF_AUTO;
    *mode_apple = 0;
    if (!text) return pref;
    const char *p = text;
    while (*p) {
        char line[200]; size_t n = 0;
        while (*p && *p != '\n' && *p != '\r') { if (n + 1 < sizeof line) line[n++] = *p; p++; }
        while (*p == '\n' || *p == '\r') p++;
        line[n] = 0;
        char *q = line; while (*q == ' ' || *q == '\t') q++;
        if (!*q || *q == '#') continue;
        if (!strncmp(q, "mode ", 5)) {
            char *v = q + 5; while (*v == ' ' || *v == '\t') v++;
            if (!strcmp(v, "apple")) *mode_apple = 1; else if (!strcmp(v, "nvidia")) *mode_apple = 0;
        } else if (!strncmp(q, "igpu ", 5) || !strncmp(q, "nvidia ", 7)) {
            int ig = q[0] == 'i';
            char *v = q + (ig ? 5 : 7); while (*v == ' ' || *v == '\t') v++;
            size_t l = strlen(v); while (l && (v[l - 1] == ' ' || v[l - 1] == '\t')) v[--l] = 0;
            if (proc && !strcmp(v, proc)) pref = ig ? NMP_PREF_IGPU : NMP_PREF_NVIDIA;   /* the last matching rule wins */
        }
    }
    return pref;
}

/* 1 = give the process the NVIDIA device. app_auto_switch: the Info.plist value, -1 when absent. */
static inline int nmp_decide(int hybrid_level, int mode_apple, int pref, int app_auto_switch, int other_metal_gpu, const char **why)
{
    const char *w = "";
    int r = 1;
    if (hybrid_level <= 0)              { w = "hybrid switch off"; r = 1; }
    else if (!other_metal_gpu)          { w = "no other Metal GPU to use instead"; r = 1; }
    else if (pref == NMP_PREF_IGPU)     { w = "policy: this process uses the integrated GPU"; r = 0; }
    else if (pref == NMP_PREF_NVIDIA)   { w = "policy: this process uses the NVIDIA GPU"; r = 1; }
    else if (!mode_apple)               { w = "mode nvidia"; r = 1; }
    else if (app_auto_switch == 1)      { w = "mode apple: the app allows automatic graphics switching"; r = 0; }
    else                                { w = "mode apple: the app does not opt into automatic switching"; r = 1; }
    if (why) *why = w;
    return r;
}

/*
 * The same choice without the hybrid switch, when another GPU drives displays of its own (an Intel GPU macOS drives
 * through its own framebuffer, with the panel on it). Then it works as on Apple's two-GPU Macs: the integrated GPU is
 * the default, the NVIDIA GPU is the high-performance one for the apps that want it. The NVIDIA GPU is offered to every
 * app and to the system services that work for them (WindowServer, WebKit's GPU process, the video decode and encode
 * services, the wallpaper and its preview), and not to the rest of macOS's own processes, which never pick a GPU: they
 * stay off the NVIDIA GPU instead of each holding a Vulkan device on it.
 *
 * The app's own choice comes first, as macOS hands it to Metal: GPUSelectionPolicy, which IOSurface composes from the
 * user's com.apple.gpu preferences, the app's Info.plist and Finder's "Prefer External GPU". avoidRemovable keeps the
 * NVIDIA GPU away from the app; preferRemovable gives it (the plugin reports the GPU as removable in that app alone, so
 * Metal makes it the app's default device). The policy file's per-process rules come before both; "mode nvidia" in it gives the device to
 * every process, as before this choice, and "mode apple" keeps Apple's rule on NSSupportsAutomaticGraphicsSwitching.
 */
enum { NMP_MODE_AUTO = 0, NMP_MODE_NVIDIA = 1, NMP_MODE_APPLE = 2 };
enum { NMP_APP_NONE = 0, NMP_APP_PREFER = 1, NMP_APP_AVOID = 2 };

/* The last "mode" line of the policy text; no line (or no file) is NMP_MODE_AUTO. */
static inline int nmp_parse_mode(const char *text)
{
    int mode = NMP_MODE_AUTO;
    if (!text) return mode;
    const char *p = text;
    while (*p) {
        char line[200]; size_t n = 0;
        while (*p && *p != '\n' && *p != '\r') { if (n + 1 < sizeof line) line[n++] = *p; p++; }
        while (*p == '\n' || *p == '\r') p++;
        line[n] = 0;
        char *q = line; while (*q == ' ' || *q == '\t') q++;
        if (strncmp(q, "mode ", 5)) continue;
        char *v = q + 5; while (*v == ' ' || *v == '\t') v++;
        size_t l = strlen(v); while (l && (v[l - 1] == ' ' || v[l - 1] == '\t')) v[--l] = 0;
        if (!strcmp(v, "nvidia")) mode = NMP_MODE_NVIDIA;
        else if (!strcmp(v, "apple")) mode = NMP_MODE_APPLE;
        else if (!strcmp(v, "auto")) mode = NMP_MODE_AUTO;
    }
    return mode;
}

/* 1 = give the process the NVIDIA device. platform: a binary of macOS itself; service: one of the system services above;
   other_gpu_display: another GPU drives at least one display. */
static inline int nmp_decide_two_gpus(int mode, int pref, int app, int platform, int service, int app_auto_switch,
                                      int other_gpu_display, const char **why)
{
    const char *w = "";
    int r = 1;
    if (!other_gpu_display)             { w = "no other GPU drives a display"; r = 1; }
    else if (pref == NMP_PREF_IGPU)     { w = "policy: this process uses the integrated GPU"; r = 0; }
    else if (pref == NMP_PREF_NVIDIA)   { w = "policy: this process uses the NVIDIA GPU"; r = 1; }
    else if (app == NMP_APP_AVOID)      { w = "the app's GPU choice: power saving (avoidRemovable)"; r = 0; }
    else if (app == NMP_APP_PREFER)     { w = "the app's GPU choice: high performance (preferRemovable)"; r = 1; }
    else if (mode == NMP_MODE_NVIDIA)   { w = "mode nvidia"; r = 1; }
    else if (mode == NMP_MODE_APPLE)    { w = app_auto_switch == 1 ? "mode apple: the app allows automatic graphics switching"
                                                                   : "mode apple: the app does not opt into automatic switching";
                                          r = app_auto_switch != 1; }
    else if (service)                   { w = "automatic: a system service that works for the apps"; r = 1; }
    else if (platform)                  { w = "automatic: a macOS process, on the integrated GPU"; r = 0; }
    else                                { w = "automatic: an app, offered the NVIDIA GPU as the high-performance one"; r = 1; }
    if (why) *why = w;
    return r;
}

/* The system services that keep the NVIDIA GPU in the automatic choice, by their exact process names; the video services
   are matched by prefix, as their names carry a suffix in some releases. */
static inline int nmp_is_service(const char *proc)
{
    static const char *const exact[] = { "WindowServer", "com.apple.WebKit.GPU", "WallpaperAerialsExtension", "DisplaysExt" };
    if (!proc) return 0;
    for (unsigned i = 0; i < sizeof exact / sizeof exact[0]; i++) if (!strcmp(proc, exact[i])) return 1;
    return !strncmp(proc, "VTDecoderXPCService", 19) || !strncmp(proc, "VTEncoderXPCService", 19);
}

#endif
