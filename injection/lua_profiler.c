#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "lua_profiler.h"

#define LP_THREAD_HASH_CAP 2048
#define LP_METHOD_META_CAP COCOS_DEBUG_IPC_METHOD_DICT_CAP
#define LP_METHOD_HASH_CAP 8192
#define LP_LOCAL_ACC_CAP 512
#define LP_LOCAL_ACC_HASH 1024
#define LP_LOCAL_INTERVAL_CAP 4096
#define LP_LOCAL_LINE_CAP 64
#define LP_NAME_CACHE_CAP 1024
#define LP_NAME_CACHE_HASH 2048
#define LP_LINE_HASH_CAP 128
#define LP_PCALL_CAP 32

typedef struct LpStackEntry {
    uint32_t method_id;
    uint16_t kind;
    uint16_t flags;
    uint64_t frame_logical_start;
    uint64_t frame_self_qpc;
} LpStackEntry;

typedef struct LpLocalAcc {
    uint32_t method_id;
    uint16_t kind;
    uint16_t flags;
    uint32_t calls_started;
    uint32_t calls_completed;
    uint32_t active_at_end;
    uint64_t self_qpc;
    uint64_t inclusive_qpc;
    uint64_t max_self_qpc;
    uint64_t max_inclusive_qpc;
} LpLocalAcc;

typedef struct LpLineAcc {
    uint32_t source_id;
    uint32_t function_id;
    int32_t line;
    uint32_t samples;
    /* Wall time spent on this line: every slice accounted while this entry
       was the thread's open line.  This is what turns the line view from an
       instruction-count histogram into a time profile, and it is the only
       way a line whose whole cost is a C call (instruction counter frozen)
       ever shows up. */
    uint64_t qpc;
} LpLineAcc;

typedef struct LpInterval {
    uint64_t begin;
    uint64_t end;
} LpInterval;

typedef struct LpThreadStorage {
    LpLocalAcc accs[LP_LOCAL_ACC_CAP];
    LpInterval intervals[LP_LOCAL_INTERVAL_CAP];
    LpLineAcc lines[LP_LOCAL_LINE_CAP];
    uint16_t line_hash[LP_LINE_HASH_CAP];
    uint16_t acc_hash[LP_LOCAL_ACC_HASH]; /* method_id -> acc index + 1 */
} LpThreadStorage;

typedef struct LpPcallBase {
    uint32_t token;
    int32_t stack_top;
    int32_t overflow_depth;
    int32_t c_api_depth;
} LpPcallBase;

typedef struct LpThread {
    lua_State *L;
    uint32_t id;
    DWORD owner_os_thread;
    int running;
    int resume_depth;
    uint32_t suspended_parent_id;
    /* Set while this thread has yielded the OS thread to a lua_resume child
       so empty-stack C-API depth does not keep attributing the child's time
       to the parent. */
    int paused;
    int stack_top;
    int overflow_depth;
    uint64_t logical_qpc;
    uint64_t last_account_qpc;
    uint64_t busy_start_qpc;
    int busy_open;
    uint64_t script_qpc;
    uint64_t c_qpc;
    /* Nesting of lua_pcall / lua_call / lua_resume on this thread.  Empty
       stack with c_api_depth > 0 is VM overhead and counts as C. */
    int c_api_depth;
    /* Lua time spent while no frame window was open (between frame_end and
       the next frame_begin).  Kept out of script_qpc/c_qpc and out of every
       method accumulator on purpose: the window owns those, and mixing the
       two would break sum(method self) == lua_cpu == script + c. */
    uint64_t outside_qpc;
    uint64_t outside_last_qpc;
    /* Thread's currently executing line (index into storage->lines + 1);
       0 when line capture is off or nothing has been sampled yet. */
    uint32_t open_line;
    uint32_t hook_events;
    uint32_t acc_count;
    uint32_t interval_count;
    uint32_t line_count;
    int pcall_depth;
    LpPcallBase pcall_base[LP_PCALL_CAP];
    CRITICAL_SECTION lock;
    int lock_ready;
    LpThreadStorage *storage;
} LpThread;

typedef struct LpMethodMeta {
    uint32_t hash;
    uint32_t method_id;
    uint16_t kind;
    uint16_t flags;
    char key[COCOS_DEBUG_IPC_METHOD_KEY];
} LpMethodMeta;

typedef struct LpNameCache {
    const void *source;
    const void *name;
    int linedefined;
    char what0;
    uint32_t method_id;
    uint16_t kind;
    uint16_t flags;
    /* Content fingerprints: a recycled TString address must not produce a
       false hit after the old string is garbage-collected. */
    char source_head[16];
    char name_head[16];
} LpNameCache;

typedef struct LpFrameState {
    int active;
    uint64_t sequence;
    uint64_t begin_qpc;
    volatile LONG hook_events;
    volatile LONG flags;
} LpFrameState;

static CocosDebugLuaProfilerApi g_api;
static LARGE_INTEGER g_qpc_frequency;
static CRITICAL_SECTION g_lock;
static int g_initialized;
static int g_recording;
static int g_line_capture;
static int g_trace_enabled;
static int g_suspend_depth;
static DWORD g_tls_guard = TLS_OUT_OF_INDEXES;
static volatile LONG g_frame_closing;

static LpThread g_threads[COCOS_DEBUG_LUA_PROFILER_THREAD_CAP];
static LpStackEntry g_stacks[COCOS_DEBUG_LUA_PROFILER_THREAD_CAP][COCOS_DEBUG_LUA_PROFILER_STACK_CAP];
static uint16_t g_thread_hash[LP_THREAD_HASH_CAP];
static uint32_t g_thread_count;

static LpMethodMeta g_method_meta[LP_METHOD_META_CAP];
static uint16_t g_method_hash[LP_METHOD_HASH_CAP];
static uint32_t g_method_meta_count;

/* Handed out to each protected call so its leave can prove the depth it
   recorded is still the one on top (the shadow stacks can be wiped wholesale
   between enter and leave, e.g. by a viewer restart command). */
static uint32_t g_pcall_token = 1u;
static uint32_t g_vm_method_id;

static LpNameCache g_name_cache[LP_NAME_CACHE_CAP];
static uint16_t g_name_cache_hash[LP_NAME_CACHE_HASH];
static uint32_t g_name_cache_count;

static LpFrameState g_frame;

/* Outside-window Lua time collected during the gap that led into the frame
   currently being recorded.  frame_begin harvests it, frame_end publishes it:
   the gap after frame N belongs to frame N+1, which is the frame that pays
   for it. */
static uint64_t g_pending_outside_qpc;

/* method_id -> out->methods index + 1, valid during frame_end merge only
   (frame_end holds g_lock). Avoids the O(accs x out_methods) linear merge. */
static uint16_t g_merge_method_slot[COCOS_DEBUG_IPC_METHOD_DICT_CAP + 1];

static uint64_t lp_now(void) {
    LARGE_INTEGER qpc;
    if (g_api.clock_now)
        return g_api.clock_now(g_api.clock_userdata);
    QueryPerformanceCounter(&qpc);
    return (uint64_t)qpc.QuadPart;
}

static void lp_or_flag(uint32_t bit) {
    LONG old;
    LONG neu;
    do {
        old = InterlockedCompareExchange((LONG *)&g_frame.flags, 0, 0);
        neu = old | (LONG)bit;
        if (neu == old) return;
    } while (InterlockedCompareExchange((LONG *)&g_frame.flags, neu, old) != old);
}

static uint32_t lp_hash_bytes(const char *text) {
    const unsigned char *p = (const unsigned char *)(text ? text : "");
    uint32_t hash = 2166136261u;
    while (*p) {
        hash ^= *p++;
        hash *= 16777619u;
    }
    return hash ? hash : 1u;
}

static uint32_t lp_hash_pointer(const void *p) {
    uintptr_t v = (uintptr_t)p;
    v >>= 3;
    v ^= v >> 16;
    return (uint32_t)(v * 2654435761u);
}

static int lp_source_is_internal(const char *source) {
    if (!source) return 0;
    return strstr(source, "cocos_debug_performance") != NULL ||
           strstr(source, "inject.lua") != NULL ||
           strstr(source, "lua_hook.lua") != NULL ||
           strstr(source, "bootstrap.lua") != NULL ||
           strstr(source, "COCOS_DEBUG") != NULL;
}

static void lp_teardown_thread(LpThread *thread) {
    if (!thread) return;
    if (thread->lock_ready) {
        DeleteCriticalSection(&thread->lock);
        thread->lock_ready = 0;
    }
    free(thread->storage);
    thread->storage = NULL;
}

static int lp_setup_thread(LpThread *thread) {
    if (!thread) return 0;
    thread->storage = (LpThreadStorage *)malloc(sizeof(LpThreadStorage));
    if (!thread->storage) return 0;
    memset(thread->storage, 0, sizeof(*thread->storage));
    InitializeCriticalSection(&thread->lock);
    thread->lock_ready = 1;
    return 1;
}

