/*
 * inject_exec.c - execute Lua code on the target window's owner thread.
 */
#define _WIN32_WINNT 0x0600
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <psapi.h>
#include <string.h>
#include <time.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
#include "lstate.h"
#include "telemetry_writer.h"
#include "luac_dump.h"
#include "lua_profiler.h"
#include "lua_api_hooks.h"
#include "x86_insn.h"
#include "patch_freeze.h"

typedef int (__cdecl *lua_pcall_fn)(lua_State*, int, int, int);
typedef int (__cdecl *luaL_loadstring_fn)(lua_State*, const char*);
typedef int (__cdecl *luaL_loadbuffer_fn)(lua_State*, const char*, size_t, const char*);
typedef const char* (__cdecl *lua_tolstring_fn)(lua_State*, int, size_t*);
typedef int (__cdecl *lua_type_fn)(lua_State*, int);
typedef void (__cdecl *lua_settop_fn)(lua_State*, int);
typedef int (__cdecl *lua_gettop_fn)(lua_State*);
typedef int (__cdecl *lua_sethook_fn)(lua_State*, lua_Hook, int, int);
typedef int (__cdecl *lua_getinfo_fn)(lua_State*, const char*, lua_Debug*);
typedef int (__cdecl *lua_getstack_fn)(lua_State*, int, lua_Debug*);
typedef void (__cdecl *lua_pushcclosure_fn)(lua_State*, lua_CFunction, int);
typedef void (__cdecl *lua_setglobal_fn)(lua_State*, const char*);
typedef void (__cdecl *lua_setfield_fn)(lua_State*, int, const char*);
typedef lua_State* (__cdecl *lua_tothread_fn)(lua_State*, int);
typedef int (__cdecl *lua_gc_fn)(lua_State*, int, int);

#define RUN_LUA_MSG (WM_APP + 0x514)
#define CODE_MAX 65536
#define REMOTE_CODE_MAX COCOS_DEBUG_IPC_REMOTE_LUA_CAP

#define PROF_DETAIL_FRAMES 180
#define PROF_AUTO_DETAIL_LUA_MS 16.67

static lua_State *g_L = NULL;
static luaL_loadstring_fn g_LS = NULL;
static luaL_loadbuffer_fn g_LB = NULL;
static lua_pcall_fn g_PC = NULL;
static lua_tolstring_fn g_TL = NULL;
static lua_type_fn g_TY = NULL;
static lua_settop_fn g_ST = NULL;
static lua_gettop_fn g_GT = NULL;
static lua_sethook_fn g_SH = NULL;
static lua_getinfo_fn g_GI = NULL;
static lua_getstack_fn g_GS = NULL;
static lua_pushcclosure_fn g_PCC = NULL;
static lua_setglobal_fn g_SG = NULL;
static lua_setfield_fn g_SFL = NULL;
static int g_profiler_c_api = 0;
static lua_tothread_fn g_TTH = NULL;
static lua_gc_fn g_GC = NULL;

static char g_code[CODE_MAX];
static char g_remote_code[REMOTE_CODE_MAX];
static volatile LONG g_remote_lua_queued = 0;
static volatile LONG g_remote_lua_running = 0;
static int g_remote_lua_wait_frames = 0;
#define REMOTE_LUA_WAIT_MAX 300
static HWND g_hwnd = NULL;
static WNDPROC g_old_proc = NULL;
static volatile LONG g_done = 0;
static volatile LONG g_running = 0;

static int g_recording = 0;
static int g_frame_index = 0;
static int g_detail_frames_remaining = 0;
static int g_detail_active = 0;
static CocosDebugLuaProfilerFrame g_precise_frame;
static double g_previous_drawscene_begin = 0.0;
static uint32_t g_director_this = 0;
static float g_current_frame_period_ms = 0.0f;
static int g_has_pending_frame = 0;
static double g_pending_begin_s = 0.0;
static int g_renderer_resolve_attempts = 0;
static LONG g_last_event_dropped = 0;

static uint64_t g_publish_sequence = 0;
static int g_last_command_sequence = 0;

static uint32_t g_drawscene_addr = 0;
static uint32_t g_cocos_module_base = 0;
static uint32_t g_cocos_module_size = 0;

#define CPP_HOOK_MAX 12
#define CPP_HOOK_SITES 64
#define CPP_HOOK_STACK 64
#define CPP_PATCH_LEN 5
#define CPP_NAME_LEN 80

typedef struct {
    uint32_t ret;
    double t0;
} CppHookRec;

typedef struct {
    char name[CPP_NAME_LEN];
    uint32_t orig;
    int installed;
    int site_count;
    uint8_t *sites[CPP_HOOK_SITES];
    uint8_t orig_bytes[CPP_HOOK_SITES][16];
    int stolen;
    int prologue_hook;
    void *stub;
    CppHookRec recs[CPP_HOOK_STACK];
    int depth;
    double last_begin;
    double last_end;
    double last_ms;
    double max_ms;
    double frame_total;
    double frame_max;
    int frame_calls;
    double prev_total;
    double prev_max;
    int prev_calls;
    volatile LONG seq;
} CppHook;

static CppHook g_cpp_hooks[CPP_HOOK_MAX];
static int g_cpp_hook_count = 0;
static int g_cpp_hooks_installed = 0;
static int g_cpp_drawscene_id = -1;

static volatile LONG g_ds_frame_seq;
static int g_ds_hook_installed;

static double prof_now(void) {
    static LARGE_INTEGER freq = {0};
    LARGE_INTEGER counter;
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&counter);
    return (double)counter.QuadPart / (double)freq.QuadPart;
}

static void cpp_hooks_roll_frame(void);
static void profiler_on_drawscene_begin(double now, uint32_t this_ptr);
static void profiler_on_drawscene_end(void);
static void remote_lua_tick_timeout(void);
static int l_drain_remote_lua(lua_State *L);
static double query_lua_mem_kb(void);
static void sync_cpp_hooks_to_config(void);
static void ensure_renderer_cpp_hook(void);
static int add_cpp_hook_target(const char *name, uint32_t addr);
static int install_one_cpp_hook(CppHook *h, int id);
static void try_hook_live_renderer(uint32_t director);
static void publish_pending_frame(float period_ms);

static void cpp_hooks_roll_frame(void) {
    int i;
    for (i = 0; i < g_cpp_hook_count; i++) {
        CppHook *h = &g_cpp_hooks[i];
        h->prev_total = h->frame_total;
        h->prev_max = h->frame_max;
        h->prev_calls = h->frame_calls;
        h->frame_total = 0;
        h->frame_max = 0;
        h->frame_calls = 0;
        h->last_ms = 0;
    }
}

static void cpp_hooks_reset_metrics(void) {
    int i;
    for (i = 0; i < g_cpp_hook_count; i++) {
        CppHook *h = &g_cpp_hooks[i];
        h->last_begin = h->last_end = h->last_ms = h->max_ms = 0;
        h->frame_total = h->frame_max = 0;
        h->frame_calls = 0;
        h->prev_total = h->prev_max = 0;
        h->prev_calls = 0;
    }
}

