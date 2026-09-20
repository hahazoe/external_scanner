#include <math.h>
#include <stdio.h>
#include <string.h>

#include "../injection/lua_profiler.h"

static uint64_t g_clock;
static int g_failures;

static uint64_t fake_clock(void *userdata) {
    (void)userdata;
    return g_clock;
}

static int __cdecl fake_sethook(lua_State *L, lua_Hook hook,
                                int mask, int count) {
    (void)L;
    (void)hook;
    (void)mask;
    (void)count;
    return 0;
}

static int __cdecl fake_getinfo(lua_State *L, const char *what,
                                lua_Debug *ar) {
    (void)L;
    (void)what;
    (void)ar;
    return 0;
}

static int __cdecl fake_getstack(lua_State *L, int level, lua_Debug *ar) {
    (void)L;
    (void)level;
    (void)ar;
    return 0;
}

static void expect_close(const char *name, double actual, double expected) {
    if (fabs(actual - expected) > 0.001) {
        fprintf(stderr, "[FAIL] %s: got %.6f expected %.6f\n",
                name, actual, expected);
        g_failures++;
    }
}

static void expect_u32(const char *name, uint32_t actual,
                       uint32_t expected) {
    if (actual != expected) {
        fprintf(stderr, "[FAIL] %s: got %u expected %u\n",
                name, actual, expected);
        g_failures++;
    }
}

static const CocosDebugIpcMethodSample *find_method(
    const CocosDebugLuaProfilerFrame *frame, uint32_t method_id) {
    uint32_t i;
    for (i = 0; i < frame->method_count; i++) {
        if (frame->methods[i].method_id == method_id)
            return &frame->methods[i];
    }
    return NULL;
}

static void reset_profiler(void) {
    cocos_debug_lua_profiler_stop();
    cocos_debug_lua_profiler_start();
}

static void test_nested_calls(void) {
    lua_State *L = (lua_State *)(uintptr_t)0x10000;
    CocosDebugLuaProfilerFrame frame;
    const CocosDebugIpcMethodSample *a;
    const CocosDebugIpcMethodSample *b;
    reset_profiler();
    cocos_debug_lua_profiler_register_thread(L);
    g_clock = 1000;
    cocos_debug_lua_profiler_frame_begin(1);
    cocos_debug_lua_profiler_test_call(
        L, 1, COCOS_DEBUG_IPC_LUA_METHOD_LUA, 0);
    g_clock = 1010;
    cocos_debug_lua_profiler_test_call(
        L, 2, COCOS_DEBUG_IPC_LUA_METHOD_LUA, 0);
    g_clock = 1030;
    cocos_debug_lua_profiler_test_return(L, 0);
    g_clock = 1040;
    cocos_debug_lua_profiler_test_return(L, 0);
    g_clock = 1050;
    cocos_debug_lua_profiler_frame_end(&frame);
    a = find_method(&frame, 1);
    b = find_method(&frame, 2);
    expect_close("nested.body", frame.body_ms, 50.0);
    expect_close("nested.wall", frame.lua_wall_ms, 40.0);
    expect_close("nested.cpu", frame.lua_cpu_ms, 40.0);
    expect_close("nested.script", frame.lua_script_self_ms, 40.0);
    expect_close("nested.c", frame.lua_c_self_ms, 0.0);
    {
        double method_self = 0.0;
        uint32_t i;
        for (i = 0; i < frame.method_count; i++)
            method_self += frame.methods[i].self_ms;
        expect_close("nested.method self sum", method_self,
                     frame.lua_cpu_ms);
    }
    if (!a || !b) {
        fprintf(stderr, "[FAIL] nested methods missing\n");
        g_failures++;
        return;
    }
    expect_close("A.self", a->self_ms, 20.0);
    expect_close("A.inclusive", a->inclusive_ms, 40.0);
    expect_close("B.self", b->self_ms, 20.0);
    expect_close("B.inclusive", b->inclusive_ms, 20.0);
    expect_u32("A.started", a->calls_started, 1);
    expect_u32("A.completed", a->calls_completed, 1);
}

