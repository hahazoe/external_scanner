#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
#include "../injection/lua_profiler.h"
#include "../injection/lua_api_hooks.h"

static char g_keys[COCOS_DEBUG_IPC_METHOD_DICT_CAP][COCOS_DEBUG_IPC_METHOD_KEY];
static uint32_t g_key_count;
static int g_failures;
static uint32_t g_event_counts[8];

static uint32_t intern_key(const char *key) {
    uint32_t i;
    for (i = 0; i < g_key_count; i++) {
        if (strcmp(g_keys[i], key) == 0) return i + 1;
    }
    if (g_key_count >= COCOS_DEBUG_IPC_METHOD_DICT_CAP) return 0;
    strncpy(g_keys[g_key_count], key, COCOS_DEBUG_IPC_METHOD_KEY - 1);
    g_keys[g_key_count][COCOS_DEBUG_IPC_METHOD_KEY - 1] = 0;
    return ++g_key_count;
}

static void capture_event(const CocosDebugIpcLuaEvent *event) {
    if (event && event->type < 8)
        g_event_counts[event->type]++;
}

static int call_lua_inner(lua_State *L) {
    lua_getglobal(L, "inner");
    lua_call(L, 0, 0);
    return 0;
}

static int busy_c(lua_State *L) {
    volatile int i;
    (void)L;
    for (i = 0; i < 100000; i++) {
    }
    return 0;
}

static void fail(const char *message) {
    fprintf(stderr, "[FAIL] %s\n", message);
    g_failures++;
}

static void validate_frame(const char *name,
                           const CocosDebugLuaProfilerFrame *frame) {
    double split = frame->lua_script_self_ms + frame->lua_c_self_ms;
    uint32_t i;
    int saw_lua = 0;
    int saw_c = 0;
    if (frame->lua_wall_ms < 0.0 ||
        frame->lua_wall_ms > frame->body_ms + 0.2)
        fail("Lua wall is outside drawScene body");
    if (fabs(split - frame->lua_cpu_ms) > 0.05) {
        fprintf(stderr,
                "[FAIL] %s self split %.3f != CPU %.3f\n",
                name, split, frame->lua_cpu_ms);
        g_failures++;
    }
    if (fabs(frame->lua_wall_ms - frame->lua_cpu_ms) > 0.5) {
        fprintf(stderr,
                "[FAIL] %s single-thread wall %.3f != CPU %.3f\n",
                name, frame->lua_wall_ms, frame->lua_cpu_ms);
        g_failures++;
    }
    if (frame->flags & COCOS_DEBUG_IPC_FRAME_STACK_DESYNC)
        fail("Lua stack desynchronized");
    {
        double script_sum = 0.0;
        double c_sum = 0.0;
        for (i = 0; i < frame->method_count; i++) {
            if (frame->methods[i].kind == COCOS_DEBUG_IPC_LUA_METHOD_C) {
                saw_c = 1;
                c_sum += frame->methods[i].self_ms;
            } else {
                if (frame->methods[i].kind == COCOS_DEBUG_IPC_LUA_METHOD_LUA ||
                    frame->methods[i].kind == COCOS_DEBUG_IPC_LUA_METHOD_MAIN)
                    saw_lua = 1;
                script_sum += frame->methods[i].self_ms;
            }
        }
        if (fabs(script_sum - frame->lua_script_self_ms) > 0.05) {
            fprintf(stderr,
                    "[FAIL] %s lua method self %.3f != script %.3f\n",
                    name, script_sum, frame->lua_script_self_ms);
            g_failures++;
        }
        if (fabs(c_sum - frame->lua_c_self_ms) > 0.05) {
            fprintf(stderr,
                    "[FAIL] %s C method self %.3f != c %.3f\n",
                    name, c_sum, frame->lua_c_self_ms);
            g_failures++;
        }
    }
    if (!saw_lua) fail("Lua method samples missing");
    if (strcmp(name, "mixed") == 0 && !saw_c)
        fail("C method samples missing");
}

static int run_chunk(lua_State *L, const char *source) {
    int status = luaL_loadstring(L, source);
    if (status == 0) status = lua_pcall(L, 0, 0, 0);
    if (status != 0) {
        fprintf(stderr, "[FAIL] Lua: %s\n", lua_tostring(L, -1));
        lua_pop(L, 1);
        g_failures++;
    }
    return status;
}