static void __attribute__((cdecl)) cpp_hook_begin(int id, uint32_t ret, uint32_t this_ptr) {
    CppHook *h;
    if (id < 0 || id >= g_cpp_hook_count) return;
    h = &g_cpp_hooks[id];
    if (id == g_cpp_drawscene_id && h->depth == 0) {
        double now = prof_now();
        /* Remote Lua runs from Scheduler (Lua context), not from this C++ hook. */
        remote_lua_tick_timeout();
        profiler_on_drawscene_begin(now, this_ptr);
    }
    if (h->depth < CPP_HOOK_STACK) {
        h->recs[h->depth].ret = ret;
        h->recs[h->depth].t0 = g_recording ? prof_now() : 0;
    }
    h->depth++;
}

static uint32_t __attribute__((cdecl)) cpp_hook_end(int id) {
    CppHook *h;
    uint32_t ret = 0;
    double t1, dt;
    if (id < 0 || id >= g_cpp_hook_count) return 0;
    h = &g_cpp_hooks[id];
    if (h->depth <= 0) return 0;
    h->depth--;
    if (h->depth >= CPP_HOOK_STACK) return 0;
    ret = h->recs[h->depth].ret;
    if (h->recs[h->depth].t0 != 0) {
        t1 = prof_now();
        dt = t1 - h->recs[h->depth].t0;
        if (dt < 0) dt = 0;
        h->last_begin = h->recs[h->depth].t0;
        h->last_end = t1;
        h->last_ms = dt * 1000.0;
        h->frame_total += h->last_ms;
        h->frame_calls++;
        if (h->last_ms > h->frame_max) h->frame_max = h->last_ms;
        if (h->last_ms > h->max_ms) h->max_ms = h->last_ms;
        InterlockedIncrement(&h->seq);
        if (id == g_cpp_drawscene_id)
            g_ds_frame_seq = h->seq;
    }
    /* Always tick on drawScene return so Start can be observed while idle. */
    if (id == g_cpp_drawscene_id && h->depth == 0)
        profiler_on_drawscene_end();
    return ret;
}

