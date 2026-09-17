#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../injection/injector.h"

typedef struct ScenarioStats {
    int frames;
    int invalid;
    double body_sum;
    double wall_sum;
    double cpu_sum;
    double script_sum;
    double c_sum;
    double body_max;
    double wall_max;
} ScenarioStats;

static void progress(int percent, const char *stage, void *user) {
    (void)user;
    printf("[inject %3d%%] %s\n", percent, stage ? stage : "");
}

static int wait_state(CocosDebugInjectionSession *session, int expected,
                      DWORD timeout_ms) {
    DWORD start = GetTickCount();
    while ((DWORD)(GetTickCount() - start) < timeout_ms) {
        if (session->ipc->header.state == expected) return 1;
        if (session->ipc->header.state == COCOS_DEBUG_IPC_STATE_ERROR) {
            fprintf(stderr, "[FAIL] target: %s\n",
                    session->ipc->header.error);
            return 0;
        }
        Sleep(10);
    }
    return 0;
}

static int read_frame(CocosDebugIpcBlock *ipc, uint32_t sequence,
                      CocosDebugIpcFrame *out) {
    CocosDebugIpcFrame *source =
        &ipc->frames[sequence % COCOS_DEBUG_IPC_FRAME_CAP];
    LONG before = InterlockedCompareExchange(
        (LONG *)&source->sequence, 0, 0);
    LONG after;
    if (before & 1) return 0;
    memcpy(out, source, sizeof(*out));
    MemoryBarrier();
    after = InterlockedCompareExchange(
        (LONG *)&source->sequence, 0, 0);
    return before == after && !(after & 1) &&
           out->publish_sequence == sequence;
}

static int validate_frame(const CocosDebugIpcFrame *frame) {
    double split = frame->lua_script_self_ms + frame->lua_c_self_ms;
    double method_self = 0.0;
    double tolerance = 0.35 + frame->lua_cpu_ms * 0.03;
    uint32_t i;
    if (frame->drawscene_body_ms < 0.0f ||
        frame->lua_wall_ms < 0.0f ||
        frame->lua_cpu_ms < 0.0f ||
        frame->lua_wall_ms > frame->drawscene_body_ms + 0.35f)
        return 0;
    if (fabs(split - frame->lua_cpu_ms) > tolerance)
        return 0;
    if (frame->flags &
        (COCOS_DEBUG_IPC_FRAME_STACK_DESYNC |
         COCOS_DEBUG_IPC_FRAME_STATE_CONCURRENT))
        return 0;
    if (!(frame->flags & COCOS_DEBUG_IPC_FRAME_METHOD_LIMITED)) {
        for (i = 0; i < frame->method_count; i++)
            method_self += frame->methods[i].self_ms;
        if (fabs(method_self - frame->lua_cpu_ms) > tolerance)
            return 0;
    }
    return 1;
}

static int collect_frames(CocosDebugInjectionSession *session, int count,
                          DWORD timeout_ms, ScenarioStats *stats) {
    CocosDebugIpcFrame *frame =
        (CocosDebugIpcFrame *)malloc(sizeof(CocosDebugIpcFrame));
    uint32_t next = (uint32_t)InterlockedCompareExchange(
        (LONG *)&session->ipc->header.writer_sequence, 0, 0) + 1;
    DWORD start = GetTickCount();
    memset(stats, 0, sizeof(*stats));
    if (!frame) return 0;
    while (stats->frames < count &&
           (DWORD)(GetTickCount() - start) < timeout_ms) {
        session->ipc->header.viewer_heartbeat_ms = GetTickCount();
        uint32_t writer = (uint32_t)InterlockedCompareExchange(
            (LONG *)&session->ipc->header.writer_sequence, 0, 0);
        while (next != writer + 1 && stats->frames < count) {
            if (read_frame(session->ipc, next, frame)) {
                if (!validate_frame(frame)) stats->invalid++;
                stats->body_sum += frame->drawscene_body_ms;
                stats->wall_sum += frame->lua_wall_ms;
                stats->cpu_sum += frame->lua_cpu_ms;
                stats->script_sum += frame->lua_script_self_ms;
                stats->c_sum += frame->lua_c_self_ms;
                if (frame->drawscene_body_ms > stats->body_max)
                    stats->body_max = frame->drawscene_body_ms;
                if (frame->lua_wall_ms > stats->wall_max)
                    stats->wall_max = frame->lua_wall_ms;
                stats->frames++;
            }
            next++;
        }
        Sleep(2);
    }
    free(frame);
    return stats->frames == count && stats->invalid == 0;
}

