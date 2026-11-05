#ifndef COCOS_DEBUG_PROFILER_IPC_H
#define COCOS_DEBUG_PROFILER_IPC_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define COCOS_DEBUG_IPC_MAGIC 0x42444343u /* CCDB */
#define COCOS_DEBUG_IPC_VERSION 12u
#define COCOS_DEBUG_IPC_MAPPING_FMT "Local\\CocosDebugProfiler_v%u_%lu"

#define COCOS_DEBUG_IPC_CPP_CAP 24
#define COCOS_DEBUG_IPC_CPP_NAME 80
#define COCOS_DEBUG_IPC_METHOD_CAP 2048
#define COCOS_DEBUG_IPC_METHOD_DICT_CAP 4096
#define COCOS_DEBUG_IPC_METHOD_KEY 256
#define COCOS_DEBUG_IPC_FRAME_CAP 256
#define COCOS_DEBUG_IPC_LINE_CAP 128
#define COCOS_DEBUG_IPC_EVENT_CAP 32768
#define COCOS_DEBUG_IPC_BOOTSTRAP_CAP 65536
#define COCOS_DEBUG_IPC_REMOTE_LUA_CAP 65536
#define COCOS_DEBUG_IPC_ERROR_CAP 256
#define COCOS_DEBUG_IPC_PATH_CAP 260

enum CocosDebugIpcState {
    COCOS_DEBUG_IPC_STATE_CREATED = 1,
    COCOS_DEBUG_IPC_STATE_INJECTING = 2,
    COCOS_DEBUG_IPC_STATE_READY = 3,
    COCOS_DEBUG_IPC_STATE_RECORDING = 4,
    COCOS_DEBUG_IPC_STATE_STOPPED = 5,
    COCOS_DEBUG_IPC_STATE_ERROR = 6,
    COCOS_DEBUG_IPC_STATE_SHUTDOWN = 7
};

enum CocosDebugIpcCommand {
    COCOS_DEBUG_IPC_CMD_NONE = 0,
    COCOS_DEBUG_IPC_CMD_START = 1,
    COCOS_DEBUG_IPC_CMD_STOP = 2,
    COCOS_DEBUG_IPC_CMD_CLEAR = 3,
    COCOS_DEBUG_IPC_CMD_SHUTDOWN = 4,
    COCOS_DEBUG_IPC_CMD_DETAIL_CAPTURE = 5,
    COCOS_DEBUG_IPC_CMD_RUN_LUA = 6,
    COCOS_DEBUG_IPC_CMD_TRACE_START = 7,
    COCOS_DEBUG_IPC_CMD_TRACE_STOP = 8
};

enum CocosDebugIpcFrameFlags {
    COCOS_DEBUG_IPC_FRAME_EVENT_DROPPED = 1u << 0,
    COCOS_DEBUG_IPC_FRAME_METHOD_LIMITED = 1u << 1,
    COCOS_DEBUG_IPC_FRAME_THREAD_LIMITED = 1u << 2,
    COCOS_DEBUG_IPC_FRAME_LINE_LIMITED = 1u << 3,
    COCOS_DEBUG_IPC_FRAME_LINE_CAPTURED = 1u << 4,
    COCOS_DEBUG_IPC_FRAME_STACK_DESYNC = 1u << 5,
    COCOS_DEBUG_IPC_FRAME_STATE_CONCURRENT = 1u << 6,
    COCOS_DEBUG_IPC_FRAME_INTERVAL_LIMITED = 1u << 7
};

enum CocosDebugIpcFeatures {
    COCOS_DEBUG_IPC_FEATURE_FRAME_METHODS = 1u << 0,
    COCOS_DEBUG_IPC_FEATURE_CPP_HOOKS = 1u << 1,
    COCOS_DEBUG_IPC_FEATURE_EVENT_STREAM = 1u << 2,
    COCOS_DEBUG_IPC_FEATURE_PRECISE_LUA = 1u << 3,
    COCOS_DEBUG_IPC_FEATURE_LINE_SAMPLES = 1u << 4,
    /* CocosDebugIpcLineSample carries a real qpc budget, so the line view is
       wall-time based instead of instruction-count based. */
    COCOS_DEBUG_IPC_FEATURE_LINE_QPC_TIME = 1u << 5,
    /* CocosDebugIpcFrame carries lua_outside_ms: Lua time spent outside the
       drawScene window (previously thrown away entirely). */
    COCOS_DEBUG_IPC_FEATURE_LUA_OUTSIDE_WINDOW = 1u << 6,
    /* lua_call trampoline installed: empty-stack VM slices inside unprotected
       C→Lua entries are classified as C. */
    COCOS_DEBUG_IPC_FEATURE_LUA_CALL_HOOK = 1u << 7
};