static int build_cpp_hook_stub(CppHook *h, int id, uint32_t resume_addr) {
    uint8_t *s;
    uint32_t begin_addr = (uint32_t)(uintptr_t)cpp_hook_begin;
    uint32_t end_addr = (uint32_t)(uintptr_t)cpp_hook_end;
    uint32_t epilog;
    uint32_t id32 = (uint32_t)id;
    int i = 0;
    int epilog_slot;
    int32_t rel;

    if (!h->stub) {
        h->stub = VirtualAlloc(NULL, 256, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
        if (!h->stub) return 0;
    }
    s = (uint8_t *)h->stub;

    s[i++] = 0x51; /* push ecx (save this) */
    s[i++] = 0x51; /* push ecx (this_ptr arg) */
    s[i++] = 0xFF; s[i++] = 0x74; s[i++] = 0x24; s[i++] = 0x08; /* push ret */
    s[i++] = 0x68;
    memcpy(s + i, &id32, 4); i += 4;
    s[i++] = 0xE8;
    rel = (int32_t)(begin_addr - ((uint32_t)(uintptr_t)(s + i + 4)));
    memcpy(s + i, &rel, 4); i += 4;
    s[i++] = 0x83; s[i++] = 0xC4; s[i++] = 0x0C; /* add esp, 12 */
    s[i++] = 0x59; /* pop ecx */
    s[i++] = 0xC7; s[i++] = 0x04; s[i++] = 0x24;
    epilog_slot = i;
    i += 4;
    s[i++] = 0xE9;
    rel = (int32_t)(resume_addr - ((uint32_t)(uintptr_t)(s + i + 4)));
    memcpy(s + i, &rel, 4); i += 4;

    epilog = (uint32_t)(uintptr_t)(s + i);
    memcpy(s + epilog_slot, &epilog, 4);

    s[i++] = 0x50;
    s[i++] = 0x52;
    s[i++] = 0x51;
    s[i++] = 0x68;
    memcpy(s + i, &id32, 4); i += 4;
    s[i++] = 0xE8;
    rel = (int32_t)(end_addr - ((uint32_t)(uintptr_t)(s + i + 4)));
    memcpy(s + i, &rel, 4); i += 4;
    s[i++] = 0x83; s[i++] = 0xC4; s[i++] = 0x04;
    s[i++] = 0x89; s[i++] = 0xC1;
    s[i++] = 0x8B; s[i++] = 0x44; s[i++] = 0x24; s[i++] = 0x08;
    s[i++] = 0x8B; s[i++] = 0x54; s[i++] = 0x24; s[i++] = 0x04;
    s[i++] = 0x83; s[i++] = 0xC4; s[i++] = 0x0C;
    s[i++] = 0xFF; s[i++] = 0xE1;
    return i;
}

static int install_prologue_hook(CppHook *h, int id) {
    uint8_t *orig = (uint8_t *)(uintptr_t)h->orig;
    uint8_t *tramp;
    DWORD old_protect;
    int stolen, i;
    int32_t rel;

    stolen = x86_measure_stolen(orig, 16);
    if (stolen < 5) return 0;
    h->stub = VirtualAlloc(NULL, 256, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!h->stub) return 0;
    tramp = (uint8_t *)h->stub + 128;
    if (x86_reloc_copy(tramp, orig, stolen, orig, tramp) != stolen) {
        VirtualFree(h->stub, 0, MEM_RELEASE);
        h->stub = NULL;
        return 0;
    }
    tramp[stolen] = 0xE9;
    rel = (int32_t)((orig + stolen) - (tramp + stolen + 5));
    memcpy(tramp + stolen + 1, &rel, 4);

    if (!build_cpp_hook_stub(h, id, (uint32_t)(uintptr_t)tramp)) {
        VirtualFree(h->stub, 0, MEM_RELEASE);
        h->stub = NULL;
        return 0;
    }

    memcpy(h->orig_bytes[0], orig, stolen);
    if (!VirtualProtect(orig, stolen, PAGE_EXECUTE_READWRITE, &old_protect)) {
        VirtualFree(h->stub, 0, MEM_RELEASE);
        h->stub = NULL;
        return 0;
    }
    /* No other thread may execute the target while the jmp is written. */
    patch_freeze_begin();
    orig[0] = 0xE9;
    rel = (int32_t)((uint8_t *)h->stub - (orig + 5));
    memcpy(orig + 1, &rel, 4);
    for (i = 5; i < stolen; i++) orig[i] = 0x90;
    patch_freeze_end();
    VirtualProtect(orig, stolen, old_protect, &old_protect);
    FlushInstructionCache(GetCurrentProcess(), orig, stolen);
    FlushInstructionCache(GetCurrentProcess(), h->stub, 256);
    h->stolen = stolen;
    h->prologue_hook = 1;
    h->site_count = 1;
    h->sites[0] = orig;
    h->installed = 1;
    return 1;
}

static int collect_e8_sites(uint32_t target, uint8_t **out, int cap) {
    uint8_t *base;
    uint32_t size, off;
    int n = 0;
    if (g_cocos_module_base < 0x10000 || g_cocos_module_size < 64 || target < 0x10000)
        return 0;
    base = (uint8_t *)(uintptr_t)g_cocos_module_base;
    size = g_cocos_module_size;
    for (off = 0; off + 5 <= size && n < cap; off++) {
        uint8_t *p = base + off;
        int32_t rel;
        if (p[0] != 0xE8) continue;
        rel = *(int32_t *)(p + 1);
        if ((uintptr_t)(p + 5 + rel) == (uintptr_t)target)
            out[n++] = p;
    }
    return n;
}

static int install_one_cpp_hook(CppHook *h, int id) {
    DWORD old_protect;
    int i, patched = 0;
    if (!h || h->orig < 0x10000) return 0;
    if (h->installed) return 1;

    if (install_prologue_hook(h, id))
        return 1;

    h->site_count = collect_e8_sites(h->orig, h->sites, CPP_HOOK_SITES);
    if (h->site_count <= 0) return 0;
    if (!build_cpp_hook_stub(h, id, h->orig)) return 0;

    /* Freeze once around all call-site patches so no thread executes a
       half-written call. */
    patch_freeze_begin();
    for (i = 0; i < h->site_count; i++) {
        uint8_t *target = h->sites[i];
        int32_t rel;
        memcpy(h->orig_bytes[i], target, CPP_PATCH_LEN);
        if (!VirtualProtect(target, CPP_PATCH_LEN, PAGE_EXECUTE_READWRITE, &old_protect))
            continue;
        rel = (int32_t)((uint8_t *)h->stub - (target + 5));
        target[0] = 0xE8;
        memcpy(target + 1, &rel, 4);
        VirtualProtect(target, CPP_PATCH_LEN, old_protect, &old_protect);
        patched++;
    }
    patch_freeze_end();
    for (i = 0; i < h->site_count; i++)
        FlushInstructionCache(GetCurrentProcess(), h->sites[i], CPP_PATCH_LEN);
    FlushInstructionCache(GetCurrentProcess(), h->stub, 256);
    if (!patched) {
        VirtualFree(h->stub, 0, MEM_RELEASE);
        h->stub = NULL;
        h->site_count = 0;
        return 0;
    }
    h->stolen = CPP_PATCH_LEN;
    h->prologue_hook = 0;
    h->installed = 1;
    return 1;
}

static void remove_one_cpp_hook(CppHook *h) {
    DWORD old_protect;
    int i;
    if (!h || !h->installed) return;
    if (h->prologue_hook && h->sites[0]) {
        uint8_t *target = h->sites[0];
        int n = h->stolen > 0 ? h->stolen : 5;
        if (VirtualProtect(target, n, PAGE_EXECUTE_READWRITE, &old_protect)) {
            memcpy(target, h->orig_bytes[0], n);
            VirtualProtect(target, n, old_protect, &old_protect);
            FlushInstructionCache(GetCurrentProcess(), target, n);
        }
    } else {
        for (i = 0; i < h->site_count; i++) {
            uint8_t *target = h->sites[i];
            if (!target) continue;
            if (VirtualProtect(target, CPP_PATCH_LEN, PAGE_EXECUTE_READWRITE, &old_protect)) {
                memcpy(target, h->orig_bytes[i], CPP_PATCH_LEN);
                VirtualProtect(target, CPP_PATCH_LEN, old_protect, &old_protect);
                FlushInstructionCache(GetCurrentProcess(), target, CPP_PATCH_LEN);
            }
        }
    }
    if (h->stub) {
        VirtualFree(h->stub, 0, MEM_RELEASE);
        h->stub = NULL;
    }
    h->site_count = 0;
    h->prologue_hook = 0;
    h->installed = 0;
    h->depth = 0;
}

static int add_cpp_hook_target(const char *name, uint32_t addr) {
    CppHook *h;
    int i;
    if (!name || !name[0] || addr < 0x10000) return -1;
    for (i = 0; i < g_cpp_hook_count; i++) {
        if (!_stricmp(g_cpp_hooks[i].name, name) || g_cpp_hooks[i].orig == addr)
            return i;
    }
    if (g_cpp_hook_count >= CPP_HOOK_MAX) return -1;
    h = &g_cpp_hooks[g_cpp_hook_count];
    memset(h, 0, sizeof(*h));
    strncpy(h->name, name, CPP_NAME_LEN - 1);
    h->orig = addr;
    if (strstr(name, "drawScene"))
        g_cpp_drawscene_id = g_cpp_hook_count;
    return g_cpp_hook_count++;
}

static int install_cpp_hooks(void) {
    int i, n = 0;
    /* Targets already loaded from IPC config; only fall back if empty. */
    if (g_cpp_hook_count == 0 && g_drawscene_addr >= 0x10000)
        add_cpp_hook_target("cc.Director:drawScene", g_drawscene_addr);
    for (i = 0; i < g_cpp_hook_count; i++) {
        if (install_one_cpp_hook(&g_cpp_hooks[i], i))
            n++;
    }
    g_cpp_hooks_installed = n > 0;
    g_ds_hook_installed = (g_cpp_drawscene_id >= 0 && g_cpp_hooks[g_cpp_drawscene_id].installed);
    sync_cpp_hooks_to_config();
    return n;
}

static int remove_cpp_hooks(void) {
    int i, n = 0;
    for (i = 0; i < g_cpp_hook_count; i++) {
        if (g_cpp_hooks[i].installed) {
            remove_one_cpp_hook(&g_cpp_hooks[i]);
            n++;
        }
    }
    g_cpp_hooks_installed = 0;
    g_ds_hook_installed = 0;
    return n > 0;
}

/* Available for future detach/cleanup; not invoked from the live trampoline. */
__attribute__((unused))
static int remove_drawscene_hook(void) {
    return remove_cpp_hooks();
}

static int install_drawscene_hook(void) {
    return install_cpp_hooks() > 0;
}

static void set_detail_capture_frames(int frames) {
    int was_active = g_detail_active;
    if (frames > g_detail_frames_remaining)
        g_detail_frames_remaining = frames;
    g_detail_active = g_detail_frames_remaining > 0;
    if (g_detail_active != was_active)
        cocos_debug_lua_profiler_set_line_capture(g_detail_active);
}

static lua_State *thread_from_lua_arg(lua_State *L, int idx) {
    if (!L || !g_TY || !g_TTH) return NULL;
    if (g_TY(L, idx) != LUA_TTHREAD) return NULL;
    return g_TTH(L, idx);
}

static void publish_precise_frame(const CocosDebugLuaProfilerFrame *frame) {
    CocosDebugIpcBlock *block = cocos_debug_telemetry_block();
    CocosDebugIpcFrame *out;
    LONG event_dropped;
    uint32_t i;
    if (!block || !frame) return;
    g_publish_sequence++;
    out = cocos_debug_telemetry_begin_frame(g_publish_sequence);
    if (!out) return;
    out->frame_begin_qpc = frame->begin_qpc;
    out->frame_end_qpc = frame->end_qpc;
    out->lua_frame_index = (uint32_t)g_frame_index;
    out->render_frame_sequence = (uint32_t)g_ds_frame_seq;
    out->frame_period_ms = g_current_frame_period_ms;
    out->drawscene_body_ms = (float)frame->body_ms;
    out->lua_wall_ms = (float)frame->lua_wall_ms;
    out->lua_cpu_ms = (float)frame->lua_cpu_ms;
    out->lua_script_self_ms = (float)frame->lua_script_self_ms;
    out->lua_c_self_ms = (float)frame->lua_c_self_ms;
    out->lua_outside_ms = (float)frame->lua_outside_ms;
    out->lua_mem_kb = (float)query_lua_mem_kb();
    out->hook_events = frame->hook_events;
    out->flags = frame->flags;
    event_dropped = InterlockedCompareExchange(
        (LONG *)&block->header.event_dropped, 0, 0);
    if (event_dropped != g_last_event_dropped)
        out->flags |= COCOS_DEBUG_IPC_FRAME_EVENT_DROPPED;
    g_last_event_dropped = event_dropped;
    out->method_count = frame->method_count > COCOS_DEBUG_IPC_METHOD_CAP
        ? COCOS_DEBUG_IPC_METHOD_CAP : frame->method_count;
    memcpy(out->methods, frame->methods,
           out->method_count * sizeof(out->methods[0]));
    out->line_count = frame->line_count > COCOS_DEBUG_IPC_LINE_CAP
        ? COCOS_DEBUG_IPC_LINE_CAP : frame->line_count;
    memcpy(out->lines, frame->lines,
           out->line_count * sizeof(out->lines[0]));
    out->cpp_count = (uint32_t)g_cpp_hook_count;
    if (out->cpp_count > COCOS_DEBUG_IPC_CPP_CAP)
        out->cpp_count = COCOS_DEBUG_IPC_CPP_CAP;
    for (i = 0; i < out->cpp_count; i++) {
        CppHook *src = &g_cpp_hooks[i];
        CocosDebugIpcCppSample *dst = &out->cpp[i];
        dst->target_index = i;
        dst->total_ms = (float)src->frame_total;
        dst->max_ms = (float)src->frame_max;
        dst->last_ms = (float)src->last_ms;
        dst->calls = (uint32_t)src->frame_calls;
        dst->call_sequence = (uint32_t)src->seq;
    }
    cocos_debug_telemetry_end_frame(out);
}

static void update_precise_detail_capture_after_frame(
    const CocosDebugLuaProfilerFrame *frame) {
    int was_active = g_detail_active;
    if (g_detail_frames_remaining > 0)
        g_detail_frames_remaining--;
    if (frame && frame->lua_wall_ms >= PROF_AUTO_DETAIL_LUA_MS &&
        g_detail_frames_remaining < PROF_DETAIL_FRAMES)
        g_detail_frames_remaining = PROF_DETAIL_FRAMES;
    g_detail_active = g_detail_frames_remaining > 0;
    if (g_detail_active != was_active)
        cocos_debug_lua_profiler_set_line_capture(g_detail_active);
}

#ifndef LUA_GCCOUNT
#define LUA_GCCOUNT 3
#endif
#ifndef LUA_GCCOUNTB
#define LUA_GCCOUNTB 4
#endif

static int g_lua_mem_sample_counter;
static double g_lua_mem_cached_kb;

static double query_lua_mem_kb(void) {
    if (!g_L || !g_GC) return 0.0;
    if (g_lua_mem_sample_counter++ < 8 && g_lua_mem_cached_kb > 0.0)
        return g_lua_mem_cached_kb;
    g_lua_mem_sample_counter = 0;
    g_lua_mem_cached_kb = (double)g_GC(g_L, LUA_GCCOUNT, 0) +
                          (double)g_GC(g_L, LUA_GCCOUNTB, 0) / 1024.0;
    return g_lua_mem_cached_kb;
}

static void discover_precise_lua_threads(lua_State *root) {
    global_State *global;
    GCObject *object;
    if (!root) return;
    global = G(root);
    if (!global) return;
    for (object = global->rootgc; object != NULL; object = object->gch.next) {
        if (object->gch.tt == LUA_TTHREAD)
            cocos_debug_lua_profiler_register_thread(gco2th(object));
    }
}

static int profiler_start_recording(void) {
    if (!g_SH || !g_GI) return 0;
    if (g_recording) return 1;
    g_recording = 1;
    g_frame_index = 0;
    cpp_hooks_reset_metrics();
    memset(&g_precise_frame, 0, sizeof(g_precise_frame));
    g_previous_drawscene_begin = 0.0;
    g_director_this = 0;
    g_current_frame_period_ms = 0.0f;
    g_has_pending_frame = 0;
    g_pending_begin_s = 0.0;
    g_renderer_resolve_attempts = 0;
    g_detail_frames_remaining = 0;
    g_detail_active = 0;
    g_lua_mem_sample_counter = 8;
    g_lua_mem_cached_kb = 0.0;
    cocos_debug_lua_profiler_start();
    cocos_debug_lua_profiler_set_line_capture(0);
    cocos_debug_lua_profiler_register_thread(g_L);
    discover_precise_lua_threads(g_L);
    cocos_debug_telemetry_set_state(COCOS_DEBUG_IPC_STATE_RECORDING, NULL);
    return 1;
}

static void profiler_stop_recording(void) {
    float tail_ms;

    if (!g_recording) return;
    if (g_has_pending_frame) {
        tail_ms = 0.0f;
        if (g_pending_begin_s > 0.0)
            tail_ms = (float)((prof_now() - g_pending_begin_s) * 1000.0);
        publish_pending_frame(tail_ms);
    }
    cocos_debug_lua_profiler_frame_abort();
    cocos_debug_lua_profiler_stop();
    g_recording = 0;
    g_detail_frames_remaining = 0;
    g_detail_active = 0;
    g_has_pending_frame = 0;
    g_pending_begin_s = 0.0;
    cocos_debug_telemetry_set_state(COCOS_DEBUG_IPC_STATE_STOPPED, NULL);
}

static float read_director_delta_time_ms(void) {
    const CocosDebugIpcInjectionConfig *config = cocos_debug_telemetry_config();
    uint32_t offset;
    float seconds;
    if (!config || g_director_this < 0x10000u)
        return -1.0f;
    offset = config->director_delta_time_offset;
    if (offset == 0 || offset >= 0x1000u)
        return -1.0f;
    seconds = *(const float *)(uintptr_t)(g_director_this + offset);
    if (!(seconds >= 0.0f) || seconds > 5.0f)
        return -1.0f;
    return seconds * 1000.0f;
}

static void publish_pending_frame(float period_ms) {
    if (!g_has_pending_frame) return;
    if (period_ms > 0.0f)
        g_current_frame_period_ms = period_ms;
    else {
        float engine_dt = read_director_delta_time_ms();
        if (engine_dt >= 0.0f)
            g_current_frame_period_ms = engine_dt;
    }
    publish_precise_frame(&g_precise_frame);
    g_has_pending_frame = 0;
}

static void profiler_on_drawscene_begin(double now, uint32_t this_ptr) {
    float loop_ms;

    if (!g_recording) return;
    g_director_this = this_ptr;
    try_hook_live_renderer(this_ptr);

    /*
     * Pair THIS body's snapshot with the begin-to-begin interval that
     * contains it (this body + present/vsync/idle until the next
     * drawScene). Engine getDeltaTime at this prologue is still the
     * previous interval, so use QPC here before rolling C++ counters.
     */
    loop_ms = 0.0f;
    if (g_pending_begin_s > 0.0 && now >= g_pending_begin_s)
        loop_ms = (float)((now - g_pending_begin_s) * 1000.0);
    else if (g_previous_drawscene_begin > 0.0 && now >= g_previous_drawscene_begin)
        loop_ms = (float)((now - g_previous_drawscene_begin) * 1000.0);
    publish_pending_frame(loop_ms);

    g_previous_drawscene_begin = now;
    cpp_hooks_roll_frame();
    cocos_debug_lua_profiler_frame_begin((uint64_t)g_frame_index + 1);
}

static void pop_one(lua_State *L);

static void publish_remote_lua_result(int status, const char *message) {
    CocosDebugIpcBlock *block = cocos_debug_telemetry_block();
    CocosDebugIpcRemoteLua *remote;
    if (!block) return;
    remote = &block->remote_lua;
    if (message && message[0]) {
        strncpy(remote->result_error, message, sizeof(remote->result_error) - 1);
        remote->result_error[sizeof(remote->result_error) - 1] = 0;
    } else {
        remote->result_error[0] = 0;
    }
    InterlockedExchange((LONG *)&remote->result_status, status);
    MemoryBarrier();
    InterlockedIncrement((LONG *)&remote->result_seq);
}

/*
 * Execute queued remote script from INSIDE Lua (Scheduler callback).
 * Calling lua_pcall from drawScene/C++ hooks is unsafe: the VM may be mid-call
 * even when our profiler bookkeeping looks idle.
 */
static int l_drain_remote_lua(lua_State *L) {
    int err;
    int top = 0;
    const char *s;
    size_t size;

    if (!L || !g_PC) return 0;
    if (InterlockedCompareExchange(&g_remote_lua_queued, 0, 0) == 0)
        return 0;
    if (InterlockedCompareExchange(&g_remote_lua_running, 1, 0) != 0)
        return 0;
    if (InterlockedCompareExchange(&g_remote_lua_queued, 0, 1) != 1) {
        InterlockedExchange(&g_remote_lua_running, 0);
        return 0;
    }
    g_remote_lua_wait_frames = 0;

    size = strlen(g_remote_code);
    if (size == 0) {
        publish_remote_lua_result(1, "Empty script");
        InterlockedExchange(&g_remote_lua_running, 0);
        return 0;
    }

    cocos_debug_lua_profiler_pause_accounting();
    if (g_GT) top = g_GT(L);

    /* Prefer loadstring: not patched by luac_dump. */
    err = g_LS ? g_LS(L, g_remote_code) : -1;
    if (err != 0) {
        if (g_ST) g_ST(L, top); /* drop loadstring error before fallback */
        err = cocos_debug_luac_dump_loadbuffer_orig(L, g_remote_code, size,
                                             "=@COCOS_DEBUG_runLuaString");
        if (err < 0)
            err = g_LB ? g_LB(L, g_remote_code, size, "=@COCOS_DEBUG_runLuaString") : -1;
    }
    if (err) {
        s = (err > 0 && g_TL) ? g_TL(L, -1, NULL) : NULL;
        publish_remote_lua_result(1, s ? s : (err < 0 ? "load API unavailable" : "compile error"));
        if (g_ST) g_ST(L, top);
        else if (err > 0) pop_one(L);
        cocos_debug_lua_profiler_resume_accounting();
        InterlockedExchange(&g_remote_lua_running, 0);
        return 0;
    }

    err = g_PC(L, 0, 0, 0);
    if (err) {
        s = g_TL ? g_TL(L, -1, NULL) : NULL;
        publish_remote_lua_result(2, s ? s : "runtime error");
        if (g_ST) g_ST(L, top);
        else pop_one(L);
        cocos_debug_lua_profiler_resume_accounting();
        InterlockedExchange(&g_remote_lua_running, 0);
        return 0;
    }

    if (g_ST) g_ST(L, top);
    publish_remote_lua_result(0, NULL);
    cocos_debug_lua_profiler_resume_accounting();
    InterlockedExchange(&g_remote_lua_running, 0);
    return 0;
}

/* drawScene only watches for a stuck queue; it never calls into Lua. */
static void remote_lua_tick_timeout(void) {
    if (InterlockedCompareExchange(&g_remote_lua_queued, 0, 0) == 0) {
        g_remote_lua_wait_frames = 0;
        return;
    }
    if (InterlockedCompareExchange(&g_remote_lua_running, 0, 0) != 0)
        return;
    g_remote_lua_wait_frames++;
    if (g_remote_lua_wait_frames < REMOTE_LUA_WAIT_MAX)
        return;
    if (InterlockedCompareExchange(&g_remote_lua_queued, 0, 1) == 1) {
        publish_remote_lua_result(3, "Lua scheduler drain timeout");
        g_remote_lua_wait_frames = 0;
    }
}

static void queue_remote_lua_from_ipc(void) {
    CocosDebugIpcBlock *block = cocos_debug_telemetry_block();
    CocosDebugIpcRemoteLua *remote;
    uint32_t size;
    if (!block) {
        publish_remote_lua_result(2, "IPC unavailable");
        return;
    }
    if (InterlockedCompareExchange(&g_remote_lua_running, 0, 0) != 0 ||
        InterlockedCompareExchange(&g_remote_lua_queued, 0, 0) != 0) {
        publish_remote_lua_result(3, "Remote Lua already queued");
        return;
    }
    remote = &block->remote_lua;
    size = remote->script_size;
    if (size == 0 || size >= REMOTE_CODE_MAX) {
        publish_remote_lua_result(1, "Invalid script size");
        return;
    }
    memcpy(g_remote_code, remote->script, size);
    g_remote_code[size] = 0;
    g_remote_lua_wait_frames = 0;
    InterlockedExchange(&g_remote_lua_queued, 1);
}

static int profiler_poll_and_apply_command(void) {
    int sequence = 0;
    int command = cocos_debug_telemetry_poll_command(&sequence);
    if (command == COCOS_DEBUG_IPC_CMD_NONE && g_recording && !cocos_debug_telemetry_viewer_alive())
        command = COCOS_DEBUG_IPC_CMD_STOP;
    if (sequence != 0 && sequence != g_last_command_sequence) {
        g_last_command_sequence = sequence;
        cocos_debug_telemetry_ack_command(sequence);
    }
    if (command == COCOS_DEBUG_IPC_CMD_START) {
        profiler_start_recording();
    } else if (command == COCOS_DEBUG_IPC_CMD_STOP) {
        /* Stop is applied after the current drawScene frame is closed. */
    } else if (command == COCOS_DEBUG_IPC_CMD_CLEAR) {
        if (g_recording) {
            g_frame_index = 0;
            g_previous_drawscene_begin = 0.0;
            g_director_this = 0;
            g_current_frame_period_ms = 0.0f;
            g_has_pending_frame = 0;
            g_pending_begin_s = 0.0;
            g_detail_frames_remaining = 0;
            g_detail_active = 0;
            cocos_debug_lua_profiler_clear();
            cocos_debug_lua_profiler_set_line_capture(0);
        }
    } else if (command == COCOS_DEBUG_IPC_CMD_DETAIL_CAPTURE) {
        if (g_recording)
            set_detail_capture_frames(PROF_DETAIL_FRAMES);
    } else if (command == COCOS_DEBUG_IPC_CMD_RUN_LUA) {
        queue_remote_lua_from_ipc();
    } else if (command == COCOS_DEBUG_IPC_CMD_TRACE_START) {
        cocos_debug_lua_profiler_set_trace(1);
        if (cocos_debug_telemetry_block())
            InterlockedExchange(
                (LONG *)&cocos_debug_telemetry_block()->header.trace_enabled, 1);
    } else if (command == COCOS_DEBUG_IPC_CMD_TRACE_STOP) {
        cocos_debug_lua_profiler_set_trace(0);
        if (cocos_debug_telemetry_block())
            InterlockedExchange(
                (LONG *)&cocos_debug_telemetry_block()->header.trace_enabled, 0);
    } else if (command == COCOS_DEBUG_IPC_CMD_SHUTDOWN) {
        /* Shutdown is applied after the current drawScene frame is closed. */
    }
    return command;
}

static void profiler_on_drawscene_end(void) {
    int command;

    if (g_recording && cocos_debug_lua_profiler_frame_end(&g_precise_frame)) {
        g_frame_index++;
        g_pending_begin_s = g_previous_drawscene_begin;
        g_has_pending_frame = 1;
        update_precise_detail_capture_after_frame(&g_precise_frame);
    }
    command = profiler_poll_and_apply_command();
    if (command == COCOS_DEBUG_IPC_CMD_START) return;

    if (command == COCOS_DEBUG_IPC_CMD_STOP) {
        profiler_stop_recording();
    } else if (command == COCOS_DEBUG_IPC_CMD_SHUTDOWN) {
        profiler_stop_recording();
        /* Do not unpatch drawScene from inside its trampoline. */
        cocos_debug_telemetry_set_state(COCOS_DEBUG_IPC_STATE_SHUTDOWN, NULL);
    }
}

static int l_profiler_hook_thread(lua_State *L) {
    lua_State *thread = thread_from_lua_arg(L, 1);
    if (!thread) thread = L;
    cocos_debug_lua_profiler_register_thread(thread);
    return 0;
}

static uint32_t find_local_export_contains(HMODULE mod, const char *needle,
                                           const char *prefer_suffix) {
    const unsigned char *base;
    const IMAGE_DOS_HEADER *dos;
    const IMAGE_NT_HEADERS32 *nt;
    const IMAGE_EXPORT_DIRECTORY *exp;
    const DWORD *names;
    const DWORD *fns;
    const WORD *ords;
    DWORD i, n, exp_rva;
    uint32_t first = 0;
    if (!mod || !needle || !needle[0]) return 0;
    base = (const unsigned char *)mod;
    dos = (const IMAGE_DOS_HEADER *)base;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return 0;
    nt = (const IMAGE_NT_HEADERS32 *)(base + dos->e_lfanew);
    exp_rva = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].VirtualAddress;
    if (!exp_rva) return 0;
    exp = (const IMAGE_EXPORT_DIRECTORY *)(base + exp_rva);
    names = (const DWORD *)(base + exp->AddressOfNames);
    ords = (const WORD *)(base + exp->AddressOfNameOrdinals);
    fns = (const DWORD *)(base + exp->AddressOfFunctions);
    n = exp->NumberOfNames;
    for (i = 0; i < n; i++) {
        const char *nm = (const char *)(base + names[i]);
        size_t len;
        if (!strstr(nm, needle)) continue;
        if (!fns[ords[i]]) continue;
        if (prefer_suffix) {
            len = strlen(nm);
            if (len >= strlen(prefer_suffix) &&
                strcmp(nm + len - strlen(prefer_suffix), prefer_suffix) == 0)
                return (uint32_t)(uintptr_t)(base + fns[ords[i]]);
        }
        if (!first) first = (uint32_t)(uintptr_t)(base + fns[ords[i]]);
    }
    return first;
}

