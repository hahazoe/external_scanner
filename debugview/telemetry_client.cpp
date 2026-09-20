#include "telemetry_client.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <stddef.h>
#include <stdio.h>
#include <string.h>

TelemetryClient::TelemetryClient()
    : session_(NULL),
      scratch_(new CocosDebugIpcFrame()),
      last_sequence_(0),
      local_dropped_(0),
      last_event_sequence_(0),
      local_event_dropped_(0),
      ingest_thread_(NULL),
      ingest_stop_(0),
      ingest_running_(0) {
    InitializeCriticalSection(&event_lock_);
}

TelemetryClient::~TelemetryClient() {
    stopIngest();
    DeleteCriticalSection(&event_lock_);
    delete scratch_;
}

void TelemetryClient::syncCursorToWriter() {
    if (!session_ || !session_->ipc) {
        last_sequence_ = 0;
        last_event_sequence_ = 0;
        return;
    }
    last_sequence_ = (uint32_t)InterlockedCompareExchange(
        (LONG *)&session_->ipc->header.writer_sequence, 0, 0);
    last_event_sequence_ = (uint32_t)InterlockedCompareExchange(
        (LONG *)&session_->ipc->header.event_writer_sequence, 0, 0);
    InterlockedExchange((LONG *)&session_->ipc->header.event_reader_sequence,
                        (LONG)last_event_sequence_);
}

void TelemetryClient::skipToLive() {
    bool running = ingest_thread_ != NULL;
    if (running)
        stopIngest();
    syncCursorToWriter();
    clearQueue();
    resetDropped();
    if (running) {
        heartbeat();
        startIngest();
    }
}

void TelemetryClient::attach(CocosDebugInjectionSession *session) {
    stopIngest();
    session_ = session;
    method_cache_.clear();
    syncCursorToWriter();
    clearQueue();
    resetDropped();
    heartbeat();
    startIngest();
}

void TelemetryClient::detach() {
    stopIngest();
    session_ = NULL;
    last_sequence_ = 0;
    last_event_sequence_ = 0;
    method_cache_.clear();
    queue_.clear();
    EnterCriticalSection(&event_lock_);
    event_queue_.clear();
    LeaveCriticalSection(&event_lock_);
}

void TelemetryClient::startIngest() {
    if (!session_ || !session_->ipc) return;
    if (InterlockedCompareExchange(&ingest_running_, 1, 0) != 0) return;
    InterlockedExchange(&ingest_stop_, 0);
    ingest_thread_ = CreateThread(NULL, 0, ingestThreadMain, this, 0, NULL);
    if (!ingest_thread_) {
        InterlockedExchange(&ingest_running_, 0);
        return;
    }
}

void TelemetryClient::stopIngest() {
    HANDLE thread;
    InterlockedExchange(&ingest_stop_, 1);
    thread = ingest_thread_;
    ingest_thread_ = NULL;
    if (thread) {
        WaitForSingleObject(thread, 5000);
        CloseHandle(thread);
    }
    InterlockedExchange(&ingest_running_, 0);
}

DWORD WINAPI TelemetryClient::ingestThreadMain(LPVOID param) {
    TelemetryClient *self = (TelemetryClient *)param;
    self->ingestLoop();
    InterlockedExchange(&self->ingest_running_, 0);
    return 0;
}

void TelemetryClient::ingestLoop() {
    while (InterlockedCompareExchange(&ingest_stop_, 0, 0) == 0) {
        bool got = false;
        if (session_ && session_->ipc)
            got = pollIpcOnce();
        if (!got)
            Sleep(1);
        else
            Sleep(0);
    }
}

size_t TelemetryClient::consumeFrames(std::vector<FrameSnapshot> *out, size_t max_n) {
    return queue_.popBatch(out, max_n);
}

size_t TelemetryClient::consumeEvents(std::vector<LuaTraceEventSnapshot> *out,
                                      size_t max_n) {
    size_t n = 0;
    if (!out || max_n == 0) return 0;
    EnterCriticalSection(&event_lock_);
    while (!event_queue_.empty() && n < max_n) {
        out->push_back(event_queue_.front());
        event_queue_.pop_front();
        n++;
    }
    LeaveCriticalSection(&event_lock_);
    return n;
}