static void lp_reset_all_threads(void) {
    uint32_t i;
    for (i = 0; i < g_thread_count; i++)
        lp_teardown_thread(&g_threads[i]);
    memset(g_threads, 0, sizeof(g_threads));
    memset(g_stacks, 0, sizeof(g_stacks));
    memset(g_thread_hash, 0, sizeof(g_thread_hash));
    g_thread_count = 0;
}

static LpThread *lp_find_thread_locked(lua_State *L) {
    uint32_t start;
    uint32_t i;
    if (!L) return NULL;
    start = lp_hash_pointer(L) % LP_THREAD_HASH_CAP;
    for (i = 0; i < LP_THREAD_HASH_CAP; i++) {
        uint32_t slot = (start + i) % LP_THREAD_HASH_CAP;
        uint16_t value = g_thread_hash[slot];
        if (!value) return NULL;
        if (g_threads[value - 1].L == L) return &g_threads[value - 1];
    }
    return NULL;
}

static LpThread *lp_add_thread_locked(lua_State *L) {
    LpThread *thread;
    uint32_t start;
    uint32_t i;
    if (!L) return NULL;
    thread = lp_find_thread_locked(L);
    if (thread) return thread;
    if (g_thread_count >= COCOS_DEBUG_LUA_PROFILER_THREAD_CAP) {
        lp_or_flag(COCOS_DEBUG_IPC_FRAME_THREAD_LIMITED);
        return NULL;
    }
    thread = &g_threads[g_thread_count];
    memset(thread, 0, sizeof(*thread));
    if (!lp_setup_thread(thread)) {
        lp_or_flag(COCOS_DEBUG_IPC_FRAME_THREAD_LIMITED);
        return NULL;
    }
    thread->L = L;
    thread->id = g_thread_count + 1;
    start = lp_hash_pointer(L) % LP_THREAD_HASH_CAP;
    for (i = 0; i < LP_THREAD_HASH_CAP; i++) {
        uint32_t slot = (start + i) % LP_THREAD_HASH_CAP;
        if (!g_thread_hash[slot]) {
            g_thread_hash[slot] = (uint16_t)(g_thread_count + 1);
            g_thread_count++;
            return thread;
        }
    }
    lp_teardown_thread(thread);
    memset(thread, 0, sizeof(*thread));
    lp_or_flag(COCOS_DEBUG_IPC_FRAME_THREAD_LIMITED);
    return NULL;
}

/* An anonymous C function (lua_pushcclosure of a C function Lua never gave a
   name to -- for iterators, `for ... in` generators, and every tolua-bound
   binding reached through a table) is reported as "<anonymous>" no matter who
   called it.  With 600+ hits a frame that single bucket was the largest C
   cost in the profile and completely unactionable, so its key is qualified
   with the calling method id: the cost lands on the Lua function that drove
   it, which is the one you can actually go edit. */
static int lp_is_anonymous_c(lua_Debug *ar) {
    const char *what = ar && ar->what ? ar->what : "Lua";
    if (strcmp(what, "C") != 0) return 0;
    return !ar->name || !ar->name[0] || strcmp(ar->name, "?") == 0;
}

static void lp_make_method_key(lua_Debug *ar, char *out, size_t cap,
                               uint16_t *kind, uint16_t *flags,
                               uint32_t caller_method_id) {
    const char *source = ar && ar->short_src ? ar->short_src : "?";
    const char *name = ar && ar->name ? ar->name : "<anonymous>";
    const char *what = ar && ar->what ? ar->what : "Lua";
    *flags = lp_source_is_internal(source)
        ? COCOS_DEBUG_IPC_LUA_METHOD_INTERNAL : 0u;
    if (strcmp(what, "C") == 0) {
        *kind = COCOS_DEBUG_IPC_LUA_METHOD_C;
        if (lp_is_anonymous_c(ar) && caller_method_id)
            _snprintf(out, cap, "[C]:<anonymous>@%u", caller_method_id);
        else
            _snprintf(out, cap, "[C]:%s", name);
        return;
    }
    *kind = strcmp(what, "main") == 0
        ? COCOS_DEBUG_IPC_LUA_METHOD_MAIN : COCOS_DEBUG_IPC_LUA_METHOD_LUA;
    if (source[0] == '@') source++;
    _snprintf(out, cap, "%s:%d:%s:%s",
              source, ar ? ar->linedefined : 0, what, name);
}

static void lp_copy_head(char *dst, size_t cap, const char *src) {
    size_t i = 0;
    if (!dst || cap == 0) return;
    if (src) {
        for (; i + 1 < cap && src[i]; i++)
            dst[i] = src[i];
    }
    dst[i] = 0;
}

static int lp_name_cache_lookup(lua_Debug *ar, uint32_t *method_id,
                                uint16_t *kind, uint16_t *flags) {
    uint32_t hash;
    uint32_t start;
    uint32_t i;
    const void *source;
    const void *name;
    char what0;
    if (!ar || !method_id) return 0;
    source = ar->source;
    name = ar->name;
    what0 = ar->what && ar->what[0] ? ar->what[0] : 0;
    hash = lp_hash_pointer(source) ^ lp_hash_pointer(name) ^
           (uint32_t)ar->linedefined ^ ((uint32_t)(unsigned char)what0 << 16);
    start = hash % LP_NAME_CACHE_HASH;
    for (i = 0; i < LP_NAME_CACHE_HASH; i++) {
        uint32_t slot = (start + i) % LP_NAME_CACHE_HASH;
        uint16_t value = g_name_cache_hash[slot];
        LpNameCache *entry;
        if (!value) return 0;
        entry = &g_name_cache[value - 1];
        if (entry->source == source && entry->name == name &&
            entry->linedefined == ar->linedefined && entry->what0 == what0) {
            char source_head[16];
            char name_head[16];
            lp_copy_head(source_head, sizeof(source_head), (const char *)source);
            lp_copy_head(name_head, sizeof(name_head), (const char *)name);
            if (strcmp(entry->source_head, source_head) != 0 ||
                strcmp(entry->name_head, name_head) != 0)
                continue;
            *method_id = entry->method_id;
            *kind = entry->kind;
            *flags = entry->flags;
            return 1;
        }
    }
    return 0;
}

static void lp_name_cache_store(lua_Debug *ar, uint32_t method_id,
                                uint16_t kind, uint16_t flags) {
    uint32_t hash;
    uint32_t start;
    uint32_t i;
    if (!ar || !method_id || g_name_cache_count >= LP_NAME_CACHE_CAP) return;
    hash = lp_hash_pointer(ar->source) ^ lp_hash_pointer(ar->name) ^
           (uint32_t)ar->linedefined ^
           ((uint32_t)(unsigned char)(ar->what && ar->what[0] ? ar->what[0] : 0) << 16);
    start = hash % LP_NAME_CACHE_HASH;
    for (i = 0; i < LP_NAME_CACHE_HASH; i++) {
        uint32_t slot = (start + i) % LP_NAME_CACHE_HASH;
        if (g_name_cache_hash[slot]) continue;
        {
            LpNameCache *entry = &g_name_cache[g_name_cache_count];
            entry->source = ar->source;
            entry->name = ar->name;
            entry->linedefined = ar->linedefined;
            entry->what0 = ar->what && ar->what[0] ? ar->what[0] : 0;
            entry->method_id = method_id;
            entry->kind = kind;
            entry->flags = flags;
            lp_copy_head(entry->source_head, sizeof(entry->source_head),
                         (const char *)ar->source);
            lp_copy_head(entry->name_head, sizeof(entry->name_head),
                         (const char *)ar->name);
            g_name_cache_hash[slot] = (uint16_t)(g_name_cache_count + 1);
            g_name_cache_count++;
        }
        return;
    }
}