static uint32_t resolve_local_renderer_render(void) {
    HMODULE cocos = GetModuleHandleA("libcocos2d.dll");
    FARPROC proc;
    if (!cocos) return 0;
    proc = GetProcAddress(cocos, "?render@Renderer@cocos2d@@QAEXXZ");
    if (proc) return (uint32_t)(uintptr_t)proc;
    proc = GetProcAddress(cocos, "?render@Renderer@cocos2d@@QAEXH@Z");
    if (proc) return (uint32_t)(uintptr_t)proc;
    return find_local_export_contains(cocos, "?render@Renderer@cocos2d@@", "QAEXXZ");
}

static int read_u32_safe(uint32_t addr, uint32_t *out) {
    MEMORY_BASIC_INFORMATION mbi;
    DWORD protect;
    if (!out || addr < 0x10000u) return 0;
    if (!VirtualQuery((const void *)(uintptr_t)addr, &mbi, sizeof(mbi)))
        return 0;
    if (mbi.State != MEM_COMMIT) return 0;
    if (mbi.Protect & PAGE_GUARD) return 0;
    protect = mbi.Protect & 0xFFu;
    if (protect != PAGE_READONLY && protect != PAGE_READWRITE &&
        protect != PAGE_WRITECOPY && protect != PAGE_EXECUTE_READ &&
        protect != PAGE_EXECUTE_READWRITE && protect != PAGE_EXECUTE_WRITECOPY)
        return 0;
    *out = *(const uint32_t *)(uintptr_t)addr;
    return 1;
}

