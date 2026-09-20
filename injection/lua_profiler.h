#ifndef COCOS_DEBUG_LUA_PROFILER_H
#define COCOS_DEBUG_LUA_PROFILER_H

#include <stdint.h>
#include "lua.h"
#include "../common/profiler_ipc.h"

#ifdef __cplusplus
extern "C" {
#endif

#define COCOS_DEBUG_LUA_PROFILER_STACK_CAP 256
#define COCOS_DEBUG_LUA_PROFILER_THREAD_CAP 1024
#define COCOS_DEBUG_LUA_PROFILER_INTERVAL_CAP 65536 /* retained for tests / docs */
#define COCOS_DEBUG_LUA_PROFILER_LINE_COUNT 4000

typedef int (__cdecl *CocosDebugLuaSetHookFn)(lua_State *, lua_Hook, int, int);
typedef int (__cdecl *CocosDebugLuaGetInfoFn)(lua_State *, const char *, lua_Debug *);
typedef int (__cdecl *CocosDebugLuaGetStackFn)(lua_State *, int, lua_Debug *);
typedef uint32_t (*CocosDebugLuaInternFn)(const char *);
typedef void (*CocosDebugLuaEmitEventFn)(const CocosDebugIpcLuaEvent *);
typedef uint64_t (*CocosDebugLuaClockFn)(void *);

typedef struct CocosDebugLuaProfilerApi {
    CocosDebugLuaSetHookFn sethook;
    CocosDebugLuaGetInfoFn getinfo;
    CocosDebugLuaGetStackFn getstack;
    CocosDebugLuaInternFn intern_method;
    CocosDebugLuaEmitEventFn emit_event;
    CocosDebugLuaClockFn clock_now;
    void *clock_userdata;
    uint64_t qpc_frequency;
} CocosDebugLuaProfilerApi;

typedef struct CocosDebugLuaProfilerFrame {
    uint64_t frame_sequence;
    uint64_t begin_qpc;
    uint64_t end_qpc;
    double body_ms;
    double lua_wall_ms;
    double lua_cpu_ms;
    double lua_script_self_ms;
    double lua_c_self_ms;
    double lua_outside_ms;
    uint32_t hook_events;
    uint32_t flags;
    uint32_t method_count;
    uint32_t line_count;
    CocosDebugIpcMethodSample methods[COCOS_DEBUG_IPC_METHOD_CAP];
    CocosDebugIpcLineSample lines[COCOS_DEBUG_IPC_LINE_CAP];
} CocosDebugLuaProfilerFrame;

int cocos_debug_lua_profiler_init(const CocosDebugLuaProfilerApi *api);
void cocos_debug_lua_profiler_shutdown(void);
void cocos_debug_lua_profiler_start(void);
void cocos_debug_lua_profiler_stop(void);
void cocos_debug_lua_profiler_clear(void);

void cocos_debug_lua_profiler_register_thread(lua_State *L);
void cocos_debug_lua_profiler_resume_enter(lua_State *L);
void cocos_debug_lua_profiler_resume_leave(lua_State *L, int status);

/* Protected-call boundary tracking.  A thrown Lua error unwinds with a
   longjmp, so the RET hooks of every frame between the error and the
   catching lua_pcall/lua_resume never fire.  The CALL/RET shadow stack
   would keep those dead frames forever -- every later inclusive time is
   computed against them and, once the 256 deep stack fills up, the thread
   loses all attribution.  Wrap lua_pcall with enter/leave so the shadow
   stack can be unwound to the depth recorded at entry.  Both calls are
   cheap no-ops unless a frame is being recorded. */
int cocos_debug_lua_profiler_protected_enter(lua_State *L);
void cocos_debug_lua_profiler_protected_leave(lua_State *L, int tracked,
                                              int status);

void cocos_debug_lua_profiler_set_line_capture(int enabled);
void cocos_debug_lua_profiler_set_trace(int enabled);
int cocos_debug_lua_profiler_trace_enabled(void);

void cocos_debug_lua_profiler_frame_begin(uint64_t frame_sequence);
int cocos_debug_lua_profiler_frame_end(CocosDebugLuaProfilerFrame *out);
void cocos_debug_lua_profiler_frame_abort(void);

void cocos_debug_lua_profiler_pause_accounting(void);
void cocos_debug_lua_profiler_resume_accounting(void);

#ifdef COCOS_DEBUG_LUA_PROFILER_TESTING
void cocos_debug_lua_profiler_test_call(lua_State *L, uint32_t method_id,
                                 uint16_t kind, uint16_t flags);
void cocos_debug_lua_profiler_test_return(lua_State *L, int tail_return);
#endif

#ifdef __cplusplus
}
#endif

#endif