enum CocosDebugIpcLuaMethodKind {
    COCOS_DEBUG_IPC_LUA_METHOD_LUA = 1,
    COCOS_DEBUG_IPC_LUA_METHOD_C = 2,
    COCOS_DEBUG_IPC_LUA_METHOD_MAIN = 3
};

enum CocosDebugIpcLuaMethodFlags {
    COCOS_DEBUG_IPC_LUA_METHOD_INTERNAL = 1u << 0
};

enum CocosDebugIpcLuaEventType {
    COCOS_DEBUG_IPC_LUA_EVENT_CALL = 1,
    COCOS_DEBUG_IPC_LUA_EVENT_RETURN = 2,
    COCOS_DEBUG_IPC_LUA_EVENT_TAIL_RETURN = 3,
    COCOS_DEBUG_IPC_LUA_EVENT_RESUME = 4,
    COCOS_DEBUG_IPC_LUA_EVENT_SUSPEND = 5,
    COCOS_DEBUG_IPC_LUA_EVENT_FRAME_BEGIN = 6,
    COCOS_DEBUG_IPC_LUA_EVENT_FRAME_END = 7
};

#pragma pack(push, 8)

typedef struct CocosDebugIpcCppTarget {
    char name[COCOS_DEBUG_IPC_CPP_NAME];
    uint32_t address;
} CocosDebugIpcCppTarget;

typedef struct CocosDebugIpcInjectionConfig {
    uint32_t lua_state;
    uint32_t cocos_base;
    uint32_t cocos_size;
    uint32_t drawscene_address;
    uint32_t getdeltatime_address;
    /* Byte offset of Director::_deltaTime from getDeltaTime disassembly; 0 = unknown. */
    uint32_t director_delta_time_offset;
    uint32_t cpp_count;
    CocosDebugIpcCppTarget cpp_targets[COCOS_DEBUG_IPC_CPP_CAP];
    uint32_t bootstrap_size;
    char bootstrap_lua[COCOS_DEBUG_IPC_BOOTSTRAP_CAP];
} CocosDebugIpcInjectionConfig;

typedef struct CocosDebugIpcMethodDictionaryEntry {
    volatile int32_t sequence;
    uint32_t method_id;
    uint32_t hash;
    char key[COCOS_DEBUG_IPC_METHOD_KEY];
} CocosDebugIpcMethodDictionaryEntry;

typedef struct CocosDebugIpcMethodSample {
    uint32_t method_id;
    uint16_t kind;
    uint16_t flags;
    float self_ms;
    float inclusive_ms;
    float max_self_ms;
    float max_inclusive_ms;
    uint32_t calls_started;
    uint32_t calls_completed;
    uint32_t active_at_end;
    uint32_t reserved;
} CocosDebugIpcMethodSample;

typedef struct CocosDebugIpcCppSample {
    uint32_t target_index;
    float total_ms;
    float max_ms;
    float last_ms;
    uint32_t calls;
    uint32_t call_sequence;
} CocosDebugIpcCppSample;

typedef struct CocosDebugIpcLineSample {
    uint32_t source_id;
    uint32_t function_id;
    int32_t line;
    uint32_t samples;
    /* Wall time attributed to this line: the time from this line's sample
       until the next hook event on the same thread.  Unlike `samples`
       (LUA_MASKCOUNT, one hit per 4000 VM instructions) this also covers
       time burnt inside C calls made from the line, which the instruction
       counter never sees.  0 when line capture was off. */
    uint64_t qpc;
} CocosDebugIpcLineSample;

typedef struct CocosDebugIpcLuaEvent {
    volatile int32_t sequence;
    uint32_t event_sequence;
    uint64_t qpc;
    uint64_t frame_sequence;
    uint32_t lua_thread_id;
    uint32_t os_thread_id;
    uint32_t method_id;
    int16_t depth;
    uint8_t type;
    uint8_t flags;
} CocosDebugIpcLuaEvent;

