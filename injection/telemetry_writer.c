#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "telemetry_writer.h"

static HANDLE g_mapping;
static CocosDebugIpcBlock *g_block;

static uint32_t hash_key(const char *key) {
    const unsigned char *p = (const unsigned char *)(key ? key : "");
    uint32_t hash = 2166136261u;
    while (*p) {
        hash ^= *p++;
        hash *= 16777619u;
    }
    return hash ? hash : 1u;
}

int cocos_debug_telemetry_open(DWORD pid) {
    char name[64];
    CocosDebugIpcBlock *block;
    LARGE_INTEGER freq;

    _snprintf(name, sizeof(name), COCOS_DEBUG_IPC_MAPPING_FMT,
              (unsigned)COCOS_DEBUG_IPC_VERSION, (unsigned long)pid);
    g_mapping = OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE, name);
    if (!g_mapping) return 0;
    block = (CocosDebugIpcBlock *)MapViewOfFile(g_mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(CocosDebugIpcBlock));
    if (!block) {
        CloseHandle(g_mapping);
        g_mapping = NULL;
        return 0;
    }
    if (block->header.magic != COCOS_DEBUG_IPC_MAGIC ||
        block->header.version != COCOS_DEBUG_IPC_VERSION ||
        block->header.total_size != sizeof(CocosDebugIpcBlock) ||
        block->header.target_pid != pid) {
        UnmapViewOfFile(block);
        CloseHandle(g_mapping);
        g_mapping = NULL;
        return 0;
    }
    g_block = block;
    QueryPerformanceFrequency(&freq);
    g_block->header.qpc_frequency = (uint64_t)freq.QuadPart;
    g_block->header.features =
        COCOS_DEBUG_IPC_FEATURE_FRAME_METHODS |
        COCOS_DEBUG_IPC_FEATURE_CPP_HOOKS |
        COCOS_DEBUG_IPC_FEATURE_EVENT_STREAM |
        COCOS_DEBUG_IPC_FEATURE_PRECISE_LUA |
        COCOS_DEBUG_IPC_FEATURE_LINE_SAMPLES;
    InterlockedExchange((LONG *)&g_block->header.producer_alive, 1);
    InterlockedExchange((LONG *)&g_block->header.state, COCOS_DEBUG_IPC_STATE_INJECTING);
    return 1;
}

void cocos_debug_telemetry_close(void) {
    if (g_block) {
        InterlockedExchange((LONG *)&g_block->header.producer_alive, 0);
        UnmapViewOfFile(g_block);
        g_block = NULL;
    }
    if (g_mapping) {
        CloseHandle(g_mapping);
        g_mapping = NULL;
    }
}

CocosDebugIpcBlock *cocos_debug_telemetry_block(void) {
    return g_block;
}

const CocosDebugIpcInjectionConfig *cocos_debug_telemetry_config(void) {
    return g_block ? &g_block->config : NULL;
}

void cocos_debug_telemetry_set_state(int state, const char *error) {
    if (!g_block) return;
    if (error) {
        strncpy(g_block->header.error, error, sizeof(g_block->header.error) - 1);
        g_block->header.error[sizeof(g_block->header.error) - 1] = 0;
    } else {
        g_block->header.error[0] = 0;
    }
    MemoryBarrier();
    InterlockedExchange((LONG *)&g_block->header.state, state);
    InterlockedExchange((LONG *)&g_block->header.recording,
        state == COCOS_DEBUG_IPC_STATE_RECORDING ? 1 : 0);
}

int cocos_debug_telemetry_poll_command(int *sequence) {
    LONG current;
    if (!g_block) return COCOS_DEBUG_IPC_CMD_NONE;
    g_block->header.producer_heartbeat_ms = GetTickCount();
    current = InterlockedCompareExchange((LONG *)&g_block->header.command_sequence, 0, 0);
    if (current == InterlockedCompareExchange((LONG *)&g_block->header.command_ack, 0, 0))
        return COCOS_DEBUG_IPC_CMD_NONE;
    if (sequence) *sequence = (int)current;
    return (int)InterlockedCompareExchange((LONG *)&g_block->header.command, 0, 0);
}

void cocos_debug_telemetry_ack_command(int sequence) {
    if (!g_block) return;
    InterlockedExchange((LONG *)&g_block->header.command_ack, (LONG)sequence);
}

int cocos_debug_telemetry_viewer_alive(void) {
    DWORD now, heartbeat;
    if (!g_block) return 0;
    now = GetTickCount();
    heartbeat = g_block->header.viewer_heartbeat_ms;
    return heartbeat != 0 && (DWORD)(now - heartbeat) < 5000u;
}

