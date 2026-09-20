/*
 * Dump every loaded Lua chunk from the target process.
 * - Text source  -> .lua plaintext
 * - Bytecode     -> .luac raw + .txt string/debug names extracted (Lua 5.1 undump layout)
 *
 * Full source decompilation is not available from undump; plaintext here means
 * the string constants / source names / locals that LoadString materializes.
 *
 * loadbuffer chunks are copied on the calling thread; loadfile hooks only
 * record the path and the writer thread performs the disk read, so the
 * game's loading path never pays our I/O cost.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <psapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>

#include "luac_dump.h"
#include "x86_insn.h"
#include "patch_freeze.h"

#define LUA_SIGNATURE_0 0x1B
typedef struct lua_State lua_State;
typedef int (__cdecl *luaL_loadbuffer_fn)(lua_State *, const char *, size_t, const char *);
typedef int (__cdecl *luaL_loadfile_fn)(lua_State *, const char *);

#define DUMP_QUEUE_CAP 256
#define DUMP_QUEUE_BYTES_MAX (64u * 1024u * 1024u)
#define DUMP_NAME_CAP 200
#define DUMP_SEEN_CAP 4096

typedef struct DumpJob {
    unsigned char *buf;
    size_t size;
    char *chunk_name;
    char module_name[DUMP_NAME_CAP];
    int is_bin;
    int from_file; /* buf empty: writer thread reads the file at chunk_name */
} DumpJob;

static char g_dump_dir[MAX_PATH];
static volatile LONG g_dump_seq;
static luaL_loadbuffer_fn g_loadbuffer_orig;
static luaL_loadfile_fn g_loadfile_orig;
static void *g_loadbuffer_tramp;
static void *g_loadfile_tramp;
static uint8_t g_lb_stolen[16];
static uint8_t g_lf_stolen[16];
static int g_lb_stolen_n;
static int g_lf_stolen_n;
static uint8_t *g_lb_target;
static uint8_t *g_lf_target;

static CRITICAL_SECTION g_dump_lock;
static int g_dump_lock_ready;
static HANDLE g_dump_event;
static HANDLE g_dump_thread;
static volatile LONG g_dump_stop;
static DumpJob *g_dump_queue[DUMP_QUEUE_CAP];
static int g_dump_q_head;
static int g_dump_q_tail;
static int g_dump_q_count;
static size_t g_dump_q_bytes;
static char g_dump_seen[DUMP_SEEN_CAP][DUMP_NAME_CAP];
static int g_dump_seen_count;

typedef struct {
    const unsigned char *p;
    const unsigned char *end;
    int ok;
    int little;
    int sz_int;
    int sz_size;
    int sz_ins;
    int sz_num;
    int is_int_num;
    FILE *out;
    int depth;
} LuacReader;

static void r_fail(LuacReader *r) { r->ok = 0; }

static void r_need(LuacReader *r, size_t n) {
    if (!r->ok) return;
    if ((size_t)(r->end - r->p) < n) r_fail(r);
}

static unsigned char r_byte(LuacReader *r) {
    r_need(r, 1);
    if (!r->ok) return 0;
    return *r->p++;
}

static void r_block(LuacReader *r, void *dst, size_t n) {
    r_need(r, n);
    if (!r->ok) return;
    memcpy(dst, r->p, n);
    r->p += n;
}

static uint32_t r_uint_le(const unsigned char *p, int n) {
    uint32_t v = 0;
    int i;
    for (i = 0; i < n; i++) v |= ((uint32_t)p[i]) << (8 * i);
    return v;
}

static uint32_t r_uint_be(const unsigned char *p, int n) {
    uint32_t v = 0;
    int i;
    for (i = 0; i < n; i++) v = (v << 8) | p[i];
    return v;
}

