#include <windows.h>
#include <tlhelp32.h>
#include <psapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include "injector.h"
#include "lua_vm_scan.h"
#include "inject_dll_data.h"
#include "bootstrap_lua_data.h"

#define COCOS_DEBUG_PROFILER_DLL_PREFIX "cocos_debug_profiler_"
#define COCOS_REMOTE_MOD_CAP 96
#ifndef TH32CS_SNAPMODULE32
#define TH32CS_SNAPMODULE32 0x00000010
#endif

/* Minimal dbghelp surface so MinGW builds without the Windows SDK headers. */
#ifndef SYMOPT_UNDNAME
#define SYMOPT_UNDNAME 0x00000002
#endif
#ifndef SYMOPT_DEFERRED_LOADS
#define SYMOPT_DEFERRED_LOADS 0x00000004
#endif
#ifndef SYMOPT_LOAD_LINES
#define SYMOPT_LOAD_LINES 0x00000010
#endif
#ifndef SYMOPT_INCLUDE_32BIT_MODULES
#define SYMOPT_INCLUDE_32BIT_MODULES 0x00002000
#endif

typedef struct {
    ULONG SizeOfStruct;
    ULONG TypeIndex;
    ULONG64 Reserved[2];
    ULONG Index;
    ULONG Size;
    ULONG64 ModBase;
    ULONG Flags;
    ULONG64 Value;
    ULONG64 Address;
    ULONG Register;
    ULONG Scope;
    ULONG Tag;
    ULONG NameLen;
    ULONG MaxNameLen;
    CHAR Name[1];
} CocosDbgSymbolInfo;

typedef DWORD (WINAPI *SymSetOptions_fn)(DWORD);
typedef DWORD (WINAPI *SymGetOptions_fn)(void);
typedef BOOL (WINAPI *SymInitialize_fn)(HANDLE, PCSTR, BOOL);
typedef BOOL (WINAPI *SymCleanup_fn)(HANDLE);
typedef BOOL (WINAPI *SymSetSearchPath_fn)(HANDLE, PCSTR);
typedef DWORD64 (WINAPI *SymLoadModuleExA_fn)(HANDLE, HANDLE, PCSTR, PCSTR, DWORD64, DWORD,
                                               void *, DWORD);
typedef DWORD64 (WINAPI *SymLoadModule64_fn)(HANDLE, HANDLE, PCSTR, PCSTR, DWORD64, DWORD);
typedef BOOL (WINAPI *SymUnloadModule64_fn)(HANDLE, DWORD64);
typedef BOOL (WINAPI *SymFromName_fn)(HANDLE, PCSTR, CocosDbgSymbolInfo *);

typedef struct {
    HMODULE mod;
    SymSetOptions_fn SymSetOptions;
    SymGetOptions_fn SymGetOptions;
    SymInitialize_fn SymInitialize;
    SymCleanup_fn SymCleanup;
    SymSetSearchPath_fn SymSetSearchPath;
    SymLoadModuleExA_fn SymLoadModuleExA;
    SymLoadModule64_fn SymLoadModule64;
    SymUnloadModule64_fn SymUnloadModule64;
    SymFromName_fn SymFromName;
} CocosDbgHelp;

static int cocos_dbghelp_load(CocosDbgHelp *api) {
    if (!api) return 0;
    memset(api, 0, sizeof(*api));
    api->mod = LoadLibraryA("dbghelp.dll");
    if (!api->mod) return 0;
    api->SymSetOptions = (SymSetOptions_fn)GetProcAddress(api->mod, "SymSetOptions");
    api->SymGetOptions = (SymGetOptions_fn)GetProcAddress(api->mod, "SymGetOptions");
    api->SymInitialize = (SymInitialize_fn)GetProcAddress(api->mod, "SymInitialize");
    api->SymCleanup = (SymCleanup_fn)GetProcAddress(api->mod, "SymCleanup");
    api->SymSetSearchPath = (SymSetSearchPath_fn)GetProcAddress(api->mod, "SymSetSearchPath");
    api->SymLoadModuleExA = (SymLoadModuleExA_fn)GetProcAddress(api->mod, "SymLoadModuleExA");
    if (!api->SymLoadModuleExA)
        api->SymLoadModuleExA = (SymLoadModuleExA_fn)GetProcAddress(api->mod, "SymLoadModuleEx");
    api->SymLoadModule64 = (SymLoadModule64_fn)GetProcAddress(api->mod, "SymLoadModule64");
    api->SymUnloadModule64 =
        (SymUnloadModule64_fn)GetProcAddress(api->mod, "SymUnloadModule64");
    api->SymFromName = (SymFromName_fn)GetProcAddress(api->mod, "SymFromName");
    if (!api->SymFromName)
        api->SymFromName = (SymFromName_fn)GetProcAddress(api->mod, "SymFromNameA");
    if (!api->SymSetOptions || !api->SymGetOptions || !api->SymInitialize ||
        !api->SymCleanup || !api->SymSetSearchPath || !api->SymUnloadModule64 ||
        !api->SymFromName || (!api->SymLoadModuleExA && !api->SymLoadModule64)) {
        FreeLibrary(api->mod);
        memset(api, 0, sizeof(*api));
        return 0;
    }
    return 1;
}

static void cocos_dbghelp_unload(CocosDbgHelp *api) {
    if (!api) return;
    if (api->mod) FreeLibrary(api->mod);
    memset(api, 0, sizeof(*api));
}

static DWORD64 cocos_dbghelp_load_module(CocosDbgHelp *api, HANDLE proc, const char *path,
                                         DWORD64 base, DWORD size) {
    if (!api) return 0;
    if (api->SymLoadModuleExA)
        return api->SymLoadModuleExA(proc, NULL, path, NULL, base, size, NULL, 0);
    return api->SymLoadModule64(proc, NULL, path, NULL, base, size);
}

static CocosDebugProgressCallback g_progress_callback;
static void *g_progress_user;

static int get_process_exe_name(DWORD pid, char *out, size_t cap);

static void report_progress(int percent, const char *stage) {
    if (g_progress_callback) g_progress_callback(percent, stage, g_progress_user);
}

typedef struct {
    uintptr_t base;
    uint32_t size;
    int found;
    char name[MAX_MODULE_NAME32 + 1];
    char path[MAX_PATH];
    int prefer; /* higher tried first for PDB / ownership */
} ModInfo;

typedef struct {
    uintptr_t drawscene;
    uintptr_t cocos_base;
    uint32_t cocos_size;
    int ok;
    char method[48];
} DrawSceneResult;

typedef struct {
    uintptr_t addr;
    uintptr_t cocos_base;
    uint32_t cocos_size;
    int ok;
    char method[48];
} BindingResult;

