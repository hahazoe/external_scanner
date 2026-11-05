#include "lua_vm_scan.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <tlhelp32.h>

#ifndef TH32CS_SNAPMODULE32
#define TH32CS_SNAPMODULE32 0x00000010
#endif

#define LUA_MODULE_CAP 64
#define GLOBAL_CANDIDATE_CAP 2048
#define SCAN_CHUNK 0x100000
#define LJ_CANDIDATE_CAP 256

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

/*
 * LuaJIT 2.x 32-bit (!LJ_GC64) lua_State:
 *   +0 nextgc, +4 marked, +5 gct (== ~LJ_TTHREAD, 6 on 2.1 / 8 on older),
 *   +6 dummy_ffid (FF_C=1), +7 status, +8 glref, +12 gclist,
 *   +16 base, +20 top, +24 maxstack, +28 stack, +44 stacksize.
 * Main thread is embedded in GG_State; global_State.mainthref points back to L.
 */
#define LJ_GCT 5
#define LJ_FFID 6
#define LJ_STATUS 7
#define LJ_GLREF 8
#define LJ_BASE 16
#define LJ_TOP 20
#define LJ_STACK 28
#define LJ_STACKSIZE 44
#define LJ_STATE_SIZE 48

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
static int g_lua_is_luajit;

static int is_power_of_two(int n) {
    return n > 0 && (n & (n - 1)) == 0;
}

static int rpm(HANDLE process, uintptr_t addr, void *buf, size_t n) {
    SIZE_T got = 0;
    if (!process || !buf || n == 0) return 0;
    if (!ReadProcessMemory(process, (void *)addr, buf, n, &got)) return 0;
    return got == n;
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

static int remote_module_has_export(HANDLE process, uintptr_t base, const char *name) {
    IMAGE_DOS_HEADER dos;
    IMAGE_NT_HEADERS32 nt;
    IMAGE_EXPORT_DIRECTORY exp;
    DWORD i, n, name_rva, exp_rva;
    char nm[128];

    if (!process || base < 0x10000 || !name || !name[0]) return 0;
    if (!rpm(process, base, &dos, sizeof(dos)) || dos.e_magic != IMAGE_DOS_SIGNATURE)
        return 0;
    if (!rpm(process, base + dos.e_lfanew, &nt, sizeof(nt))) return 0;
    exp_rva = nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].VirtualAddress;
    if (!exp_rva) return 0;
    if (!rpm(process, base + exp_rva, &exp, sizeof(exp))) return 0;
    n = exp.NumberOfNames;
    if (n > 30000) n = 30000;
    for (i = 0; i < n; i++) {
        SIZE_T got = 0;
        if (!rpm(process, base + exp.AddressOfNames + i * 4, &name_rva, 4)) continue;
        memset(nm, 0, sizeof(nm));
        if (!ReadProcessMemory(process, (void *)(base + name_rva), nm, sizeof(nm) - 1, &got))
            continue;
        if (strcmp(nm, name) == 0) return 1;
    }
    return 0;
}

static int detect_luajit(HANDLE process) {
    int i;
    if (!process) return 0;
    for (i = 0; i < g_lua_module_count; i++) {
        if (remote_module_has_export(process, g_lua_modules[i].begin, "luaJIT_setmode"))
            return 1;
        if (remote_module_has_export(process, g_lua_modules[i].begin, "luaJIT_version_2_1_0_beta3") ||
            remote_module_has_export(process, g_lua_modules[i].begin, "luaJIT_version_2_1_0_beta2") ||
            remote_module_has_export(process, g_lua_modules[i].begin, "luaJIT_version_2_0_5"))
            return 1;
    }
    return 0;
}