static void test_lua_and_c_split(void) {
    lua_State *L = (lua_State *)(uintptr_t)0x20000;
    CocosDebugLuaProfilerFrame frame;
    reset_profiler();
    cocos_debug_lua_profiler_register_thread(L);
    g_clock = 2000;
    cocos_debug_lua_profiler_frame_begin(2);
    cocos_debug_lua_profiler_test_call(
        L, 1, COCOS_DEBUG_IPC_LUA_METHOD_LUA, 0);
    g_clock = 2010;
    cocos_debug_lua_profiler_test_call(
        L, 3, COCOS_DEBUG_IPC_LUA_METHOD_C, 0);
    g_clock = 2040;
    cocos_debug_lua_profiler_test_return(L, 0);
    g_clock = 2050;
    cocos_debug_lua_profiler_test_return(L, 0);
    cocos_debug_lua_profiler_frame_end(&frame);
    expect_close("split.cpu", frame.lua_cpu_ms, 50.0);
    expect_close("split.script", frame.lua_script_self_ms, 20.0);
    expect_close("split.c", frame.lua_c_self_ms, 30.0);
}

static void test_yield_and_cross_frame(void) {
    lua_State *L = (lua_State *)(uintptr_t)0x30000;
    CocosDebugLuaProfilerFrame first;
    CocosDebugLuaProfilerFrame second;
    const CocosDebugIpcMethodSample *a;
    reset_profiler();
    cocos_debug_lua_profiler_register_thread(L);
    g_clock = 3000;
    cocos_debug_lua_profiler_frame_begin(3);
    cocos_debug_lua_profiler_test_call(
        L, 1, COCOS_DEBUG_IPC_LUA_METHOD_LUA, 0);
    g_clock = 3010;
    cocos_debug_lua_profiler_resume_leave(L, LUA_YIELD);
    g_clock = 3030;
    cocos_debug_lua_profiler_frame_end(&first);
    a = find_method(&first, 1);
    expect_close("yield.first.wall", first.lua_wall_ms, 10.0);
    expect_close("yield.first.cpu", first.lua_cpu_ms, 10.0);
    if (!a) {
        fprintf(stderr, "[FAIL] yield first method missing\n");
        g_failures++;
    } else {
        expect_close("yield.first.inclusive", a->inclusive_ms, 10.0);
        expect_u32("yield.first.active", a->active_at_end, 1);
    }

    g_clock = 3040;
    cocos_debug_lua_profiler_frame_begin(4);
    g_clock = 3050;
    cocos_debug_lua_profiler_resume_enter(L);
    g_clock = 3070;
    cocos_debug_lua_profiler_frame_end(&second);
    a = find_method(&second, 1);
    expect_close("yield.second.wall", second.lua_wall_ms, 20.0);
    expect_close("yield.second.cpu", second.lua_cpu_ms, 20.0);
    if (!a) {
        fprintf(stderr, "[FAIL] yield second method missing\n");
        g_failures++;
    } else {
        expect_close("yield.second.inclusive", a->inclusive_ms, 20.0);
        expect_u32("yield.second.started", a->calls_started, 0);
        expect_u32("yield.second.active", a->active_at_end, 1);
    }
}

static void test_concurrent_union(void) {
    lua_State *L1 = (lua_State *)(uintptr_t)0x40000;
    lua_State *L2 = (lua_State *)(uintptr_t)0x50000;
    CocosDebugLuaProfilerFrame frame;
    reset_profiler();
    cocos_debug_lua_profiler_register_thread(L1);
    cocos_debug_lua_profiler_register_thread(L2);
    g_clock = 4000;
    cocos_debug_lua_profiler_frame_begin(5);
    cocos_debug_lua_profiler_test_call(
        L1, 1, COCOS_DEBUG_IPC_LUA_METHOD_LUA, 0);
    g_clock = 4010;
    cocos_debug_lua_profiler_test_call(
        L2, 2, COCOS_DEBUG_IPC_LUA_METHOD_LUA, 0);
    g_clock = 4050;
    cocos_debug_lua_profiler_test_return(L1, 0);
    g_clock = 4070;
    cocos_debug_lua_profiler_test_return(L2, 0);
    g_clock = 4100;
    cocos_debug_lua_profiler_frame_end(&frame);
    expect_close("union.wall", frame.lua_wall_ms, 70.0);
    expect_close("union.cpu", frame.lua_cpu_ms, 110.0);
    expect_close("union.script", frame.lua_script_self_ms, 110.0);
}

