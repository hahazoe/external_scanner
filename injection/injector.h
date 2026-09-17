#ifndef COCOS_DEBUG_INJECTOR_H
#define COCOS_DEBUG_INJECTOR_H

#include <windows.h>
#include "../common/profiler_ipc.h"

#ifdef __cplusplus
extern "C" {
#endif

#define COCOS_DEBUG_PROCESS_NAME_CAP 260

typedef struct CocosDebugProcessInfo {
    DWORD pid;
    char name[COCOS_DEBUG_PROCESS_NAME_CAP];
    HWND window;
} CocosDebugProcessInfo;

typedef void (*CocosDebugProgressCallback)(int percent, const char *stage, void *user);

typedef struct CocosDebugInjectionSession {
    DWORD pid;
    HWND target_window;
    HANDLE mapping;
    CocosDebugIpcBlock *ipc;
} CocosDebugInjectionSession;

int cocos_debug_list_processes(CocosDebugProcessInfo *items, int capacity);
int cocos_debug_inject_process(DWORD pid, CocosDebugProgressCallback callback, void *user,
                        CocosDebugInjectionSession *session, char *error, int error_cap);
void cocos_debug_close_session(CocosDebugInjectionSession *session);
int cocos_debug_send_command(CocosDebugInjectionSession *session, int command);
int cocos_debug_run_lua_string(CocosDebugInjectionSession *session, const char *script,
                        char *error, int error_cap);

#ifdef __cplusplus
}
#endif

#endif
