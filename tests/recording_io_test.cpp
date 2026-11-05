#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <string.h>

#include <deque>
#include <string>
#include <vector>

#include "../debugview/recording_io.h"

static int g_failures;

#define CHECK(condition, message) \
    do { if (!(condition)) { \
        fprintf(stderr, "[FAIL] %s\n", message); \
        g_failures++; \
    } } while (0)

static void write_raw(FILE *fp, const void *value, size_t size) {
    fwrite(value, 1, size, fp);
}

static void write_u32(FILE *fp, uint32_t value) {
    write_raw(fp, &value, sizeof(value));
}

static void write_u64(FILE *fp, uint64_t value) {
    write_raw(fp, &value, sizeof(value));
}

static void write_f32(FILE *fp, float value) {
    write_raw(fp, &value, sizeof(value));
}

static void write_f64(FILE *fp, double value) {
    write_raw(fp, &value, sizeof(value));
}

static void test_round_trip(const char *path) {
    std::deque<FrameSnapshot> input;
    std::deque<FrameSnapshot> output;
    std::vector<FramePreviewImage> images;
    std::string error;
    FrameSnapshot frame;
    LuaMethodSnapshot method;
    LuaLineSnapshot line;
    LuaTraceEventSnapshot event;
    CppMethodSnapshot cpp;

    frame.publish_sequence = 9;
    frame.frame_begin_time_seconds = 10.25;
    frame.frame_end_time_seconds = 10.275;
    frame.lua_frame_index = 7;
    frame.render_frame_sequence = 8;
    frame.frame_period_ms = 16.67f;
    frame.drawscene_body_ms = 25.0f;
    frame.lua_wall_ms = 12.0f;
    frame.lua_cpu_ms = 13.0f;
    frame.lua_script_self_ms = 8.0f;
    frame.lua_c_self_ms = 5.0f;
    frame.lua_mem_kb = 2048.0f;
    frame.hook_events = 123;
    frame.flags = COCOS_DEBUG_IPC_FRAME_LINE_CAPTURED;
    frame.actor_x = 128.5f;
    frame.actor_y = -64.25f;
    frame.actor_valid = 1;
    frame.proc_ws_kb = 512000.0f;
    frame.proc_private_kb = 480000.0f;

    method.name = cocos_debug_pool_intern("foo.lua:10:Lua:update");
    method.kind = COCOS_DEBUG_IPC_LUA_METHOD_LUA;
    method.self_ms = 4.0f;
    method.inclusive_ms = 9.0f;
    method.max_self_ms = 3.0f;
    method.max_inclusive_ms = 8.0f;
    method.calls_started = 2;
    method.calls_completed = 1;
    method.active_at_end = 1;
    frame.lua_methods.push_back(method);

    cpp.name = cocos_debug_pool_intern("cocos2d::Director::drawScene");
    cpp.total_ms = 25.0f;
    cpp.max_ms = 25.0f;
    cpp.last_ms = 25.0f;
    cpp.calls = 1;
    cpp.call_sequence = 77;
    frame.cpp_methods.push_back(cpp);

    line.source = cocos_debug_pool_intern("foo.lua");
    line.function_name = method.name;
    line.line = 12;
    line.samples = 5;
    frame.lua_lines.push_back(line);

    event.event_sequence = 33;
    event.qpc = 1000;
    event.time_seconds = 10.26;
    event.frame_sequence = 7;
    event.lua_thread_id = 2;
    event.os_thread_id = 3;
    event.method_id = 4;
    event.method_name = method.name;
    event.depth = 2;
    event.type = COCOS_DEBUG_IPC_LUA_EVENT_CALL;
    frame.trace_events.push_back(event);
    input.push_back(frame);

    CHECK(cocos_debug_save_recording(path, input, NULL, &error),
          "save recording");
    CHECK(cocos_debug_load_recording(path, &output, &images, &error),
          "load recording");
    CHECK(output.size() == 1, "frame count");
    if (output.size() == 1) {
        const FrameSnapshot &loaded = output[0];
        CHECK(loaded.publish_sequence == frame.publish_sequence,
              "publish sequence");
        CHECK(loaded.lua_wall_ms == frame.lua_wall_ms,
              "Lua wall");
        CHECK(loaded.lua_cpu_ms == frame.lua_cpu_ms,
              "Lua CPU");
        CHECK(loaded.lua_methods.size() == 1 &&
              loaded.lua_methods[0].inclusive_ms == method.inclusive_ms,
              "method fields");
        CHECK(loaded.cpp_methods.size() == 1 &&
              loaded.cpp_methods[0].call_sequence == cpp.call_sequence,
              "C++ call sequence");
        CHECK(loaded.lua_lines.size() == 1 &&
              loaded.lua_lines[0].samples == line.samples,
              "line samples");
        CHECK(loaded.trace_events.size() == 1 &&
              loaded.trace_events[0].event_sequence == event.event_sequence,
              "trace events");
        CHECK(loaded.actor_valid == 1 &&
              loaded.actor_x == frame.actor_x &&
              loaded.actor_y == frame.actor_y,
              "actor position");
        CHECK(loaded.proc_ws_kb == frame.proc_ws_kb &&
              loaded.proc_private_kb == frame.proc_private_kb,
              "process memory");
    }
}