static int path_is_system_module(const char *path) {
    char lower[MAX_PATH];
    size_t i;
    if (!path || !path[0]) return 1;
    for (i = 0; path[i] && i + 1 < sizeof(lower); i++) {
        char c = path[i];
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        lower[i] = c;
    }
    lower[i] = 0;
    return strstr(lower, "\\windows\\") != NULL ||
           strstr(lower, "\\system32\\") != NULL ||
           strstr(lower, "\\syswow64\\") != NULL ||
           strstr(lower, "\\winsxs\\") != NULL;
}

static int module_prefer_score(const char *name, const char *path, const char *exe_name) {
    if (!name) return 0;
    if (!_stricmp(name, "libcocos2d.dll")) return 100;
    if (!_stricmp(name, "cocos2d.dll") || !_stricmp(name, "libcocos2dx.dll") ||
        !_stricmp(name, "libcocos2d-x.dll"))
        return 90;
    if (strstr(name, "cocos") || strstr(name, "Cocos") || strstr(name, "COCOS"))
        return 80;
    if (exe_name && !_stricmp(name, exe_name)) return 70;
    if (path && !path_is_system_module(path)) return 20;
    return 5;
}

static int collect_remote_modules(DWORD pid, ModInfo *out, int cap) {
    HANDLE h;
    MODULEENTRY32 me;
    char exe_name[MAX_PATH];
    int n = 0, i, j;
    if (!out || cap <= 0) return 0;
    memset(out, 0, (size_t)cap * sizeof(*out));
    get_process_exe_name(pid, exe_name, sizeof(exe_name));
    h = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (h == INVALID_HANDLE_VALUE)
        h = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, pid);
    if (h == INVALID_HANDLE_VALUE) return 0;
    me.dwSize = sizeof(me);
    if (Module32First(h, &me)) {
        do {
            ModInfo *slot;
            if (n >= cap) break;
            slot = &out[n];
            slot->base = (uintptr_t)me.modBaseAddr;
            slot->size = me.modBaseSize;
            slot->found = 1;
            strncpy(slot->name, me.szModule, sizeof(slot->name) - 1);
            strncpy(slot->path, me.szExePath, sizeof(slot->path) - 1);
            slot->prefer = module_prefer_score(slot->name, slot->path, exe_name);
            n++;
        } while (Module32Next(h, &me));
    }
    CloseHandle(h);
    /* Sort descending by prefer so libcocos2d / cocos* / exe come first. */
    for (i = 0; i < n; i++) {
        for (j = i + 1; j < n; j++) {
            if (out[j].prefer > out[i].prefer) {
                ModInfo tmp = out[i];
                out[i] = out[j];
                out[j] = tmp;
            }
        }
    }
    return n;
}

static int find_module(DWORD pid, const char *name, ModInfo *out) {
    ModInfo mods[COCOS_REMOTE_MOD_CAP];
    int n, i;
    memset(out, 0, sizeof(*out));
    if (!name || !name[0]) return 0;
    n = collect_remote_modules(pid, mods, COCOS_REMOTE_MOD_CAP);
    for (i = 0; i < n; i++) {
        if (!_stricmp(mods[i].name, name)) {
            *out = mods[i];
            return 1;
        }
    }
    return 0;
}