size_t TelemetryClient::queueSize() const {
    return queue_.size();
}

size_t TelemetryClient::eventQueueSize() const {
    size_t n;
    EnterCriticalSection(&event_lock_);
    n = event_queue_.size();
    LeaveCriticalSection(&event_lock_);
    return n;
}

void TelemetryClient::clearQueue() {
    queue_.clear();
    EnterCriticalSection(&event_lock_);
    event_queue_.clear();
    LeaveCriticalSection(&event_lock_);
}

void TelemetryClient::resetDropped() {
    local_dropped_ = 0;
    local_event_dropped_ = 0;
    if (session_ && session_->ipc) {
        InterlockedExchange((LONG *)&session_->ipc->header.dropped_frames, 0);
        InterlockedExchange((LONG *)&session_->ipc->header.event_dropped, 0);
    }
}

void TelemetryClient::heartbeat() {
    if (session_ && session_->ipc)
        session_->ipc->header.viewer_heartbeat_ms = GetTickCount();
}

const std::string *TelemetryClient::methodName(uint32_t method_id) {
    char fallback[32];
    CocosDebugIpcMethodDictionaryEntry *entry;
    LONG before, after;
    char key[COCOS_DEBUG_IPC_METHOD_KEY];
    if (!session_ || !session_->ipc || method_id == 0 ||
        method_id > COCOS_DEBUG_IPC_METHOD_DICT_CAP) {
        _snprintf(fallback, sizeof(fallback), "#%u", method_id);
        return cocos_debug_pool_intern(fallback);
    }
    if (method_id <= method_cache_.size() && method_cache_[method_id - 1])
        return method_cache_[method_id - 1];

    entry = &session_->ipc->dictionary[method_id - 1];
    before = InterlockedCompareExchange((LONG *)&entry->sequence, 0, 0);
    if (before & 1) return cocos_debug_pool_intern("<updating>");
    memcpy(key, entry->key, sizeof(key));
    key[sizeof(key) - 1] = 0;
    MemoryBarrier();
    after = InterlockedCompareExchange((LONG *)&entry->sequence, 0, 0);
    if (before != after || (after & 1) || entry->method_id != method_id)
        return cocos_debug_pool_intern("<unavailable>");
    {
        const std::string *pooled = cocos_debug_pool_intern(key);
        if (method_cache_.size() < method_id)
            method_cache_.resize(method_id);
        method_cache_[method_id - 1] = pooled;
        return pooled;
    }
}