static int renderer_hook_has_fired(void) {
    int i;
    for (i = 0; i < g_cpp_hook_count; i++) {
        if (strstr(g_cpp_hooks[i].name, "Renderer:render") &&
            g_cpp_hooks[i].seq != 0)
            return 1;
        if (strstr(g_cpp_hooks[i].name, "Scene:render") &&
            g_cpp_hooks[i].seq != 0)
            return 1;
    }
    return 0;
}

static int score_renderer_object(uint32_t obj, uint32_t renderer_export) {
    uint32_t vtable, fn;
    int slot, nearby = 0, in_mod = 0;
    if (!read_u32_safe(obj, &vtable) || vtable < 0x10000u) return -1;
    for (slot = 0; slot < 64; slot++) {
        if (!read_u32_safe(vtable + (uint32_t)slot * 4u, &fn)) break;
        if (g_cocos_module_base >= 0x10000u &&
            fn >= g_cocos_module_base &&
            fn < g_cocos_module_base + g_cocos_module_size)
            in_mod++;
        if (renderer_export >= 0x10000u) {
            uint32_t d = fn > renderer_export ? fn - renderer_export
                                              : renderer_export - fn;
            if (d < 0x4000u) nearby++;
        }
        if (fn == renderer_export) nearby += 8;
    }
    if (in_mod < 6) return -1;
    return nearby;
}

