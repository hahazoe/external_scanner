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

static CocosDebugProgressCallback g_progress_callback;
static void *g_progress_user;

static void report_progress(int percent, const char *stage) {
    if (g_progress_callback) g_progress_callback(percent, stage, g_progress_user);
}

typedef struct { uintptr_t base; uint32_t size; int found; } ModInfo;
typedef struct {
    uintptr_t drawscene;
    uintptr_t cocos_base;
    uint32_t cocos_size;
    int ok;
    char method[32];
} DrawSceneResult;

typedef struct {
    uintptr_t addr;
    uintptr_t cocos_base;
    int ok;
    char method[32];
} BindingResult;

static int find_module(DWORD pid, const char *name, ModInfo *out) {
    HANDLE h;
    MODULEENTRY32 me;
    out->base = 0;
    out->size = 0;
    out->found = 0;
    h = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, pid);
    if (h == INVALID_HANDLE_VALUE) return 0;
    me.dwSize = sizeof(me);
    if (Module32First(h, &me)) {
        do {
            if (!_stricmp(me.szModule, name)) {
                out->base = (uintptr_t)me.modBaseAddr;
                out->size = me.modBaseSize;
                out->found = 1;
                break;
            }
        } while (Module32Next(h, &me));
    }
    CloseHandle(h);
    return out->found;
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

static void sanitize_process_stem(const char *exe_name, char *out, size_t cap) {
    size_t i, n;
    if (!out || cap < 2) return;
    out[0] = 0;
    if (!exe_name) exe_name = "";
    n = 0;
    for (i = 0; exe_name[i] && n + 1 < cap; i++) {
        unsigned char c = (unsigned char)exe_name[i];
        if (c == '.')
            break;
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '_' || c == '-')
            out[n++] = (char)c;
        else if (n > 0 && out[n - 1] != '_')
            out[n++] = '_';
    }
    while (n > 0 && out[n - 1] == '_')
        n--;
    out[n] = 0;
    if (!out[0])
        strncpy(out, "unknown", cap - 1);
    out[cap - 1] = 0;
}

static int rpm(HANDLE hp, uintptr_t addr, void *buf, size_t n) {
    SIZE_T rd = 0;
    if (!ReadProcessMemory(hp, (void*)addr, buf, n, &rd)) return 0;
    return rd == n;
}

static int resolve_cpp_method_addr(HANDLE hp, DWORD pid, const char *name, BindingResult *res);

