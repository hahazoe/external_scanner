#include "lua_vm_scan.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <tlhelp32.h>

#define LUA_MODULE_CAP 64
#define GLOBAL_CANDIDATE_CAP 2048
#define SCAN_CHUNK 0x100000

/*
 * Lua 5.1 global_State layout (32-bit), used only as a scanner heuristic:
 *   +0  strt.hash, +4 strt.nuse, +8 strt.size, +12 frealloc,
 *   +21 currentwhite, +28 rootgc (GCObject* chain).
 */
#define GS_STRT_NUSE 4
#define GS_STRT_SIZE 8
#define GS_FREALLOC 12
#define GS_CURRENTWHITE 21
#define GS_ROOTGC 28

#define LUA_TTHREAD 8

typedef struct LuaModuleRange {
    uintptr_t begin;
    uintptr_t end;
} LuaModuleRange;

typedef struct ThreadCandidate {
    uint32_t address;
    int status;
    int stack_len;
    int stacksize;
} ThreadCandidate;

static LuaModuleRange g_lua_modules[LUA_MODULE_CAP];
static int g_lua_module_count;

static int is_power_of_two(int n) {
    return n > 0 && (n & (n - 1)) == 0;
}

static int module_name_contains_lua(const char *name) {
    int i;
    if (!name) return 0;
    /* Skip our own temp dumps: lua_*.dll / LUA_*.dll */
    if ((name[0] == 'L' || name[0] == 'l') &&
        (name[1] == 'U' || name[1] == 'u') &&
        (name[2] == 'A' || name[2] == 'a') &&
        name[3] == '_')
        return 0;
    for (i = 0; name[i]; i++) {
        char a = (char)(name[i] | 32);
        char b = (char)(name[i + 1] | 32);
        char c = (char)(name[i + 2] | 32);
        if (a == 'l' && b == 'u' && c == 'a')
            return 1;
    }
    return 0;
}

int cocos_debug_collect_lua_modules(DWORD pid) {
    HANDLE snapshot;
    MODULEENTRY32 module;
    g_lua_module_count = 0;
    snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, pid);
    if (snapshot == INVALID_HANDLE_VALUE) return 0;
    memset(&module, 0, sizeof(module));
    module.dwSize = sizeof(module);
    if (Module32First(snapshot, &module)) {
        do {
            if (!module_name_contains_lua(module.szModule)) continue;
            if (g_lua_module_count >= LUA_MODULE_CAP) break;
            g_lua_modules[g_lua_module_count].begin =
                (uintptr_t)module.modBaseAddr;
            g_lua_modules[g_lua_module_count].end =
                g_lua_modules[g_lua_module_count].begin + module.modBaseSize;
            g_lua_module_count++;
        } while (Module32Next(snapshot, &module));
    }
    CloseHandle(snapshot);
    return g_lua_module_count > 0;
}

int cocos_debug_lua_module_count(void) {
    return g_lua_module_count;
}

int cocos_debug_address_in_lua_module(uintptr_t address) {
    int i;
    for (i = 0; i < g_lua_module_count; i++) {
        if (address >= g_lua_modules[i].begin &&
            address < g_lua_modules[i].end)
            return 1;
    }
    return 0;
}

static int plausible_global_state(const BYTE *bytes) {
    int size;
    int nuse;
    if (!bytes) return 0;
    if (!cocos_debug_address_in_lua_module(*(uint32_t *)(bytes + GS_FREALLOC)))
        return 0;
    if (bytes[GS_CURRENTWHITE] > 5) return 0;
    size = *(int *)(bytes + GS_STRT_SIZE);
    nuse = *(int *)(bytes + GS_STRT_NUSE);
    if (size < 32 || size > 2000000 || !is_power_of_two(size)) return 0;
    if (nuse < 0 || nuse > size) return 0;
    return 1;
}