static void hook_named_render_addr(const char *name, uint32_t addr) {
    int id;
    if (addr < 0x10000u || !name) return;
    id = add_cpp_hook_target(name, addr);
    if (id < 0) return;
    if (install_one_cpp_hook(&g_cpp_hooks[id], id))
        sync_cpp_hooks_to_config();
}

static void try_hook_live_renderer(uint32_t director) {
    uint32_t renderer_export, off, obj, best_obj = 0, vtable, fn, best_fn = 0;
    uint32_t best_dist = 0x7fffffffu;
    int best_score = 0, score, slot, id;
    if (director < 0x10000u) return;
    if (renderer_hook_has_fired()) return;
    if (g_renderer_resolve_attempts >= 4) return;
    g_renderer_resolve_attempts++;
    /* Give the export prologue one full frame to prove it is on the path. */
    if (g_renderer_resolve_attempts < 2) return;

    renderer_export = 0;
    for (id = 0; id < g_cpp_hook_count; id++) {
        if (strstr(g_cpp_hooks[id].name, "Renderer:render") &&
            g_cpp_hooks[id].orig >= 0x10000u) {
            renderer_export = g_cpp_hooks[id].orig;
            break;
        }
    }
    if (!renderer_export)
        renderer_export = resolve_local_renderer_render();
    if (!renderer_export) return;

    for (off = 4; off < 0x400u; off += 4) {
        if (!read_u32_safe(director + off, &obj) || obj < 0x10000u) continue;
        score = score_renderer_object(obj, renderer_export);
        if (score > best_score) {
            best_score = score;
            best_obj = obj;
        }
    }
    if (best_score < 2 || !best_obj) return;
    if (!read_u32_safe(best_obj, &vtable) || vtable < 0x10000u) return;
    for (slot = 0; slot < 64; slot++) {
        uint32_t dist;
        if (!read_u32_safe(vtable + (uint32_t)slot * 4u, &fn) || fn < 0x10000u)
            break;
        dist = fn > renderer_export ? fn - renderer_export : renderer_export - fn;
        if (dist < best_dist) {
            best_dist = dist;
            best_fn = fn;
        }
    }
    if (best_fn < 0x10000u || best_dist > 0x8000u) return;
    hook_named_render_addr("cc.Renderer:render/vt", best_fn);
}