static int get_process_exe_name(DWORD pid, char *out, size_t cap) {
    HANDLE snapshot;
    PROCESSENTRY32 entry;
    if (!out || cap < 2) return 0;
    out[0] = 0;
    snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return 0;
    memset(&entry, 0, sizeof(entry));
    entry.dwSize = sizeof(entry);
    if (Process32First(snapshot, &entry)) {
        do {
            if (entry.th32ProcessID == pid) {
                strncpy(out, entry.szExeFile, cap - 1);
                out[cap - 1] = 0;
                CloseHandle(snapshot);
                return out[0] != 0;
            }
        } while (Process32Next(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return 0;
}

static int rpm(HANDLE hp, uintptr_t addr, void *buf, size_t n) {
    SIZE_T rd = 0;
    if (!ReadProcessMemory(hp, (void*)addr, buf, n, &rd)) return 0;
    return rd == n;
}

static int resolve_cpp_method_addr(HANDLE hp, DWORD pid, const char *name, BindingResult *res);

static int resolve_drawscene(HANDLE hp, DWORD pid, DrawSceneResult *res) {
    BindingResult br;
    ModInfo cocos = {0};
    memset(res, 0, sizeof(*res));
    if (!resolve_cpp_method_addr(hp, pid, "cc.Director:drawScene", &br) || !br.ok)
        return 0;
    res->drawscene = br.addr;
    /* Prefer a dedicated cocos DLL as the engine image range when present;
       otherwise use the module that actually owned drawScene (static link). */
    if (find_module(pid, "libcocos2d.dll", &cocos) ||
        find_module(pid, "cocos2d.dll", &cocos) ||
        find_module(pid, "libcocos2dx.dll", &cocos)) {
        res->cocos_base = cocos.base;
        res->cocos_size = cocos.size;
    } else {
        res->cocos_base = br.cocos_base;
        res->cocos_size = br.cocos_size;
    }
    strncpy(res->method, br.method, sizeof(res->method) - 1);
    res->ok = 1;
    return 1;
}

static int resolve_getdeltatime(HANDLE hp, DWORD pid, uintptr_t *out_addr, uintptr_t *cocos_base);

static int rpm_string(HANDLE hp, uintptr_t addr, char *out, size_t cap) {
    size_t filled = 0;
    if (!out || cap == 0) return 0;
    memset(out, 0, cap);
    while (filled + 1 < cap) {
        BYTE chunk[16];
        SIZE_T rd = 0;
        SIZE_T want = sizeof(chunk);
        const BYTE *zero;
        if (want > cap - 1 - filled) want = cap - 1 - filled;
        if (!ReadProcessMemory(hp, (void *)(addr + filled), chunk, want, &rd) || rd == 0)
            break;
        memcpy(out + filled, chunk, rd);
        zero = (const BYTE *)memchr(chunk, 0, rd);
        if (zero) return 1;
        filled += rd;
    }
    out[cap - 1] = 0;
    return out[0] != 0;
}

static uintptr_t resolve_remote_export(HANDLE hp, const ModInfo *mod, const char *name) {
    IMAGE_DOS_HEADER dos;
    IMAGE_NT_HEADERS32 nt;
    IMAGE_EXPORT_DIRECTORY exp;
    DWORD i, n, name_rva, fn_rva;
    WORD ord;
    char nm[256];
    DWORD exp_rva;
    if (!mod || !mod->found || !name || !name[0]) return 0;
    if (!rpm(hp, mod->base, &dos, sizeof(dos)) || dos.e_magic != IMAGE_DOS_SIGNATURE) return 0;
    if (!rpm(hp, mod->base + dos.e_lfanew, &nt, sizeof(nt))) return 0;
    exp_rva = nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].VirtualAddress;
    if (!exp_rva) return 0;
    if (!rpm(hp, mod->base + exp_rva, &exp, sizeof(exp))) return 0;
    n = exp.NumberOfNames;
    if (n > 30000) n = 30000;
    for (i = 0; i < n; i++) {
        if (!rpm(hp, mod->base + exp.AddressOfNames + i * 4, &name_rva, 4)) continue;
        if (!rpm_string(hp, mod->base + name_rva, nm, sizeof(nm))) continue;
        if (strcmp(nm, name) != 0) continue;
        if (!rpm(hp, mod->base + exp.AddressOfNameOrdinals + i * 2, &ord, 2)) return 0;
        if (!rpm(hp, mod->base + exp.AddressOfFunctions + (DWORD)ord * 4, &fn_rva, 4)) return 0;
        if (!fn_rva) return 0;
        return mod->base + fn_rva;
    }
    return 0;
}

static uintptr_t resolve_remote_export_contains(HANDLE hp, const ModInfo *mod,
                                                const char *needle,
                                                const char *prefer_suffix) {
    IMAGE_DOS_HEADER dos;
    IMAGE_NT_HEADERS32 nt;
    IMAGE_EXPORT_DIRECTORY exp;
    DWORD i, n, name_rva, fn_rva, exp_rva;
    WORD ord;
    char nm[256];
    uintptr_t first = 0;
    if (!mod || !mod->found || !needle || !needle[0]) return 0;
    if (!rpm(hp, mod->base, &dos, sizeof(dos)) || dos.e_magic != IMAGE_DOS_SIGNATURE) return 0;
    if (!rpm(hp, mod->base + dos.e_lfanew, &nt, sizeof(nt))) return 0;
    exp_rva = nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].VirtualAddress;
    if (!exp_rva) return 0;
    if (!rpm(hp, mod->base + exp_rva, &exp, sizeof(exp))) return 0;
    n = exp.NumberOfNames;
    if (n > 30000) n = 30000;
    for (i = 0; i < n; i++) {
        size_t len;
        if (!rpm(hp, mod->base + exp.AddressOfNames + i * 4, &name_rva, 4)) continue;
        if (!rpm_string(hp, mod->base + name_rva, nm, sizeof(nm))) continue;
        if (!strstr(nm, needle)) continue;
        if (!rpm(hp, mod->base + exp.AddressOfNameOrdinals + i * 2, &ord, 2)) continue;
        if (!rpm(hp, mod->base + exp.AddressOfFunctions + (DWORD)ord * 4, &fn_rva, 4) ||
            !fn_rva)
            continue;
        if (prefer_suffix) {
            len = strlen(nm);
            if (len >= strlen(prefer_suffix) &&
                strcmp(nm + len - strlen(prefer_suffix), prefer_suffix) == 0)
                return mod->base + fn_rva;
        }
        if (!first) first = mod->base + fn_rva;
    }
    return first;
}

static const char *cpp_export_aliases(const char *friendly, int index) {
    static const struct { const char *friendly; const char *exp; } kMap[] = {
        /* MSVC thiscall / const variants */
        { "cc.Director:drawScene", "?drawScene@Director@cocos2d@@QAEXXZ" },
        { "cc.Director:drawScene", "?drawScene@Director@cocos2d@@AAEXXZ" },
        { "cc.Director:drawScene", "_ZN7cocos2d8Director9drawSceneEv" },
        { "cc.Director:getDeltaTime", "?getDeltaTime@Director@cocos2d@@QBEMXZ" },
        { "cc.Director:getDeltaTime", "?getDeltaTime@Director@cocos2d@@QAEMXZ" },
        { "cc.Director:getDeltaTime", "_ZNK7cocos2d8Director12getDeltaTimeEv" },
        { "cc.Renderer:render", "?render@Renderer@cocos2d@@QAEXXZ" },
        { "cc.Renderer:render", "?render@Renderer@cocos2d@@QAEXH@Z" },
        { "cc.Renderer:render", "_ZN7cocos2d8Renderer6renderEv" },
        { "cc.Renderer:clear", "?clear@Renderer@cocos2d@@QAEXXZ" },
        { "cc.Scene:render", "?render@Scene@cocos2d@@UAEXPAVRenderer@2@@Z" },
        { "cc.Scene:render", "?render@Scene@cocos2d@@QAEXPAVRenderer@2@@Z" },
        { "cc.Scene:render", "?render@Scene@cocos2d@@QAEXPAVRenderer@2@ABVMat4@math@2@@Z" },
        { "cc.Scene:render", "?render@Scene@cocos2d@@QAEXPAVRenderer@2@ABVMat4@math@2@PBV452@@Z" },
        { "cc.Scene:render", "?render@Scene@cocos2d@@QAEXPAVRenderer@2@ABVMat4@math@2@PBV342@@Z" },
        { "cc.Scheduler:update", "?update@Scheduler@cocos2d@@QAEXM@Z" },
        { "cc.EventDispatcher:dispatchEvent",
          "?dispatchEvent@EventDispatcher@cocos2d@@QAEXPAVEvent@2@@Z" },
        /* Prefer path-string overload first (most common load path). */
        { "cc.TextureCache:addImage",
          "?addImage@TextureCache@cocos2d@@QAEPAVTexture2D@2@ABV?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@@Z" },
        { "cc.TextureCache:addImage",
          "?addImage@TextureCache@cocos2d@@QAEPAVTexture2D@2@PAVImage@2@ABV?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@@Z" },
        { NULL, NULL }
    };
    int n = 0, i;
    if (!friendly) return NULL;
    if (friendly[0] == '?' && index == 0) return friendly;
    for (i = 0; kMap[i].friendly; i++) {
        if (_stricmp(kMap[i].friendly, friendly) != 0) continue;
        if (n == index) return kMap[i].exp;
        n++;
    }
    return NULL;
}

static const char *cpp_undecorated_name(const char *friendly) {
    if (!friendly) return NULL;
    if (!_stricmp(friendly, "cc.Director:drawScene")) return "cocos2d::Director::drawScene";
    if (!_stricmp(friendly, "cc.Director:getDeltaTime")) return "cocos2d::Director::getDeltaTime";
    if (!_stricmp(friendly, "cc.Renderer:render")) return "cocos2d::Renderer::render";
    if (!_stricmp(friendly, "cc.Renderer:clear")) return "cocos2d::Renderer::clear";
    if (!_stricmp(friendly, "cc.Scene:render")) return "cocos2d::Scene::render";
    if (!_stricmp(friendly, "cc.Scheduler:update")) return "cocos2d::Scheduler::update";
    if (!_stricmp(friendly, "cc.EventDispatcher:dispatchEvent"))
        return "cocos2d::EventDispatcher::dispatchEvent";
    if (!_stricmp(friendly, "cc.TextureCache:addImage")) return "cocos2d::TextureCache::addImage";
    return NULL;
}

static const char *cpp_export_scan_needle(const char *friendly, const char **prefer_suffix) {
    if (prefer_suffix) *prefer_suffix = NULL;
    if (!friendly) return NULL;
    if (!_stricmp(friendly, "cc.Director:drawScene")) {
        if (prefer_suffix) *prefer_suffix = "QAEXXZ";
        return "?drawScene@Director@cocos2d@@";
    }
    if (!_stricmp(friendly, "cc.Director:getDeltaTime"))
        return "?getDeltaTime@Director@cocos2d@@";
    if (!_stricmp(friendly, "cc.Renderer:render")) {
        if (prefer_suffix) *prefer_suffix = "QAEXXZ";
        return "?render@Renderer@cocos2d@@";
    }
    if (!_stricmp(friendly, "cc.Renderer:clear"))
        return "?clear@Renderer@cocos2d@@";
    if (!_stricmp(friendly, "cc.Scene:render"))
        return "?render@Scene@cocos2d@@";
    if (!_stricmp(friendly, "cc.Scheduler:update"))
        return "?update@Scheduler@cocos2d@@";
    if (!_stricmp(friendly, "cc.EventDispatcher:dispatchEvent"))
        return "?dispatchEvent@EventDispatcher@cocos2d@@";
    if (!_stricmp(friendly, "cc.TextureCache:addImage"))
        return "?addImage@TextureCache@cocos2d@@";
    return NULL;
}

static void binding_set(BindingResult *res, uintptr_t addr, const ModInfo *mod, const char *how) {
    if (!res || !mod) return;
    res->addr = addr;
    res->cocos_base = mod->base;
    res->cocos_size = mod->size;
    _snprintf(res->method, sizeof(res->method), "%s:%s", how ? how : "?", mod->name);
    res->ok = 1;
}

static int resolve_via_exports(HANDLE hp, const ModInfo *mods, int nmods, const char *name,
                               BindingResult *res) {
    const char *exp;
    const char *needle;
    const char *prefer = NULL;
    int alias, mi;
    uintptr_t addr;
    for (mi = 0; mi < nmods; mi++) {
        alias = 0;
        while ((exp = cpp_export_aliases(name, alias++)) != NULL) {
            addr = resolve_remote_export(hp, &mods[mi], exp);
            if (!addr) continue;
            binding_set(res, addr, &mods[mi], "export");
            return 1;
        }
    }
    needle = cpp_export_scan_needle(name, &prefer);
    if (!needle) return 0;
    for (mi = 0; mi < nmods; mi++) {
        addr = resolve_remote_export_contains(hp, &mods[mi], needle, prefer);
        if (!addr && prefer)
            addr = resolve_remote_export_contains(hp, &mods[mi], needle, NULL);
        if (!addr) continue;
        binding_set(res, addr, &mods[mi], "export-scan");
        return 1;
    }
    return 0;
}

static int resolve_via_pdb(const ModInfo *mods, int nmods, const char *name, BindingResult *res) {
    const char *undec = cpp_undecorated_name(name);
    const char *exp;
    CocosDbgHelp dbg;
    HANDLE proc;
    DWORD opts;
    char search[MAX_PATH * 2];
    int mi, alias;
    int ok = 0;
    BYTE sym_buf[sizeof(CocosDbgSymbolInfo) + 1024];
    CocosDbgSymbolInfo *info = (CocosDbgSymbolInfo *)sym_buf;

    if (!undec && !cpp_export_aliases(name, 0)) return 0;
    if (!cocos_dbghelp_load(&dbg)) return 0;
    proc = GetCurrentProcess();
    opts = dbg.SymGetOptions();
    dbg.SymSetOptions(opts | SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES |
                      SYMOPT_INCLUDE_32BIT_MODULES);
    if (!dbg.SymInitialize(proc, NULL, FALSE)) {
        cocos_dbghelp_unload(&dbg);
        return 0;
    }

    for (mi = 0; mi < nmods && !ok; mi++) {
        DWORD64 loaded;
        char dir[MAX_PATH];
        char *slash;
        if (mods[mi].prefer < 20) continue; /* skip system images for PDB */
        if (!mods[mi].path[0] || mods[mi].base < 0x10000) continue;
        strncpy(dir, mods[mi].path, sizeof(dir) - 1);
        dir[sizeof(dir) - 1] = 0;
        slash = strrchr(dir, '\\');
        if (slash) *slash = 0;
        _snprintf(search, sizeof(search), "%s;%s", dir, mods[mi].path);
        dbg.SymSetSearchPath(proc, search);
        loaded = cocos_dbghelp_load_module(&dbg, proc, mods[mi].path,
                                           (DWORD64)mods[mi].base, mods[mi].size);
        if (!loaded) loaded = (DWORD64)mods[mi].base;

        memset(sym_buf, 0, sizeof(sym_buf));
        info->SizeOfStruct = sizeof(CocosDbgSymbolInfo);
        info->MaxNameLen = 1024;
        if (undec && dbg.SymFromName(proc, undec, info) && info->Address >= 0x10000ull) {
            binding_set(res, (uintptr_t)info->Address, &mods[mi], "pdb");
            ok = 1;
        }
        if (!ok) {
            alias = 0;
            dbg.SymSetOptions((dbg.SymGetOptions() & ~(DWORD)SYMOPT_UNDNAME) |
                              SYMOPT_DEFERRED_LOADS | SYMOPT_INCLUDE_32BIT_MODULES);
            while ((exp = cpp_export_aliases(name, alias++)) != NULL) {
                memset(sym_buf, 0, sizeof(sym_buf));
                info->SizeOfStruct = sizeof(CocosDbgSymbolInfo);
                info->MaxNameLen = 1024;
                if (dbg.SymFromName(proc, exp, info) && info->Address >= 0x10000ull) {
                    binding_set(res, (uintptr_t)info->Address, &mods[mi], "pdb-m");
                    ok = 1;
                    break;
                }
            }
            dbg.SymSetOptions(opts | SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS |
                              SYMOPT_INCLUDE_32BIT_MODULES);
        }
        dbg.SymUnloadModule64(proc, loaded);
    }

    dbg.SymCleanup(proc);
    dbg.SymSetOptions(opts);
    cocos_dbghelp_unload(&dbg);
    return ok;
}

static int resolve_cpp_method_addr(HANDLE hp, DWORD pid, const char *name, BindingResult *res) {
    ModInfo mods[COCOS_REMOTE_MOD_CAP];
    int nmods;
    memset(res, 0, sizeof(*res));
    if (!name || !name[0]) return 0;
    nmods = collect_remote_modules(pid, mods, COCOS_REMOTE_MOD_CAP);
    if (nmods <= 0) return 0;
    if (resolve_via_exports(hp, mods, nmods, name, res)) return 1;
    if (resolve_via_pdb(mods, nmods, name, res)) return 1;
    return 0;
}

static int resolve_getdeltatime(HANDLE hp, DWORD pid, uintptr_t *out_addr, uintptr_t *cocos_base) {
    BindingResult res;
    if (!out_addr) return 0;
    *out_addr = 0;
    if (cocos_base) *cocos_base = 0;
    if (!resolve_cpp_method_addr(hp, pid, "cc.Director:getDeltaTime", &res)) return 0;
    *out_addr = res.addr;
    if (cocos_base) *cocos_base = res.cocos_base;
    return 1;
}

/* Decode Director::_deltaTime member offset from getDeltaTime prologue. */
static int decode_delta_time_offset(HANDLE hp, uintptr_t getdeltatime_addr, uint32_t *out_offset) {
    uint8_t code[32];
    int i;
    if (!out_offset || getdeltatime_addr < 0x10000) return 0;
    *out_offset = 0;
    memset(code, 0, sizeof(code));
    if (!rpm(hp, getdeltatime_addr, code, sizeof(code))) return 0;
    for (i = 0; i + 5 <= (int)sizeof(code); i++) {
        /* movss xmm0, dword ptr [ecx+disp8] : F3 0F 10 41 xx */
        if (code[i] == 0xF3 && code[i + 1] == 0x0F && code[i + 2] == 0x10 &&
            code[i + 3] == 0x41) {
            *out_offset = code[i + 4];
            return *out_offset > 0 && *out_offset < 0x1000;
        }
        /* movss xmm0, dword ptr [ecx+disp32] : F3 0F 10 81 xx xx xx xx */
        if (i + 8 <= (int)sizeof(code) && code[i] == 0xF3 && code[i + 1] == 0x0F &&
            code[i + 2] == 0x10 && code[i + 3] == 0x81) {
            memcpy(out_offset, code + i + 4, 4);
            return *out_offset > 0 && *out_offset < 0x1000;
        }
        /* fld dword ptr [ecx+disp8] : D9 41 xx */
        if (code[i] == 0xD9 && code[i + 1] == 0x41) {
            *out_offset = code[i + 2];
            return *out_offset > 0 && *out_offset < 0x1000;
        }
        /* fld dword ptr [ecx+disp32] : D9 81 xx xx xx xx */
        if (i + 6 <= (int)sizeof(code) && code[i] == 0xD9 && code[i + 1] == 0x81) {
            memcpy(out_offset, code + i + 2, 4);
            return *out_offset > 0 && *out_offset < 0x1000;
        }
    }
    return 0;
}

/* PlistManager::addSpriteFrames is not exported. Lua SealUtilToLua:addSpriteFrames
   goes through one cdecl thunk:
     movzx eax,[ebp+0Ch]; push eax; push [ebp+8]; call <getter>;
     mov ecx,eax; call <PlistManager::addSpriteFrames>; pop ebp; ret
   Follow that second call. One match only. */
static uintptr_t resolve_plist_add_sprite_frames(HANDLE hp, const ModInfo *mod) {
    static const uint8_t kPrefix[13] = {
        0x55, 0x8B, 0xEC, 0x0F, 0xB6, 0x45, 0x0C, 0x50, 0x8B, 0x4D, 0x08, 0x51, 0xE8
    };
    const size_t kThunk = 26;
    const size_t kChunk = 1024 * 1024;
    uint8_t *buf;
    uintptr_t off = 0;
    uintptr_t found = 0;
    int hits = 0;
    if (!hp || !mod || mod->base < 0x10000 || mod->size < kThunk) return 0;
    buf = (uint8_t *)malloc(kChunk + kThunk);
    if (!buf) return 0;
    while (off + kThunk <= mod->size && hits < 2) {
        size_t n = kChunk + kThunk;
        size_t i;
        size_t step;
        if (off + n > mod->size) n = (size_t)(mod->size - off);
        if (!rpm(hp, mod->base + off, buf, n)) break;
        for (i = 0; i + kThunk <= n; i++) {
            int32_t rel;
            uintptr_t method;
            uint8_t pro[3];
            if (memcmp(buf + i, kPrefix, sizeof(kPrefix)) != 0) continue;
            if (buf[i + 17] != 0x8B || buf[i + 18] != 0xC8 || buf[i + 19] != 0xE8)
                continue;
            if (buf[i + 24] != 0x5D || buf[i + 25] != 0xC3) continue;
            memcpy(&rel, buf + i + 20, 4);
            method = (mod->base + off + i + 24) + (intptr_t)rel;
            if (method < 0x10000) continue;
            if (!rpm(hp, method, pro, sizeof(pro))) continue;
            if (pro[0] != 0x55 || pro[1] != 0x8B || pro[2] != 0xEC) continue;
            found = method;
            hits++;
            if (hits > 1) break;
        }
        step = (n >= kThunk) ? (n - kThunk + 1) : n;
        if (step == 0) break;
        off += step;
    }
    free(buf);
    return hits == 1 ? found : 0;
}

/*
 * FileUtilsWin32::getData is static and has no export. The exported
 * getDataFromFile wrapper is a short function with one near call; that
 * target is getData. Both early returns (empty name, CreateFileW failure)
 * jump to the same SEH epilogue and a single ret, so the prologue hook's
 * return-address swap covers every exit. Reject the site unless the
 * callee starts with the SEH frame push (push 4Ch).
 */
static uintptr_t resolve_win32_getdata(HANDLE hp, const ModInfo *mod) {
    uintptr_t wrapper;
    uint8_t buf[48];
    uint8_t pro[2];
    int i;
    int calls = 0;
    uintptr_t target = 0;
    if (!hp || !mod || !mod->found || mod->base < 0x10000) return 0;
    wrapper = resolve_remote_export_contains(
        hp, mod, "?getDataFromFile@FileUtilsWin32@cocos2d@@", NULL);
    if (wrapper < 0x10000) return 0;
    if (!rpm(hp, wrapper, buf, sizeof(buf))) return 0;
    for (i = 0; i + 5 <= (int)sizeof(buf); i++) {
        int32_t rel;
        uintptr_t cand;
        if (buf[i] == 0xC2 || buf[i] == 0xC3) break;
        if (buf[i] != 0xE8) continue;
        memcpy(&rel, buf + i + 1, 4);
        cand = (wrapper + (uintptr_t)i + 5) + (intptr_t)rel;
        if (cand < mod->base || cand >= mod->base + mod->size) continue;
        calls++;
        target = cand;
    }
    if (calls != 1) return 0;
    if (!rpm(hp, target, pro, sizeof(pro))) return 0;
    if (pro[0] != 0x6A || pro[1] != 0x4C) return 0;
    return target;
}

/* Optional suspects: only occupy a slot when the export resolves. */
static void try_add_cpp_target(HANDLE hp, DWORD pid, CocosDebugIpcInjectionConfig *config,
                               const char *name) {
    BindingResult br;
    if (!config || !name || !name[0]) return;
    if (config->cpp_count >= COCOS_DEBUG_IPC_CPP_CAP) return;
    memset(&br, 0, sizeof(br));
    if (!resolve_cpp_method_addr(hp, pid, name, &br) || !br.ok || br.addr < 0x10000)
        return;
    strncpy(config->cpp_targets[config->cpp_count].name, name, COCOS_DEBUG_IPC_CPP_NAME - 1);
    config->cpp_targets[config->cpp_count].name[COCOS_DEBUG_IPC_CPP_NAME - 1] = 0;
    config->cpp_targets[config->cpp_count].address = (uint32_t)br.addr;
    config->cpp_count++;
}

static void resolve_cpp_hooks(HANDLE hp, DWORD pid, uintptr_t drawscene_addr,
                              CocosDebugIpcInjectionConfig *config) {
    BindingResult renderer_render;
    static const char *kOptional[] = {
        "cc.Scheduler:update",
        "cc.EventDispatcher:dispatchEvent",
        "cc.Renderer:clear",
        "cc.TextureCache:addImage",
        NULL
    };
    int i;
    if (!config) return;
    config->cpp_count = 0;
    memset(config->cpp_targets, 0, sizeof(config->cpp_targets));

    /* Slot 0: Director::drawScene */
    if (drawscene_addr >= 0x10000 && config->cpp_count < COCOS_DEBUG_IPC_CPP_CAP) {
        strncpy(config->cpp_targets[config->cpp_count].name, "cc.Director:drawScene",
                COCOS_DEBUG_IPC_CPP_NAME - 1);
        config->cpp_targets[config->cpp_count].address = (uint32_t)drawscene_addr;
        config->cpp_count++;
    }

    /* Slot 1: Renderer::render — keep slot even if unresolved (runtime may patch). */
    memset(&renderer_render, 0, sizeof(renderer_render));
    if (config->cpp_count < COCOS_DEBUG_IPC_CPP_CAP) {
        strncpy(config->cpp_targets[config->cpp_count].name, "cc.Renderer:render",
                COCOS_DEBUG_IPC_CPP_NAME - 1);
        if (resolve_cpp_method_addr(hp, pid, "cc.Renderer:render", &renderer_render) &&
            renderer_render.ok)
            config->cpp_targets[config->cpp_count].address =
                (uint32_t)renderer_render.addr;
        else
            config->cpp_targets[config->cpp_count].address = 0;
        config->cpp_count++;
    }

    try_add_cpp_target(hp, pid, config, "cc.Scene:render");

    for (i = 0; kOptional[i]; i++)
        try_add_cpp_target(hp, pid, config, kOptional[i]);

    /* Static FileUtilsWin32::getData. Per-frame time uses the existing prologue hook. */
    if (config->cpp_count < COCOS_DEBUG_IPC_CPP_CAP) {
        ModInfo cocos;
        uintptr_t addr = 0;
        memset(&cocos, 0, sizeof(cocos));
        if (find_module(pid, "libcocos2d.dll", &cocos) ||
            find_module(pid, "cocos2d.dll", &cocos) ||
            find_module(pid, "libcocos2dx.dll", &cocos) ||
            find_module(pid, "libcocos2d-x.dll", &cocos))
            addr = resolve_win32_getdata(hp, &cocos);
        if (addr >= 0x10000) {
            strncpy(config->cpp_targets[config->cpp_count].name,
                    "FileUtilsWin32:getData", COCOS_DEBUG_IPC_CPP_NAME - 1);
            config->cpp_targets[config->cpp_count].name[COCOS_DEBUG_IPC_CPP_NAME - 1] = 0;
            config->cpp_targets[config->cpp_count].address = (uint32_t)addr;
            config->cpp_count++;
        }
    }

    /* Game code, not a cocos export. Scan the main exe for the Lua thunk. */
    if (config->cpp_count < COCOS_DEBUG_IPC_CPP_CAP) {
        ModInfo mods[COCOS_REMOTE_MOD_CAP];
        int nmods = collect_remote_modules(pid, mods, COCOS_REMOTE_MOD_CAP);
        int mi;
        uintptr_t addr = 0;
        for (mi = 0; mi < nmods && !addr; mi++) {
            if (mods[mi].prefer != 70) continue;
            addr = resolve_plist_add_sprite_frames(hp, &mods[mi]);
        }
        if (addr >= 0x10000) {
            strncpy(config->cpp_targets[config->cpp_count].name,
                    "PlistManager:addSpriteFrames", COCOS_DEBUG_IPC_CPP_NAME - 1);
            config->cpp_targets[config->cpp_count].name[COCOS_DEBUG_IPC_CPP_NAME - 1] = 0;
            config->cpp_targets[config->cpp_count].address = (uint32_t)addr;
            config->cpp_count++;
        }
    }
}

typedef struct WindowSearch {
    DWORD pid;
    HWND window;
} WindowSearch;

static BOOL CALLBACK find_window_proc(HWND hwnd, LPARAM param) {
    WindowSearch *search = (WindowSearch *)param;
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid != search->pid || !IsWindowVisible(hwnd) || GetAncestor(hwnd, GA_ROOT) != hwnd)
        return TRUE;
    search->window = hwnd;
    return FALSE;
}