static uint32_t lp_method_id_locked(lua_Debug *ar, uint16_t *kind,
                                    uint16_t *flags,
                                    uint32_t caller_method_id) {
    char key[COCOS_DEBUG_IPC_METHOD_KEY];
    uint32_t hash;
    uint32_t start;
    uint32_t i;
    uint32_t cached_id = 0;
    /* The name cache keys on (source, name, linedefined, what) -- it cannot
       tell two anonymous C calls from different call sites apart, and would
       happily hand the first one's id to all of them. */
    int anon_c = lp_is_anonymous_c(ar);
    if (!anon_c && lp_name_cache_lookup(ar, &cached_id, kind, flags))
        return cached_id;
    lp_make_method_key(ar, key, sizeof(key), kind, flags, caller_method_id);
    key[sizeof(key) - 1] = 0;
    hash = lp_hash_bytes(key);
    start = hash % LP_METHOD_HASH_CAP;
    for (i = 0; i < LP_METHOD_HASH_CAP; i++) {
        uint32_t slot = (start + i) % LP_METHOD_HASH_CAP;
        uint16_t value = g_method_hash[slot];
        if (value) {
            LpMethodMeta *meta = &g_method_meta[value - 1];
            if (meta->hash == hash &&
                strncmp(meta->key, key, sizeof(meta->key)) == 0) {
                *kind = meta->kind;
                *flags = meta->flags;
                if (!anon_c)
                    lp_name_cache_store(ar, meta->method_id, meta->kind,
                                        meta->flags);
                return meta->method_id;
            }
            continue;
        }
        if (g_method_meta_count >= LP_METHOD_META_CAP) {
            lp_or_flag(COCOS_DEBUG_IPC_FRAME_METHOD_LIMITED);
            return 0;
        }
        {
            LpMethodMeta *meta = &g_method_meta[g_method_meta_count];
            memset(meta, 0, sizeof(*meta));
            meta->hash = hash;
            meta->kind = *kind;
            meta->flags = *flags;
            strncpy(meta->key, key, sizeof(meta->key) - 1);
            meta->method_id = g_api.intern_method
                ? g_api.intern_method(meta->key) : g_method_meta_count + 1;
            if (!meta->method_id ||
                meta->method_id > COCOS_DEBUG_IPC_METHOD_DICT_CAP) {
                lp_or_flag(COCOS_DEBUG_IPC_FRAME_METHOD_LIMITED);
                return 0;
            }
            g_method_hash[slot] = (uint16_t)(g_method_meta_count + 1);
            g_method_meta_count++;
            if (!anon_c)
                lp_name_cache_store(ar, meta->method_id, meta->kind,
                                    meta->flags);
            return meta->method_id;
        }
    }
    lp_or_flag(COCOS_DEBUG_IPC_FRAME_METHOD_LIMITED);
    return 0;
}

static uint32_t lp_intern_key_locked(const char *key_in, uint16_t kind,
                                     uint16_t flags) {
    char key[COCOS_DEBUG_IPC_METHOD_KEY];
    uint32_t hash;
    uint32_t start;
    uint32_t i;
    if (!key_in || !key_in[0]) return 0;
    strncpy(key, key_in, sizeof(key) - 1);
    key[sizeof(key) - 1] = 0;
    hash = lp_hash_bytes(key);
    start = hash % LP_METHOD_HASH_CAP;
    for (i = 0; i < LP_METHOD_HASH_CAP; i++) {
        uint32_t slot = (start + i) % LP_METHOD_HASH_CAP;
        uint16_t value = g_method_hash[slot];
        if (value) {
            LpMethodMeta *meta = &g_method_meta[value - 1];
            if (meta->hash == hash &&
                strncmp(meta->key, key, sizeof(meta->key)) == 0)
                return meta->method_id;
            continue;
        }
        if (g_method_meta_count >= LP_METHOD_META_CAP) {
            lp_or_flag(COCOS_DEBUG_IPC_FRAME_METHOD_LIMITED);
            return 0;
        }
        {
            LpMethodMeta *meta = &g_method_meta[g_method_meta_count];
            memset(meta, 0, sizeof(*meta));
            meta->hash = hash;
            meta->kind = kind;
            meta->flags = flags;
            strncpy(meta->key, key, sizeof(meta->key) - 1);
            meta->method_id = g_api.intern_method
                ? g_api.intern_method(meta->key) : g_method_meta_count + 1;
            if (!meta->method_id ||
                meta->method_id > COCOS_DEBUG_IPC_METHOD_DICT_CAP) {
                lp_or_flag(COCOS_DEBUG_IPC_FRAME_METHOD_LIMITED);
                return 0;
            }
            g_method_hash[slot] = (uint16_t)(g_method_meta_count + 1);
            g_method_meta_count++;
            return meta->method_id;
        }
    }
    lp_or_flag(COCOS_DEBUG_IPC_FRAME_METHOD_LIMITED);
    return 0;
}

static uint32_t lp_vm_method_id_locked(void) {
    if (g_vm_method_id) return g_vm_method_id;
    /* Core tests pass method ids explicitly and have no intern callback.
       Keep the VM bucket off that id space. */
    if (g_api.intern_method) {
        g_vm_method_id = lp_intern_key_locked(
            "[C]:<vm>", COCOS_DEBUG_IPC_LUA_METHOD_C, 0);
        if (g_vm_method_id) return g_vm_method_id;
    }
    g_vm_method_id = COCOS_DEBUG_IPC_METHOD_DICT_CAP;
    return g_vm_method_id;
}

static LpLocalAcc *lp_touch_local(LpThread *thread, uint32_t method_id,
                                  uint16_t kind, uint16_t flags) {
    uint32_t start;
    uint32_t i;
    LpLocalAcc *acc;
    if (!thread || !thread->storage || !method_id) return NULL;
    /* method_id is dense (1..dict cap): multiply-shift spreads it evenly. */
    start = (method_id * 2654435761u) >> 22;
    for (i = 0; i < LP_LOCAL_ACC_HASH; i++) {
        uint32_t slot = (start + i) & (LP_LOCAL_ACC_HASH - 1);
        uint16_t value = thread->storage->acc_hash[slot];
        if (!value) break;
        if (thread->storage->accs[value - 1].method_id == method_id)
            return &thread->storage->accs[value - 1];
    }
    if (thread->acc_count >= LP_LOCAL_ACC_CAP) {
        lp_or_flag(COCOS_DEBUG_IPC_FRAME_METHOD_LIMITED);
        return NULL;
    }
    acc = &thread->storage->accs[thread->acc_count++];
    memset(acc, 0, sizeof(*acc));
    acc->method_id = method_id;
    acc->kind = kind;
    acc->flags = flags;
    for (i = 0; i < LP_LOCAL_ACC_HASH; i++) {
        uint32_t slot = (start + i) & (LP_LOCAL_ACC_HASH - 1);
        if (!thread->storage->acc_hash[slot]) {
            thread->storage->acc_hash[slot] = (uint16_t)thread->acc_count;
            break;
        }
    }
    return acc;
}

/* Discard shadow frames Lua has already dropped without emitting RET hooks.
   Frames can only ever pile up on top of the real ones, so truncating to the
   depth recorded at the protected-call boundary restores exact alignment.

   Each slice stays on the method that owned the stack top when it was
   accounted -- including C vs Lua kind -- so script/c totals still match
   the method table after an error unwind. */
static void lp_drop_frames_locked(LpThread *thread, int target_top) {
    int old_top;
    if (!thread) return;
    if (target_top < 0) target_top = 0;
    old_top = thread->stack_top;
    if (target_top >= old_top) return;
    memset(&g_stacks[thread->id - 1][target_top], 0,
           (size_t)(old_top - target_top) * sizeof(LpStackEntry));
    thread->stack_top = target_top;
}

static void lp_rebuild_stack_locked(LpThread *thread) {
    lua_Debug frames[COCOS_DEBUG_LUA_PROFILER_STACK_CAP];
    int count = 0;
    int level;
    if (!thread || !g_api.getstack || !g_api.getinfo ||
        thread->stack_top > 0 || thread->overflow_depth > 0)
        return;
    while (count < COCOS_DEBUG_LUA_PROFILER_STACK_CAP &&
           g_api.getstack(thread->L, count, &frames[count]))
        count++;
    if (count == COCOS_DEBUG_LUA_PROFILER_STACK_CAP) {
        lua_Debug extra;
        if (g_api.getstack(thread->L, count, &extra))
            lp_or_flag(COCOS_DEBUG_IPC_FRAME_METHOD_LIMITED);
    }
    uint32_t caller_method_id = 0;
    for (level = count - 1; level >= 0; level--) {
        uint16_t kind = COCOS_DEBUG_IPC_LUA_METHOD_LUA;
        uint16_t flags = 0;
        uint32_t method_id = 0;
        LpStackEntry *entry;
        if (g_api.getinfo(thread->L, "nS", &frames[level]))
            method_id = lp_method_id_locked(&frames[level], &kind, &flags,
                                            caller_method_id);
        /* Outermost first, so the frame just built is the caller of the next
           one -- which is what an anonymous C entry needs to be split by. */
        caller_method_id = method_id;
        entry = &g_stacks[thread->id - 1][thread->stack_top++];
        memset(entry, 0, sizeof(*entry));
        entry->method_id = method_id;
        entry->kind = kind;
        entry->flags = flags;
        entry->frame_logical_start = thread->logical_qpc;
    }
}

static void lp_append_local_interval(LpThread *thread, uint64_t begin, uint64_t end) {
    LpInterval *interval;
    if (!g_frame.active || !thread || !thread->storage || end <= begin) return;
    if (begin < g_frame.begin_qpc) begin = g_frame.begin_qpc;
    if (end <= begin) return;
    if (thread->interval_count > 0) {
        LpInterval *last = &thread->storage->intervals[thread->interval_count - 1];
        if (begin <= last->end) {
            if (end > last->end) last->end = end;
            return;
        }
    }
    if (thread->interval_count >= LP_LOCAL_INTERVAL_CAP) {
        lp_or_flag(COCOS_DEBUG_IPC_FRAME_INTERVAL_LIMITED);
        return;
    }
    interval = &thread->storage->intervals[thread->interval_count++];
    interval->begin = begin;
    interval->end = end;
}

