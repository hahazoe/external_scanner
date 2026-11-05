#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <string.h>

#include "lua.h"
#include "lua_profiler.h"
#include "lua_api_hooks.h"
#include "x86_insn.h"
#include "patch_freeze.h"

typedef int (__cdecl *LuaResumeFn)(lua_State *, int);
typedef lua_State *(__cdecl *LuaNewThreadFn)(lua_State *);
typedef int (__cdecl *LuaPcallFn)(lua_State *, int, int, int);
typedef void (__cdecl *LuaCallFn)(lua_State *, int, int);

typedef struct ApiHook {
    uint8_t *target;
    void *trampoline;
    uint8_t original[16];
    int original_size;
} ApiHook;

static ApiHook g_resume_hook;
static ApiHook g_newthread_hook;
static ApiHook g_pcall_hook;
static ApiHook g_call_hook;
static LuaResumeFn g_resume_original;
static LuaNewThreadFn g_newthread_original;
static LuaPcallFn g_pcall_original;
static LuaCallFn g_call_original;

static int install_hook(ApiHook *hook, void *target_address,
                        void *replacement) {
    DWORD old_protect;
    uint8_t *trampoline;
    int32_t relative;
    int stolen;
    int i;
    if (!hook || !target_address || !replacement) return 0;
    memset(hook, 0, sizeof(*hook));
    hook->target = (uint8_t *)target_address;
    stolen = x86_measure_stolen(hook->target, 16);
    if (stolen < 5) return 0;
    trampoline = (uint8_t *)VirtualAlloc(
        NULL, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!trampoline) return 0;
    if (x86_reloc_copy(trampoline, hook->target, stolen,
                       hook->target, trampoline) != stolen) {
        VirtualFree(trampoline, 0, MEM_RELEASE);
        memset(hook, 0, sizeof(*hook));
        return 0;
    }
    trampoline[stolen] = 0xE9;
    relative = (int32_t)((hook->target + stolen) -
                         (trampoline + stolen + 5));
    memcpy(trampoline + stolen + 1, &relative, sizeof(relative));
    memcpy(hook->original, hook->target, stolen);
    hook->original_size = stolen;
    if (!VirtualProtect(hook->target, stolen, PAGE_EXECUTE_READWRITE,
                        &old_protect)) {
        VirtualFree(trampoline, 0, MEM_RELEASE);
        memset(hook, 0, sizeof(*hook));
        return 0;
    }
    /* No other thread may execute the target while the jmp is written. */
    patch_freeze_begin();
    hook->target[0] = 0xE9;
    relative = (int32_t)((uint8_t *)replacement - (hook->target + 5));
    memcpy(hook->target + 1, &relative, sizeof(relative));
    for (i = 5; i < stolen; i++) hook->target[i] = 0x90;
    patch_freeze_end();
    VirtualProtect(hook->target, stolen, old_protect, &old_protect);
    FlushInstructionCache(GetCurrentProcess(), hook->target, stolen);
    FlushInstructionCache(GetCurrentProcess(), trampoline, 64);
    hook->trampoline = trampoline;
    return 1;
}

static void remove_hook(ApiHook *hook) {
    DWORD old_protect;
    if (!hook || !hook->target || !hook->original_size) return;
    if (VirtualProtect(hook->target, hook->original_size,
                       PAGE_EXECUTE_READWRITE, &old_protect)) {
        memcpy(hook->target, hook->original, hook->original_size);
        VirtualProtect(hook->target, hook->original_size,
                       old_protect, &old_protect);
        FlushInstructionCache(GetCurrentProcess(), hook->target,
                              hook->original_size);
    }
    if (hook->trampoline)
        VirtualFree(hook->trampoline, 0, MEM_RELEASE);
    memset(hook, 0, sizeof(*hook));
}

static int __cdecl hook_lua_resume(lua_State *L, int nargs) {
    int result;
    cocos_debug_lua_profiler_resume_enter(L);
    result = g_resume_original(L, nargs);
    cocos_debug_lua_profiler_resume_leave(L, result);
    return result;
}

static lua_State *__cdecl hook_lua_newthread(lua_State *L) {
    lua_State *thread = g_newthread_original(L);
    if (thread) cocos_debug_lua_profiler_register_thread(thread);
    return thread;
}

/* A thrown error longjmps straight out of the protected call, so every Lua
   frame in between is dropped without a RET hook.  Bracketing lua_pcall is
   the only way to learn that the profiler's shadow stack has to be unwound
   by that many frames. */
static int __cdecl hook_lua_pcall(lua_State *L, int nargs, int nresults,
                                  int errfunc) {
    int tracked = cocos_debug_lua_profiler_protected_enter(L);
    int result = g_pcall_original(L, nargs, nresults, errfunc);
    cocos_debug_lua_profiler_protected_leave(L, tracked, result);
    return result;
}

/* lua_call does not catch errors.  If the call throws, this leave is skipped
   and the enclosing pcall restores C-API depth from its saved base. */
static void __cdecl hook_lua_call(lua_State *L, int nargs, int nresults) {
    cocos_debug_lua_profiler_call_enter(L);
    g_call_original(L, nargs, nresults);
    cocos_debug_lua_profiler_call_leave(L);
}

int cocos_debug_lua_api_hooks_install(void *lua_resume_address,
                               void *lua_newthread_address,
                               void *lua_pcall_address,
                               void *lua_call_address) {
    int installed = 0;
    if (lua_resume_address &&
        install_hook(&g_resume_hook, lua_resume_address,
                     (void *)hook_lua_resume)) {
        g_resume_original = (LuaResumeFn)g_resume_hook.trampoline;
        installed |= 1;
    }
    if (lua_newthread_address &&
        install_hook(&g_newthread_hook, lua_newthread_address,
                     (void *)hook_lua_newthread)) {
        g_newthread_original = (LuaNewThreadFn)g_newthread_hook.trampoline;
        installed |= 2;
    }
    if (lua_pcall_address &&
        install_hook(&g_pcall_hook, lua_pcall_address,
                     (void *)hook_lua_pcall)) {
        g_pcall_original = (LuaPcallFn)g_pcall_hook.trampoline;
        installed |= 4;
    }
    if (lua_call_address &&
        install_hook(&g_call_hook, lua_call_address,
                     (void *)hook_lua_call)) {
        g_call_original = (LuaCallFn)g_call_hook.trampoline;
        installed |= 8;
    }
    return installed;
}

void cocos_debug_lua_api_hooks_remove(void) {
    remove_hook(&g_call_hook);
    remove_hook(&g_pcall_hook);
    remove_hook(&g_newthread_hook);
    remove_hook(&g_resume_hook);
    g_call_original = NULL;
    g_pcall_original = NULL;
    g_newthread_original = NULL;
    g_resume_original = NULL;
}
