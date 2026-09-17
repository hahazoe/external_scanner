#ifndef COCOS_DEBUG_DEBUGVIEW_TELEMETRY_CLIENT_H
#define COCOS_DEBUG_DEBUGVIEW_TELEMETRY_CLIENT_H

#include <stdint.h>
#include <deque>
#include <string>
#include <vector>

#include "../injection/injector.h"

struct LuaMethodSnapshot {
    std::string name;
    uint16_t kind;
    uint16_t flags;
    float self_ms;
    float inclusive_ms;
    float max_self_ms;
    float max_inclusive_ms;
    uint32_t calls_started;
    uint32_t calls_completed;
    uint32_t active_at_end;

    LuaMethodSnapshot()
        : kind(0), flags(0), self_ms(0), inclusive_ms(0),
          max_self_ms(0), max_inclusive_ms(0), calls_started(0),
          calls_completed(0), active_at_end(0) {}
};

struct CppMethodSnapshot {
    std::string name;
    float total_ms;
    float max_ms;
    float last_ms;
    uint32_t calls;
    uint32_t call_sequence;

    CppMethodSnapshot()
        : total_ms(0), max_ms(0), last_ms(0), calls(0), call_sequence(0) {}
};

struct LuaLineSnapshot {
    std::string source;
    std::string function_name;
    int32_t line;
    uint32_t samples;

    LuaLineSnapshot() : line(0), samples(0) {}
};

struct LuaTraceEventSnapshot {
    uint32_t event_sequence;
    uint64_t qpc;
    double time_seconds;
    uint64_t frame_sequence;
    uint32_t lua_thread_id;
    uint32_t os_thread_id;
    uint32_t method_id;
    std::string method_name;
    int16_t depth;
    uint8_t type;
    uint8_t flags;

    LuaTraceEventSnapshot()
        : event_sequence(0), qpc(0), time_seconds(0), frame_sequence(0),
          lua_thread_id(0), os_thread_id(0), method_id(0), depth(0),
          type(0), flags(0) {}
};

struct FrameSnapshot {
    uint64_t publish_sequence;
    double frame_begin_time_seconds;
    double frame_end_time_seconds;
    uint32_t lua_frame_index;
    uint32_t render_frame_sequence;
    float frame_period_ms;
    float drawscene_body_ms;
    float lua_wall_ms;
    float lua_cpu_ms;
    float lua_script_self_ms;
    float lua_c_self_ms;
    float lua_mem_kb;
    uint32_t hook_events;
    uint32_t flags;
    float scene_render_ms;
    bool scene_render_hook_present;
    std::vector<LuaMethodSnapshot> lua_methods;
    std::vector<CppMethodSnapshot> cpp_methods;
    std::vector<LuaLineSnapshot> lua_lines;
    std::vector<LuaTraceEventSnapshot> trace_events;

    FrameSnapshot()
        : publish_sequence(0), frame_begin_time_seconds(0),
          frame_end_time_seconds(0), lua_frame_index(0),
          render_frame_sequence(0), frame_period_ms(0),
          drawscene_body_ms(0), lua_wall_ms(0), lua_cpu_ms(0),
          lua_script_self_ms(0), lua_c_self_ms(0), lua_mem_kb(0),
          hook_events(0), flags(0), scene_render_ms(0),
          scene_render_hook_present(false) {}
};

inline void cocos_debug_fill_scene_render(FrameSnapshot *frame) {
    const CppMethodSnapshot *best_renderer = 0;
    const CppMethodSnapshot *best_scene = 0;
    if (!frame) return;
    frame->scene_render_ms = 0.0f;
    frame->scene_render_hook_present = false;
    for (size_t i = 0; i < frame->cpp_methods.size(); i++) {
        const CppMethodSnapshot &item = frame->cpp_methods[i];
        if (item.name.find("Renderer:render") != std::string::npos) {
            if (!best_renderer || item.calls > best_renderer->calls ||
                item.total_ms > best_renderer->total_ms)
                best_renderer = &item;
        } else if (item.name.find("Scene:render") != std::string::npos ||
                   item.name.find("Scene::render") != std::string::npos) {
            if (!best_scene || item.calls > best_scene->calls ||
                item.total_ms > best_scene->total_ms)
                best_scene = &item;
        }
    }
    if (best_renderer)
        frame->scene_render_hook_present = true;
    if (best_renderer && (best_renderer->calls > 0 || best_renderer->total_ms > 0.02f)) {
        frame->scene_render_ms = best_renderer->total_ms;
        return;
    }
    if (best_scene && (best_scene->calls > 0 || best_scene->total_ms > 0.02f)) {
        frame->scene_render_hook_present = true;
        frame->scene_render_ms = best_scene->total_ms;
        return;
    }
    if (best_scene)
        frame->scene_render_hook_present = true;
}

#include "frame_queue.h"

class TelemetryClient {
public:
    TelemetryClient();
    ~TelemetryClient();

    void attach(CocosDebugInjectionSession *session);
    void detach();
    void startIngest();
    void stopIngest();
    /* UI thread: drain ingest queue into out (does not touch IPC). */
    size_t consumeFrames(std::vector<FrameSnapshot> *out, size_t max_n);
    size_t consumeEvents(std::vector<LuaTraceEventSnapshot> *out, size_t max_n);
    size_t queueSize() const;
    size_t eventQueueSize() const;
    void clearQueue();
    void resetDropped();
    /* Ignore frames/events already sitting in the IPC ring. */
    void skipToLive();
    int state() const;
    bool recording() const;
    const char *error() const;
    uint32_t droppedFrames() const;
    uint32_t droppedEvents() const;

private:
    static DWORD WINAPI ingestThreadMain(LPVOID param);
    void ingestLoop();
    void heartbeat();
    void syncCursorToWriter();
    bool pollIpcOnce();
    bool pollEvents(CocosDebugIpcBlock *ipc);
    std::string methodName(uint32_t method_id);

    CocosDebugInjectionSession *session_;
    CocosDebugIpcFrame *scratch_;
    uint64_t last_sequence_;
    uint32_t local_dropped_;
    uint32_t last_event_sequence_;
    uint32_t local_event_dropped_;
    std::vector<std::string> method_cache_;
    FrameQueue queue_;
    mutable CRITICAL_SECTION event_lock_;
    std::deque<LuaTraceEventSnapshot> event_queue_;
    HANDLE ingest_thread_;
    volatile LONG ingest_stop_;
    volatile LONG ingest_running_;
};

#endif