static uint32_t r_uint(LuacReader *r, int n) {
    unsigned char b[8];
    if (n <= 0 || n > 8) { r_fail(r); return 0; }
    r_block(r, b, (size_t)n);
    if (!r->ok) return 0;
    return r->little ? r_uint_le(b, n) : r_uint_be(b, n);
}

static int r_int(LuacReader *r) {
    return (int)r_uint(r, r->sz_int);
}

static size_t r_size(LuacReader *r) {
    return (size_t)r_uint(r, r->sz_size);
}

static void r_emit(LuacReader *r, const char *fmt, ...) {
    va_list ap;
    if (!r->out) return;
    va_start(ap, fmt);
    vfprintf(r->out, fmt, ap);
    va_end(ap);
}

static void r_emit_str(LuacReader *r, const char *s, size_t n) {
    size_t i;
    if (!r->out || !s) return;
    fputc('"', r->out);
    for (i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == '\\' || c == '"') fprintf(r->out, "\\%c", c);
        else if (c >= 32 && c < 127) fputc(c, r->out);
        else if (c == '\n') fputs("\\n", r->out);
        else if (c == '\r') fputs("\\r", r->out);
        else if (c == '\t') fputs("\\t", r->out);
        else fprintf(r->out, "\\x%02X", c);
    }
    fputc('"', r->out);
}

/* Returns malloc'd string without trailing NUL from format; NULL if empty/fail. */
static char *r_load_string(LuacReader *r, size_t *out_len) {
    size_t size;
    char *s;
    size = r_size(r);
    if (!r->ok) return NULL;
    if (size == 0) {
        if (out_len) *out_len = 0;
        return NULL;
    }
    r_need(r, size);
    if (!r->ok) return NULL;
    s = (char *)malloc(size);
    if (!s) { r_fail(r); return NULL; }
    memcpy(s, r->p, size);
    r->p += size;
    /* bytecode stores trailing '\0' in size */
    if (out_len) *out_len = size > 0 ? size - 1 : 0;
    return s;
}

static void r_skip_vector(LuacReader *r, size_t elem, int count) {
    if (count < 0) { r_fail(r); return; }
    r_need(r, elem * (size_t)count);
    if (!r->ok) return;
    r->p += elem * (size_t)count;
}

static void load_function(LuacReader *r);

static void load_constants(LuacReader *r) {
    int i, n;
    n = r_int(r);
    if (!r->ok || n < 0 || n > 1000000) { r_fail(r); return; }
    for (i = 0; i < n; i++) {
        int t = r_byte(r);
        if (!r->ok) return;
        switch (t) {
        case 0: /* nil */ break;
        case 1: /* bool */ r_byte(r); break;
        case 3: /* number */ r_skip_vector(r, (size_t)r->sz_num, 1); break;
        case 4: { /* string */
            size_t len = 0;
            char *s = r_load_string(r, &len);
            if (s) {
                r_emit(r, "  const[%d] = ", i);
                r_emit_str(r, s, len);
                r_emit(r, "\n");
                free(s);
            }
            break;
        }
        default:
            r_fail(r);
            return;
        }
    }
    n = r_int(r);
    if (!r->ok || n < 0 || n > 100000) { r_fail(r); return; }
    for (i = 0; i < n; i++)
        load_function(r);
}

static void load_debug(LuacReader *r) {
    int i, n;
    n = r_int(r); /* lineinfo */
    if (!r->ok || n < 0 || n > 5000000) { r_fail(r); return; }
    r_skip_vector(r, (size_t)r->sz_int, n);
    n = r_int(r); /* locals */
    if (!r->ok || n < 0 || n > 100000) { r_fail(r); return; }
    for (i = 0; i < n; i++) {
        size_t len = 0;
        char *name = r_load_string(r, &len);
        int startpc = r_int(r);
        int endpc = r_int(r);
        if (name) {
            r_emit(r, "  local[%d] = ", i);
            r_emit_str(r, name, len);
            r_emit(r, "  pc=%d..%d\n", startpc, endpc);
            free(name);
        }
        if (!r->ok) return;
    }
    n = r_int(r); /* upvalues */
    if (!r->ok || n < 0 || n > 100000) { r_fail(r); return; }
    for (i = 0; i < n; i++) {
        size_t len = 0;
        char *name = r_load_string(r, &len);
        if (name) {
            r_emit(r, "  upvalue[%d] = ", i);
            r_emit_str(r, name, len);
            r_emit(r, "\n");
            free(name);
        }
        if (!r->ok) return;
    }
}

