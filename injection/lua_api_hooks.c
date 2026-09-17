#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <string.h>

#include "lua.h"
#include "lua_profiler.h"
#include "lua_api_hooks.h"

typedef int (__cdecl *LuaResumeFn)(lua_State *, int);
typedef lua_State *(__cdecl *LuaNewThreadFn)(lua_State *);

typedef struct ApiHook {
    uint8_t *target;
    void *trampoline;
    uint8_t original[16];
    int original_size;
} ApiHook;

static ApiHook g_resume_hook;
static ApiHook g_newthread_hook;
static LuaResumeFn g_resume_original;
static LuaNewThreadFn g_newthread_original;

static int x86_modrm_size(const uint8_t *p) {
    uint8_t modrm = p[0];
    int mod = modrm >> 6;
    int rm = modrm & 7;
    int n = 1;
    if (mod == 3) return 1;
    if (rm == 4) {
        n++;
        if (mod == 0 && (p[1] & 7) == 5) n += 4;
    } else if (mod == 0 && rm == 5) {
        n += 4;
    }
    if (mod == 1) n += 1;
    else if (mod == 2) n += 4;
    return n;
}

static int x86_insn_len(const uint8_t *p) {
    int i = 0;
    int n;
    uint8_t op;
    while (i < 4) {
        uint8_t b = p[i];
        if (b == 0x66 || b == 0x67 || b == 0xF0 || b == 0xF2 ||
            b == 0xF3 || b == 0x2E || b == 0x36 || b == 0x3E ||
            b == 0x26) {
            i++;
            continue;
        }
        break;
    }
    op = p[i++];
    if (op == 0x0F) return 0;
    if ((op >= 0x50 && op <= 0x5F) || op == 0x90 || op == 0xC3)
        return i;
    if (op == 0x6A || op == 0xEB) return i + 1;
    if (op == 0x68 || op == 0xE8 || op == 0xE9) return i + 4;
    if (op >= 0xB8 && op <= 0xBF) return i + 4;
    if (op == 0x81) return i + x86_modrm_size(p + i) + 4;
    if (op == 0x83 || op == 0xC6) return i + x86_modrm_size(p + i) + 1;
    if (op == 0xC7) return i + x86_modrm_size(p + i) + 4;
    if (op == 0x8B || op == 0x89 || op == 0x8D || op == 0x85 ||
        op == 0x33 || op == 0x3B || op == 0x01 || op == 0x29 ||
        op == 0x39 || op == 0x31 || op == 0x8A || op == 0x88) {
        n = x86_modrm_size(p + i);
        if (n <= 0) return 0;
        return i + n;
    }
    return 0;
}

static int measure_stolen(const uint8_t *target) {
    int stolen = 0;
    while (stolen < 5) {
        int size = x86_insn_len(target + stolen);
        if (size <= 0 || stolen + size > 16) return 0;
        stolen += size;
    }
    return stolen;
}

static int relocate_stolen(const uint8_t *source, uint8_t *copy, int size) {
    int offset = 0;
    while (offset < size) {
        int length = x86_insn_len(source + offset);
        int prefix = 0;
        uint8_t opcode;
        if (length <= 0 || offset + length > size) return 0;
        while (prefix < length) {
            uint8_t value = source[offset + prefix];
            if (value == 0x66 || value == 0x67 || value == 0xF0 ||
                value == 0xF2 || value == 0xF3 || value == 0x2E ||
                value == 0x36 || value == 0x3E || value == 0x26) {
                prefix++;
                continue;
            }
            break;
        }
        opcode = source[offset + prefix];
        if (opcode == 0xE8 || opcode == 0xE9) {
            int32_t old_relative;
            int32_t new_relative;
            const uint8_t *destination;
            memcpy(&old_relative, source + offset + prefix + 1, 4);
            destination = source + offset + prefix + 5 + old_relative;
            new_relative = (int32_t)(destination -
                (copy + offset + prefix + 5));
            memcpy(copy + offset + prefix + 1, &new_relative, 4);
        } else if (opcode == 0xEB) {
            /* Expanding a short branch needs a larger relocation engine. */
            return 0;
        }
        offset += length;
    }
    return 1;
}

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
    stolen = measure_stolen(hook->target);
    if (stolen < 5) return 0;
    trampoline = (uint8_t *)VirtualAlloc(
        NULL, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!trampoline) return 0;
    memcpy(trampoline, hook->target, stolen);
    if (!relocate_stolen(hook->target, trampoline, stolen)) {
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
    hook->target[0] = 0xE9;
    relative = (int32_t)((uint8_t *)replacement - (hook->target + 5));
    memcpy(hook->target + 1, &relative, sizeof(relative));
    for (i = 5; i < stolen; i++) hook->target[i] = 0x90;
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
    cocos_debug_lua_profiler_resume_leave(L);
    return result;
}

static lua_State *__cdecl hook_lua_newthread(lua_State *L) {
    lua_State *thread = g_newthread_original(L);
    if (thread) cocos_debug_lua_profiler_register_thread(thread);
    return thread;
}

int cocos_debug_lua_api_hooks_install(void *lua_resume_address,
                               void *lua_newthread_address) {
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
    return installed;
}

void cocos_debug_lua_api_hooks_remove(void) {
    remove_hook(&g_newthread_hook);
    remove_hook(&g_resume_hook);
    g_newthread_original = NULL;
    g_resume_original = NULL;
}