static void sync_cpp_hooks_to_config(void) {
    CocosDebugIpcBlock *block = cocos_debug_telemetry_block();
    uint32_t i;
    if (!block) return;
    block->config.cpp_count = 0;
    memset(block->config.cpp_targets, 0, sizeof(block->config.cpp_targets));
    for (i = 0; i < (uint32_t)g_cpp_hook_count && i < COCOS_DEBUG_IPC_CPP_CAP; i++) {
        strncpy(block->config.cpp_targets[i].name, g_cpp_hooks[i].name,
                COCOS_DEBUG_IPC_CPP_NAME - 1);
        block->config.cpp_targets[i].name[COCOS_DEBUG_IPC_CPP_NAME - 1] = 0;
        block->config.cpp_targets[i].address = g_cpp_hooks[i].installed
            ? (uint32_t)g_cpp_hooks[i].orig : 0;
        block->config.cpp_count++;
    }
}

static void ensure_renderer_cpp_hook(void) {
    int i;
    uint32_t addr;
    for (i = 0; i < g_cpp_hook_count; i++) {
        if (strstr(g_cpp_hooks[i].name, "Renderer:render") &&
            g_cpp_hooks[i].orig >= 0x10000)
            return;
    }
    addr = resolve_local_renderer_render();
    if (addr)
        add_cpp_hook_target("cc.Renderer:render", addr);
}

static void load_cocos_ipc(void) {
    const CocosDebugIpcInjectionConfig *config = cocos_debug_telemetry_config();
    uint32_t i;
    g_drawscene_addr = 0;
    g_cocos_module_base = 0;
    g_cocos_module_size = 0;
    if (!config) return;
    g_drawscene_addr = config->drawscene_address;
    g_cocos_module_base = config->cocos_base;
    g_cocos_module_size = config->cocos_size;
    for (i = 0; i < config->cpp_count && i < COCOS_DEBUG_IPC_CPP_CAP; i++)
        add_cpp_hook_target(config->cpp_targets[i].name, config->cpp_targets[i].address);
    ensure_renderer_cpp_hook();
}

static FARPROC resolve_proc(const char *name) {
    static HMODULE mods[1024];
    static int modc = 0;
    static int inited = 0;
    const char *aliases[3];
    char undec[96];
    FARPROC proc;
    HMODULE all[1024];
    DWORD cb;
    int i, j, k;

    aliases[0] = name;
    _snprintf(undec, sizeof(undec), "_%s", name);
    aliases[1] = undec;
    aliases[2] = NULL;

    if (!inited) {
        mods[modc++] = GetModuleHandleA(NULL);
        if (EnumProcessModules(GetCurrentProcess(), all, sizeof(all), &cb)) {
            int n = (int)(cb / sizeof(HMODULE));
            for (i = 0; i < n && modc < (int)(sizeof(mods) / sizeof(mods[0])); i++) {
                for (k = 0; k < modc; k++) {
                    if (mods[k] == all[i]) break;
                }
                if (k == modc) mods[modc++] = all[i];
            }
        }
        inited = 1;
    }

    for (j = 0; aliases[j]; j++) {
        for (i = 0; i < modc; i++) {
            proc = GetProcAddress(mods[i], aliases[j]);
            if (proc) return proc;
        }
    }
    return NULL;
}

static void resolve_lua_apis(void) {
    g_LS = (luaL_loadstring_fn)resolve_proc("luaL_loadstring");
    g_LB = (luaL_loadbuffer_fn)resolve_proc("luaL_loadbuffer");
    g_PC = (lua_pcall_fn)resolve_proc("lua_pcall");
    g_TL = (lua_tolstring_fn)resolve_proc("lua_tolstring");
    g_TY = (lua_type_fn)resolve_proc("lua_type");
    g_ST = (lua_settop_fn)resolve_proc("lua_settop");
    g_GT = (lua_gettop_fn)resolve_proc("lua_gettop");
    g_SH = (lua_sethook_fn)resolve_proc("lua_sethook");
    g_GI = (lua_getinfo_fn)resolve_proc("lua_getinfo");
    g_GS = (lua_getstack_fn)resolve_proc("lua_getstack");
    g_PCC = (lua_pushcclosure_fn)resolve_proc("lua_pushcclosure");
    g_SG = (lua_setglobal_fn)resolve_proc("lua_setglobal");
    g_SFL = (lua_setfield_fn)resolve_proc("lua_setfield");
    g_TTH = (lua_tothread_fn)resolve_proc("lua_tothread");
    g_GC = (lua_gc_fn)resolve_proc("lua_gc");
    g_profiler_c_api =
        g_PCC && (g_SG || g_SFL) && g_TTH && g_TY && g_SH && g_GI && g_GS;
}

static void register_global_cfunction(lua_State *L, const char *name, lua_CFunction fn) {
    if (!g_PCC) return;
    g_PCC(L, fn, 0);
    if (g_SG) g_SG(L, name);
    else if (g_SFL) g_SFL(L, LUA_GLOBALSINDEX, name);
}

/* Lua script talks to C through coroutine hooks + remote Lua drain. */
static void register_profiler_api(lua_State *L) {
    if (g_profiler_c_api) {
        register_global_cfunction(L, "_LUA_TOOL_profiler_hook_thread", l_profiler_hook_thread);
    }
    /* Drain only needs pushcclosure; register even if hook APIs are incomplete. */
    if (g_PCC && (g_SG || g_SFL))
        register_global_cfunction(L, "_LUA_TOOL_drain_remote_lua", l_drain_remote_lua);
}

static void pop_one(lua_State *L) {
    if (g_ST) g_ST(L, -2);
}