static void load_function(LuacReader *r) {
    size_t len = 0;
    char *source;
    int linedefined, lastlinedefined;
    int nups, numparams, is_vararg, maxstack;
    int code_n;
    if (!r->ok) return;
    if (++r->depth > 200) { r_fail(r); return; }
    source = r_load_string(r, &len);
    linedefined = r_int(r);
    lastlinedefined = r_int(r);
    nups = r_byte(r);
    numparams = r_byte(r);
    is_vararg = r_byte(r);
    maxstack = r_byte(r);
    if (!r->ok) { free(source); r->depth--; return; }
    r_emit(r, "\nfunction depth=%d lines=%d..%d nups=%d params=%d vararg=%d slots=%d\n",
           r->depth, linedefined, lastlinedefined, nups, numparams, is_vararg, maxstack);
    if (source) {
        r_emit(r, "  source = ");
        r_emit_str(r, source, len);
        r_emit(r, "\n");
        free(source);
    }
    code_n = r_int(r);
    if (!r->ok || code_n < 0 || code_n > 5000000) { r_fail(r); r->depth--; return; }
    r_skip_vector(r, (size_t)r->sz_ins, code_n);
    load_constants(r);
    if (r->ok) load_debug(r);
    r->depth--;
}

static int parse_and_write_plaintext(const unsigned char *buf, size_t size, FILE *out) {
    LuacReader r;
    unsigned char ver, fmt, endian;
    memset(&r, 0, sizeof(r));
    r.p = buf;
    r.end = buf + size;
    r.ok = 1;
    r.out = out;
    if (size < 12 || buf[0] != LUA_SIGNATURE_0) return 0;
    /* \033Lua */
    if (size < 4 || memcmp(buf, "\033Lua", 4) != 0) return 0;
    r.p = buf + 4;
    ver = r_byte(&r);
    fmt = r_byte(&r);
    endian = r_byte(&r);
    r.sz_int = r_byte(&r);
    r.sz_size = r_byte(&r);
    r.sz_ins = r_byte(&r);
    r.sz_num = r_byte(&r);
    r.is_int_num = r_byte(&r);
    r.little = endian != 0;
    if (!r.ok || ver != 0x51) {
        fprintf(out, "# unsupported luac header ver=0x%02X fmt=%u\n", ver, fmt);
        return 0;
    }
    if (r.sz_int < 2 || r.sz_int > 8 || r.sz_size < 2 || r.sz_size > 8 ||
        r.sz_ins < 2 || r.sz_ins > 8 || r.sz_num < 4 || r.sz_num > 8) {
        fprintf(out, "# bad size fields in header\n");
        return 0;
    }
    fprintf(out, "# Lua 5.1 bytecode plaintext extract (strings / debug names)\n");
    fprintf(out, "# endian=%s int=%d size_t=%d Instruction=%d Number=%d\n",
            r.little ? "LE" : "BE", r.sz_int, r.sz_size, r.sz_ins, r.sz_num);
    load_function(&r);
    if (!r.ok) {
        fprintf(out, "\n# parse incomplete or corrupt at offset %u\n",
                (unsigned)(r.p - buf));
        return 0;
    }
    fprintf(out, "\n# ok\n");
    return 1;
}

/*
 * app/gui/UITimelimitFrameTittle.luac -> app\gui\UITimelimitFrameTittle
 * Creates a relative path under luac_dump (directories + basename, no extension).
 */