static LpStackEntry *lp_top_entry(LpThread *thread) {
    if (!thread || thread->stack_top <= 0 ||
        thread->stack_top > COCOS_DEBUG_LUA_PROFILER_STACK_CAP)
        return NULL;
    return &g_stacks[thread->id - 1][thread->stack_top - 1];
}

static int lp_thread_in_lua(const LpThread *thread) {
    return thread && !thread->paused &&
           (thread->running || thread->c_api_depth > 0);
}

static void lp_add_self_locked(LpThread *thread, uint32_t method_id,
                               uint16_t kind, uint16_t flags, uint64_t delta) {
    LpLocalAcc *acc;
    if (!thread || !delta) return;
    if (kind == COCOS_DEBUG_IPC_LUA_METHOD_C)
        thread->c_qpc += delta;
    else
        thread->script_qpc += delta;
    if (!method_id) {
        lp_or_flag(COCOS_DEBUG_IPC_FRAME_METHOD_LIMITED);
        return;
    }
    acc = lp_touch_local(thread, method_id, kind, flags);
    if (acc)
        acc->self_qpc += delta;
}

static void lp_account_until_locked(LpThread *thread, uint64_t now) {
    uint64_t start;
    uint64_t delta;
    LpStackEntry *top;
    if (g_suspend_depth > 0 || !thread) return;
    if (!g_frame.active) {
        if (!lp_thread_in_lua(thread)) return;
        start = thread->outside_last_qpc;
        if (!start) start = now;
        if (now > start) thread->outside_qpc += now - start;
        thread->outside_last_qpc = now;
        return;
    }
    if (!lp_thread_in_lua(thread)) return;
    start = thread->last_account_qpc
        ? thread->last_account_qpc : g_frame.begin_qpc;
    if (now <= start) return;
    delta = now - start;
    thread->logical_qpc += delta;
    if (thread->open_line && thread->storage &&
        thread->open_line <= LP_LOCAL_LINE_CAP)
        thread->storage->lines[thread->open_line - 1].qpc += delta;
    top = lp_top_entry(thread);
    if (top) {
        top->frame_self_qpc += delta;
        lp_add_self_locked(thread, top->method_id, top->kind, top->flags,
                           delta);
        return;
    }
    if (thread->c_api_depth > 0) {
        lp_add_self_locked(thread, lp_vm_method_id_locked(),
                           COCOS_DEBUG_IPC_LUA_METHOD_C, 0, delta);
        return;
    }
    lp_or_flag(COCOS_DEBUG_IPC_FRAME_STACK_DESYNC);
}

static void lp_sync_busy_locked(LpThread *thread, uint64_t now) {
    int should;
    if (!thread) return;
    should = g_frame.active && g_suspend_depth <= 0 &&
             lp_thread_in_lua(thread);
    if (should) {
        if (!thread->busy_open) {
            thread->busy_start_qpc = now;
            thread->busy_open = 1;
        }
        return;
    }
    if (!thread->busy_open) return;
    lp_append_local_interval(thread, thread->busy_start_qpc, now);
    thread->busy_start_qpc = 0;
    thread->busy_open = 0;
}

static void lp_arm_account_locked(LpThread *thread, uint64_t now) {
    if (!thread) return;
    if (!lp_thread_in_lua(thread)) {
        thread->last_account_qpc = 0;
        lp_sync_busy_locked(thread, now);
        return;
    }
    if (g_frame.active && g_suspend_depth <= 0) {
        thread->last_account_qpc = now;
        thread->outside_last_qpc = 0;
    } else {
        thread->last_account_qpc = 0;
        thread->outside_last_qpc = now;
    }
    lp_sync_busy_locked(thread, now);
}

static void lp_emit_locked(uint8_t type, LpThread *thread,
                           uint32_t method_id, int depth, uint64_t qpc) {
    CocosDebugIpcLuaEvent event;
    if (!g_trace_enabled || !g_api.emit_event) return;
    memset(&event, 0, sizeof(event));
    event.qpc = qpc;
    event.frame_sequence = g_frame.active ? g_frame.sequence : 0;
    event.lua_thread_id = thread ? thread->id : 0;
    event.os_thread_id = GetCurrentThreadId();
    event.method_id = method_id;
    event.depth = (int16_t)(depth > 32767 ? 32767 : depth);
    event.type = type;
    g_api.emit_event(&event);
}

static uint32_t lp_line_hash(uint32_t source_id, uint32_t function_id, int32_t line) {
    uint32_t hash = source_id * 16777619u;
    hash ^= function_id;
    hash *= 16777619u;
    hash ^= (uint32_t)line;
    return hash ? hash : 1u;
}

static void lp_sample_line_local(lua_State *L, lua_Debug *ar, LpThread *thread) {
    const char *source;
    uint32_t source_id;
    uint32_t function_id = 0;
    uint32_t start;
    uint32_t i;
    LpStackEntry *top;
    if (!g_line_capture || !thread || !thread->storage || !g_api.getinfo ||
        !g_api.getinfo(L, "Sl", ar))
        return;
    if (!ar->what ||
        (strcmp(ar->what, "Lua") != 0 && strcmp(ar->what, "main") != 0) ||
        ar->currentline <= 0)
        return;
    source = ar->source && ar->source[0] ? ar->source : ar->short_src;
    if (!source || !source[0] || lp_source_is_internal(source)) return;
    /* Intern ar->short_src, never the raw chunk.  ar->source is the whole
       file -- or, for a chunk handed to loadstring, the whole script text --
       and a key longer than the dictionary's fixed key field could not be
       matched against its own stored copy, so interning it burned a fresh
       dictionary slot on *every sample* until the process-global dictionary
       (4096 entries, never cleared) was full and new methods all got
       method_id 0.  short_src is capped at LUA_IDSIZE and still names the
       file well enough for the line view. */
    {
        char source_key[COCOS_DEBUG_IPC_METHOD_KEY];
        _snprintf(source_key, sizeof(source_key), "@src:%s",
                  ar->short_src[0] ? ar->short_src : source);
        source_key[sizeof(source_key) - 1] = 0;
        source_id = g_api.intern_method ? g_api.intern_method(source_key) : 0;
    }
    top = lp_top_entry(thread);
    if (top) function_id = top->method_id;
    start = lp_line_hash(source_id, function_id, ar->currentline) % LP_LINE_HASH_CAP;
    for (i = 0; i < LP_LINE_HASH_CAP; i++) {
        uint32_t slot = (start + i) % LP_LINE_HASH_CAP;
        uint16_t value = thread->storage->line_hash[slot];
        LpLineAcc *line;
        if (!value) break;
        line = &thread->storage->lines[value - 1];
        if (line->source_id == source_id &&
            line->function_id == function_id &&
            line->line == ar->currentline) {
            line->samples++;
            /* From here until the next hook event, wall time belongs to this
               line.  This is what makes a line that spends its whole slice
               inside a C call show up at all: the instruction counter never
               ticks there, but the clock does. */
            thread->open_line = value;
            return;
        }
    }
    if (thread->line_count >= LP_LOCAL_LINE_CAP) {
        lp_or_flag(COCOS_DEBUG_IPC_FRAME_LINE_LIMITED);
        return;
    }
    thread->storage->lines[thread->line_count].source_id = source_id;
    thread->storage->lines[thread->line_count].function_id = function_id;
    thread->storage->lines[thread->line_count].line = ar->currentline;
    thread->storage->lines[thread->line_count].samples = 1;
    thread->storage->lines[thread->line_count].qpc = 0;
    for (i = 0; i < LP_LINE_HASH_CAP; i++) {
        uint32_t slot = (start + i) % LP_LINE_HASH_CAP;
        if (!thread->storage->line_hash[slot]) {
            thread->storage->line_hash[slot] = (uint16_t)(thread->line_count + 1);
            break;
        }
    }
    thread->open_line = thread->line_count + 1;
    thread->line_count++;
    lp_or_flag(COCOS_DEBUG_IPC_FRAME_LINE_CAPTURED);
}