static HWND find_main_window(DWORD pid) {
    WindowSearch search;
    search.pid = pid;
    search.window = NULL;
    EnumWindows(find_window_proc, (LPARAM)&search);
    return search.window;
}

static void set_error(char *error, int cap, const char *message) {
    if (!error || cap <= 0) return;
    strncpy(error, message ? message : "unknown error", cap - 1);
    error[cap - 1] = 0;
}

static int enable_debug_privilege(void) {
    HANDLE token;
    TOKEN_PRIVILEGES privileges;
    LUID luid;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token))
        return 0;
    if (!LookupPrivilegeValueA(NULL, "SeDebugPrivilege", &luid)) {
        CloseHandle(token);
        return 0;
    }
    privileges.PrivilegeCount = 1;
    privileges.Privileges[0].Luid = luid;
    privileges.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    AdjustTokenPrivileges(token, FALSE, &privileges, sizeof(privileges), NULL, NULL);
    CloseHandle(token);
    return GetLastError() == ERROR_SUCCESS;
}

static int create_session_mapping(DWORD pid, CocosDebugInjectionSession *session,
                                  char *error, int error_cap) {
    char name[64];
    LARGE_INTEGER freq;
    HANDLE mapping;
    CocosDebugIpcBlock *ipc;
    DWORD create_error;
    _snprintf(name, sizeof(name), COCOS_DEBUG_IPC_MAPPING_FMT,
              (unsigned)COCOS_DEBUG_IPC_VERSION, (unsigned long)pid);
    mapping = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0,
                                 (DWORD)sizeof(CocosDebugIpcBlock), name);
    if (!mapping) {
        set_error(error, error_cap, "CreateFileMapping failed");
        return 0;
    }
    create_error = GetLastError();
    ipc = (CocosDebugIpcBlock *)MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0,
                                        sizeof(CocosDebugIpcBlock));
    if (!ipc) {
        CloseHandle(mapping);
        set_error(error, error_cap, "MapViewOfFile failed");
        return 0;
    }
    if (create_error == ERROR_ALREADY_EXISTS) {
        if (ipc->header.magic != COCOS_DEBUG_IPC_MAGIC ||
            ipc->header.version != COCOS_DEBUG_IPC_VERSION ||
            ipc->header.total_size != sizeof(CocosDebugIpcBlock)) {
            UnmapViewOfFile(ipc);
            CloseHandle(mapping);
            set_error(error, error_cap, "Existing profiler IPC uses an incompatible version");
            return 0;
        }
        if (ipc->header.producer_alive) {
            session->pid = pid;
            session->target_window = find_main_window(pid);
            session->mapping = mapping;
            session->ipc = ipc;
            return 2;
        }
    }
    memset(ipc, 0, sizeof(*ipc));
    QueryPerformanceFrequency(&freq);
    ipc->header.magic = COCOS_DEBUG_IPC_MAGIC;
    ipc->header.version = COCOS_DEBUG_IPC_VERSION;
    ipc->header.header_size = sizeof(CocosDebugIpcHeader);
    ipc->header.total_size = sizeof(CocosDebugIpcBlock);
    ipc->header.target_pid = pid;
    ipc->header.features =
        COCOS_DEBUG_IPC_FEATURE_FRAME_METHODS |
        COCOS_DEBUG_IPC_FEATURE_CPP_HOOKS |
        COCOS_DEBUG_IPC_FEATURE_EVENT_STREAM |
        COCOS_DEBUG_IPC_FEATURE_PRECISE_LUA |
        COCOS_DEBUG_IPC_FEATURE_LINE_SAMPLES |
        COCOS_DEBUG_IPC_FEATURE_LINE_QPC_TIME |
        COCOS_DEBUG_IPC_FEATURE_LUA_OUTSIDE_WINDOW |
        COCOS_DEBUG_IPC_FEATURE_GETDATA_FILES;
    ipc->header.qpc_frequency = (uint64_t)freq.QuadPart;
    ipc->header.state = COCOS_DEBUG_IPC_STATE_CREATED;
    ipc->header.viewer_heartbeat_ms = GetTickCount();
    session->pid = pid;
    session->target_window = find_main_window(pid);
    session->mapping = mapping;
    session->ipc = ipc;
    return 1;
}