static void relative_path_from_chunk(const char *in, char *out, size_t cap) {
    size_t i, j = 0;
    size_t len;
    if (!out || cap == 0) return;
    out[0] = 0;
    if (!in || !in[0]) in = "chunk";
    if (in[0] == '@' || in[0] == '=') in++;
    for (i = 0; in[i] && j + 1 < cap; i++) {
        char c = in[i];
        if (c == '/' || c == '\\' || c == ':') {
            if (j > 0 && out[j - 1] != '\\') out[j++] = '\\';
            continue;
        }
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.') {
            out[j++] = c;
        } else if (j > 0 && out[j - 1] != '\\') {
            out[j++] = '_';
        }
    }
    while (j > 0 && (out[j - 1] == '\\' || out[j - 1] == '.')) j--;
    out[j] = 0;
    len = j;
    if (len >= 5 && _stricmp(out + len - 5, ".luac") == 0) {
        out[len - 5] = 0;
        len -= 5;
    }
    if (len >= 4 && _stricmp(out + len - 4, ".lua") == 0) {
        out[len - 4] = 0;
        len -= 4;
    }
    while (len > 0 && (out[len - 1] == '\\' || out[len - 1] == '.')) {
        out[--len] = 0;
    }
    if (len == 0) {
        strncpy(out, "chunk", cap - 1);
        out[cap - 1] = 0;
    }
}

static void ensure_parent_dirs(const char *file_path) {
    char dir[MAX_PATH];
    size_t i, n;
    if (!file_path || !file_path[0]) return;
    strncpy(dir, file_path, sizeof(dir) - 1);
    dir[sizeof(dir) - 1] = 0;
    n = strlen(dir);
    for (i = 0; i < n; i++) {
        if (dir[i] != '\\' && dir[i] != '/') continue;
        if (i == 0) continue;
        if (i == 2 && dir[1] == ':') continue; /* C:\ */
        {
            char saved = dir[i];
            dir[i] = 0;
            CreateDirectoryA(dir, NULL);
            dir[i] = saved;
        }
    }
}

/* True for path-like chunk names from file/module loaders, not loadstring. */
static int looks_like_script_name(const char *name) {
    if (!name || !name[0]) return 0;
    if (name[0] == '@') return 1;
    if (strchr(name, '/') || strchr(name, '\\')) return 1;
    if (strstr(name, ".lua") || strstr(name, ".luac") ||
        strstr(name, ".LUA") || strstr(name, ".LUAC"))
        return 1;
    return 0;
}

static int should_skip(const char *name, const void *buf, size_t size) {
    const unsigned char *b = (const unsigned char *)buf;
    if (name) {
        if (strstr(name, "COCOS_DEBUG") || strstr(name, "cocos_debug") ||
            strstr(name, "bootstrap") || strstr(name, "_LUA_TOOL"))
            return 1;
    }
    /* Real bytecode: always keep (unless bootstrap name above). */
    if (b && size > 0 && b[0] == LUA_SIGNATURE_0)
        return 0;

    /*
     * luaL_loadstring(L, s) calls loadbuffer(L, s, strlen(s), s):
     * chunk name == source. Games also loadbuffer tiny "return xxx"
     * expressions for pb/cmd dispatch — not script files.
     */
    if (name && size > 0 && size < 8192) {
        size_t nlen = strlen(name);
        if (nlen == size && memcmp(name, buf, size) == 0)
            return 1;
    }
    if (!looks_like_script_name(name))
        return 1;
    return 0;
}

static void write_tsv_field(FILE *fp, const char *value) {
    const unsigned char *p = (const unsigned char *)(value ? value : "");
    while (*p) {
        unsigned char c = *p++;
        if (c == '\t' || c == '\r' || c == '\n')
            fputc(' ', fp);
        else
            fputc(c, fp);
    }
}