static void run_lua_on_window_thread(void) {
    int err;
    const char *s;

    if (InterlockedExchange(&g_running, 1) != 0) return;

    if (!g_L || !g_LS || !g_PC) {
        cocos_debug_telemetry_set_state(COCOS_DEBUG_IPC_STATE_ERROR, "missing Lua state or API");
        InterlockedExchange(&g_running, 0);
        InterlockedExchange(&g_done, 1);
        return;
    }

    register_profiler_api(g_L);

    err = g_LS(g_L, g_code);
    if (!err) {
        err = g_PC(g_L, 0, 1, 0);
        if (!err) {
            pop_one(g_L);
            if (!install_drawscene_hook()) {
                cocos_debug_telemetry_set_state(COCOS_DEBUG_IPC_STATE_ERROR, "C++ frame hook install failed");
            } else {
                cocos_debug_telemetry_set_state(COCOS_DEBUG_IPC_STATE_READY, NULL);
            }
        } else {
            s = g_TL ? g_TL(g_L, -1, NULL) : NULL;
            cocos_debug_telemetry_set_state(COCOS_DEBUG_IPC_STATE_ERROR, s ? s : "Lua bootstrap failed");
            pop_one(g_L);
        }
    } else {
        s = g_TL ? g_TL(g_L, -1, NULL) : NULL;
        cocos_debug_telemetry_set_state(COCOS_DEBUG_IPC_STATE_ERROR, s ? s : "Lua bootstrap compile failed");
        pop_one(g_L);
    }

    if (g_hwnd && g_old_proc) {
        SetWindowLongPtrA(g_hwnd, GWLP_WNDPROC, (LONG_PTR)g_old_proc);
    }
    InterlockedExchange(&g_running, 0);
    InterlockedExchange(&g_done, 1);
}

static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == RUN_LUA_MSG) {
        run_lua_on_window_thread();
        return 0;
    }
    return CallWindowProcA(g_old_proc, hwnd, msg, wp, lp);
}

static BOOL CALLBACK enum_windows_proc(HWND hwnd, LPARAM param) {
    DWORD target_pid = (DWORD)param;
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid != target_pid) return TRUE;
    if (!IsWindowVisible(hwnd)) return TRUE;
    if (GetAncestor(hwnd, GA_ROOT) != hwnd) return TRUE;
    g_hwnd = hwnd;
    return FALSE;
}

static HMODULE find_lua_module(void) {
    HMODULE mods[1024];
    DWORD cb;
    HMODULE found = NULL;
    if (EnumProcessModules(GetCurrentProcess(), mods, sizeof(mods), &cb)) {
        int n = (int)(cb / sizeof(HMODULE));
        int i;
        for (i = 0; i < n; i++) {
            char nm[MAX_PATH];
            if (GetModuleBaseNameA(GetCurrentProcess(), mods[i], nm, sizeof(nm))) {
                if (strstr(nm, "lua") || strstr(nm, "Lua")) {
                    found = mods[i];
                    break;
                }
            }
        }
    }
    return found ? found : GetModuleHandleA(NULL);
}

static DWORD WINAPI thread(LPVOID u) {
    (void)u;
    HMODULE dll;
    const CocosDebugIpcInjectionConfig *config;
    CocosDebugLuaProfilerApi profiler_api;
    int api_hook_flags;
    int i;

    if (!cocos_debug_telemetry_open(GetCurrentProcessId())) return 1;
    config = cocos_debug_telemetry_config();
    if (!config || config->lua_state < 0x10000 ||
        config->bootstrap_size == 0 || config->bootstrap_size >= sizeof(g_code)) {
        cocos_debug_telemetry_set_state(COCOS_DEBUG_IPC_STATE_ERROR, "invalid injection configuration");
        return 1;
    }
    load_cocos_ipc();
    dll = find_lua_module();

    if (!dll) {
        cocos_debug_telemetry_set_state(COCOS_DEBUG_IPC_STATE_ERROR, "no Lua module");
        return 1;
    }

    resolve_lua_apis();
    if (!g_LS || !g_PC || !g_SH || !g_GI || !g_GS) {
        cocos_debug_telemetry_set_state(COCOS_DEBUG_IPC_STATE_ERROR, "missing core Lua API");
        return 1;
    }
    memset(&profiler_api, 0, sizeof(profiler_api));
    profiler_api.sethook = (CocosDebugLuaSetHookFn)g_SH;
    profiler_api.getinfo = (CocosDebugLuaGetInfoFn)g_GI;
    profiler_api.getstack = (CocosDebugLuaGetStackFn)g_GS;
    profiler_api.intern_method = cocos_debug_telemetry_intern_method;
    profiler_api.emit_event = cocos_debug_telemetry_emit_lua_event;
    if (!cocos_debug_lua_profiler_init(&profiler_api)) {
        cocos_debug_telemetry_set_state(
            COCOS_DEBUG_IPC_STATE_ERROR, "precise Lua profiler init failed");
        return 1;
    }
    api_hook_flags = cocos_debug_lua_api_hooks_install(
        (void *)resolve_proc("lua_resume"),
        (void *)resolve_proc("lua_newthread"),
        (void *)g_PC);
    if ((api_hook_flags & 1) == 0) {
        cocos_debug_telemetry_set_state(
            COCOS_DEBUG_IPC_STATE_ERROR, "lua_resume hook install failed");
        return 1;
    }
    if ((api_hook_flags & 4) == 0) {
        /* Without it every error caught by pcall leaves the profiler's shadow
           stack permanently one shift too deep, which silently corrupts all
           later inclusive times for that Lua thread.  Refuse to report data
           that would be wrong rather than degrade quietly. */
        cocos_debug_telemetry_set_state(
            COCOS_DEBUG_IPC_STATE_ERROR, "lua_pcall hook install failed");
        return 1;
    }
    cocos_debug_luac_dump_init(config->luac_dump_dir);
    cocos_debug_luac_dump_install_hooks();
    g_L = (lua_State *)(uintptr_t)config->lua_state;
    memcpy(g_code, config->bootstrap_lua, config->bootstrap_size);
    g_code[config->bootstrap_size] = 0;

    for (i = 0; i < 100 && !g_hwnd; i++) {
        EnumWindows(enum_windows_proc, (LPARAM)GetCurrentProcessId());
        if (!g_hwnd) Sleep(50);
    }
    if (!g_hwnd) {
        cocos_debug_telemetry_set_state(COCOS_DEBUG_IPC_STATE_ERROR, "no target window");
        return 1;
    }

    SetLastError(0);
    g_old_proc = (WNDPROC)SetWindowLongPtrA(g_hwnd, GWLP_WNDPROC, (LONG_PTR)wnd_proc);
    if (!g_old_proc && GetLastError() != 0) {
        cocos_debug_telemetry_set_state(COCOS_DEBUG_IPC_STATE_ERROR, "SetWindowLongPtr failed");
        return 1;
    }

    PostMessageA(g_hwnd, RUN_LUA_MSG, 0, 0);
    for (i = 0; i < 300 && !g_done; i++) Sleep(50);
    return g_done ? 0 : 2;
}

BOOL APIENTRY DllMain(HMODULE h, DWORD r, LPVOID v) {
    (void)v;
    if (r == DLL_PROCESS_ATTACH) {
        HANDLE t;
        DisableThreadLibraryCalls(h);
        t = CreateThread(NULL, 0, thread, NULL, 0, NULL);
        if (t) CloseHandle(t);
    } else if (r == DLL_PROCESS_DETACH) {
        cocos_debug_telemetry_close();
    }
    return TRUE;
}