int cocos_debug_collect_lua_modules(DWORD pid) {
    HANDLE snapshot;
    MODULEENTRY32 module;
    g_lua_module_count = 0;
    g_lua_is_luajit = 0;
    snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (snapshot == INVALID_HANDLE_VALUE)
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

int cocos_debug_lua_is_luajit(void) {
    return g_lua_is_luajit;
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

static int scan_lua51_state(HANDLE process, uint32_t *out_state,
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
    if (progress) progress(25, "Scanning Lua 5.1 VM memory");

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
    if (progress) progress(60, "Validating Lua 5.1 candidates");

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

static int luajit_glref_points_to_main(HANDLE process, uint32_t L, uint32_t glref) {
    BYTE gblob[256];
    int off;
    if (glref < 0x10000u) return 0;
    /* Main-thread L lives in GG_State; global_State follows within ~0x100. */
    if (glref < L || glref - L > 0x200u) return 0;
    if (!rpm(process, glref, gblob, sizeof(gblob))) return 0;
    for (off = 0; off + 4 <= (int)sizeof(gblob); off += 4) {
        uint32_t v;
        memcpy(&v, gblob + off, 4);
        if (v == L) return 1;
    }
    return 0;
}

static int scan_luajit_state(HANDLE process, uint32_t *out_state,
                             CocosDebugScanProgressFn progress) {
    MEMORY_BASIC_INFORMATION info;
    uintptr_t address = 0;
    BYTE *chunk;
    uint32_t cands[LJ_CANDIDATE_CAP];
    int cand_count = 0;
    int i;

    if (!process || !out_state) return 0;
    *out_state = 0;
    chunk = (BYTE *)malloc(SCAN_CHUNK);
    if (!chunk) return 0;
    if (progress) progress(25, "Scanning LuaJIT VM memory");

    while (VirtualQueryEx(process, (void *)address, &info, sizeof(info)) ==
           sizeof(info)) {
        int writable = (info.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE)) != 0;
        if (info.State == MEM_COMMIT && writable) {
            uint32_t region = (uint32_t)(uintptr_t)info.BaseAddress;
            uint32_t region_end = region + (uint32_t)info.RegionSize;
            uint32_t cursor;
            for (cursor = region;
                 cursor < region_end && cand_count < LJ_CANDIDATE_CAP;
                 cursor += SCAN_CHUNK) {
                SIZE_T to_read = SCAN_CHUNK;
                SIZE_T got = 0;
                SIZE_T offset;
                if (cursor + to_read > region_end) to_read = region_end - cursor;
                if (!ReadProcessMemory(process, (void *)cursor, chunk, to_read, &got))
                    continue;
                for (offset = 0;
                     offset + LJ_STATE_SIZE <= got && cand_count < LJ_CANDIDATE_CAP;
                     offset += 4) {
                    const BYTE *p = chunk + offset;
                    uint8_t gct = p[LJ_GCT];
                    uint8_t ffid = p[LJ_FFID];
                    uint8_t status = p[LJ_STATUS];
                    uint32_t glref, basep, topp, stack, stacksize, L;
                    if (gct != 6 && gct != 8) continue; /* ~LJ_TTHREAD */
                    if (ffid != 1) continue;            /* FF_C */
                    if (status > 1) continue;
                    memcpy(&glref, p + LJ_GLREF, 4);
                    memcpy(&basep, p + LJ_BASE, 4);
                    memcpy(&topp, p + LJ_TOP, 4);
                    memcpy(&stack, p + LJ_STACK, 4);
                    memcpy(&stacksize, p + LJ_STACKSIZE, 4);
                    if (glref < 0x10000u || stack < 0x10000u) continue;
                    if (stacksize < 40u || stacksize > 1000000u) continue;
                    if (!(stack <= basep && basep <= topp)) continue;
                    L = cursor + (uint32_t)offset;
                    if (!luajit_glref_points_to_main(process, L, glref)) continue;
                    cands[cand_count++] = L;
                }
            }
        }
        address = (uintptr_t)info.BaseAddress + info.RegionSize;
        if (address > 0x7FFFFFFFu) break;
    }
    free(chunk);
    if (progress) progress(60, "Validating LuaJIT candidates");
    if (cand_count <= 0) return 0;

    /* Prefer status==0 main thread; all candidates already passed mainthref. */
    *out_state = cands[0];
    for (i = 0; i < cand_count; i++) {
        BYTE st[8];
        if (!rpm(process, cands[i], st, sizeof(st))) continue;
        if (st[LJ_STATUS] == 0) {
            *out_state = cands[i];
            break;
        }
    }
    return 1;
}

int cocos_debug_scan_lua_state(HANDLE process, uint32_t *out_state,
                               CocosDebugScanProgressFn progress) {
    if (!process || !out_state) return 0;
    *out_state = 0;
    g_lua_is_luajit = detect_luajit(process);

    if (g_lua_is_luajit) {
        if (scan_luajit_state(process, out_state, progress)) return 1;
        /* Some builds ship luaJIT_* exports but still use PUC-Rio 5.1 layout. */
        return scan_lua51_state(process, out_state, progress);
    }
    if (scan_lua51_state(process, out_state, progress)) return 1;
    if (scan_luajit_state(process, out_state, progress)) {
        g_lua_is_luajit = 1;
        return 1;
    }
    return 0;
}