static void lp_profile_hook(lua_State *L, lua_Debug *ar) {
    uint64_t entry_qpc;
    uint64_t exit_qpc;
    DWORD os_thread;
    LpThread *thread;
    LpStackEntry *entry;
    LpLocalAcc *acc;
    uint32_t event_method = 0;
    uint16_t kind = COCOS_DEBUG_IPC_LUA_METHOD_LUA;
    uint16_t flags = 0;
    int was_running;
    int is_call;
    int got_info = 0;

    if (!g_recording || !g_initialized || !ar ||
        g_tls_guard == TLS_OUT_OF_INDEXES)
        return;
    if (TlsGetValue(g_tls_guard)) return;
    TlsSetValue(g_tls_guard, (LPVOID)1);

    is_call = (g_suspend_depth <= 0 && ar->event == LUA_HOOKCALL);
    if (is_call && g_api.getinfo)
        got_info = g_api.getinfo(L, "nS", ar);

    entry_qpc = lp_now();
    os_thread = GetCurrentThreadId();

    EnterCriticalSection(&g_lock);
    thread = lp_find_thread_locked(L);
    if (!thread) thread = lp_add_thread_locked(L);
    if (!thread) {
        LeaveCriticalSection(&g_lock);
        TlsSetValue(g_tls_guard, NULL);
        return;
    }
    if (is_call && got_info) {
        /* Qualify an anonymous C callee with whoever is calling it, so the
           cost lands on the Lua function that drove it. */
        uint32_t caller_method_id = 0;
        LpStackEntry *caller_top = lp_top_entry(thread);
        if (caller_top) caller_method_id = caller_top->method_id;
        event_method = lp_method_id_locked(ar, &kind, &flags, caller_method_id);
    }
    LeaveCriticalSection(&g_lock);

    EnterCriticalSection(&thread->lock);
    if (thread->running && thread->owner_os_thread &&
        thread->owner_os_thread != os_thread)
        lp_or_flag(COCOS_DEBUG_IPC_FRAME_STATE_CONCURRENT);
    thread->owner_os_thread = os_thread;
    was_running = thread->running;
    lp_account_until_locked(thread, entry_qpc);
    if (g_frame.active) {
        thread->hook_events++;
        InterlockedIncrement(&g_frame.hook_events);
    }

    if (is_call) {
        /* A call that finds the thread idle starts a fresh logical run, so
           the clock baseline is reset here -- *before* the new frame latches
           it.  Resetting at the end of the hook (as this used to) left the
           previous run's accumulated logical time sitting in logical_qpc, so
           every root call after the first in a frame stored a stale
           frame_logical_start and its inclusive collapsed to 0 whenever the
           new run was shorter than the stale value. */
        if (!was_running && g_frame.active)
            thread->logical_qpc = 0;
        if (thread->stack_top < COCOS_DEBUG_LUA_PROFILER_STACK_CAP) {
            entry = &g_stacks[thread->id - 1][thread->stack_top++];
            memset(entry, 0, sizeof(*entry));
            entry->method_id = event_method;
            entry->kind = kind;
            entry->flags = flags;
            entry->frame_logical_start = thread->logical_qpc;
            if (g_frame.active) {
                acc = lp_touch_local(thread, event_method, kind, flags);
                if (acc) acc->calls_started++;
            }
        } else {
            thread->overflow_depth++;
            lp_or_flag(COCOS_DEBUG_IPC_FRAME_METHOD_LIMITED);
        }
        thread->running = 1;
        lp_emit_locked(COCOS_DEBUG_IPC_LUA_EVENT_CALL, thread, event_method,
                       thread->stack_top + thread->overflow_depth, entry_qpc);
    } else if (g_suspend_depth <= 0 &&
               (ar->event == LUA_HOOKRET ||
                ar->event == LUA_HOOKTAILRET)) {
        if (thread->overflow_depth > 0) {
            thread->overflow_depth--;
        } else if (thread->stack_top > 0) {
            uint64_t inclusive;
            entry = &g_stacks[thread->id - 1][thread->stack_top - 1];
            event_method = entry->method_id;
            if (g_frame.active) {
                inclusive = thread->logical_qpc >= entry->frame_logical_start
                    ? thread->logical_qpc - entry->frame_logical_start : 0;
                acc = lp_touch_local(thread, entry->method_id,
                                     entry->kind, entry->flags);
                if (acc) {
                    acc->inclusive_qpc += inclusive;
                    if (inclusive > acc->max_inclusive_qpc)
                        acc->max_inclusive_qpc = inclusive;
                    if (entry->frame_self_qpc > acc->max_self_qpc)
                        acc->max_self_qpc = entry->frame_self_qpc;
                    acc->calls_completed++;
                }
            }
            memset(entry, 0, sizeof(*entry));
            thread->stack_top--;
        } else {
            lp_or_flag(COCOS_DEBUG_IPC_FRAME_STACK_DESYNC);
        }
        lp_emit_locked(ar->event == LUA_HOOKTAILRET
                           ? COCOS_DEBUG_IPC_LUA_EVENT_TAIL_RETURN
                           : COCOS_DEBUG_IPC_LUA_EVENT_RETURN,
                       thread, event_method,
                       thread->stack_top + thread->overflow_depth, entry_qpc);
        if (thread->stack_top <= 0 && thread->overflow_depth <= 0)
            thread->running = 0;
    } else if (g_suspend_depth <= 0 &&
               (ar->event == LUA_HOOKCOUNT || ar->event == LUA_HOOKLINE)) {
        lp_sample_line_local(L, ar, thread);
    }

    if (g_line_capture || g_trace_enabled)
        exit_qpc = lp_now();
    else
        exit_qpc = entry_qpc;
    lp_arm_account_locked(thread, exit_qpc);
    LeaveCriticalSection(&thread->lock);
    TlsSetValue(g_tls_guard, NULL);
}

static void lp_install_thread_hook(lua_State *L, int enable) {
    int mask;
    int count;
    if (!L || !g_api.sethook) return;
    if (!enable) {
        g_api.sethook(L, NULL, 0, 0);
        return;
    }
    mask = LUA_MASKCALL | LUA_MASKRET;
    count = 0;
    if (g_line_capture) {
        mask |= LUA_MASKCOUNT;
        count = COCOS_DEBUG_LUA_PROFILER_LINE_COUNT;
    }
    g_api.sethook(L, lp_profile_hook, mask, count);
}

static uint64_t lp_union_thread_intervals(uint64_t frame_end) {
    uint32_t index[COCOS_DEBUG_LUA_PROFILER_THREAD_CAP];
    uint64_t total = 0;
    uint64_t begin = 0;
    uint64_t end = 0;
    uint32_t i;
    if (!g_thread_count) return 0;
    memset(index, 0, g_thread_count * sizeof(uint32_t));
    for (;;) {
        int best = -1;
        uint64_t best_begin = ~(uint64_t)0;
        for (i = 0; i < g_thread_count; i++) {
            LpThread *thread = &g_threads[i];
            LpInterval *interval;
            if (!thread->storage || index[i] >= thread->interval_count)
                continue;
            interval = &thread->storage->intervals[index[i]];
            if (best < 0 || interval->begin < best_begin) {
                best = (int)i;
                best_begin = interval->begin;
            }
        }
        if (best < 0) break;
        {
            LpInterval *current = &g_threads[best].storage->intervals[index[best]];
            uint64_t current_end = current->end > frame_end ? frame_end : current->end;
            index[best]++;
            if (current_end <= current->begin) continue;
            if (!begin) {
                begin = current->begin;
                end = current_end;
            } else if (current->begin <= end) {
                if (current_end > end) end = current_end;
            } else {
                total += end - begin;
                begin = current->begin;
                end = current_end;
            }
        }
    }
    if (begin) total += end - begin;
    return total;
}

static double lp_qpc_to_ms(uint64_t value) {
    if (!g_qpc_frequency.QuadPart) return 0.0;
    return (double)value * 1000.0 / (double)g_qpc_frequency.QuadPart;
}

static void lp_merge_method(CocosDebugLuaProfilerFrame *out, const LpLocalAcc *src) {
    CocosDebugIpcMethodSample *dst;
    if (!out || !src || !src->method_id) return;
    if (src->method_id <= COCOS_DEBUG_IPC_METHOD_DICT_CAP) {
        uint32_t slot = g_merge_method_slot[src->method_id];
        if (slot) {
            dst = &out->methods[slot - 1];
            dst->self_ms += (float)lp_qpc_to_ms(src->self_qpc);
            dst->inclusive_ms += (float)lp_qpc_to_ms(src->inclusive_qpc);
            if ((float)lp_qpc_to_ms(src->max_self_qpc) > dst->max_self_ms)
                dst->max_self_ms = (float)lp_qpc_to_ms(src->max_self_qpc);
            if ((float)lp_qpc_to_ms(src->max_inclusive_qpc) > dst->max_inclusive_ms)
                dst->max_inclusive_ms = (float)lp_qpc_to_ms(src->max_inclusive_qpc);
            dst->calls_started += src->calls_started;
            dst->calls_completed += src->calls_completed;
            dst->active_at_end += src->active_at_end;
            return;
        }
    }
    if (out->method_count >= COCOS_DEBUG_IPC_METHOD_CAP) {
        lp_or_flag(COCOS_DEBUG_IPC_FRAME_METHOD_LIMITED);
        return;
    }
    dst = &out->methods[out->method_count++];
    memset(dst, 0, sizeof(*dst));
    dst->method_id = src->method_id;
    dst->kind = src->kind;
    dst->flags = src->flags;
    dst->self_ms = (float)lp_qpc_to_ms(src->self_qpc);
    dst->inclusive_ms = (float)lp_qpc_to_ms(src->inclusive_qpc);
    dst->max_self_ms = (float)lp_qpc_to_ms(src->max_self_qpc);
    dst->max_inclusive_ms = (float)lp_qpc_to_ms(src->max_inclusive_qpc);
    dst->calls_started = src->calls_started;
    dst->calls_completed = src->calls_completed;
    dst->active_at_end = src->active_at_end;
    if (src->method_id <= COCOS_DEBUG_IPC_METHOD_DICT_CAP)
        g_merge_method_slot[src->method_id] = (uint16_t)out->method_count;
}