static void cleanup_stale_profiler_dlls(const char *temp_dir) {
    WIN32_FIND_DATAA data;
    HANDLE find;
    char pattern[MAX_PATH];
    char path[MAX_PATH];
    int deleted = 0;
    if (!temp_dir || !temp_dir[0]) return;
    _snprintf(pattern, sizeof(pattern), "%scocos_debug_profiler_*.dll", temp_dir);
    find = FindFirstFileA(pattern, &data);
    if (find == INVALID_HANDLE_VALUE) return;
    do {
        if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        _snprintf(path, sizeof(path), "%s%s", temp_dir, data.cFileName);
        if (DeleteFileA(path)) deleted++;
    } while (FindNextFileA(find, &data) && deleted < 64);
    FindClose(find);
}

static int inject_dll(HANDLE hp, DWORD pid, char *error, int error_cap) {
    char temp_dir[MAX_PATH], dll_path[MAX_PATH];
    FILE *file;
    LPVOID remote;
    HANDLE thread;
    DWORD exit_code = 0;
    SIZE_T path_size;
    LPTHREAD_START_ROUTINE load_library;

    if (!GetTempPathA(sizeof(temp_dir), temp_dir)) {
        set_error(error, error_cap, "GetTempPath failed");
        return 0;
    }
    cleanup_stale_profiler_dlls(temp_dir);
    _snprintf(dll_path, sizeof(dll_path), "%scocos_debug_profiler_%lu_%lu.dll",
              temp_dir, (unsigned long)pid, (unsigned long)GetTickCount());
    file = fopen(dll_path, "wb");
    if (!file) {
        set_error(error, error_cap, "Cannot create temporary injection DLL");
        return 0;
    }
    if (fwrite(inject_dll_data, 1, INJECT_DLL_SIZE, file) != INJECT_DLL_SIZE) {
        fclose(file);
        DeleteFileA(dll_path);
        set_error(error, error_cap, "Cannot write temporary injection DLL");
        return 0;
    }
    fclose(file);
    path_size = strlen(dll_path) + 1;
    remote = VirtualAllocEx(hp, NULL, path_size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remote || !WriteProcessMemory(hp, remote, dll_path, path_size, NULL)) {
        if (remote) VirtualFreeEx(hp, remote, 0, MEM_RELEASE);
        DeleteFileA(dll_path);
        set_error(error, error_cap, "Cannot copy DLL path into target");
        return 0;
    }
    load_library = (LPTHREAD_START_ROUTINE)GetProcAddress(GetModuleHandleA("kernel32.dll"), "LoadLibraryA");
    thread = CreateRemoteThread(hp, NULL, 0, load_library, remote, 0, NULL);
    if (!thread) {
        VirtualFreeEx(hp, remote, 0, MEM_RELEASE);
        DeleteFileA(dll_path);
        set_error(error, error_cap, "CreateRemoteThread failed");
        return 0;
    }
    WaitForSingleObject(thread, 15000);
    GetExitCodeThread(thread, &exit_code);
    CloseHandle(thread);
    VirtualFreeEx(hp, remote, 0, MEM_RELEASE);
    if (!DeleteFileA(dll_path)) MoveFileExA(dll_path, NULL, MOVEFILE_DELAY_UNTIL_REBOOT);
    if (!exit_code) {
        set_error(error, error_cap, "LoadLibrary failed in target");
        return 0;
    }
    return 1;
}