static void append_dump_map(LONG seq, const char *name, const char *raw_path,
                            const char *plain_path, int is_bin) {
    char map_path[MAX_PATH];
    FILE *fp;
    _snprintf(map_path, sizeof(map_path), "%s\\luac_dump_map.tsv", g_dump_dir);
    map_path[sizeof(map_path) - 1] = 0;
    fp = fopen(map_path, "ab");
    if (!fp) return;
    fprintf(fp, "%ld\t%s\t", (long)seq, is_bin ? "luac" : "lua");
    write_tsv_field(fp, name ? name : "");
    fputc('\t', fp);
    write_tsv_field(fp, raw_path ? raw_path : "");
    fputc('\t', fp);
    write_tsv_field(fp, plain_path ? plain_path : "");
    fputc('\n', fp);
    fclose(fp);
}

static int dump_seen_contains(const char *module_name) {
    int i;
    for (i = 0; i < g_dump_seen_count; i++) {
        if (_stricmp(g_dump_seen[i], module_name) == 0)
            return 1;
    }
    return 0;
}

static void dump_seen_add(const char *module_name) {
    if (!module_name || !module_name[0]) return;
    if (dump_seen_contains(module_name)) return;
    if (g_dump_seen_count >= DUMP_SEEN_CAP) return;
    strncpy(g_dump_seen[g_dump_seen_count], module_name, DUMP_NAME_CAP - 1);
    g_dump_seen[g_dump_seen_count][DUMP_NAME_CAP - 1] = 0;
    g_dump_seen_count++;
}

static void free_dump_job(DumpJob *job) {
    if (!job) return;
    free(job->buf);
    free(job->chunk_name);
    free(job);
}

static void write_dump_job(DumpJob *job) {
    LONG seq;
    char path_main[MAX_PATH];
    char path_txt[MAX_PATH];
    FILE *fp;

    if (!job || !g_dump_dir[0] || !job->module_name[0]) {
        free_dump_job(job);
        return;
    }

    /* File-referenced job: disk I/O happens here, on the writer thread,
       never on the game's loading path. */
    if (job->from_file) {
        fp = fopen(job->chunk_name ? job->chunk_name : "", "rb");
        if (fp) {
            long sz;
            if (fseek(fp, 0, SEEK_END) == 0 && (sz = ftell(fp)) > 0 &&
                sz < 32 * 1024 * 1024) {
                job->buf = (unsigned char *)malloc((size_t)sz);
                if (job->buf) {
                    fseek(fp, 0, SEEK_SET);
                    if (fread(job->buf, 1, (size_t)sz, fp) == (size_t)sz)
                        job->size = (size_t)sz;
                    else {
                        free(job->buf);
                        job->buf = NULL;
                    }
                }
            }
            fclose(fp);
        }
        if (!job->buf) {
            free_dump_job(job);
            return;
        }
        job->is_bin = job->buf[0] == LUA_SIGNATURE_0;
    }

    if (!job->buf || job->size == 0) {
        free_dump_job(job);
        return;
    }

    seq = InterlockedIncrement(&g_dump_seq);
    CreateDirectoryA(g_dump_dir, NULL);

    if (job->is_bin) {
        _snprintf(path_main, sizeof(path_main), "%s\\%s.luac",
                  g_dump_dir, job->module_name);
        path_main[sizeof(path_main) - 1] = 0;
        _snprintf(path_txt, sizeof(path_txt), "%s\\%s.luac.txt",
                  g_dump_dir, job->module_name);
        path_txt[sizeof(path_txt) - 1] = 0;

        ensure_parent_dirs(path_main);
        fp = fopen(path_main, "wb");
        if (fp) {
            fwrite(job->buf, 1, job->size, fp);
            fclose(fp);
        }
        fp = fopen(path_txt, "wb");
        if (fp) {
            fprintf(fp, "# name=%s\n# bytes=%u\n",
                    job->chunk_name ? job->chunk_name : "(null)",
                    (unsigned)job->size);
            parse_and_write_plaintext(job->buf, job->size, fp);
            fclose(fp);
        }
        append_dump_map(seq, job->chunk_name, path_main, path_txt, 1);
    } else {
        _snprintf(path_main, sizeof(path_main), "%s\\%s.lua",
                  g_dump_dir, job->module_name);
        path_main[sizeof(path_main) - 1] = 0;
        ensure_parent_dirs(path_main);
        fp = fopen(path_main, "wb");
        if (fp) {
            fwrite(job->buf, 1, job->size, fp);
            fclose(fp);
        }
        append_dump_map(seq, job->chunk_name, path_main, path_main, 0);
    }

    free_dump_job(job);
}

