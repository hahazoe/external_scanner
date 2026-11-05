#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <string.h>

#include "../common/profiler_ipc.h"
#include "../injection/telemetry_writer.h"

#define CHECK(condition, message) \
    do { if (!(condition)) { fprintf(stderr, "[FAIL] %s\n", message); return 1; } } while (0)

int main(void) {
    char name[64];
    DWORD pid = GetCurrentProcessId();
    HANDLE mapping;
    CocosDebugIpcBlock *ipc;
    CocosDebugIpcFrame *frame;
    CocosDebugIpcLuaEvent event;
    CocosDebugIpcLuaEvent *published_event;
    uint32_t first_method, same_method, second_method;
    int command_sequence = 0;

    CHECK(sizeof(CocosDebugIpcBlock) < 48u * 1024u * 1024u, "IPC mapping unexpectedly large");
    CHECK(COCOS_DEBUG_IPC_VERSION == 12u, "unexpected IPC version");
    CHECK(sizeof(CocosDebugIpcMethodSample) == 40u, "method layout");
    CHECK(sizeof(CocosDebugIpcLuaEvent) == 40u, "event layout");
    /* Lines carry a qpc budget now, so the layout is 24 bytes, not 16. */
    CHECK(sizeof(CocosDebugIpcLineSample) == 24u, "line layout");
    _snprintf(name, sizeof(name), COCOS_DEBUG_IPC_MAPPING_FMT,
              (unsigned)COCOS_DEBUG_IPC_VERSION, (unsigned long)pid);
    mapping = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0,
                                 sizeof(CocosDebugIpcBlock), name);
    CHECK(mapping != NULL, "CreateFileMapping");
    ipc = (CocosDebugIpcBlock *)MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0,
                                        sizeof(CocosDebugIpcBlock));
    CHECK(ipc != NULL, "MapViewOfFile");
    memset(ipc, 0, sizeof(*ipc));
    ipc->header.magic = COCOS_DEBUG_IPC_MAGIC;
    ipc->header.version = COCOS_DEBUG_IPC_VERSION;
    ipc->header.total_size = sizeof(CocosDebugIpcBlock);
    ipc->header.target_pid = pid;
    ipc->header.viewer_heartbeat_ms = GetTickCount();

    CHECK(cocos_debug_telemetry_open(pid), "telemetry writer open");
    first_method = cocos_debug_telemetry_intern_method("foo.lua:10:Lua:update");
    same_method = cocos_debug_telemetry_intern_method("foo.lua:10:Lua:update");
    second_method = cocos_debug_telemetry_intern_method("bar.lua:20:Lua:draw");
    CHECK(first_method != 0, "first dictionary insert");
    CHECK(first_method == same_method, "dictionary de-duplication");
    CHECK(second_method != 0 && second_method != first_method, "second dictionary insert");

    /* Regression: a key longer than the dictionary's fixed key field was
       hashed over its full length while only the first bytes were stored, so
       it could never compare equal to its own entry.  Every call burned a
       fresh slot, and this dictionary is process global and never cleared:
       after 4096 such keys every new method came back as method_id 0 and its
       time silently dropped out of the method table. */
    {
        static char long_key[900];
        uint32_t before_count;
        uint32_t long_a, long_b, long_c;
        size_t i;
        for (i = 0; i + 1 < sizeof(long_key); i++)
            long_key[i] = (char)('a' + (int)(i % 26));
        long_key[sizeof(long_key) - 1] = 0;
        before_count = (uint32_t)ipc->header.dictionary_count;
        long_a = cocos_debug_telemetry_intern_method(long_key);
        CHECK(long_a != 0, "long key insert");
        long_b = cocos_debug_telemetry_intern_method(long_key);
        CHECK(long_a == long_b, "long key de-duplication");
        CHECK((uint32_t)ipc->header.dictionary_count == before_count + 1u,
              "long keys must not leak dictionary slots");
        long_key[0] = 'z';
        before_count = (uint32_t)ipc->header.dictionary_count;
        long_c = cocos_debug_telemetry_intern_method(long_key);
        CHECK(long_c != 0 && long_c != long_a, "long key distinctness");
        CHECK((uint32_t)ipc->header.dictionary_count == before_count + 1u,
              "second long key must take exactly one slot");
    }

    frame = cocos_debug_telemetry_begin_frame(1);
    CHECK(frame != NULL && (frame->sequence & 1), "frame write seqlock");
    frame->lua_frame_index = 42;
    frame->render_frame_sequence = 41;
    frame->method_count = 1;
    frame->methods[0].method_id = first_method;
    frame->methods[0].kind = COCOS_DEBUG_IPC_LUA_METHOD_LUA;
    frame->methods[0].self_ms = 1.25f;
    frame->methods[0].inclusive_ms = 2.5f;
    frame->methods[0].calls_started = 1;
    frame->methods[0].calls_completed = 1;
    cocos_debug_telemetry_end_frame(frame);
    CHECK(!(frame->sequence & 1), "frame publish seqlock");
    CHECK(ipc->header.writer_sequence == 1, "writer sequence");
    CHECK(frame->lua_frame_index == 42 && frame->render_frame_sequence == 41,
          "dual frame indexes");
    CHECK(frame->methods[0].inclusive_ms == 2.5f,
          "precise method fields");

    memset(&event, 0, sizeof(event));
    event.qpc = 1234567;
    event.frame_sequence = 42;
    event.lua_thread_id = 3;
    event.os_thread_id = 9;
    event.method_id = first_method;
    event.depth = 2;
    event.type = COCOS_DEBUG_IPC_LUA_EVENT_CALL;
    cocos_debug_telemetry_emit_lua_event(&event);
    CHECK(ipc->header.event_writer_sequence == 1, "event writer sequence");
    published_event = &ipc->events[0];
    CHECK(!(published_event->sequence & 1), "event publish seqlock");
    CHECK(published_event->event_sequence == 1 &&
          published_event->qpc == event.qpc &&
          published_event->frame_sequence == event.frame_sequence,
          "event payload");
    CHECK((ipc->header.features & COCOS_DEBUG_IPC_FEATURE_PRECISE_LUA) != 0,
          "precise Lua feature");

    ipc->header.command = COCOS_DEBUG_IPC_CMD_START;
    ipc->header.command_sequence = 7;
    CHECK(cocos_debug_telemetry_poll_command(&command_sequence) == COCOS_DEBUG_IPC_CMD_START,
          "command polling");
    CHECK(command_sequence == 7, "command sequence");
    cocos_debug_telemetry_ack_command(command_sequence);
    CHECK(ipc->header.command_ack == 7, "command acknowledgement");
    CHECK(cocos_debug_telemetry_viewer_alive(), "viewer heartbeat");

    cocos_debug_telemetry_close();
    UnmapViewOfFile(ipc);
    CloseHandle(mapping);
    puts("[OK] profiler IPC self-test");
    return 0;
}