static void lp_merge_line(CocosDebugLuaProfilerFrame *out, const LpLineAcc *src) {
    uint32_t i;
    if (!out || !src) return;
    for (i = 0; i < out->line_count; i++) {
        if (out->lines[i].source_id == src->source_id &&
            out->lines[i].function_id == src->function_id &&
            out->lines[i].line == src->line) {
            out->lines[i].samples += src->samples;
            out->lines[i].qpc += src->qpc;
            return;
        }
    }
    if (out->line_count >= COCOS_DEBUG_IPC_LINE_CAP) {
        lp_or_flag(COCOS_DEBUG_IPC_FRAME_LINE_LIMITED);
        return;
    }
    out->lines[out->line_count].source_id = src->source_id;
    out->lines[out->line_count].function_id = src->function_id;
    out->lines[out->line_count].line = src->line;
    out->lines[out->line_count].samples = src->samples;
    out->lines[out->line_count].qpc = src->qpc;
    out->line_count++;
}

int cocos_debug_lua_profiler_init(const CocosDebugLuaProfilerApi *api) {
    if (!api || !api->sethook || !api->getinfo || !api->getstack) return 0;
    if (g_initialized) return 1;
    memset(&g_api, 0, sizeof(g_api));
    g_api = *api;
    if (api->qpc_frequency)
        g_qpc_frequency.QuadPart = (LONGLONG)api->qpc_frequency;
    else
        QueryPerformanceFrequency(&g_qpc_frequency);
    InitializeCriticalSection(&g_lock);
    g_tls_guard = TlsAlloc();
    if (g_tls_guard == TLS_OUT_OF_INDEXES) {
        DeleteCriticalSection(&g_lock);
        memset(&g_api, 0, sizeof(g_api));
        return 0;
    }
    g_initialized = 1;
    return 1;
}

void cocos_debug_lua_profiler_shutdown(void) {
    if (!g_initialized) return;
    cocos_debug_lua_profiler_stop();
    EnterCriticalSection(&g_lock);
    lp_reset_all_threads();
    g_vm_method_id = 0;
    LeaveCriticalSection(&g_lock);
    if (g_tls_guard != TLS_OUT_OF_INDEXES) {
        TlsFree(g_tls_guard);
        g_tls_guard = TLS_OUT_OF_INDEXES;
    }
    DeleteCriticalSection(&g_lock);
    memset(&g_api, 0, sizeof(g_api));
    g_initialized = 0;
}

void cocos_debug_lua_profiler_clear(void) {
    uint32_t i;
    if (!g_initialized) return;
    EnterCriticalSection(&g_lock);
    memset(&g_frame, 0, sizeof(g_frame));
    g_pending_outside_qpc = 0;
    for (i = 0; i < g_thread_count; i++) {
        LpThread *thread = &g_threads[i];
        EnterCriticalSection(&thread->lock);
        thread->logical_qpc = 0;
        thread->last_account_qpc = 0;
        thread->busy_start_qpc = 0;
        thread->busy_open = 0;
        thread->script_qpc = 0;
        thread->c_qpc = 0;
        thread->c_api_depth = 0;
        thread->paused = 0;
        thread->outside_qpc = 0;
        thread->outside_last_qpc = 0;
        thread->open_line = 0;
        thread->hook_events = 0;
        thread->acc_count = 0;
        thread->interval_count = 0;
        thread->line_count = 0;
        if (thread->storage) {
            memset(thread->storage->line_hash, 0, sizeof(thread->storage->line_hash));
            memset(thread->storage->acc_hash, 0, sizeof(thread->storage->acc_hash));
        }
        LeaveCriticalSection(&thread->lock);
    }
    LeaveCriticalSection(&g_lock);
}

void cocos_debug_lua_profiler_start(void) {
    if (!g_initialized || g_recording) return;
    EnterCriticalSection(&g_lock);
    lp_reset_all_threads();
    memset(g_name_cache, 0, sizeof(g_name_cache));
    memset(g_name_cache_hash, 0, sizeof(g_name_cache_hash));
    g_name_cache_count = 0;
    memset(&g_frame, 0, sizeof(g_frame));
    g_pending_outside_qpc = 0;
    g_suspend_depth = 0;
    g_recording = 1;
    lp_vm_method_id_locked();
    LeaveCriticalSection(&g_lock);
}

void cocos_debug_lua_profiler_stop(void) {
    uint32_t i;
    if (!g_initialized || !g_recording) return;
    EnterCriticalSection(&g_lock);
    g_recording = 0;
    g_frame.active = 0;
    for (i = 0; i < g_thread_count; i++)
        lp_install_thread_hook(g_threads[i].L, 0);
    LeaveCriticalSection(&g_lock);
}

void cocos_debug_lua_profiler_register_thread(lua_State *L) {
    LpThread *thread;
    if (!g_initialized || !L) return;
    EnterCriticalSection(&g_lock);
    thread = lp_add_thread_locked(L);
    if (thread && g_recording) {
        EnterCriticalSection(&thread->lock);
        lp_rebuild_stack_locked(thread);
        lp_install_thread_hook(L, 1);
        LeaveCriticalSection(&thread->lock);
    }
    LeaveCriticalSection(&g_lock);
}

void cocos_debug_lua_profiler_resume_enter(lua_State *L) {
    uint64_t trace_qpc;
    uint64_t start_qpc;
    LpThread *thread;
    DWORD os_thread;
    if (!g_initialized || !g_recording || !L) return;
    trace_qpc = lp_now();
    os_thread = GetCurrentThreadId();
    EnterCriticalSection(&g_lock);
    thread = lp_add_thread_locked(L);
    if (thread) {
        uint32_t i;
        LpThread *parent = NULL;
        for (i = 0; i < g_thread_count; i++) {
            LpThread *candidate = &g_threads[i];
            if (candidate != thread && candidate->running &&
                candidate->owner_os_thread == os_thread) {
                parent = candidate;
                break;
            }
        }
        if (parent) {
            EnterCriticalSection(&parent->lock);
            lp_account_until_locked(parent, trace_qpc);
            parent->running = 0;
            parent->paused = 1;
            lp_arm_account_locked(parent, trace_qpc);
            LeaveCriticalSection(&parent->lock);
            thread->suspended_parent_id = parent->id;
        } else {
            thread->suspended_parent_id = 0;
        }
        EnterCriticalSection(&thread->lock);
        lp_rebuild_stack_locked(thread);
        if (thread->running && thread->owner_os_thread &&
            thread->owner_os_thread != os_thread)
            lp_or_flag(COCOS_DEBUG_IPC_FRAME_STATE_CONCURRENT);
        thread->owner_os_thread = os_thread;
        thread->resume_depth++;
        thread->c_api_depth++;
        thread->paused = 0;
        lp_emit_locked(COCOS_DEBUG_IPC_LUA_EVENT_RESUME, thread, 0,
                       thread->stack_top + thread->overflow_depth, trace_qpc);
        start_qpc = lp_now();
        thread->running = 1;
        lp_arm_account_locked(thread, start_qpc);
        LeaveCriticalSection(&thread->lock);
    }
    LeaveCriticalSection(&g_lock);
}

void cocos_debug_lua_profiler_resume_leave(lua_State *L, int status) {
    uint64_t now;
    LpThread *thread;
    if (!g_initialized || !g_recording || !L) return;
    now = lp_now();
    EnterCriticalSection(&g_lock);
    thread = lp_find_thread_locked(L);
    if (thread) {
        LpThread *parent = NULL;
        uint64_t restart_qpc;
        EnterCriticalSection(&thread->lock);
        lp_account_until_locked(thread, now);
        if (thread->resume_depth > 0) thread->resume_depth--;
        if (thread->c_api_depth > 0) thread->c_api_depth--;
        thread->running = 0;
        /* A coroutine that comes back with an error is dead: Lua unwound
           every frame in it without emitting RET hooks, and it can never be
           resumed again.  Clear the leftovers so the thread is not left
           permanently holding a stack it will never pop. */
        if (status != 0 && status != LUA_YIELD) {
            lp_drop_frames_locked(thread, 0);
            thread->overflow_depth = 0;
        }
        lp_emit_locked(COCOS_DEBUG_IPC_LUA_EVENT_SUSPEND, thread, 0,
                       thread->stack_top + thread->overflow_depth, now);
        lp_arm_account_locked(thread, now);
        LeaveCriticalSection(&thread->lock);
        if (thread->suspended_parent_id > 0 &&
            thread->suspended_parent_id <= g_thread_count)
            parent = &g_threads[thread->suspended_parent_id - 1];
        thread->suspended_parent_id = 0;
        if (parent) {
            EnterCriticalSection(&parent->lock);
            restart_qpc = lp_now();
            parent->paused = 0;
            parent->running = 1;
            parent->owner_os_thread = GetCurrentThreadId();
            lp_arm_account_locked(parent, restart_qpc);
            LeaveCriticalSection(&parent->lock);
        }
    }
    LeaveCriticalSection(&g_lock);
}