static void test_frame_outside_excluded(void) {
    lua_State *L = (lua_State *)(uintptr_t)0x60000;
    CocosDebugLuaProfilerFrame frame;
    const CocosDebugIpcMethodSample *a;
    reset_profiler();
    cocos_debug_lua_profiler_register_thread(L);
    g_clock = 5000;
    cocos_debug_lua_profiler_test_call(
        L, 1, COCOS_DEBUG_IPC_LUA_METHOD_LUA, 0);
    g_clock = 5010;
    cocos_debug_lua_profiler_frame_begin(6);
    g_clock = 5030;
    cocos_debug_lua_profiler_frame_end(&frame);
    a = find_method(&frame, 1);
    expect_close("outside.wall", frame.lua_wall_ms, 20.0);
    expect_close("outside.cpu", frame.lua_cpu_ms, 20.0);
    if (!a) {
        fprintf(stderr, "[FAIL] outside method missing\n");
        g_failures++;
    } else {
        expect_u32("outside.started", a->calls_started, 0);
        expect_u32("outside.completed", a->calls_completed, 0);
        expect_u32("outside.active", a->active_at_end, 1);
    }
}

static void test_resume_does_not_double_count_parent(void) {
    lua_State *parent = (lua_State *)(uintptr_t)0x70000;
    lua_State *child = (lua_State *)(uintptr_t)0x80000;
    CocosDebugLuaProfilerFrame frame;
    reset_profiler();
    cocos_debug_lua_profiler_register_thread(parent);
    cocos_debug_lua_profiler_register_thread(child);
    g_clock = 6000;
    cocos_debug_lua_profiler_frame_begin(7);
    cocos_debug_lua_profiler_test_call(
        parent, 1, COCOS_DEBUG_IPC_LUA_METHOD_LUA, 0);
    g_clock = 6010;
    cocos_debug_lua_profiler_test_call(
        parent, 3, COCOS_DEBUG_IPC_LUA_METHOD_C, 0);
    g_clock = 6020;
    cocos_debug_lua_profiler_resume_enter(child);
    cocos_debug_lua_profiler_test_call(
        child, 2, COCOS_DEBUG_IPC_LUA_METHOD_LUA, 0);
    g_clock = 6050;
    cocos_debug_lua_profiler_test_return(child, 0);
    cocos_debug_lua_profiler_resume_leave(child, 0);
    g_clock = 6060;
    cocos_debug_lua_profiler_test_return(parent, 0);
    g_clock = 6070;
    cocos_debug_lua_profiler_test_return(parent, 0);
    cocos_debug_lua_profiler_frame_end(&frame);
    expect_close("resume-parent.wall", frame.lua_wall_ms, 70.0);
    expect_close("resume-parent.cpu", frame.lua_cpu_ms, 70.0);
    expect_close("resume-parent.script", frame.lua_script_self_ms, 50.0);
    expect_close("resume-parent.c", frame.lua_c_self_ms, 20.0);
}

static void expect_method_self_sum(const char *name,
                                   const CocosDebugLuaProfilerFrame *frame) {
    double method_self = 0.0;
    uint32_t i;
    for (i = 0; i < frame->method_count; i++)
        method_self += frame->methods[i].self_ms;
    if (fabs(method_self - frame->lua_cpu_ms) > 0.001) {
        fprintf(stderr, "[FAIL] %s: method self sum %.6f != lua_cpu %.6f\n",
                name, method_self, frame->lua_cpu_ms);
        g_failures++;
    }
}

/* Regression: two root calls in one frame.  The second used to latch the
   first call's leftover logical clock as its start, so its inclusive time
   came out as 0 (now - stale_start underflowed to the clamp). */