bool TelemetryClient::pollIpcOnce() {
    uint64_t writer, first, sequence;
    CocosDebugIpcBlock *ipc;
    bool any = false;
    if (!session_ || !session_->ipc) return false;
    ipc = session_->ipc;
    heartbeat();
    if (pollEvents(ipc)) any = true;
    writer = (uint32_t)InterlockedCompareExchange((LONG *)&ipc->header.writer_sequence, 0, 0);
    if (writer == 0 || writer == last_sequence_) return any;
    if (writer < last_sequence_) last_sequence_ = 0;
    first = last_sequence_ + 1;
    if (writer - first + 1 > COCOS_DEBUG_IPC_FRAME_CAP) {
        uint64_t missed = writer - first + 1 - COCOS_DEBUG_IPC_FRAME_CAP;
        local_dropped_ += (uint32_t)missed;
        first = writer - COCOS_DEBUG_IPC_FRAME_CAP + 1;
    }
    for (sequence = first; sequence <= writer; sequence++) {
        CocosDebugIpcFrame *source = &ipc->frames[sequence % COCOS_DEBUG_IPC_FRAME_CAP];
        LONG before = InterlockedCompareExchange((LONG *)&source->sequence, 0, 0);
        LONG after;
        uint32_t i;
        FrameSnapshot snapshot;
        if (before & 1) {
            local_dropped_++;
            continue;
        }
        memcpy(scratch_, source, COCOS_DEBUG_IPC_FRAME_PREFIX_SIZE);
        {
            uint32_t method_count = scratch_->method_count;
            uint32_t line_count = scratch_->line_count;
            if (method_count > COCOS_DEBUG_IPC_METHOD_CAP)
                method_count = COCOS_DEBUG_IPC_METHOD_CAP;
            if (line_count > COCOS_DEBUG_IPC_LINE_CAP)
                line_count = COCOS_DEBUG_IPC_LINE_CAP;
            if (method_count > 0)
                memcpy(scratch_->methods, source->methods,
                       method_count * sizeof(scratch_->methods[0]));
            if (line_count > 0)
                memcpy(scratch_->lines, source->lines,
                       line_count * sizeof(scratch_->lines[0]));
            scratch_->method_count = method_count;
            scratch_->line_count = line_count;
        }
        MemoryBarrier();
        after = InterlockedCompareExchange((LONG *)&source->sequence, 0, 0);
        if (before != after || (after & 1) || scratch_->publish_sequence != sequence) {
            local_dropped_++;
            continue;
        }
        snapshot.publish_sequence = scratch_->publish_sequence;
        snapshot.frame_begin_time_seconds = ipc->header.qpc_frequency
            ? (double)scratch_->frame_begin_qpc / (double)ipc->header.qpc_frequency : 0;
        snapshot.frame_end_time_seconds = ipc->header.qpc_frequency
            ? (double)scratch_->frame_end_qpc / (double)ipc->header.qpc_frequency : 0;
        snapshot.lua_frame_index = scratch_->lua_frame_index;
        snapshot.render_frame_sequence = scratch_->render_frame_sequence;
        snapshot.frame_period_ms = scratch_->frame_period_ms;
        snapshot.drawscene_body_ms = scratch_->drawscene_body_ms;
        snapshot.lua_wall_ms = scratch_->lua_wall_ms;
        snapshot.lua_cpu_ms = scratch_->lua_cpu_ms;
        snapshot.lua_script_self_ms = scratch_->lua_script_self_ms;
        snapshot.lua_c_self_ms = scratch_->lua_c_self_ms;
        snapshot.lua_outside_ms = scratch_->lua_outside_ms;
        snapshot.lua_mem_kb = scratch_->lua_mem_kb;
        snapshot.hook_events = scratch_->hook_events;
        snapshot.flags = scratch_->flags;
        snapshot.lua_methods.reserve(scratch_->method_count);
        for (i = 0; i < scratch_->method_count && i < COCOS_DEBUG_IPC_METHOD_CAP; i++) {
            const CocosDebugIpcMethodSample &value = scratch_->methods[i];
            LuaMethodSnapshot item;
            item.name = methodName(value.method_id);
            item.method_id = value.method_id;
            item.kind = value.kind;
            item.flags = value.flags;
            item.self_ms = value.self_ms;
            item.inclusive_ms = value.inclusive_ms;
            item.max_self_ms = value.max_self_ms;
            item.max_inclusive_ms = value.max_inclusive_ms;
            item.calls_started = value.calls_started;
            item.calls_completed = value.calls_completed;
            item.active_at_end = value.active_at_end;
            snapshot.lua_methods.push_back(item);
        }
        snapshot.cpp_methods.reserve(scratch_->cpp_count);
        for (i = 0; i < scratch_->cpp_count && i < COCOS_DEBUG_IPC_CPP_CAP; i++) {
            const CocosDebugIpcCppSample &value = scratch_->cpp[i];
            CppMethodSnapshot item;
            if (value.target_index < ipc->config.cpp_count)
                item.name = cocos_debug_pool_intern(
                    ipc->config.cpp_targets[value.target_index].name);
            else
                item.name = cocos_debug_pool_intern("<cpp>");
            item.total_ms = value.total_ms;
            item.max_ms = value.max_ms;
            item.last_ms = value.last_ms;
            item.calls = value.calls;
            item.call_sequence = value.call_sequence;
            snapshot.cpp_methods.push_back(item);
        }
        snapshot.lua_lines.reserve(scratch_->line_count);
        for (i = 0; i < scratch_->line_count && i < COCOS_DEBUG_IPC_LINE_CAP; i++) {
            const CocosDebugIpcLineSample &value = scratch_->lines[i];
            LuaLineSnapshot item;
            item.source = methodName(value.source_id);
            item.function_name = value.function_id
                ? methodName(value.function_id)
                : cocos_debug_pool_intern("");
            item.line = value.line;
            item.samples = value.samples;
            item.line_ms = ipc->header.qpc_frequency
                ? (float)((double)value.qpc * 1000.0 /
                          (double)ipc->header.qpc_frequency)
                : 0.0f;
            snapshot.lua_lines.push_back(item);
        }
        cocos_debug_fill_scene_render(&snapshot);
        queue_.push(snapshot);
        any = true;
    }
    last_sequence_ = writer;
    return any;
}