int cocos_debug_lua_profiler_protected_enter(lua_State *L) {
    LpThread *thread;
    LpPcallBase *base;
    int tracked = 0;
    uint64_t now;
    if (!g_initialized || !g_recording || !L) return 0;
    now = lp_now();
    EnterCriticalSection(&g_lock);
    thread = lp_add_thread_locked(L);
    if (thread) {
        EnterCriticalSection(&thread->lock);
        lp_account_until_locked(thread, now);
        if (thread->pcall_depth < LP_PCALL_CAP) {
            if (++g_pcall_token == 0) g_pcall_token = 1;
            base = &thread->pcall_base[thread->pcall_depth++];
            base->token = g_pcall_token;
            base->stack_top = thread->stack_top;
            base->overflow_depth = thread->overflow_depth;
            base->c_api_depth = thread->c_api_depth;
            thread->c_api_depth++;
            tracked = (int)base->token;
            lp_arm_account_locked(thread, now);
        }
        LeaveCriticalSection(&thread->lock);
    }
    LeaveCriticalSection(&g_lock);
    return tracked;
}

void cocos_debug_lua_profiler_protected_leave(lua_State *L, int tracked,
                                              int status) {
    LpThread *thread;
    LpPcallBase base;
    uint64_t now;
    int failed;
    if (!tracked || !g_initialized || !g_recording || !L) return;
    failed = (status != 0 && status != LUA_YIELD);
    now = lp_now();
    EnterCriticalSection(&g_lock);
    thread = lp_find_thread_locked(L);
    if (!thread || thread->pcall_depth <= 0) {
        LeaveCriticalSection(&g_lock);
        return;
    }
    EnterCriticalSection(&thread->lock);
    base = thread->pcall_base[thread->pcall_depth - 1];
    if (base.token != (uint32_t)tracked) {
        /* Our record is gone: the shadow stacks were wiped under this call,
           so the depth we saved says nothing about the stack we have now.
           Leave the (newer) record alone and report the desync. */
        if (failed) lp_or_flag(COCOS_DEBUG_IPC_FRAME_STACK_DESYNC);
        LeaveCriticalSection(&thread->lock);
        LeaveCriticalSection(&g_lock);
        return;
    }
    thread->pcall_depth--;
    memset(&thread->pcall_base[thread->pcall_depth], 0, sizeof(base));
    lp_account_until_locked(thread, now);
    if (failed) {
        lp_drop_frames_locked(thread, base.stack_top);
        thread->overflow_depth = base.overflow_depth;
        thread->c_api_depth = base.c_api_depth;
        if (thread->stack_top <= 0 && thread->overflow_depth <= 0)
            thread->running = 0;
    } else if (thread->c_api_depth > 0) {
        thread->c_api_depth--;
    }
    lp_arm_account_locked(thread, now);
    LeaveCriticalSection(&thread->lock);
    LeaveCriticalSection(&g_lock);
}

void cocos_debug_lua_profiler_call_enter(lua_State *L) {
    LpThread *thread;
    uint64_t now;
    if (!g_initialized || !g_recording || !L) return;
    now = lp_now();
    EnterCriticalSection(&g_lock);
    thread = lp_add_thread_locked(L);
    if (thread) {
        EnterCriticalSection(&thread->lock);
        lp_account_until_locked(thread, now);
        thread->c_api_depth++;
        lp_arm_account_locked(thread, now);
        LeaveCriticalSection(&thread->lock);
    }
    LeaveCriticalSection(&g_lock);
}

void cocos_debug_lua_profiler_call_leave(lua_State *L) {
    LpThread *thread;
    uint64_t now;
    if (!g_initialized || !g_recording || !L) return;
    now = lp_now();
    EnterCriticalSection(&g_lock);
    thread = lp_find_thread_locked(L);
    if (thread) {
        EnterCriticalSection(&thread->lock);
        lp_account_until_locked(thread, now);
        if (thread->c_api_depth > 0) thread->c_api_depth--;
        lp_arm_account_locked(thread, now);
        LeaveCriticalSection(&thread->lock);
    }
    LeaveCriticalSection(&g_lock);
}

void cocos_debug_lua_profiler_set_line_capture(int enabled) {
    uint32_t i;
    if (!g_initialized) return;
    EnterCriticalSection(&g_lock);
    g_line_capture = enabled ? 1 : 0;
    if (g_recording) {
        for (i = 0; i < g_thread_count; i++)
            lp_install_thread_hook(g_threads[i].L, 1);
    }
    LeaveCriticalSection(&g_lock);
}

void cocos_debug_lua_profiler_set_trace(int enabled) {
    g_trace_enabled = enabled ? 1 : 0;
}

int cocos_debug_lua_profiler_trace_enabled(void) {
    return g_trace_enabled;
}

void cocos_debug_lua_profiler_frame_begin(uint64_t frame_sequence) {
    uint32_t i;
    uint64_t marker_qpc;
    uint64_t begin_qpc;
    uint64_t carried_outside = 0;
    if (!g_initialized || !g_recording) return;
    EnterCriticalSection(&g_lock);
    memset(&g_frame, 0, sizeof(g_frame));
    g_frame.sequence = frame_sequence;
    for (i = 0; i < g_thread_count; i++) {
        LpThread *thread = &g_threads[i];
        int si;
        EnterCriticalSection(&thread->lock);
        /* The gap since the previous frame ended belongs to the frame that is
           starting now -- it is the Lua this frame has to make room for. */
        carried_outside += thread->outside_qpc;
        thread->outside_qpc = 0;
        thread->outside_last_qpc = 0;
        thread->open_line = 0;
        thread->logical_qpc = 0;
        thread->last_account_qpc = 0;
        thread->busy_start_qpc = 0;
        thread->busy_open = 0;
        thread->script_qpc = 0;
        thread->c_qpc = 0;
        thread->hook_events = 0;
        thread->acc_count = 0;
        thread->interval_count = 0;
        thread->line_count = 0;
        if (thread->storage) {
            memset(thread->storage->line_hash, 0, sizeof(thread->storage->line_hash));
            memset(thread->storage->acc_hash, 0, sizeof(thread->storage->acc_hash));
        }
        for (si = 0; si < thread->stack_top; si++) {
            g_stacks[i][si].frame_logical_start = 0;
            g_stacks[i][si].frame_self_qpc = 0;
        }
        LeaveCriticalSection(&thread->lock);
    }
    g_pending_outside_qpc = carried_outside;
    marker_qpc = lp_now();
    g_frame.active = 1;
    lp_emit_locked(COCOS_DEBUG_IPC_LUA_EVENT_FRAME_BEGIN, NULL, 0, 0, marker_qpc);
    begin_qpc = lp_now();
    g_frame.begin_qpc = begin_qpc;
    for (i = 0; i < g_thread_count; i++) {
        LpThread *thread = &g_threads[i];
        EnterCriticalSection(&thread->lock);
        if (lp_thread_in_lua(thread))
            lp_arm_account_locked(thread, begin_qpc);
        LeaveCriticalSection(&thread->lock);
    }
    LeaveCriticalSection(&g_lock);
}