static int run_lua_and_wait(CocosDebugInjectionSession *session,
                            const char *script) {
    char error[256];
    LONG result_before = InterlockedCompareExchange(
        (LONG *)&session->ipc->remote_lua.result_seq, 0, 0);
    DWORD start;
    if (!cocos_debug_run_lua_string(session, script, error, sizeof(error))) {
        fprintf(stderr, "[FAIL] run Lua send: %s\n", error);
        return 0;
    }
    start = GetTickCount();
    while ((DWORD)(GetTickCount() - start) < 10000) {
        session->ipc->header.viewer_heartbeat_ms = GetTickCount();
        LONG result = InterlockedCompareExchange(
            (LONG *)&session->ipc->remote_lua.result_seq, 0, 0);
        if (result != result_before) {
            if (session->ipc->remote_lua.result_status != 0) {
                fprintf(stderr, "[FAIL] run Lua: %s\n",
                        session->ipc->remote_lua.result_error);
                return 0;
            }
            return 1;
        }
        Sleep(10);
    }
    fprintf(stderr, "[FAIL] run Lua timeout\n");
    return 0;
}

static void print_stats(const char *name, const ScenarioStats *stats) {
    double n = stats->frames ? (double)stats->frames : 1.0;
    printf("[%s] frames=%d invalid=%d avg body=%.3f wall=%.3f "
           "cpu=%.3f Lua-self=%.3f C-self=%.3f max body=%.3f wall=%.3f\n",
           name, stats->frames, stats->invalid,
           stats->body_sum / n, stats->wall_sum / n,
           stats->cpu_sum / n, stats->script_sum / n,
           stats->c_sum / n, stats->body_max, stats->wall_max);
}

int main(int argc, char **argv) {
    static const char mixed_script[] =
        "do local sch=cc.Director:getInstance():getScheduler() "
        "local left=45 local id "
        "id=sch:scheduleScriptFunc(function() "
        "local x=0 for i=1,500 do x=x+math.sin(i) end "
        "left=left-1 if left<=0 then sch:unscheduleScriptEntry(id) end "
        "end,0,false) end";
    static const char coroutine_script[] =
        "do local sch=cc.Director:getInstance():getScheduler() "
        "local workers={} "
        "for i=1,40 do workers[i]=coroutine.create(function() "
        "for step=1,45 do local x=0 for j=1,80 do x=x+j end "
        "coroutine.yield() end end) end "
        "local left=45 local id "
        "id=sch:scheduleScriptFunc(function() "
        "for i=1,#workers do if coroutine.status(workers[i])~='dead' "
        "then assert(coroutine.resume(workers[i])) end end "
        "left=left-1 if left<=0 then sch:unscheduleScriptEntry(id) end "
        "end,0,false) end";
    CocosDebugInjectionSession session;
    ScenarioStats baseline;
    ScenarioStats mixed;
    ScenarioStats coroutines;
    char error[512];
    DWORD pid;
    int ok = 1;
    if (argc != 2) {
        fprintf(stderr, "usage: runtime_ab_test.exe <pid>\n");
        return 2;
    }
    pid = (DWORD)strtoul(argv[1], NULL, 10);
    memset(&session, 0, sizeof(session));
    if (!cocos_debug_inject_process(pid, progress, NULL, &session,
                             error, sizeof(error))) {
        fprintf(stderr, "[FAIL] inject: %s\n", error);
        return 1;
    }
    if (session.ipc->header.state != COCOS_DEBUG_IPC_STATE_READY &&
        session.ipc->header.state != COCOS_DEBUG_IPC_STATE_STOPPED &&
        !wait_state(&session, COCOS_DEBUG_IPC_STATE_READY, 20000)) {
        cocos_debug_close_session(&session);
        return 1;
    }
    session.ipc->header.viewer_heartbeat_ms = GetTickCount();
    cocos_debug_send_command(&session, COCOS_DEBUG_IPC_CMD_START);
    if (!wait_state(&session, COCOS_DEBUG_IPC_STATE_RECORDING, 10000)) {
        fprintf(stderr, "[FAIL] recording did not start\n");
        cocos_debug_close_session(&session);
        return 1;
    }
    ok &= collect_frames(&session, 60, 30000, &baseline);
    print_stats("baseline", &baseline);

    ok &= run_lua_and_wait(&session, mixed_script);
    ok &= collect_frames(&session, 60, 30000, &mixed);
    print_stats("Lua-C-mixed", &mixed);

    ok &= run_lua_and_wait(&session, coroutine_script);
    ok &= collect_frames(&session, 60, 30000, &coroutines);
    print_stats("coroutines", &coroutines);

    if (mixed.c_sum <= baseline.c_sum)
        fprintf(stderr, "[FAIL] mixed scenario did not raise C self\n"), ok = 0;
    if (coroutines.wall_sum <= baseline.wall_sum)
        fprintf(stderr, "[FAIL] coroutine scenario did not raise Lua wall\n"), ok = 0;
    cocos_debug_send_command(&session, COCOS_DEBUG_IPC_CMD_STOP);
    wait_state(&session, COCOS_DEBUG_IPC_STATE_STOPPED, 10000);
    cocos_debug_close_session(&session);
    if (!ok) return 1;
    puts("[OK] live-target A/B invariants");
    return 0;
}