static void test_two_root_calls_in_one_frame(void) {
    lua_State *L = (lua_State *)(uintptr_t)0x90000;
    CocosDebugLuaProfilerFrame frame;
    const CocosDebugIpcMethodSample *a;
    const CocosDebugIpcMethodSample *b;
    reset_profiler();
    cocos_debug_lua_profiler_register_thread(L);
    g_clock = 7000;
    cocos_debug_lua_profiler_frame_begin(8);
    cocos_debug_lua_profiler_test_call(
        L, 1, COCOS_DEBUG_IPC_LUA_METHOD_LUA, 0);
    g_clock = 7010;
    cocos_debug_lua_profiler_test_return(L, 0);
    cocos_debug_lua_profiler_test_call(
        L, 2, COCOS_DEBUG_IPC_LUA_METHOD_LUA, 0);
    g_clock = 7040;
    cocos_debug_lua_profiler_test_return(L, 0);
    g_clock = 7050;
    cocos_debug_lua_profiler_frame_end(&frame);
    expect_close("root2.body", frame.body_ms, 50.0);
    expect_close("root2.wall", frame.lua_wall_ms, 40.0);
    expect_close("root2.cpu", frame.lua_cpu_ms, 40.0);
    expect_close("root2.script", frame.lua_script_self_ms, 40.0);
    expect_method_self_sum("root2", &frame);
    a = find_method(&frame, 1);
    b = find_method(&frame, 2);
    if (!a || !b) {
        fprintf(stderr, "[FAIL] root2 methods missing\n");
        g_failures++;
        return;
    }
    expect_close("root2.first.inclusive", a->inclusive_ms, 10.0);
    expect_close("root2.first.self", a->self_ms, 10.0);
    expect_close("root2.second.inclusive", b->inclusive_ms, 30.0);
    expect_close("root2.second.self", b->self_ms, 30.0);
    expect_u32("root2.second.completed", b->calls_completed, 1);
}

/* Regression: an error caught by a protected call.  Lua longjmps out of the
   failing chain, so no RET hooks arrive for the frames in between; before the
   fix they stayed on the shadow stack forever and swallowed the attribution
   of everything that ran afterwards. */
static void test_protected_error_unwind(void) {
    lua_State *L = (lua_State *)(uintptr_t)0xa0000;
    CocosDebugLuaProfilerFrame frame;
    const CocosDebugIpcMethodSample *pcall_method;
    const CocosDebugIpcMethodSample *chain_root;
    const CocosDebugIpcMethodSample *dead_inner;
    const CocosDebugIpcMethodSample *after;
    int tracked;
    reset_profiler();
    cocos_debug_lua_profiler_register_thread(L);
    g_clock = 8000;
    cocos_debug_lua_profiler_frame_begin(9);
    /* pcall(chunk): the Lua builtin shows up as a C frame. */
    cocos_debug_lua_profiler_test_call(
        L, 1, COCOS_DEBUG_IPC_LUA_METHOD_C, 0);
    g_clock = 8010;
    tracked = cocos_debug_lua_profiler_protected_enter(L);
    if (!tracked) {
        fprintf(stderr, "[FAIL] protected_enter refused\n");
        g_failures++;
        return;
    }
    cocos_debug_lua_profiler_test_call(
        L, 2, COCOS_DEBUG_IPC_LUA_METHOD_LUA, 0);
    g_clock = 8030;
    cocos_debug_lua_profiler_test_call(
        L, 3, COCOS_DEBUG_IPC_LUA_METHOD_LUA, 0);
    g_clock = 8060;
    /* error() longjmps away: frames 2 and 3 never emit a return hook. */
    cocos_debug_lua_profiler_protected_leave(L, tracked, LUA_ERRRUN);
    g_clock = 8070;
    cocos_debug_lua_profiler_test_return(L, 0); /* pcall returns the error */
    g_clock = 8080;
    /* The caller carries on.  This root call is the real regression check:
       with leaked frames its inclusive collapsed and its self time landed on
       a ghost entry. */
    cocos_debug_lua_profiler_test_call(
        L, 4, COCOS_DEBUG_IPC_LUA_METHOD_LUA, 0);
    g_clock = 8100;
    cocos_debug_lua_profiler_test_return(L, 0);
    g_clock = 8110;
    cocos_debug_lua_profiler_frame_end(&frame);
    expect_close("unwind.body", frame.body_ms, 110.0);
    expect_close("unwind.wall", frame.lua_wall_ms, 90.0);
    expect_close("unwind.cpu", frame.lua_cpu_ms, 90.0);
    expect_close("unwind.script", frame.lua_script_self_ms, 70.0);
    expect_close("unwind.c", frame.lua_c_self_ms, 20.0);
    expect_method_self_sum("unwind", &frame);
    pcall_method = find_method(&frame, 1);
    chain_root = find_method(&frame, 2);
    dead_inner = find_method(&frame, 3);
    after = find_method(&frame, 4);
    if (!pcall_method || !chain_root || !after) {
        fprintf(stderr, "[FAIL] unwind methods missing\n");
        g_failures++;
        return;
    }
    expect_close("unwind.pcall.self", pcall_method->self_ms, 20.0);
    expect_close("unwind.pcall.inclusive", pcall_method->inclusive_ms, 70.0);
    expect_u32("unwind.pcall.completed", pcall_method->calls_completed, 1);
    /* The outermost dead frame keeps the time; the inner one is folded in. */
    expect_close("unwind.chain.self", chain_root->self_ms, 50.0);
    expect_close("unwind.chain.inclusive", chain_root->inclusive_ms, 0.0);
    expect_u32("unwind.chain.started", chain_root->calls_started, 1);
    expect_u32("unwind.chain.completed", chain_root->calls_completed, 0);
    if (dead_inner) {
        expect_close("unwind.inner.self", dead_inner->self_ms, 0.0);
        expect_close("unwind.inner.inclusive", dead_inner->inclusive_ms, 0.0);
    }
    expect_close("unwind.after.inclusive", after->inclusive_ms, 20.0);
    expect_close("unwind.after.self", after->self_ms, 20.0);
    expect_u32("unwind.after.completed", after->calls_completed, 1);
}