static DWORD WINAPI dump_writer_thread(LPVOID param) {
    (void)param;
    for (;;) {
        DumpJob *job = NULL;
        EnterCriticalSection(&g_dump_lock);
        while (g_dump_q_count == 0 &&
               InterlockedCompareExchange(&g_dump_stop, 0, 0) == 0) {
            LeaveCriticalSection(&g_dump_lock);
            WaitForSingleObject(g_dump_event, 250);
            EnterCriticalSection(&g_dump_lock);
        }
        if (g_dump_q_count == 0) {
            LeaveCriticalSection(&g_dump_lock);
            if (InterlockedCompareExchange(&g_dump_stop, 0, 0) != 0)
                break;
            continue;
        }
        job = g_dump_queue[g_dump_q_head];
        g_dump_queue[g_dump_q_head] = NULL;
        g_dump_q_head = (g_dump_q_head + 1) % DUMP_QUEUE_CAP;
        g_dump_q_count--;
        if (job) {
            if (g_dump_q_bytes >= job->size)
                g_dump_q_bytes -= job->size;
            else
                g_dump_q_bytes = 0;
        }
        LeaveCriticalSection(&g_dump_lock);
        write_dump_job(job);
    }
    return 0;
}

static int ensure_dump_writer(void) {
    if (!g_dump_lock_ready) {
        InitializeCriticalSection(&g_dump_lock);
        g_dump_lock_ready = 1;
    }
    if (!g_dump_event) {
        g_dump_event = CreateEventA(NULL, FALSE, FALSE, NULL);
        if (!g_dump_event) return 0;
    }
    if (!g_dump_thread) {
        InterlockedExchange(&g_dump_stop, 0);
        g_dump_thread = CreateThread(NULL, 0, dump_writer_thread, NULL, 0, NULL);
        if (!g_dump_thread) return 0;
    }
    return 1;
}

static void enqueue_dump_chunk(const void *buf, size_t size, const char *name) {
    DumpJob *job;
    char module_name[DUMP_NAME_CAP];
    int is_bin;

    if (!g_dump_dir[0] || !buf || size == 0) return;
    if (should_skip(name, buf, size)) return;
    if (!ensure_dump_writer()) return;

    relative_path_from_chunk(name, module_name, sizeof(module_name));
    is_bin = ((const unsigned char *)buf)[0] == LUA_SIGNATURE_0;

    EnterCriticalSection(&g_dump_lock);
    if (dump_seen_contains(module_name) ||
        g_dump_q_count >= DUMP_QUEUE_CAP ||
        g_dump_q_bytes + size > DUMP_QUEUE_BYTES_MAX) {
        LeaveCriticalSection(&g_dump_lock);
        return;
    }
    LeaveCriticalSection(&g_dump_lock);

    job = (DumpJob *)calloc(1, sizeof(*job));
    if (!job) return;
    job->buf = (unsigned char *)malloc(size);
    if (!job->buf) {
        free_dump_job(job);
        return;
    }
    memcpy(job->buf, buf, size);
    job->size = size;
    job->is_bin = is_bin;
    strncpy(job->module_name, module_name, sizeof(job->module_name) - 1);
    if (name && name[0]) {
        size_t nlen = strlen(name) + 1;
        job->chunk_name = (char *)malloc(nlen);
        if (job->chunk_name)
            memcpy(job->chunk_name, name, nlen);
    }

    EnterCriticalSection(&g_dump_lock);
    if (dump_seen_contains(module_name) ||
        g_dump_q_count >= DUMP_QUEUE_CAP ||
        g_dump_q_bytes + size > DUMP_QUEUE_BYTES_MAX) {
        LeaveCriticalSection(&g_dump_lock);
        free_dump_job(job);
        return;
    }
    dump_seen_add(module_name);
    g_dump_queue[g_dump_q_tail] = job;
    g_dump_q_tail = (g_dump_q_tail + 1) % DUMP_QUEUE_CAP;
    g_dump_q_count++;
    g_dump_q_bytes += size;
    LeaveCriticalSection(&g_dump_lock);
    SetEvent(g_dump_event);
}