uint32_t cocos_debug_telemetry_intern_method(const char *key) {
    char bounded[COCOS_DEBUG_IPC_METHOD_KEY];
    const char *name;
    uint32_t hash, start, i;
    if (!g_block || !key || !key[0]) return 0;
    /* Truncate before hashing, not after.  The stored copy is only
       COCOS_DEBUG_IPC_METHOD_KEY bytes, so a longer key hashed over its full
       length could never compare equal to that copy: every call with such a
       key allocated a fresh dictionary slot.  The dictionary is process
       global and never cleared, so 4096 of those and every later method gets
       method_id 0 -- its time then only shows up in the lua_cpu total and
       disappears from the method table.  Line sampling is what produced
       over-long keys, by interning whole chunk sources. */
    strncpy(bounded, key, sizeof(bounded) - 1);
    bounded[sizeof(bounded) - 1] = 0;
    name = bounded;
    hash = hash_key(name);
    start = hash % COCOS_DEBUG_IPC_METHOD_DICT_CAP;
    for (i = 0; i < COCOS_DEBUG_IPC_METHOD_DICT_CAP; i++) {
        uint32_t slot = (start + i) % COCOS_DEBUG_IPC_METHOD_DICT_CAP;
        CocosDebugIpcMethodDictionaryEntry *entry = &g_block->dictionary[slot];
        if (entry->method_id != 0) {
            if (entry->hash == hash &&
                strncmp(entry->key, name, COCOS_DEBUG_IPC_METHOD_KEY) == 0)
                return entry->method_id;
            continue;
        }
        InterlockedIncrement((LONG *)&entry->sequence);
        entry->method_id = slot + 1;
        entry->hash = hash;
        strncpy(entry->key, name, COCOS_DEBUG_IPC_METHOD_KEY - 1);
        entry->key[COCOS_DEBUG_IPC_METHOD_KEY - 1] = 0;
        MemoryBarrier();
        InterlockedIncrement((LONG *)&entry->sequence);
        InterlockedIncrement((LONG *)&g_block->header.dictionary_count);
        return entry->method_id;
    }
    return 0;
}

CocosDebugIpcFrame *cocos_debug_telemetry_begin_frame(uint64_t publish_sequence) {
    CocosDebugIpcFrame *frame;
    size_t clear_offset = offsetof(CocosDebugIpcFrame, reserved0);
    if (!g_block) return NULL;
    frame = &g_block->frames[publish_sequence % COCOS_DEBUG_IPC_FRAME_CAP];
    InterlockedIncrement((LONG *)&frame->sequence);
    /* Skip the 2048 method slots; publisher copies only method_count entries. */
    memset((unsigned char *)frame + clear_offset, 0,
           COCOS_DEBUG_IPC_FRAME_PREFIX_SIZE - clear_offset);
    memset(frame->lines, 0, sizeof(frame->lines));
    frame->publish_sequence = publish_sequence;
    return frame;
}

void cocos_debug_telemetry_end_frame(CocosDebugIpcFrame *frame) {
    if (!g_block || !frame) return;
    MemoryBarrier();
    InterlockedIncrement((LONG *)&frame->sequence);
    MemoryBarrier();
    InterlockedExchange((LONG *)&g_block->header.writer_sequence, (LONG)frame->publish_sequence);
    g_block->header.producer_heartbeat_ms = GetTickCount();
}

void cocos_debug_telemetry_emit_lua_event(const CocosDebugIpcLuaEvent *event) {
    LONG sequence;
    LONG reader;
    CocosDebugIpcLuaEvent *slot;
    size_t clear_offset = offsetof(CocosDebugIpcLuaEvent, qpc);
    if (!g_block || !event) return;
    sequence = InterlockedIncrement((LONG *)&g_block->header.event_writer_sequence);
    reader = InterlockedCompareExchange(
        (LONG *)&g_block->header.event_reader_sequence, 0, 0);
    if ((uint32_t)(sequence - reader) > COCOS_DEBUG_IPC_EVENT_CAP)
        InterlockedIncrement((LONG *)&g_block->header.event_dropped);
    slot = &g_block->events[(uint32_t)(sequence - 1) % COCOS_DEBUG_IPC_EVENT_CAP];
    InterlockedIncrement((LONG *)&slot->sequence);
    memcpy((unsigned char *)slot + clear_offset,
           (const unsigned char *)event + clear_offset,
           sizeof(*slot) - clear_offset);
    slot->event_sequence = (uint32_t)sequence;
    MemoryBarrier();
    InterlockedIncrement((LONG *)&slot->sequence);
}