/* Regression: Lua running in the gap between frame_end and the next
   frame_begin used to be dropped entirely -- the window owns the method
   accumulators, so nothing could be attributed.  It is now kept as a
   thread-level counter, harvested by the frame_begin that has to make room
   for it, and published as lua_outside_ms.  It must stay OUT of lua_cpu
   (sum(method self) == lua_cpu has to keep holding). */
static void test_outside_window_lua(void) {
    lua_State *L = (lua_State *)(uintptr_t)0xb0000;
    CocosDebugLuaProfilerFrame first;
    CocosDebugLuaProfilerFrame second;
    reset_profiler();
    cocos_debug_lua_profiler_register_thread(L);
    g_clock = 9000;
    cocos_debug_lua_profiler_frame_begin(10);
    cocos_debug_lua_profiler_test_call(
        L, 1, COCOS_DEBUG_IPC_LUA_METHOD_LUA, 0);
    g_clock = 9020;
    /* Still running as the window closes. */
    cocos_debug_lua_profiler_frame_end(&first);
    expect_close("outside.first.cpu", first.lua_cpu_ms, 20.0);
    expect_close("outside.first.outside", first.lua_outside_ms, 0.0);

    g_clock = 9040;
    /* The Lua run finishes in the gap: 20 ticks with no window open. */
    cocos_debug_lua_profiler_test_return(L, 0);
    g_clock = 9050;
    cocos_debug_lua_profiler_frame_begin(11);
    g_clock = 9060;
    cocos_debug_lua_profiler_frame_end(&second);
    /* The gap Lua is reported, and only reported. */
    expect_close("outside.second.outside", second.lua_outside_ms, 20.0);
    expect_close("outside.second.cpu", second.lua_cpu_ms, 0.0);
    expect_u32("outside.second.methods", second.method_count, 0);
    expect_close("outside.second.body", second.body_ms, 10.0);
}

int main(void) {
    CocosDebugLuaProfilerApi api;
    memset(&api, 0, sizeof(api));
    api.sethook = fake_sethook;
    api.getinfo = fake_getinfo;
    api.getstack = fake_getstack;
    api.clock_now = fake_clock;
    api.qpc_frequency = 1000;
    if (!cocos_debug_lua_profiler_init(&api)) {
        fprintf(stderr, "[FAIL] profiler init\n");
        return 1;
    }
    test_nested_calls();
    test_lua_and_c_split();
    test_yield_and_cross_frame();
    test_concurrent_union();
    test_frame_outside_excluded();
    test_resume_does_not_double_count_parent();
    test_two_root_calls_in_one_frame();
    test_protected_error_unwind();
    test_outside_window_lua();
    cocos_debug_lua_profiler_shutdown();
    if (g_failures) {
        fprintf(stderr, "[FAIL] %d assertions\n", g_failures);
        return 1;
    }
    printf("[OK] precise Lua profiler core\n");
    return 0;
}