int main(void) {
    lua_State *L;
    lua_State *co;
    CocosDebugLuaProfilerApi api;
    CocosDebugLuaProfilerFrame frame;
    int status;
    uint32_t i;

    L = luaL_newstate();
    if (!L) return 1;
    /* This fork's int64 library is intentionally 64-bit-only. */
    luaopen_base(L);
    lua_settop(L, 0);
    lua_pushcfunction(L, busy_c);
    lua_setglobal(L, "busy_c");

    memset(&api, 0, sizeof(api));
    api.sethook = lua_sethook;
    api.getinfo = lua_getinfo;
    api.getstack = lua_getstack;
    api.intern_method = intern_key;
    api.emit_event = capture_event;
    if (!cocos_debug_lua_profiler_init(&api)) {
        lua_close(L);
        return 1;
    }
    {
        int hooks = cocos_debug_lua_api_hooks_install(
            (void *)lua_resume, (void *)lua_newthread,
            (void *)lua_pcall, (void *)lua_call);
        if ((hooks & 1) == 0)
            fail("lua_resume API hook install failed");
        if ((hooks & 4) == 0)
            fail("lua_pcall API hook install failed");
        if ((hooks & 8) == 0)
            fail("lua_call API hook install failed");
    }
    cocos_debug_lua_profiler_start();
    cocos_debug_lua_profiler_set_trace(1);
    cocos_debug_lua_profiler_register_thread(L);

    run_chunk(L,
        "function inner() local n=0 for i=1,2000 do n=n+i end end\n"
        "function outer() inner(); busy_c(); inner() end\n"
        "function tail(n) if n==0 then return 1 end "
        "return tail(n-1) end");

    lua_pushcfunction(L, call_lua_inner);
    lua_setglobal(L, "call_from_c");

    cocos_debug_lua_profiler_frame_begin(1);
    lua_getglobal(L, "outer");
    status = lua_pcall(L, 0, 0, 0);
    cocos_debug_lua_profiler_frame_end(&frame);
    if (status != 0) fail("mixed pcall failed");
    validate_frame("mixed", &frame);

    cocos_debug_lua_profiler_frame_begin(8);
    lua_getglobal(L, "call_from_c");
    status = lua_pcall(L, 0, 0, 0);
    cocos_debug_lua_profiler_frame_end(&frame);
    if (status != 0) fail("lua-call pcall failed");
    validate_frame("lua-call", &frame);

    cocos_debug_lua_profiler_frame_begin(2);
    lua_getglobal(L, "tail");
    lua_pushinteger(L, 20);
    status = lua_pcall(L, 1, 1, 0);
    lua_pop(L, 1);
    cocos_debug_lua_profiler_frame_end(&frame);
    if (status != 0) fail("tail pcall failed");
    validate_frame("tail", &frame);

    run_chunk(L,
        "co2 = coroutine.create(function() "
        "local n=0 for i=1,2000 do n=n+i end return n end)");
    status = luaL_loadstring(L, "assert(coroutine.resume(co2))");
    if (status != 0) fail("Lua coroutine driver compile failed");
    cocos_debug_lua_profiler_frame_begin(3);
    if (status == 0) status = lua_pcall(L, 0, 0, 0);
    cocos_debug_lua_profiler_frame_end(&frame);
    if (status != 0) fail("Lua coroutine.resume failed");
    validate_frame("lua-resume", &frame);

    co = lua_newthread(L);
    status = luaL_loadstring(
        co,
        "local n=0 for i=1,1000 do n=n+i end "
        "coroutine.yield(n) "
        "for i=1,1000 do n=n+i end return n");
    if (status != 0) fail("coroutine compile failed");
    cocos_debug_lua_profiler_register_thread(co);

    cocos_debug_lua_profiler_frame_begin(4);
    status = lua_resume(co, 0);
    cocos_debug_lua_profiler_frame_end(&frame);
    if (status != LUA_YIELD) fail("coroutine did not yield");
    validate_frame("yield", &frame);

    lua_settop(co, 0);
    cocos_debug_lua_profiler_frame_begin(5);
    status = lua_resume(co, 0);
    cocos_debug_lua_profiler_frame_end(&frame);
    if (status != 0) fail("coroutine did not finish");
    validate_frame("resume", &frame);

    /* An error raised inside a protected call unwinds by longjmp, so the
       profiler gets no return hooks for the frames in between.  Those frames
       used to stay on its shadow stack forever; the work that followed in the
       same frame was then attributed against a stale depth. */
    run_chunk(L,
        "function boom() local t = nil return t.x end\n"
        "function after() local n=0 for i=1,2000 do n=n+i end return n end");
    cocos_debug_lua_profiler_frame_begin(6);
    lua_getglobal(L, "boom");
    status = lua_pcall(L, 0, 0, 0);
    if (status == 0) fail("boom() did not raise");
    lua_pop(L, 1);
    lua_getglobal(L, "after");
    status = lua_pcall(L, 0, 1, 0);
    lua_pop(L, 1);
    cocos_debug_lua_profiler_frame_end(&frame);
    if (status != 0) fail("after() failed");
    validate_frame("pcall-error", &frame);
    for (i = 0; i < frame.method_count; i++) {
        if (frame.methods[i].active_at_end != 0) {
            fprintf(stderr,
                    "[FAIL] method %u still active after a caught error\n",
                    frame.methods[i].method_id);
            g_failures++;
        }
    }

    /* Line time.  Instruction sampling (one hit per 4000 VM instructions)
       cannot see time burnt inside a C call: the counter is frozen while the
       C function runs.  The line entry now also carries a real clock budget,
       so a line whose whole cost is `busy_c()` shows up with samples near
       zero but a non-zero time. */
    cocos_debug_lua_profiler_set_line_capture(1);
    run_chunk(L,
        "function cheavy() local n=0 for i=1,4000 do n=n+i end "
        "busy_c(); return n end");
    cocos_debug_lua_profiler_frame_begin(7);
    lua_getglobal(L, "cheavy");
    status = lua_pcall(L, 0, 0, 0);
    cocos_debug_lua_profiler_frame_end(&frame);
    if (status != 0) fail("cheavy failed");
    {
        LARGE_INTEGER freq;
        double total_line_ms = 0.0;
        uint32_t timed_lines = 0;
        QueryPerformanceFrequency(&freq);
        if (!frame.line_count) fail("no line samples with line capture on");
        for (i = 0; i < frame.line_count; i++) {
            if (frame.lines[i].qpc > 0) timed_lines++;
            if (freq.QuadPart)
                total_line_ms += (double)frame.lines[i].qpc * 1000.0 /
                                 (double)freq.QuadPart;
        }
        if (!timed_lines)
            fail("no line carries a time budget (line qpc never attributed)");
        /* Line time is a slice of the same clock as lua_cpu, so it can never
           add up to more than lua_cpu -- that would mean double counting. */
        if (total_line_ms > frame.lua_cpu_ms + 0.2) {
            fprintf(stderr,
                    "[FAIL] line time %.3f exceeds lua_cpu %.3f\n",
                    total_line_ms, frame.lua_cpu_ms);
            g_failures++;
        }
    }

    if (!g_event_counts[COCOS_DEBUG_IPC_LUA_EVENT_CALL] ||
        !g_event_counts[COCOS_DEBUG_IPC_LUA_EVENT_RETURN] ||
        !g_event_counts[COCOS_DEBUG_IPC_LUA_EVENT_TAIL_RETURN] ||
        !g_event_counts[COCOS_DEBUG_IPC_LUA_EVENT_RESUME] ||
        !g_event_counts[COCOS_DEBUG_IPC_LUA_EVENT_SUSPEND] ||
        !g_event_counts[COCOS_DEBUG_IPC_LUA_EVENT_FRAME_BEGIN] ||
        !g_event_counts[COCOS_DEBUG_IPC_LUA_EVENT_FRAME_END])
        fail("raw Lua event stream is incomplete");

    cocos_debug_lua_profiler_stop();
    cocos_debug_lua_api_hooks_remove();
    cocos_debug_lua_profiler_shutdown();
    lua_close(L);
    if (g_failures) {
        fprintf(stderr, "[FAIL] %d Lua 5.1 integration assertions\n",
                g_failures);
        return 1;
    }
    printf("[OK] Lua 5.1 profiler integration\n");
    return 0;
}