bool TelemetryClient::pollEvents(CocosDebugIpcBlock *ipc) {
    uint32_t writer;
    uint32_t available;
    uint32_t first;
    uint32_t i;
    bool any = false;
    if (!ipc) return false;
    writer = (uint32_t)InterlockedCompareExchange(
        (LONG *)&ipc->header.event_writer_sequence, 0, 0);
    available = writer - last_event_sequence_;
    if (available == 0) return false;
    if (available > COCOS_DEBUG_IPC_EVENT_CAP) {
        local_event_dropped_ += available - COCOS_DEBUG_IPC_EVENT_CAP;
        available = COCOS_DEBUG_IPC_EVENT_CAP;
    }
    first = writer - available + 1;
    for (i = 0; i < available; i++) {
        uint32_t sequence = first + i;
        CocosDebugIpcLuaEvent *source =
            &ipc->events[(sequence - 1u) % COCOS_DEBUG_IPC_EVENT_CAP];
        CocosDebugIpcLuaEvent copy;
        LONG before = InterlockedCompareExchange((LONG *)&source->sequence, 0, 0);
        LONG after;
        LuaTraceEventSnapshot snapshot;
        if (before & 1) {
            local_event_dropped_++;
            continue;
        }
        memcpy(&copy, source, sizeof(copy));
        MemoryBarrier();
        after = InterlockedCompareExchange((LONG *)&source->sequence, 0, 0);
        if (before != after || (after & 1) || copy.event_sequence != sequence) {
            local_event_dropped_++;
            continue;
        }
        snapshot.event_sequence = copy.event_sequence;
        snapshot.qpc = copy.qpc;
        snapshot.time_seconds = ipc->header.qpc_frequency
            ? (double)copy.qpc / (double)ipc->header.qpc_frequency : 0;
        snapshot.frame_sequence = copy.frame_sequence;
        snapshot.lua_thread_id = copy.lua_thread_id;
        snapshot.os_thread_id = copy.os_thread_id;
        snapshot.method_id = copy.method_id;
        snapshot.method_name = copy.method_id
            ? methodName(copy.method_id)
            : cocos_debug_pool_intern("");
        snapshot.depth = copy.depth;
        snapshot.type = copy.type;
        snapshot.flags = copy.flags;
        EnterCriticalSection(&event_lock_);
        if (event_queue_.size() >= COCOS_DEBUG_IPC_EVENT_CAP) {
            event_queue_.pop_front();
            local_event_dropped_++;
        }
        event_queue_.push_back(snapshot);
        LeaveCriticalSection(&event_lock_);
        any = true;
    }
    last_event_sequence_ = writer;
    InterlockedExchange((LONG *)&ipc->header.event_reader_sequence, (LONG)writer);
    return any;
}

int TelemetryClient::state() const {
    return session_ && session_->ipc ? session_->ipc->header.state : 0;
}

bool TelemetryClient::recording() const {
    return session_ && session_->ipc && session_->ipc->header.recording != 0;
}

const char *TelemetryClient::error() const {
    return session_ && session_->ipc ? session_->ipc->header.error : "";
}

uint32_t TelemetryClient::droppedFrames() const {
    return local_dropped_ + (session_ && session_->ipc
        ? (uint32_t)session_->ipc->header.dropped_frames : 0);
}

uint32_t TelemetryClient::droppedEvents() const {
    uint32_t producer = session_ && session_->ipc
        ? (uint32_t)session_->ipc->header.event_dropped : 0;
    return local_event_dropped_ > producer ? local_event_dropped_ : producer;
}