int cocos_debug_lua_profiler_frame_end(CocosDebugLuaProfilerFrame *out) {
    uint64_t end_qpc;
    uint64_t wall_qpc;
    uint64_t script_qpc = 0;
    uint64_t c_qpc = 0;
    uint32_t i;
    if (!out) return 0;
    memset(out, 0, sizeof(*out));
    if (!g_initialized || !g_recording) return 0;
    InterlockedExchange(&g_frame_closing, 1);
    end_qpc = lp_now();
    EnterCriticalSection(&g_lock);
    if (!g_frame.active) {
        LeaveCriticalSection(&g_lock);
        InterlockedExchange(&g_frame_closing, 0);
        return 0;
    }
    for (i = 0; i < g_thread_count; i++) {
        LpThread *thread = &g_threads[i];
        int si;
        EnterCriticalSection(&thread->lock);
        if (lp_thread_in_lua(thread)) {
            lp_account_until_locked(thread, end_qpc);
            /* Still in Lua as the window closes: hand the ongoing slice over
               to the outside-window clock so the gap is not lost. */
            thread->outside_last_qpc = end_qpc;
        }
        if (thread->busy_open) {
            lp_append_local_interval(thread, thread->busy_start_qpc, end_qpc);
            thread->busy_start_qpc = 0;
            thread->busy_open = 0;
        }
        for (si = 0; si < thread->stack_top; si++) {
            LpStackEntry *entry = &g_stacks[i][si];
            LpLocalAcc *acc;
            uint64_t inclusive = thread->logical_qpc >= entry->frame_logical_start
                ? thread->logical_qpc - entry->frame_logical_start : 0;
            acc = lp_touch_local(thread, entry->method_id,
                                 entry->kind, entry->flags);
            if (!acc) continue;
            acc->inclusive_qpc += inclusive;
            if (inclusive > acc->max_inclusive_qpc)
                acc->max_inclusive_qpc = inclusive;
            if (entry->frame_self_qpc > acc->max_self_qpc)
                acc->max_self_qpc = entry->frame_self_qpc;
            acc->active_at_end++;
        }
        script_qpc += thread->script_qpc;
        c_qpc += thread->c_qpc;
        thread->last_account_qpc = 0;
        LeaveCriticalSection(&thread->lock);
    }
    lp_emit_locked(COCOS_DEBUG_IPC_LUA_EVENT_FRAME_END, NULL, 0, 0, end_qpc);
    wall_qpc = lp_union_thread_intervals(end_qpc);

    memset(g_merge_method_slot, 0, sizeof(g_merge_method_slot));
    for (i = 0; i < g_thread_count; i++) {
        LpThread *thread = &g_threads[i];
        uint32_t n;
        EnterCriticalSection(&thread->lock);
        for (n = 0; n < thread->acc_count; n++)
            lp_merge_method(out, &thread->storage->accs[n]);
        for (n = 0; n < thread->line_count; n++)
            lp_merge_line(out, &thread->storage->lines[n]);
        LeaveCriticalSection(&thread->lock);
    }

    out->frame_sequence = g_frame.sequence;
    out->begin_qpc = g_frame.begin_qpc;
    out->end_qpc = end_qpc;
    out->body_ms = lp_qpc_to_ms(end_qpc - g_frame.begin_qpc);
    out->lua_wall_ms = lp_qpc_to_ms(wall_qpc);
    out->lua_cpu_ms = lp_qpc_to_ms(script_qpc + c_qpc);
    out->lua_script_self_ms = lp_qpc_to_ms(script_qpc);
    out->lua_c_self_ms = lp_qpc_to_ms(c_qpc);
    out->lua_outside_ms = lp_qpc_to_ms(g_pending_outside_qpc);
    out->hook_events = (uint32_t)g_frame.hook_events;
    out->flags = (uint32_t)g_frame.flags;
    g_frame.active = 0;
    LeaveCriticalSection(&g_lock);
    InterlockedExchange(&g_frame_closing, 0);
    return 1;
}

void cocos_debug_lua_profiler_frame_abort(void) {
    uint32_t i;
    if (!g_initialized) return;
    EnterCriticalSection(&g_lock);
    g_frame.active = 0;
    for (i = 0; i < g_thread_count; i++) {
        EnterCriticalSection(&g_threads[i].lock);
        g_threads[i].last_account_qpc = 0;
        g_threads[i].busy_start_qpc = 0;
        g_threads[i].busy_open = 0;
        LeaveCriticalSection(&g_threads[i].lock);
    }
    LeaveCriticalSection(&g_lock);
}

void cocos_debug_lua_profiler_pause_accounting(void) {
    uint64_t now;
    uint32_t i;
    if (!g_initialized || !g_recording) return;
    now = lp_now();
    EnterCriticalSection(&g_lock);
    if (g_suspend_depth == 0 && g_frame.active) {
        for (i = 0; i < g_thread_count; i++) {
            EnterCriticalSection(&g_threads[i].lock);
            if (lp_thread_in_lua(&g_threads[i]))
                lp_account_until_locked(&g_threads[i], now);
            LeaveCriticalSection(&g_threads[i].lock);
        }
    }
    g_suspend_depth++;
    for (i = 0; i < g_thread_count; i++) {
        EnterCriticalSection(&g_threads[i].lock);
        g_threads[i].last_account_qpc = 0;
        lp_sync_busy_locked(&g_threads[i], now);
        LeaveCriticalSection(&g_threads[i].lock);
    }
    LeaveCriticalSection(&g_lock);
}

void cocos_debug_lua_profiler_resume_accounting(void) {
    uint64_t now;
    uint32_t i;
    if (!g_initialized || !g_recording) return;
    EnterCriticalSection(&g_lock);
    if (g_suspend_depth > 0) g_suspend_depth--;
    if (g_suspend_depth == 0 && g_frame.active) {
        now = lp_now();
        for (i = 0; i < g_thread_count; i++) {
            EnterCriticalSection(&g_threads[i].lock);
            lp_arm_account_locked(&g_threads[i], now);
            LeaveCriticalSection(&g_threads[i].lock);
        }
    }
    LeaveCriticalSection(&g_lock);
}

#ifdef COCOS_DEBUG_LUA_PROFILER_TESTING
void cocos_debug_lua_profiler_test_call(lua_State *L, uint32_t method_id,
                                 uint16_t kind, uint16_t flags) {
    uint64_t now = lp_now();
    LpThread *thread;
    LpStackEntry *entry;
    LpLocalAcc *acc;
    int was_running;
    EnterCriticalSection(&g_lock);
    thread = lp_add_thread_locked(L);
    if (!thread) {
        LeaveCriticalSection(&g_lock);
        return;
    }
    EnterCriticalSection(&thread->lock);
    thread->owner_os_thread = GetCurrentThreadId();
    was_running = thread->running;
    lp_account_until_locked(thread, now);
    if (g_frame.active) {
        thread->hook_events++;
        InterlockedIncrement(&g_frame.hook_events);
    }
    if (!was_running && g_frame.active)
        thread->logical_qpc = 0;
    if (thread->stack_top < COCOS_DEBUG_LUA_PROFILER_STACK_CAP) {
        entry = &g_stacks[thread->id - 1][thread->stack_top++];
        memset(entry, 0, sizeof(*entry));
        entry->method_id = method_id;
        entry->kind = kind;
        entry->flags = flags;
        entry->frame_logical_start = thread->logical_qpc;
        if (g_frame.active) {
            acc = lp_touch_local(thread, method_id, kind, flags);
            if (acc) acc->calls_started++;
        }
    } else {
        thread->overflow_depth++;
        lp_or_flag(COCOS_DEBUG_IPC_FRAME_METHOD_LIMITED);
    }
    thread->running = 1;
    lp_arm_account_locked(thread, now);
    LeaveCriticalSection(&thread->lock);
    LeaveCriticalSection(&g_lock);
}

void cocos_debug_lua_profiler_test_return(lua_State *L, int tail_return) {
    uint64_t now = lp_now();
    LpThread *thread;
    LpStackEntry *entry;
    LpLocalAcc *acc;
    (void)tail_return;
    EnterCriticalSection(&g_lock);
    thread = lp_find_thread_locked(L);
    if (!thread) {
        LeaveCriticalSection(&g_lock);
        return;
    }
    EnterCriticalSection(&thread->lock);
    lp_account_until_locked(thread, now);
    if (g_frame.active) {
        thread->hook_events++;
        InterlockedIncrement(&g_frame.hook_events);
    }
    if (thread->overflow_depth > 0) {
        thread->overflow_depth--;
    } else if (thread->stack_top > 0) {
        uint64_t inclusive;
        entry = &g_stacks[thread->id - 1][thread->stack_top - 1];
        inclusive = thread->logical_qpc >= entry->frame_logical_start
            ? thread->logical_qpc - entry->frame_logical_start : 0;
        if (g_frame.active) {
            acc = lp_touch_local(thread, entry->method_id,
                                 entry->kind, entry->flags);
            if (acc) {
                acc->inclusive_qpc += inclusive;
                if (inclusive > acc->max_inclusive_qpc)
                    acc->max_inclusive_qpc = inclusive;
                if (entry->frame_self_qpc > acc->max_self_qpc)
                    acc->max_self_qpc = entry->frame_self_qpc;
                acc->calls_completed++;
            }
        }
        memset(entry, 0, sizeof(*entry));
        thread->stack_top--;
    } else {
        lp_or_flag(COCOS_DEBUG_IPC_FRAME_STACK_DESYNC);
    }
    if (thread->stack_top <= 0 && thread->overflow_depth <= 0)
        thread->running = 0;
    lp_arm_account_locked(thread, now);
    LeaveCriticalSection(&thread->lock);
    LeaveCriticalSection(&g_lock);
}
#endif