static void dump_chunk(const void *buf, size_t size, const char *name) {
    enqueue_dump_chunk(buf, size, name);
}

/*
 * loadfile hook: only capture the path on the game thread. The writer
 * thread opens and reads the file itself, so the game's loading path never
 * pays for our disk I/O (loading screens can fire hundreds of these).
 */
static void enqueue_dump_file(const char *filename) {
    DumpJob *job;
    char module_name[DUMP_NAME_CAP];
    char full_path[MAX_PATH];
    const char *name = filename;

    if (!g_dump_dir[0] || !filename || !filename[0]) return;
    /* Resolve now: the game CWD may differ when the writer thread runs. */
    if (GetFullPathNameA(filename, sizeof(full_path), full_path, NULL))
        name = full_path;
    if (should_skip(name, NULL, 0)) return;
    if (!ensure_dump_writer()) return;

    relative_path_from_chunk(name, module_name, sizeof(module_name));

    EnterCriticalSection(&g_dump_lock);
    if (dump_seen_contains(module_name) || g_dump_q_count >= DUMP_QUEUE_CAP) {
        LeaveCriticalSection(&g_dump_lock);
        return;
    }
    LeaveCriticalSection(&g_dump_lock);

    job = (DumpJob *)calloc(1, sizeof(*job));
    if (!job) return;
    job->from_file = 1;
    strncpy(job->module_name, module_name, sizeof(job->module_name) - 1);
    {
        size_t nlen = strlen(name) + 1;
        job->chunk_name = (char *)malloc(nlen);
        if (!job->chunk_name) {
            free_dump_job(job);
            return;
        }
        memcpy(job->chunk_name, name, nlen);
    }

    EnterCriticalSection(&g_dump_lock);
    if (dump_seen_contains(module_name) || g_dump_q_count >= DUMP_QUEUE_CAP) {
        LeaveCriticalSection(&g_dump_lock);
        free_dump_job(job);
        return;
    }
    dump_seen_add(module_name);
    g_dump_queue[g_dump_q_tail] = job;
    g_dump_q_tail = (g_dump_q_tail + 1) % DUMP_QUEUE_CAP;
    g_dump_q_count++;
    LeaveCriticalSection(&g_dump_lock);
    SetEvent(g_dump_event);
}

/* ---- prologue hook helpers (shared decoder in common/x86_insn.h) ---- */