static void create_old_recording(const char *path, uint32_t version) {
    static const char magic[8] =
        {'C', 'C', 'D', 'B', 'R', 'E', 'C', '1'};
    FILE *fp = fopen(path, "wb");
    write_raw(fp, magic, sizeof(magic));
    write_u32(fp, version);
    write_u64(fp, 0);
    write_u32(fp, 0);
    fclose(fp);
}

static void test_old_version_rejected(const char *path) {
    std::deque<FrameSnapshot> frames;
    std::string error;
    create_old_recording(path, 4);
    CHECK(!cocos_debug_load_recording(path, &frames, NULL, &error),
          "old recording version rejected");
    CHECK(error.find("unsupported recording version") != std::string::npos,
          "old version error message");
}

static void test_v7_process_memory_defaults(const char *path) {
    static const char magic[8] =
        {'C', 'C', 'D', 'B', 'R', 'E', 'C', '1'};
    std::deque<FrameSnapshot> frames;
    std::string error;
    FILE *fp = fopen(path, "wb");
    write_raw(fp, magic, sizeof(magic));
    write_u32(fp, 7);
    write_u64(fp, 1);
    write_u64(fp, 3);
    write_f64(fp, 1.0);
    write_f64(fp, 1.016);
    write_u32(fp, 1);
    write_u32(fp, 1);
    write_f32(fp, 16.0f);
    write_f32(fp, 4.0f);
    write_f32(fp, 2.0f);
    write_f32(fp, 2.0f);
    write_f32(fp, 1.0f);
    write_f32(fp, 1.0f);
    write_f32(fp, 1.0f);
    write_f32(fp, 1024.0f);
    write_u32(fp, 0);
    write_u32(fp, 0);
    write_f32(fp, 10.0f);
    write_f32(fp, 20.0f);
    write_u32(fp, 1);
    write_u32(fp, 0);
    write_u32(fp, 0);
    write_u32(fp, 0);
    write_u32(fp, 0);
    write_u32(fp, 0);
    fclose(fp);
    CHECK(cocos_debug_load_recording(path, &frames, NULL, &error),
          "v7 recording loads");
    CHECK(frames.size() == 1 &&
          frames[0].actor_valid == 1 &&
          frames[0].proc_ws_kb == 0.0f &&
          frames[0].proc_private_kb == 0.0f,
          "v7 process memory defaults to 0");
}

static void test_bad_magic(const char *path) {
    std::deque<FrameSnapshot> frames;
    std::string error;
    FILE *fp = fopen(path, "wb");
    fputs("not a recording", fp);
    fclose(fp);
    CHECK(!cocos_debug_load_recording(path, &frames, NULL, &error),
          "bad magic rejected");
}

int main(void) {
    const char *current = "build\\generated\\recording_current_test.cocosdbg";
    const char *old = "build\\generated\\recording_old_test.cocosdbg";
    const char *v7 = "build\\generated\\recording_v7_test.cocosdbg";
    const char *bad = "build\\generated\\recording_bad_test.cocosdbg";
    test_round_trip(current);
    test_old_version_rejected(old);
    test_v7_process_memory_defaults(v7);
    test_bad_magic(bad);
    DeleteFileA(current);
    DeleteFileA(old);
    DeleteFileA(v7);
    DeleteFileA(bad);
    if (g_failures) {
        fprintf(stderr, "[FAIL] %d recording assertions\n", g_failures);
        return 1;
    }
    puts("[OK] recording format");
    return 0;
}