static int resolve_drawscene(HANDLE hp, DWORD pid, DrawSceneResult *res) {
    ModInfo cocos = {0};
    BindingResult br;
    memset(res, 0, sizeof(*res));
    if (!find_module(pid, "libcocos2d.dll", &cocos)) return 0;
    res->cocos_base = cocos.base;
    res->cocos_size = cocos.size;
    if (!resolve_cpp_method_addr(hp, pid, "cc.Director:drawScene", &br) || !br.ok)
        return 0;
    res->drawscene = br.addr;
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
        { "cc.Director:drawScene", "?drawScene@Director@cocos2d@@QAEXXZ" },
        { "cc.Director:getDeltaTime", "?getDeltaTime@Director@cocos2d@@QBEMXZ" },
        { "cc.Renderer:render", "?render@Renderer@cocos2d@@QAEXXZ" },
        { "cc.Renderer:render", "?render@Renderer@cocos2d@@QAEXH@Z" },
        { "cc.Scene:render", "?render@Scene@cocos2d@@UAEXPAVRenderer@2@@Z" },
        { "cc.Scene:render", "?render@Scene@cocos2d@@QAEXPAVRenderer@2@@Z" },
        { "cc.Scene:render", "?render@Scene@cocos2d@@QAEXPAVRenderer@2@ABVMat4@math@2@@Z" },
        { "cc.Scene:render", "?render@Scene@cocos2d@@QAEXPAVRenderer@2@ABVMat4@math@2@PBV452@@Z" },
        { "cc.Scene:render", "?render@Scene@cocos2d@@QAEXPAVRenderer@2@ABVMat4@math@2@PBV342@@Z" },
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

static int resolve_cpp_method_addr(HANDLE hp, DWORD pid, const char *name, BindingResult *res) {
    char exe_name[MAX_PATH];
    const char *mods[4];
    int nmods = 0;
    ModInfo cocos = {0};
    ModInfo mod = {0};
    const char *exp;
    int alias, mi;
    uintptr_t addr;
    memset(res, 0, sizeof(*res));
    mods[nmods++] = "libcocos2d.dll";
    if (get_process_exe_name(pid, exe_name, sizeof(exe_name)) &&
        _stricmp(exe_name, "libcocos2d.dll") != 0)
        mods[nmods++] = exe_name;
    mods[nmods] = NULL;
    find_module(pid, "libcocos2d.dll", &cocos);
    res->cocos_base = cocos.base;
    for (mi = 0; mods[mi]; mi++) {
        if (!find_module(pid, mods[mi], &mod)) continue;
        alias = 0;
        while ((exp = cpp_export_aliases(name, alias++)) != NULL) {
            addr = resolve_remote_export(hp, &mod, exp);
            if (!addr) continue;
            res->addr = addr;
            _snprintf(res->method, sizeof(res->method), "export:%s", mods[mi]);
            res->ok = 1;
            return 1;
        }
        if (strcmp(name, "cc.Renderer:render") == 0) {
            addr = resolve_remote_export_contains(hp, &mod, "?render@Renderer@cocos2d@@",
                                                  "QAEXXZ");
            if (addr) {
                res->addr = addr;
                _snprintf(res->method, sizeof(res->method), "export-scan:%s", mods[mi]);
                res->ok = 1;
                return 1;
            }
        }
        if (strcmp(name, "cc.Scene:render") == 0) {
            addr = resolve_remote_export_contains(hp, &mod, "?render@Scene@cocos2d@@",
                                                  NULL);
            if (addr) {
                res->addr = addr;
                _snprintf(res->method, sizeof(res->method), "export-scan:%s", mods[mi]);
                res->ok = 1;
                return 1;
            }
        }
    }
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

static void resolve_cpp_hooks(HANDLE hp, DWORD pid, uintptr_t drawscene_addr,
                              CocosDebugIpcInjectionConfig *config) {
    BindingResult renderer_render;
    BindingResult scene_render;
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

    /* Slot 1: Renderer::render — typical Cocos2d submit path; Scene::render is optional. */
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

    memset(&scene_render, 0, sizeof(scene_render));
    if (resolve_cpp_method_addr(hp, pid, "cc.Scene:render", &scene_render) &&
        scene_render.ok && config->cpp_count < COCOS_DEBUG_IPC_CPP_CAP) {
        strncpy(config->cpp_targets[config->cpp_count].name, "cc.Scene:render",
                COCOS_DEBUG_IPC_CPP_NAME - 1);
        config->cpp_targets[config->cpp_count].address = (uint32_t)scene_render.addr;
        config->cpp_count++;
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
        COCOS_DEBUG_IPC_FEATURE_LINE_SAMPLES;
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
        set_error(error, error_cap, "No valid Lua 5.1 state found");
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
    {
        char exe_path[MAX_PATH];
        char parent[MAX_PATH];
        char proc_exe[MAX_PATH];
        char stem[64];
        char *slash;
        session->ipc->config.luac_dump_dir[0] = 0;
        if (GetModuleFileNameA(NULL, exe_path, MAX_PATH) > 0) {
            slash = strrchr(exe_path, '\\');
            if (slash) {
                *slash = 0;
                if (!get_process_exe_name(pid, proc_exe, sizeof(proc_exe)))
                    strncpy(proc_exe, "unknown", sizeof(proc_exe) - 1);
                proc_exe[sizeof(proc_exe) - 1] = 0;
                sanitize_process_stem(proc_exe, stem, sizeof(stem));
                _snprintf(parent, sizeof(parent), "%s\\luac_dump", exe_path);
                parent[sizeof(parent) - 1] = 0;
                CreateDirectoryA(parent, NULL);
                _snprintf(session->ipc->config.luac_dump_dir,
                          sizeof(session->ipc->config.luac_dump_dir),
                          "%s\\%s", parent, stem);
                session->ipc->config.luac_dump_dir[
                    sizeof(session->ipc->config.luac_dump_dir) - 1] = 0;
                CreateDirectoryA(session->ipc->config.luac_dump_dir, NULL);
            }
        }
    }
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