int cocos_debug_list_processes(CocosDebugProcessInfo *items, int capacity) {
    HANDLE snapshot;
    PROCESSENTRY32 entry;
    int count = 0;
    if (!items || capacity <= 0) return 0;
    snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return 0;
    memset(&entry, 0, sizeof(entry));
    entry.dwSize = sizeof(entry);
    if (Process32First(snapshot, &entry)) {
        do {
            if (count >= capacity) break;
            items[count].pid = entry.th32ProcessID;
            strncpy(items[count].name, entry.szExeFile, sizeof(items[count].name) - 1);
            items[count].name[sizeof(items[count].name) - 1] = 0;
            items[count].window = find_main_window(entry.th32ProcessID);
            if (items[count].window) count++;
        } while (Process32Next(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return count;
}

static int has_resident_cocos_debug_profiler(DWORD pid) {
    HANDLE snapshot;
    MODULEENTRY32 module;
    int found = 0;
    snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, pid);
    if (snapshot == INVALID_HANDLE_VALUE) return 0;
    memset(&module, 0, sizeof(module));
    module.dwSize = sizeof(module);
    if (Module32First(snapshot, &module)) {
        do {
            if (_strnicmp(module.szModule, COCOS_DEBUG_PROFILER_DLL_PREFIX,
                           (int)strlen(COCOS_DEBUG_PROFILER_DLL_PREFIX)) == 0) {
                found = 1;
                break;
            }
        } while (Module32Next(snapshot, &module));
    }
    CloseHandle(snapshot);
    return found;
}

int cocos_debug_inject_process(DWORD pid, CocosDebugProgressCallback callback, void *user,
                        CocosDebugInjectionSession *session, char *error, int error_cap) {
    HANDLE hp;
    DrawSceneResult ds;
    uintptr_t gdt = 0, gdt_base = 0;
    int mapping_result, i;
    if (!session || !pid) {
        set_error(error, error_cap, "Invalid target process");
        return 0;
    }
    memset(session, 0, sizeof(*session));
    g_progress_callback = callback;
    g_progress_user = user;
    report_progress(2, "Creating profiler session");
    mapping_result = create_session_mapping(pid, session, error, error_cap);
    if (!mapping_result) return 0;
    if (mapping_result == 2) {
        report_progress(100, "Connected to existing profiler");
        return 1;
    }
    if (InterlockedCompareExchange(
            (LONG *)&session->ipc->header.state,
            COCOS_DEBUG_IPC_STATE_INJECTING,
            COCOS_DEBUG_IPC_STATE_CREATED) != COCOS_DEBUG_IPC_STATE_CREATED) {
        set_error(error, error_cap,
                  "Another profiler injection is already in progress. Restart the target process if this persists.");
        cocos_debug_close_session(session);
        return 0;
    }
    if (has_resident_cocos_debug_profiler(pid)) {
        set_error(error, error_cap,
                  "A profiler DLL is already loaded in the target process. Restart the process, then inject again.");
        cocos_debug_close_session(session);
        return 0;
    }
    enable_debug_privilege();
    hp = OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_INFORMATION | PROCESS_CREATE_THREAD |
                     PROCESS_VM_OPERATION | PROCESS_VM_WRITE, FALSE, pid);
    if (!hp) {
        set_error(error, error_cap, "OpenProcess failed");
        cocos_debug_close_session(session);
        return 0;
    }
    if (!cocos_debug_collect_lua_modules(pid) || cocos_debug_lua_module_count() <= 0) {
        set_error(error, error_cap, "No Lua module found");
        CloseHandle(hp);
        cocos_debug_close_session(session);
        return 0;
    }
    report_progress(8, "Resolving Cocos2d exports");
    memset(&ds, 0, sizeof(ds));
    if (!resolve_drawscene(hp, pid, &ds)) {
        set_error(error, error_cap, "Cannot resolve cocos2d::Director::drawScene");
        CloseHandle(hp);
        cocos_debug_close_session(session);
        return 0;
    }
    session->ipc->config.cocos_base = (uint32_t)ds.cocos_base;
    session->ipc->config.cocos_size = ds.cocos_size;
    session->ipc->config.drawscene_address = (uint32_t)ds.drawscene;
    session->ipc->config.director_delta_time_offset = 0;
    if (resolve_getdeltatime(hp, pid, &gdt, &gdt_base)) {
        uint32_t delta_offset = 0;
        session->ipc->config.getdeltatime_address = (uint32_t)gdt;
        if (decode_delta_time_offset(hp, gdt, &delta_offset))
            session->ipc->config.director_delta_time_offset = delta_offset;
    }
    resolve_cpp_hooks(hp, pid, ds.drawscene, &session->ipc->config);
    if (!cocos_debug_scan_lua_state(hp, &session->ipc->config.lua_state, report_progress)) {
        set_error(error, error_cap,
                  cocos_debug_lua_is_luajit()
                      ? "No valid LuaJIT state found"
                      : "No valid Lua 5.1 state found");
        CloseHandle(hp);
        cocos_debug_close_session(session);
        return 0;
    }
    if (BOOTSTRAP_LUA_SIZE >= COCOS_DEBUG_IPC_BOOTSTRAP_CAP) {
        set_error(error, error_cap, "Embedded bootstrap script is too large");
        CloseHandle(hp);
        cocos_debug_close_session(session);
        return 0;
    }
    memcpy(session->ipc->config.bootstrap_lua, bootstrap_lua_data, BOOTSTRAP_LUA_SIZE);
    session->ipc->config.bootstrap_lua[BOOTSTRAP_LUA_SIZE] = 0;
    session->ipc->config.bootstrap_size = BOOTSTRAP_LUA_SIZE;
    session->ipc->header.state = COCOS_DEBUG_IPC_STATE_INJECTING;
    session->ipc->header.viewer_heartbeat_ms = GetTickCount();
    MemoryBarrier();
    report_progress(75, "Injecting profiler DLL");
    if (!inject_dll(hp, pid, error, error_cap)) {
        session->ipc->header.state = COCOS_DEBUG_IPC_STATE_ERROR;
        CloseHandle(hp);
        return 0;
    }
    CloseHandle(hp);
    report_progress(90, "Waiting for in-process profiler");
    for (i = 0; i < 100; i++) {
        session->ipc->header.viewer_heartbeat_ms = GetTickCount();
        if (session->ipc->header.state == COCOS_DEBUG_IPC_STATE_READY ||
            session->ipc->header.state == COCOS_DEBUG_IPC_STATE_STOPPED) {
            report_progress(100, "Profiler ready");
            return 1;
        }
        if (session->ipc->header.state == COCOS_DEBUG_IPC_STATE_ERROR) {
            set_error(error, error_cap, session->ipc->header.error);
            return 0;
        }
        Sleep(50);
    }
    set_error(error, error_cap, "Timed out waiting for injected profiler");
    return 0;
}

void cocos_debug_close_session(CocosDebugInjectionSession *session) {
    if (!session) return;
    if (session->ipc) UnmapViewOfFile(session->ipc);
    if (session->mapping) CloseHandle(session->mapping);
    memset(session, 0, sizeof(*session));
}

int cocos_debug_send_command(CocosDebugInjectionSession *session, int command) {
    if (!session || !session->ipc) return 0;
    InterlockedExchange((LONG *)&session->ipc->header.command, command);
    MemoryBarrier();
    InterlockedIncrement((LONG *)&session->ipc->header.command_sequence);
    return 1;
}

int cocos_debug_run_lua_string(CocosDebugInjectionSession *session, const char *script,
                        char *error, int error_cap) {
    size_t len;
    CocosDebugIpcRemoteLua *remote;
    if (!session || !session->ipc) {
        set_error(error, error_cap, "Not connected");
        return 0;
    }
    if (!script) {
        set_error(error, error_cap, "Empty script");
        return 0;
    }
    len = strlen(script);
    if (len == 0) {
        set_error(error, error_cap, "Empty script");
        return 0;
    }
    if (len >= COCOS_DEBUG_IPC_REMOTE_LUA_CAP) {
        set_error(error, error_cap, "Script exceeds 64KB limit");
        return 0;
    }
    remote = &session->ipc->remote_lua;
    memcpy(remote->script, script, len);
    remote->script[len] = 0;
    remote->script_size = (uint32_t)len;
    remote->result_error[0] = 0;
    InterlockedExchange((LONG *)&remote->result_status, 0);
    MemoryBarrier();
    InterlockedIncrement((LONG *)&remote->request_seq);
    return cocos_debug_send_command(session, COCOS_DEBUG_IPC_CMD_RUN_LUA);
}