int cocos_debug_scan_lua_state(HANDLE process, uint32_t *out_state,
                        CocosDebugScanProgressFn progress) {
    uint32_t globals[GLOBAL_CANDIDATE_CAP];
    int global_count = 0;
    uint32_t max_address = 0;
    MEMORY_BASIC_INFORMATION info;
    uintptr_t address = 0;
    BYTE *chunk;
    int i;
    int found = 0;

    if (!process || !out_state) return 0;
    *out_state = 0;
    chunk = (BYTE *)malloc(SCAN_CHUNK);
    if (!chunk) return 0;
    if (progress) progress(25, "Scanning Lua VM memory");

    while (VirtualQueryEx(process, (void *)address, &info, sizeof(info)) ==
           sizeof(info)) {
        int writable = (info.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE)) != 0;
        if (info.State == MEM_COMMIT && writable) {
            uint32_t region = (uint32_t)(uintptr_t)info.BaseAddress;
            uint32_t region_end = region + (uint32_t)info.RegionSize;
            uint32_t cursor;
            if (region_end > max_address) max_address = region_end;
            for (cursor = region;
                 cursor < region_end && global_count < GLOBAL_CANDIDATE_CAP;
                 cursor += SCAN_CHUNK) {
                SIZE_T to_read = SCAN_CHUNK;
                SIZE_T got = 0;
                SIZE_T offset;
                if (cursor + to_read > region_end) to_read = region_end - cursor;
                if (!ReadProcessMemory(process, (void *)cursor, chunk, to_read, &got))
                    continue;
                for (offset = 0;
                     offset + 52 <= got && global_count < GLOBAL_CANDIDATE_CAP;
                     offset += 4) {
                    if (!plausible_global_state(chunk + offset)) continue;
                    globals[global_count++] = cursor + (uint32_t)offset;
                }
            }
        }
        address = (uintptr_t)info.BaseAddress + info.RegionSize;
        if (address > 0x7FFFFFFFu) break;
    }
    free(chunk);
    if (progress) progress(60, "Validating Lua VM candidates");

    for (i = 0; i < global_count && !found; i++) {
        uint32_t global_addr = globals[i];
        uint32_t gc_heads[6];
        BYTE header[64];
        SIZE_T got = 0;
        ThreadCandidate threads[256];
        ThreadCandidate unique[128];
        int thread_count = 0;
        int unique_count = 0;
        int list;
        int j;

        if (!ReadProcessMemory(process, (void *)global_addr, header, 64, &got) ||
            got < 52)
            continue;
        if (!plausible_global_state(header)) continue;
        for (j = 0; j < 6; j++)
            gc_heads[j] = *(uint32_t *)(header + GS_ROOTGC + j * 4);

        for (list = 0; list < 6 && thread_count < 256; list++) {
            uint32_t current = gc_heads[list];
            int seen = 0;
            while (current >= 0x10000u && current < max_address &&
                   seen++ < 50000 && thread_count < 256) {
                BYTE node[16];
                if (!ReadProcessMemory(process, (void *)current, node, 16, &got) ||
                    got < 16)
                    break;
                if (node[4] == LUA_TTHREAD) {
                    BYTE state[80];
                    if (ReadProcessMemory(process, (void *)current, state, 80, &got) &&
                        got >= 32) {
                        threads[thread_count].address = current;
                        threads[thread_count].status = state[6];
                        threads[thread_count].stack_len =
                            (int)(*(uint32_t *)(state + 8) - *(uint32_t *)(state + 12));
                        threads[thread_count].stacksize =
                            (int)(*(uint32_t *)(state + 28) - *(uint32_t *)(state + 12));
                        thread_count++;
                    }
                }
                memcpy(&current, node, sizeof(current));
            }
        }
        if (!thread_count) continue;
        {
            int valid = 0;
            for (j = 0; j < thread_count; j++) {
                if (threads[j].status <= 1 &&
                    threads[j].stacksize >= 20 &&
                    threads[j].stacksize < 100000)
                    valid = 1;
            }
            if (!valid) continue;
        }
        for (j = 0; j < thread_count && unique_count < 128; j++) {
            int k;
            int duplicate = 0;
            for (k = 0; k < unique_count; k++) {
                if (unique[k].address == threads[j].address) duplicate = 1;
            }
            if (!duplicate) unique[unique_count++] = threads[j];
        }
        if (!unique_count) continue;
        *out_state = unique[0].address;
        for (j = 0; j < unique_count; j++) {
            if (unique[j].status == 0) {
                *out_state = unique[j].address;
                break;
            }
        }
        found = 1;
    }
    return found;
}