static int install_cdecl_hook(uint8_t *target, void *hook_fn, void **out_tramp,
                              uint8_t *stolen_bytes, int *stolen_n) {
    int stolen;
    uint8_t *tramp;
    DWORD old_protect;
    int32_t rel;
    int i;
    if (!target || !hook_fn || !out_tramp) return 0;
    stolen = x86_measure_stolen(target, 16);
    if (stolen < 5) return 0;
    tramp = (uint8_t *)VirtualAlloc(NULL, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!tramp) return 0;
    if (x86_reloc_copy(tramp, target, stolen, target, tramp) != stolen) {
        VirtualFree(tramp, 0, MEM_RELEASE);
        return 0;
    }
    tramp[stolen] = 0xE9;
    rel = (int32_t)((target + stolen) - (tramp + stolen + 5));
    memcpy(tramp + stolen + 1, &rel, 4);
    memcpy(stolen_bytes, target, stolen);
    *stolen_n = stolen;
    if (!VirtualProtect(target, stolen, PAGE_EXECUTE_READWRITE, &old_protect)) {
        VirtualFree(tramp, 0, MEM_RELEASE);
        return 0;
    }
    /* No other thread may execute the target while the jmp is written. */
    patch_freeze_begin();
    target[0] = 0xE9;
    rel = (int32_t)((uint8_t *)hook_fn - (target + 5));
    memcpy(target + 1, &rel, 4);
    for (i = 5; i < stolen; i++) target[i] = 0x90;
    patch_freeze_end();
    VirtualProtect(target, stolen, old_protect, &old_protect);
    FlushInstructionCache(GetCurrentProcess(), target, stolen);
    FlushInstructionCache(GetCurrentProcess(), tramp, 64);
    *out_tramp = tramp;
    return 1;
}

static int __cdecl hook_loadbuffer(lua_State *L, const char *buff, size_t size, const char *name) {
    if (buff && size > 0)
        dump_chunk(buff, size, name);
    return g_loadbuffer_orig(L, buff, size, name);
}

static int __cdecl hook_loadfile(lua_State *L, const char *filename) {
    if (filename && filename[0])
        enqueue_dump_file(filename);
    return g_loadfile_orig(L, filename);
}

static FARPROC resolve_proc_local(const char *name) {
    HMODULE mods[512];
    DWORD cb = 0;
    char undec[96];
    unsigned i;
    if (!EnumProcessModules(GetCurrentProcess(), mods, sizeof(mods), &cb))
        return NULL;
    _snprintf(undec, sizeof(undec), "_%s", name);
    for (i = 0; i < cb / sizeof(HMODULE); i++) {
        FARPROC p = GetProcAddress(mods[i], name);
        if (p) return p;
        p = GetProcAddress(mods[i], undec);
        if (p) return p;
    }
    return NULL;
}

void cocos_debug_luac_dump_init(const char *dump_dir) {
    g_dump_dir[0] = 0;
    if (!dump_dir || !dump_dir[0]) return;
    strncpy(g_dump_dir, dump_dir, sizeof(g_dump_dir) - 1);
    g_dump_dir[sizeof(g_dump_dir) - 1] = 0;
    CreateDirectoryA(g_dump_dir, NULL);
    ensure_dump_writer();
}

int cocos_debug_luac_dump_loadbuffer_orig(lua_State *L, const char *buff, size_t size,
                                    const char *name) {
    if (g_loadbuffer_orig)
        return g_loadbuffer_orig(L, buff, size, name);
    return -1;
}

int cocos_debug_luac_dump_install_hooks(void) {
    FARPROC lb, lf;
    int n = 0;
    if (!g_dump_dir[0]) return 0;
    lb = resolve_proc_local("luaL_loadbuffer");
    lf = resolve_proc_local("luaL_loadfile");
    if (lb) {
        g_lb_target = (uint8_t *)lb;
        if (install_cdecl_hook(g_lb_target, (void *)hook_loadbuffer,
                               &g_loadbuffer_tramp, g_lb_stolen, &g_lb_stolen_n)) {
            g_loadbuffer_orig = (luaL_loadbuffer_fn)g_loadbuffer_tramp;
            n++;
        }
    }
    if (lf) {
        g_lf_target = (uint8_t *)lf;
        if (install_cdecl_hook(g_lf_target, (void *)hook_loadfile,
                               &g_loadfile_tramp, g_lf_stolen, &g_lf_stolen_n)) {
            g_loadfile_orig = (luaL_loadfile_fn)g_loadfile_tramp;
            n++;
        }
    }
    return n;
}