typedef struct CocosDebugIpcFrame {
    volatile int32_t sequence;
    uint32_t reserved0;
    uint64_t publish_sequence;
    uint64_t frame_begin_qpc;
    uint64_t frame_end_qpc;
    uint32_t lua_frame_index;
    uint32_t render_frame_sequence;
    float frame_period_ms;
    float drawscene_body_ms;
    float lua_wall_ms;
    /* Lua total inside the drawScene window: script + C, by construction. */
    float lua_cpu_ms;
    float lua_script_self_ms;
    float lua_c_self_ms;
    /* Lua wall time spent in the gap since the previous frame ended, i.e.
       outside the drawScene body window.  Not part of lua_cpu_ms and not
       attributed to any method -- it is the Lua the old window discarded. */
    float lua_outside_ms;
    float lua_mem_kb;
    /* Task Manager process-list memory: private working set, in KB.
       proc_private_kb is the private commit. 0 if the sample failed. */
    float proc_ws_kb;
    float proc_private_kb;
    uint32_t hook_events;
    uint32_t flags;
    uint32_t method_count;
    uint32_t cpp_count;
    uint32_t line_count;
    uint32_t reserved1;
    CocosDebugIpcCppSample cpp[COCOS_DEBUG_IPC_CPP_CAP];
    CocosDebugIpcMethodSample methods[COCOS_DEBUG_IPC_METHOD_CAP];
    CocosDebugIpcLineSample lines[COCOS_DEBUG_IPC_LINE_CAP];
    /* Legacy v7 recording fields (always 0). Kept for shared-memory ABI. */
    float actor_x;
    float actor_y;
    uint32_t actor_valid;
} CocosDebugIpcFrame;

typedef struct CocosDebugIpcHeader {
    uint32_t magic;
    uint16_t version;
    uint16_t header_size;
    uint32_t total_size;
    uint32_t target_pid;
    uint32_t features;
    uint64_t qpc_frequency;
    volatile int32_t state;
    volatile int32_t producer_alive;
    volatile int32_t command_sequence;
    volatile int32_t command_ack;
    volatile int32_t command;
    volatile int32_t recording;
    volatile int32_t writer_sequence;
    volatile int32_t dictionary_count;
    volatile int32_t dropped_frames;
    volatile int32_t event_writer_sequence;
    volatile int32_t event_reader_sequence;
    volatile int32_t event_dropped;
    volatile int32_t trace_enabled;
    volatile uint32_t viewer_heartbeat_ms;
    volatile uint32_t producer_heartbeat_ms;
    char error[COCOS_DEBUG_IPC_ERROR_CAP];
} CocosDebugIpcHeader;

typedef struct CocosDebugIpcRemoteLua {
    volatile int32_t request_seq;
    volatile int32_t result_seq;
    volatile int32_t result_status; /* 0=ok, 1=compile, 2=runtime, 3=busy */
    char result_error[COCOS_DEBUG_IPC_ERROR_CAP];
    uint32_t script_size;
    char script[COCOS_DEBUG_IPC_REMOTE_LUA_CAP];
} CocosDebugIpcRemoteLua;

typedef struct CocosDebugIpcBlock {
    CocosDebugIpcHeader header;
    CocosDebugIpcInjectionConfig config;
    CocosDebugIpcMethodDictionaryEntry dictionary[COCOS_DEBUG_IPC_METHOD_DICT_CAP];
    CocosDebugIpcFrame frames[COCOS_DEBUG_IPC_FRAME_CAP];
    CocosDebugIpcLuaEvent events[COCOS_DEBUG_IPC_EVENT_CAP];
    CocosDebugIpcRemoteLua remote_lua;
} CocosDebugIpcBlock;

#pragma pack(pop)

#define COCOS_DEBUG_IPC_FRAME_PREFIX_SIZE (offsetof(CocosDebugIpcFrame, methods))

#ifdef __cplusplus
static_assert(sizeof(CocosDebugIpcMethodSample) == 40, "IPC method layout changed");
static_assert(sizeof(CocosDebugIpcCppSample) == 24, "IPC C++ layout changed");
static_assert(sizeof(CocosDebugIpcLineSample) == 24, "IPC line layout changed");
static_assert(sizeof(CocosDebugIpcLuaEvent) == 40, "IPC event layout changed");
static_assert(sizeof(CocosDebugIpcBlock) < 48u * 1024u * 1024u, "IPC mapping is too large");
#elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
_Static_assert(sizeof(CocosDebugIpcMethodSample) == 40, "IPC method layout changed");
_Static_assert(sizeof(CocosDebugIpcCppSample) == 24, "IPC C++ layout changed");
_Static_assert(sizeof(CocosDebugIpcLineSample) == 24, "IPC line layout changed");
_Static_assert(sizeof(CocosDebugIpcLuaEvent) == 40, "IPC event layout changed");
_Static_assert(sizeof(CocosDebugIpcBlock) < 48u * 1024u * 1024u, "IPC mapping is too large");
#endif

#ifdef __cplusplus
}
#endif

#endif
