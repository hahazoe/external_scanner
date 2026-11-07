#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d9.h>
#include <psapi.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <deque>
#include <map>
#include <string>
#include <vector>

#include "imgui.h"
#include "imgui_impl_dx9.h"
#include "imgui_impl_win32.h"
#include "implot.h"

#include "telemetry_client.h"
#include "frame_preview.h"
#include "recording_io.h"
#include "debug_list.h"
#include "../injection/injector.h"

#include <commdlg.h>

static LPDIRECT3D9 g_d3d;
static LPDIRECT3DDEVICE9 g_device;
static D3DPRESENT_PARAMETERS g_present;
static bool g_device_lost;
static UINT g_resize_width, g_resize_height;
static HWND g_window;
static FramePreview g_preview;

static bool create_device(HWND window);
static void cleanup_device();
static void reset_device();
static LRESULT WINAPI window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam);
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(
    HWND window, UINT message, WPARAM wparam, LPARAM lparam);

static bool debug_list_is_hotkey_key(ImGuiKey key) {
    return key >= ImGuiKey_Tab && key <= ImGuiKey_Oem102 &&
           key != ImGuiKey_LeftCtrl && key != ImGuiKey_RightCtrl &&
           key != ImGuiKey_LeftShift && key != ImGuiKey_RightShift &&
           key != ImGuiKey_LeftAlt && key != ImGuiKey_RightAlt &&
           key != ImGuiKey_LeftSuper && key != ImGuiKey_RightSuper;
}

static int debug_list_parse_hotkey(const char *text) {
    const char *p = text;
    int chord = 0;
    ImGuiKey key;
    if (!p || !p[0] || strcmp(p, "-") == 0)
        return 0;
    for (;;) {
        if (_strnicmp(p, "Ctrl+", 5) == 0) { chord |= ImGuiMod_Ctrl; p += 5; continue; }
        if (_strnicmp(p, "Control+", 8) == 0) { chord |= ImGuiMod_Ctrl; p += 8; continue; }
        if (_strnicmp(p, "Shift+", 6) == 0) { chord |= ImGuiMod_Shift; p += 6; continue; }
        if (_strnicmp(p, "Alt+", 4) == 0) { chord |= ImGuiMod_Alt; p += 4; continue; }
        if (_strnicmp(p, "Super+", 6) == 0) { chord |= ImGuiMod_Super; p += 6; continue; }
        if (_strnicmp(p, "Win+", 4) == 0) { chord |= ImGuiMod_Super; p += 4; continue; }
        if (_strnicmp(p, "Cmd+", 4) == 0) { chord |= ImGuiMod_Super; p += 4; continue; }
        break;
    }
    if (!p[0])
        return 0;
    for (key = ImGuiKey_Tab; key <= ImGuiKey_Oem102; key = (ImGuiKey)(key + 1)) {
        const char *name;
        if (!debug_list_is_hotkey_key(key))
            continue;
        name = ImGui::GetKeyName(key);
        if (name && _stricmp(name, p) == 0)
            return chord | key;
    }
    return 0;
}

static void debug_list_format_hotkey(int chord, char *out, size_t cap) {
    ImGuiKey key;
    const char *key_name;
    char text[96];
    text[0] = 0;
    if (!out || cap == 0)
        return;
    out[0] = 0;
    if (!chord)
        return;
    if (chord & ImGuiMod_Ctrl)
        strcat(text, "Ctrl+");
    if (chord & ImGuiMod_Shift)
        strcat(text, "Shift+");
    if (chord & ImGuiMod_Alt)
        strcat(text, "Alt+");
    if (chord & ImGuiMod_Super)
        strcat(text, "Super+");
    key = (ImGuiKey)(chord & ~ImGuiMod_Mask_);
    key_name = debug_list_is_hotkey_key(key) ? ImGui::GetKeyName(key) : NULL;
    if (key_name && key_name[0])
        strcat(text, key_name);
    else if (text[0])
        text[strlen(text) - 1] = 0;
    strncpy(out, text, cap - 1);
    out[cap - 1] = 0;
}

#ifndef MOD_NOREPEAT
#define MOD_NOREPEAT 0x4000
#endif
#define DEBUG_LIST_HOTKEY_ID_BASE 0xA000

static int g_pending_global_hotkeys[16];
static int g_pending_global_hotkey_count;

static int debug_list_hotkey_vk(ImGuiKey key) {
    if (key >= ImGuiKey_A && key <= ImGuiKey_Z)
        return 'A' + (int)(key - ImGuiKey_A);
    if (key >= ImGuiKey_0 && key <= ImGuiKey_9)
        return '0' + (int)(key - ImGuiKey_0);
    if (key >= ImGuiKey_F1 && key <= ImGuiKey_F24)
        return VK_F1 + (int)(key - ImGuiKey_F1);
    if (key >= ImGuiKey_Keypad0 && key <= ImGuiKey_Keypad9)
        return VK_NUMPAD0 + (int)(key - ImGuiKey_Keypad0);
    switch (key) {
    case ImGuiKey_Tab: return VK_TAB;
    case ImGuiKey_LeftArrow: return VK_LEFT;
    case ImGuiKey_RightArrow: return VK_RIGHT;
    case ImGuiKey_UpArrow: return VK_UP;
    case ImGuiKey_DownArrow: return VK_DOWN;
    case ImGuiKey_PageUp: return VK_PRIOR;
    case ImGuiKey_PageDown: return VK_NEXT;
    case ImGuiKey_Home: return VK_HOME;
    case ImGuiKey_End: return VK_END;
    case ImGuiKey_Insert: return VK_INSERT;
    case ImGuiKey_Delete: return VK_DELETE;
    case ImGuiKey_Backspace: return VK_BACK;
    case ImGuiKey_Space: return VK_SPACE;
    case ImGuiKey_Enter: return VK_RETURN;
    case ImGuiKey_Escape: return VK_ESCAPE;
    case ImGuiKey_Apostrophe: return VK_OEM_7;
    case ImGuiKey_Comma: return VK_OEM_COMMA;
    case ImGuiKey_Minus: return VK_OEM_MINUS;
    case ImGuiKey_Period: return VK_OEM_PERIOD;
    case ImGuiKey_Slash: return VK_OEM_2;
    case ImGuiKey_Semicolon: return VK_OEM_1;
    case ImGuiKey_Equal: return VK_OEM_PLUS;
    case ImGuiKey_LeftBracket: return VK_OEM_4;
    case ImGuiKey_Backslash: return VK_OEM_5;
    case ImGuiKey_RightBracket: return VK_OEM_6;
    case ImGuiKey_GraveAccent: return VK_OEM_3;
    case ImGuiKey_CapsLock: return VK_CAPITAL;
    case ImGuiKey_ScrollLock: return VK_SCROLL;
    case ImGuiKey_NumLock: return VK_NUMLOCK;
    case ImGuiKey_PrintScreen: return VK_SNAPSHOT;
    case ImGuiKey_Pause: return VK_PAUSE;
    case ImGuiKey_KeypadDecimal: return VK_DECIMAL;
    case ImGuiKey_KeypadDivide: return VK_DIVIDE;
    case ImGuiKey_KeypadMultiply: return VK_MULTIPLY;
    case ImGuiKey_KeypadSubtract: return VK_SUBTRACT;
    case ImGuiKey_KeypadAdd: return VK_ADD;
    case ImGuiKey_KeypadEnter: return VK_RETURN;
    default: return 0;
    }
}

static UINT debug_list_hotkey_winmods(int chord) {
    UINT mods = MOD_NOREPEAT;
    if (chord & ImGuiMod_Ctrl) mods |= MOD_CONTROL;
    if (chord & ImGuiMod_Shift) mods |= MOD_SHIFT;
    if (chord & ImGuiMod_Alt) mods |= MOD_ALT;
    if (chord & ImGuiMod_Super) mods |= MOD_WIN;
    return mods;
}

static bool debug_list_hotkey_can_register(int chord) {
    ImGuiKey key = (ImGuiKey)(chord & ~ImGuiMod_Mask_);
    int mods = chord & ImGuiMod_Mask_;
    if (!debug_list_hotkey_vk(key))
        return false;
    if (mods)
        return true;
    return key >= ImGuiKey_F1 && key <= ImGuiKey_F24;
}

struct InjectionWorkerResult {
    CocosDebugInjectionSession session;
    char error[COCOS_DEBUG_IPC_ERROR_CAP];
    bool success;
};

/* Measured on this profiler: 300k Lua calls cost 20.8 ms bare and 158.3 ms
   with the hook installed, i.e. ~0.23 us per hook event.  That cost lands in
   drawScene body and never in lua_cpu, so it has to be subtracted -- not
   ignored -- before a frame's Lua share is judged. */
static const double kHookOverheadMsPerEvent = 0.00023;

/* One Lua method summed over a window of frames.  A single frame is the unit
   the profiler reports, but the questions that matter ("which method only
   fires on 20 of 969 frames and still owns half the dropped frames?") cannot
   be answered from any single frame -- they need the hit count, the
   distribution, and the split between quiet frames and spike frames. */
struct MethodAggregate {
    const std::string *name;
    uint32_t method_id;
    uint16_t kind;
    uint32_t hit_frames;
    double self_sum;
    double self_max;
    double inclusive_sum;
    uint64_t calls_started;
    /* Split by frame kind: is this method's cost a constant background, or
       does it only show up in the frames that blow the budget? */
    uint32_t baseline_hits;
    double baseline_self_sum;
    uint32_t spike_hits;
    double spike_self_sum;
    std::vector<float> self_samples;

    MethodAggregate()
        : name(0), method_id(0), kind(0), hit_frames(0), self_sum(0),
          self_max(0), inclusive_sum(0), calls_started(0), baseline_hits(0),
          baseline_self_sum(0), spike_hits(0), spike_self_sum(0) {}

    double selfPerHit() const {
        return hit_frames ? self_sum / (double)hit_frames : 0.0;
    }
    double baselineMean() const {
        return baseline_hits ? baseline_self_sum / (double)baseline_hits : 0.0;
    }
    double spikeMean() const {
        return spike_hits ? spike_self_sum / (double)spike_hits : 0.0;
    }
    double spikeDelta() const { return spikeMean() - baselineMean(); }
    double p95Self() const {
        if (self_samples.empty()) return 0.0;
        std::vector<float> sorted(self_samples);
        std::sort(sorted.begin(), sorted.end());
        size_t index = (size_t)(0.95 * (double)(sorted.size() - 1) + 0.5);
        if (index >= sorted.size()) index = sorted.size() - 1;
        return (double)sorted[index];
    }
};

struct Application {
    CocosDebugProcessInfo processes[256];
    int process_count;
    int selected_process;
    DWORD last_process_refresh;
    CRITICAL_SECTION worker_lock;
    HANDLE worker;
    volatile LONG worker_done;
    volatile LONG injecting;
    int progress;
    char progress_text[128];
    InjectionWorkerResult worker_result;
    CocosDebugInjectionSession session;
    TelemetryClient telemetry;
    std::deque<FrameSnapshot> history;
    std::deque<LuaTraceEventSnapshot> pending_trace_events;
    uint64_t selected_sequence;
    bool paused;
    bool pending_autosave;
    bool was_recording;
    float frame_window;
    float spike_threshold_ms;
    bool show_spike_hints;
    bool show_cpp_hooks_plot;
    char method_filter[128];
    int method_kind_filter;
    /* Cross-frame analysis (aggregate / spike diff / per-method curve). */
    bool show_cross_frame;
    char aggregate_filter[128];
    int aggregate_window;
    uint32_t selected_method_id;
    std::string selected_method_label;
    std::map<uint32_t, size_t> aggregate_index;
    std::vector<MethodAggregate> aggregates;
    size_t aggregate_cached_size;
    uint64_t aggregate_cached_first;
    DWORD aggregate_last_build;
    size_t aggregate_frame_count;
    size_t aggregate_spike_count;
    double aggregate_median_baseline_ms;
    std::vector<double> ts_x;
    std::vector<double> ts_y;
    std::vector<double> ts_body;
    bool ts_dirty;
    std::string status_message;
    DWORD last_preview_tick;
    double last_plot_xmin;
    double last_plot_xmax;
    std::vector<double> plot_x;
    std::vector<double> plot_draw;
    std::vector<double> plot_delta;
    std::vector<double> plot_render;
    std::vector<double> plot_lua_wall;
    std::vector<double> plot_lua_cpu;
    std::vector<double> plot_lua_self;
    std::vector<double> plot_c_self;
    std::vector<double> plot_lua_outside;
    std::vector<double> plot_lua_mem;
    std::vector<double> plot_proc_ws;
    std::vector<double> plot_proc_private;
    std::vector<std::string> plot_cpp_labels;
    std::vector<std::vector<double> > plot_cpp_series;
    size_t plot_begin_index;
    bool show_lua_editor;
    char lua_editor_text[COCOS_DEBUG_IPC_REMOTE_LUA_CAP];
    char lua_editor_status[COCOS_DEBUG_IPC_ERROR_CAP];
    char lua_editor_error[COCOS_DEBUG_IPC_ERROR_CAP];
    int32_t lua_editor_wait_result_seq;
    bool lua_editor_waiting;
    bool show_debug_list;
    bool focus_lua_editor;
    bool focus_debug_list;
    bool debug_list_loaded;
    bool debug_list_capturing_hotkey;
    std::vector<int> debug_list_os_hotkeys;
    int debug_list_selected;
    CocosDebugDebugList debug_list;
    char debug_list_name[128];
    char debug_list_code[COCOS_DEBUG_IPC_REMOTE_LUA_CAP];
    std::string debug_list_note;

    Application()
        : process_count(0), selected_process(-1), last_process_refresh(0),
          worker(NULL), worker_done(0), injecting(0), progress(0),
          selected_sequence(0), paused(false), pending_autosave(false),
          was_recording(false), frame_window(180.0f),
          spike_threshold_ms(16.67f), show_spike_hints(true),
          show_cpp_hooks_plot(true),
          method_kind_filter(0),
          show_cross_frame(false), aggregate_window(600),
          selected_method_id(0), aggregate_cached_size(0),
          aggregate_cached_first(0), aggregate_last_build(0),
          aggregate_frame_count(0), aggregate_spike_count(0),
          aggregate_median_baseline_ms(0), ts_dirty(true),
          last_preview_tick(0),
          last_plot_xmin(0), last_plot_xmax(0), plot_begin_index(0),
          show_lua_editor(false),
          lua_editor_wait_result_seq(0), lua_editor_waiting(false),
          show_debug_list(false), focus_lua_editor(false),
          focus_debug_list(false), debug_list_loaded(false),
          debug_list_capturing_hotkey(false), debug_list_selected(-1) {
        memset(&session, 0, sizeof(session));
        memset(&worker_result, 0, sizeof(worker_result));
        memset(progress_text, 0, sizeof(progress_text));
        memset(method_filter, 0, sizeof(method_filter));
        memset(aggregate_filter, 0, sizeof(aggregate_filter));
        memset(lua_editor_text, 0, sizeof(lua_editor_text));
        memset(lua_editor_status, 0, sizeof(lua_editor_status));
        memset(lua_editor_error, 0, sizeof(lua_editor_error));
        memset(debug_list_name, 0, sizeof(debug_list_name));
        memset(debug_list_code, 0, sizeof(debug_list_code));
        strncpy(lua_editor_status, "Idle", sizeof(lua_editor_status) - 1);
        InitializeCriticalSection(&worker_lock);
    }

    ~Application() {
        flushDebugListEditor();
        unregisterDebugListGlobalHotkeys();
        if (debug_list_loaded)
            cocos_debug_debug_list_flush_all(&debug_list);
        if (worker) {
            WaitForSingleObject(worker, INFINITE);
            CloseHandle(worker);
        }
        if (session.ipc) {
            cocos_debug_send_command(&session, COCOS_DEBUG_IPC_CMD_STOP);
            Sleep(50);
        }
        telemetry.detach();
        g_preview.clear();
        cocos_debug_close_session(&session);
        DeleteCriticalSection(&worker_lock);
    }

    void refreshProcesses() {
        DWORD previous_pid = selected_process >= 0 && selected_process < process_count
            ? processes[selected_process].pid : 0;
        process_count = cocos_debug_list_processes(processes, 256);
        selected_process = -1;
        for (int i = 0; i < process_count; i++) {
            if (processes[i].pid == previous_pid) selected_process = i;
        }
        if (selected_process < 0 && process_count > 0) selected_process = 0;
        last_process_refresh = GetTickCount();
    }

    static void progressCallback(int percent, const char *stage, void *user) {
        Application *app = (Application *)user;
        EnterCriticalSection(&app->worker_lock);
        app->progress = percent;
        strncpy(app->progress_text, stage ? stage : "", sizeof(app->progress_text) - 1);
        app->progress_text[sizeof(app->progress_text) - 1] = 0;
        LeaveCriticalSection(&app->worker_lock);
    }

    static DWORD WINAPI injectionThread(LPVOID param) {
        Application *app = (Application *)param;
        DWORD pid;
        EnterCriticalSection(&app->worker_lock);
        pid = app->worker_result.session.pid;
        memset(&app->worker_result, 0, sizeof(app->worker_result));
        LeaveCriticalSection(&app->worker_lock);
        InjectionWorkerResult result;
        memset(&result, 0, sizeof(result));
        result.success = cocos_debug_inject_process(pid, progressCallback, app,
                                             &result.session, result.error,
                                             sizeof(result.error)) != 0;
        if (!result.success && result.session.ipc)
            cocos_debug_close_session(&result.session);
        EnterCriticalSection(&app->worker_lock);
        app->worker_result = result;
        LeaveCriticalSection(&app->worker_lock);
        InterlockedExchange(&app->worker_done, 1);
        return 0;
    }

    void beginInjection() {
        if (selected_process < 0 || selected_process >= process_count || injecting) return;
        telemetry.detach();
        g_preview.clear();
        if (session.ipc)
            cocos_debug_close_session(&session);
        history.clear();
        pending_trace_events.clear();
        selected_sequence = 0;
        memset(&worker_result, 0, sizeof(worker_result));
        worker_result.session.pid = processes[selected_process].pid;
        progress = 0;
        strcpy(progress_text, "Starting");
        status_message.clear();
        InterlockedExchange(&worker_done, 0);
        InterlockedExchange(&injecting, 1);
        worker = CreateThread(NULL, 0, injectionThread, this, 0, NULL);
        if (!worker) {
            injecting = 0;
            status_message = "CreateThread failed";
        }
    }

    void finishWorkerIfReady() {
        if (!injecting || !worker_done) return;
        WaitForSingleObject(worker, INFINITE);
        CloseHandle(worker);
        worker = NULL;
        EnterCriticalSection(&worker_lock);
        InjectionWorkerResult result = worker_result;
        LeaveCriticalSection(&worker_lock);
        if (result.success) {
            session = result.session;
            history.clear();
            pending_trace_events.clear();
            selected_sequence = 0;
            g_preview.clear();
            telemetry.attach(&session);
            status_message = "Connected";
        } else {
            if (result.error[0]) {
                status_message = result.error;
                if (strstr(result.error, "restart") ||
                    strstr(result.error, "already loaded") ||
                    strstr(result.error, "already in progress"))
                    status_message += "  Please restart the target process, then inject again.";
            } else {
                status_message = "Injection failed. If a profiler DLL is still loaded, restart the target process.";
            }
        }
        InterlockedExchange(&injecting, 0);
        InterlockedExchange(&worker_done, 0);
    }

    bool saveHistoryToPath(const char *path) {
        std::string error;
        std::vector<FramePreviewImage> images;
        g_preview.exportImages(&images);
        if (!cocos_debug_save_recording(path, history, &images, &error)) {
            status_message = "Save failed: " + error;
            return false;
        }
        char msg[512];
        _snprintf(msg, sizeof(msg), "Saved %s (%u frames, %u previews)",
                  path, (unsigned)history.size(), (unsigned)images.size());
        msg[sizeof(msg) - 1] = 0;
        status_message = msg;
        return true;
    }

    bool autosaveCapture() {
        char path[MAX_PATH];
        if (history.empty()) {
            status_message = "Stop: nothing to save";
            return false;
        }
        if (!cocos_debug_make_capture_path(path, sizeof(path))) {
            status_message = "Stop: failed to build capture path";
            return false;
        }
        return saveHistoryToPath(path);
    }

    bool loadRecordingDialog() {
        OPENFILENAMEA ofn;
        char path[MAX_PATH];
        std::string error;
        std::deque<FrameSnapshot> loaded;
        std::vector<FramePreviewImage> images;
        memset(&ofn, 0, sizeof(ofn));
        memset(path, 0, sizeof(path));
        ofn.lStructSize = sizeof(ofn);
        ofn.hwndOwner = g_window;
        ofn.lpstrFilter = "Cocos2d Capture (*.cocosdbg)\0*.cocosdbg\0All Files (*.*)\0*.*\0";
        ofn.lpstrFile = path;
        ofn.nMaxFile = MAX_PATH;
        ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
        ofn.lpstrDefExt = "cocosdbg";
        if (!GetOpenFileNameA(&ofn)) return false;
        if (!cocos_debug_load_recording(path, &loaded, &images, &error)) {
            status_message = "Load failed: " + error;
            return false;
        }
        history.swap(loaded);
        pending_trace_events.clear();
        selected_sequence = 0;
        paused = false;
        pending_autosave = false;
        g_preview.importImages(images);
        {
            char msg[512];
            _snprintf(msg, sizeof(msg), "Loaded %s (%u frames, %u previews)",
                      path, (unsigned)history.size(), (unsigned)images.size());
            msg[sizeof(msg) - 1] = 0;
            status_message = msg;
        }
        return true;
    }

    bool saveRecordingDialog() {
        OPENFILENAMEA ofn;
        char path[MAX_PATH];
        memset(&ofn, 0, sizeof(ofn));
        memset(path, 0, sizeof(path));
        if (!cocos_debug_make_capture_path(path, sizeof(path)))
            path[0] = 0;
        ofn.lStructSize = sizeof(ofn);
        ofn.hwndOwner = g_window;
        ofn.lpstrFilter = "Cocos2d Capture (*.cocosdbg)\0*.cocosdbg\0All Files (*.*)\0*.*\0";
        ofn.lpstrFile = path;
        ofn.nMaxFile = MAX_PATH;
        ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
        ofn.lpstrDefExt = "cocosdbg";
        if (!GetSaveFileNameA(&ofn)) return false;
        return saveHistoryToPath(path);
    }

    void pollTelemetry() {
        bool recording_now;
        std::vector<FrameSnapshot> frames;
        std::vector<LuaTraceEventSnapshot> events;
        size_t n;

        /* Always drain the ingest queue (works for live IPC and keeps UI off the hot path). */
        n = telemetry.consumeFrames(&frames, 4096);
        for (size_t i = 0; i < frames.size(); i++) {
            history.push_back(frames[i]);
            while (history.size() > 10000) {
                if (selected_sequence == history.front().publish_sequence)
                    selected_sequence = 0;
                history.pop_front();
            }
        }
        telemetry.consumeEvents(&events, 65536);
        for (size_t i = 0; i < events.size(); i++)
            pending_trace_events.push_back(events[i]);
        if (!pending_trace_events.empty() && !history.empty()) {
            std::deque<LuaTraceEventSnapshot> still_pending;
            const uint32_t oldest_frame = history.front().lua_frame_index;
            while (!pending_trace_events.empty()) {
                LuaTraceEventSnapshot event = pending_trace_events.front();
                pending_trace_events.pop_front();
                bool attached = false;
                if (event.frame_sequence != 0) {
                    if (event.frame_sequence < oldest_frame) {
                        /* Frame already evicted from history: drop the event. */
                    } else {
                        /* Trace events are near-real-time; only the history
                           tail can still match, so don't scan it all. */
                        size_t scanned = 0;
                        for (std::deque<FrameSnapshot>::reverse_iterator it = history.rbegin();
                             it != history.rend() && scanned < 512; ++it, ++scanned) {
                            if (it->lua_frame_index == event.frame_sequence) {
                                it->trace_events.push_back(event);
                                attached = true;
                                break;
                            }
                        }
                        /* Not found yet: frame may still be in flight. */
                        if (!attached)
                            still_pending.push_back(event);
                    }
                }
            }
            pending_trace_events.swap(still_pending);
            while (pending_trace_events.size() > COCOS_DEBUG_IPC_EVENT_CAP)
                pending_trace_events.pop_front();
        }
        if (!paused && session.target_window && !frames.empty()) {
            DWORD now = GetTickCount();
            if (last_preview_tick == 0 || now - last_preview_tick >= 80) {
                g_preview.capture(session.target_window, frames.back().lua_frame_index);
                last_preview_tick = now;
            }
        }

        if (!session.ipc) {
            was_recording = false;
            return;
        }
        recording_now = telemetry.recording();
        if (pending_autosave && was_recording && !recording_now) {
            pending_autosave = false;
            autosaveCapture();
        }
        was_recording = recording_now;
        (void)n;
    }

    const FrameSnapshot *selectedFrame() const {
        if (history.empty()) return NULL;
        if (!selected_sequence) {
            if (paused) return NULL;
            return &history.back();
        }
        for (size_t i = 0; i < history.size(); i++)
            if (history[i].publish_sequence == selected_sequence) return &history[i];
        /* While paused, never fall back to the live tail. */
        if (paused) return NULL;
        return &history.back();
    }

    size_t frameIndexOf(const FrameSnapshot *frame) const {
        for (size_t i = 0; i < history.size(); i++)
            if (&history[i] == frame) return i;
        return (size_t)-1;
    }

    void renderProcessPanel() {
        if (!last_process_refresh || GetTickCount() - last_process_refresh > 3000)
            refreshProcesses();
        ImGui::TextUnformatted("Target");
        ImGui::SameLine();
        const char *preview = selected_process >= 0
            ? processes[selected_process].name : "<none>";
        if (ImGui::BeginCombo("##process", preview)) {
            for (int i = 0; i < process_count; i++) {
                char label[320];
                _snprintf(label, sizeof(label), "%s  (PID %lu)",
                          processes[i].name, (unsigned long)processes[i].pid);
                bool selected = i == selected_process;
                if (ImGui::Selectable(label, selected)) selected_process = i;
                if (selected) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        if (ImGui::Button("Refresh")) refreshProcesses();
        ImGui::SameLine();
        ImGui::BeginDisabled(injecting != 0 || selected_process < 0);
        if (ImGui::Button("Inject and connect")) beginInjection();
        ImGui::EndDisabled();
        if (injecting) {
            int shown_progress;
            char shown_text[128];
            EnterCriticalSection(&worker_lock);
            shown_progress = progress;
            strncpy(shown_text, progress_text, sizeof(shown_text));
            LeaveCriticalSection(&worker_lock);
            ImGui::ProgressBar(shown_progress / 100.0f, ImVec2(-1, 0), shown_text);
        }
        if (!status_message.empty()) ImGui::TextWrapped("%s", status_message.c_str());
    }

    void renderControls() {
        bool connected = session.ipc != NULL;
        bool recording = telemetry.recording();
        bool tracing = connected && session.ipc->header.trace_enabled != 0;
        bool pause_clicked;
        ImGui::BeginDisabled(!connected || recording);
        if (ImGui::Button("Start profiling")) {
            history.clear();
            pending_trace_events.clear();
            selected_sequence = 0;
            g_preview.clear();
            telemetry.skipToLive();
            cocos_debug_send_command(&session, COCOS_DEBUG_IPC_CMD_START);
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(!connected || !recording);
        if (ImGui::Button("Capture Lua details"))
            cocos_debug_send_command(&session, COCOS_DEBUG_IPC_CMD_DETAIL_CAPTURE);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Temporarily enables Lua VM count sampling for hot source lines.");
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(!connected || !recording || tracing);
        if (ImGui::Button("Trace Start"))
            cocos_debug_send_command(&session, COCOS_DEBUG_IPC_CMD_TRACE_START);
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(!connected || !tracing);
        if (ImGui::Button("Trace Stop"))
            cocos_debug_send_command(&session, COCOS_DEBUG_IPC_CMD_TRACE_STOP);
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(!connected || !recording);
        if (ImGui::Button("Stop")) {
            cocos_debug_send_command(&session, COCOS_DEBUG_IPC_CMD_STOP);
            pending_autosave = true;
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Clear history")) {
            history.clear();
            pending_trace_events.clear();
            selected_sequence = 0;
            g_preview.clear();
            telemetry.clearQueue();
            telemetry.resetDropped();
            if (connected) cocos_debug_send_command(&session, COCOS_DEBUG_IPC_CMD_CLEAR);
        }
        ImGui::SameLine();
        pause_clicked = ImGui::Checkbox("Pause view", &paused);
        if (pause_clicked) {
            if (paused && !history.empty() && selected_sequence == 0)
                selected_sequence = history.back().publish_sequence;
            else if (!paused)
                selected_sequence = 0;
        }
        ImGui::SameLine();
        ImGui::BeginDisabled(history.empty());
        if (ImGui::Button("Save now")) saveRecordingDialog();
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Load recording")) loadRecordingDialog();
        ImGui::SameLine();
        ImGui::BeginDisabled(!connected);
        if (ImGui::Button("Run Lua String")) {
            show_lua_editor = true;
            focus_lua_editor = true;
            if (!lua_editor_text[0])
                strncpy(lua_editor_text, "print('hello from DebugView')\n",
                        sizeof(lua_editor_text) - 1);
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("DebugList")) {
            show_debug_list = true;
            focus_debug_list = true;
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Manage saved Lua snippets. Each entry is a local .lua file.");
        ImGui::SliderFloat("Visible frames", &frame_window, 30.0f, 600.0f, "%.0f");
        ImGui::SameLine();
        ImGui::Checkbox("Spike hints", &show_spike_hints);
        if (show_spike_hints) {
            ImGui::SameLine();
            ImGui::SetNextItemWidth(160.0f);
            ImGui::SliderFloat("Budget ms##spike", &spike_threshold_ms, 8.0f, 50.0f, "%.2f");
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Highlight frames whose drawScene body exceeds this budget.\n"
                                  "Default 16.67 ms ≈ 60 FPS.");
        }
        ImGui::SameLine();
        ImGui::Checkbox("Plot C++ hooks", &show_cpp_hooks_plot);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("在曲线上叠加 Scheduler / EventDispatcher / 贴图等 C++ 钩子。\n"
                              "drawScene 与 Renderer:render 仍用原来的专用曲线。");
        ImGui::Text("buffered: %u  frame queue: %u  event queue: %u",
                    (unsigned)history.size(), (unsigned)telemetry.queueSize(),
                    (unsigned)telemetry.eventQueueSize());
        if (connected) {
            ImGui::Text("State: %d  trace: %s  dropped frames: %u  dropped events: %u  "
                        "_deltaTime offset: %u",
                        telemetry.state(), tracing ? "on" : "off",
                        telemetry.droppedFrames(), telemetry.droppedEvents(),
                        session.ipc
                            ? (unsigned)session.ipc->config.director_delta_time_offset : 0u);
            if (telemetry.error()[0]) ImGui::TextColored(ImVec4(1, .35f, .3f, 1),
                                                          "%s", telemetry.error());
            {
                uint32_t installed = installedCppHookCount();
                uint32_t configured = session.ipc ? session.ipc->config.cpp_count : 0;
                ImGui::Text("C++ hooks: %u installed / %u configured",
                            (unsigned)installed, (unsigned)configured);
                if (ImGui::IsItemHovered() && session.ipc) {
                    ImGui::BeginTooltip();
                    ImGui::TextUnformatted("注入时解析到的 C++ 目标：");
                    for (uint32_t i = 0; i < configured && i < COCOS_DEBUG_IPC_CPP_CAP; i++) {
                        const CocosDebugIpcCppTarget &t = session.ipc->config.cpp_targets[i];
                        if (t.address >= 0x10000u)
                            ImGui::Text("%s  @ 0x%08X", t.name, (unsigned)t.address);
                        else
                            ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.2f, 1.0f),
                                               "%s  (未解析)", t.name);
                    }
                    ImGui::EndTooltip();
                }
                if (installed <= 3)
                    ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.2f, 1.0f),
                                       "钩子偏少：请重启目标进程后用新 DebugView 重新注入。");
            }
            if (!rendererHookConfigured())
                ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.2f, 1.0f),
                                   "render hook: not installed (cc.Renderer:render). "
                                   "Restart the target process, then inject again.");
            else if (!rendererHasSamples())
                ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.2f, 1.0f),
                                   "render hook: installed, 0 calls "
                                   "(export is not the submit path)");
            else
                ImGui::TextDisabled("render hook: cc.Renderer:render");
        }
    }

    static bool isPrimaryCppPlotName(const std::string &name) {
        /* Already covered by dedicated DrawScene / render series. */
        return name.find("drawScene") != std::string::npos ||
               name.find("Renderer:render") != std::string::npos;
    }

    static std::string shortCppHookLabel(const std::string &name) {
        size_t pos = name.rfind("cc.");
        if (pos != std::string::npos) return name.substr(pos + 3);
        pos = name.rfind("::");
        if (pos != std::string::npos) return name.substr(pos + 2);
        return name;
    }

    uint32_t installedCppHookCount() const {
        uint32_t n = 0;
        uint32_t i;
        if (!session.ipc) return 0;
        for (i = 0; i < session.ipc->config.cpp_count && i < COCOS_DEBUG_IPC_CPP_CAP; i++) {
            if (session.ipc->config.cpp_targets[i].address >= 0x10000u)
                n++;
        }
        return n;
    }

    bool rendererHookConfigured() const {
        uint32_t i;
        if (!history.empty() && history.back().scene_render_hook_present)
            return true;
        if (!session.ipc) return false;
        for (i = 0; i < session.ipc->config.cpp_count && i < COCOS_DEBUG_IPC_CPP_CAP; i++) {
            if (strstr(session.ipc->config.cpp_targets[i].name, "Renderer:render") &&
                session.ipc->config.cpp_targets[i].address >= 0x10000u)
                return true;
        }
        return false;
    }

    bool rendererHasSamples() const {
        size_t i;
        size_t n = history.size();
        size_t begin = n > 64 ? n - 64 : 0;
        for (i = begin; i < n; i++) {
            if (history[i].scene_render_ms > 0.02f)
                return true;
            for (size_t j = 0; j < history[i].cpp_methods.size(); j++) {
                const CppMethodSnapshot &method = history[i].cpp_methods[j];
                if (method.calls == 0) continue;
                if (cocos_debug_snapshot_text(method.name).find("Renderer:render") != std::string::npos ||
                    cocos_debug_snapshot_text(method.name).find("Scene:render") != std::string::npos)
                    return true;
            }
        }
        return false;
    }

    static float sceneRenderMs(const FrameSnapshot &frame) {
        return frame.scene_render_ms;
    }

    void fillPlotWindow(double view_xmin, double view_xmax) {
        size_t begin = 0;
        size_t end = history.size();
        size_t i;
        size_t n;
        while (begin < end && (double)history[begin].lua_frame_index < view_xmin)
            begin++;
        while (end > begin && (double)history[end - 1].lua_frame_index > view_xmax)
            end--;
        n = end - begin;
        plot_begin_index = begin;
        plot_x.resize(n);
        plot_draw.resize(n);
        plot_delta.resize(n);
        plot_render.resize(n);
        plot_lua_wall.resize(n);
        plot_lua_cpu.resize(n);
        plot_lua_self.resize(n);
        plot_c_self.resize(n);
        plot_lua_outside.resize(n);
        plot_lua_mem.resize(n);
        plot_proc_ws.resize(n);
        plot_proc_private.resize(n);
        plot_cpp_labels.clear();
        plot_cpp_series.clear();
        if (show_cpp_hooks_plot && n > 0) {
            std::map<std::string, size_t> cpp_index;
            for (i = 0; i < n; i++) {
                const FrameSnapshot &frame = history[begin + i];
                for (size_t j = 0; j < frame.cpp_methods.size(); j++) {
                    const std::string &name =
                        cocos_debug_snapshot_text(frame.cpp_methods[j].name);
                    if (name.empty() || isPrimaryCppPlotName(name)) continue;
                    if (cpp_index.find(name) != cpp_index.end()) continue;
                    if (plot_cpp_labels.size() >= 20) break;
                    cpp_index[name] = plot_cpp_labels.size();
                    plot_cpp_labels.push_back(shortCppHookLabel(name));
                }
                if (plot_cpp_labels.size() >= 20) break;
            }
            plot_cpp_series.resize(plot_cpp_labels.size());
            for (size_t s = 0; s < plot_cpp_series.size(); s++) {
                plot_cpp_series[s].assign(n, 0.0);
            }
            for (i = 0; i < n; i++) {
                const FrameSnapshot &frame = history[begin + i];
                for (size_t j = 0; j < frame.cpp_methods.size(); j++) {
                    const CppMethodSnapshot &method = frame.cpp_methods[j];
                    const std::string &name = cocos_debug_snapshot_text(method.name);
                    std::map<std::string, size_t>::iterator it = cpp_index.find(name);
                    if (it == cpp_index.end()) continue;
                    plot_cpp_series[it->second][i] = (double)method.total_ms;
                }
            }
        }
        for (i = 0; i < n; i++) {
            const FrameSnapshot &frame = history[begin + i];
            plot_x[i] = (double)frame.lua_frame_index;
            plot_draw[i] = frame.drawscene_body_ms;
            plot_delta[i] = frame.frame_period_ms;
            plot_render[i] = frame.scene_render_ms;
            plot_lua_wall[i] = frame.lua_wall_ms;
            plot_lua_cpu[i] = frame.lua_cpu_ms;
            plot_lua_self[i] = frame.lua_script_self_ms;
            plot_c_self[i] = frame.lua_c_self_ms;
            plot_lua_outside[i] = frame.lua_outside_ms;
            plot_lua_mem[i] = (double)frame.lua_mem_kb / 1024.0;
            plot_proc_ws[i] = (double)frame.proc_ws_kb / 1024.0;
            plot_proc_private[i] = (double)frame.proc_private_kb / 1024.0;
        }
    }

    void renderPlot() {
        if (history.empty()) {
            ImGui::TextDisabled("Start profiling to receive real-time frames.");
            return;
        }
        double view_xmin = (double)history.back().lua_frame_index - (double)frame_window;
        double view_xmax = (double)history.back().lua_frame_index;
        if (paused && last_plot_xmax > last_plot_xmin) {
            view_xmin = last_plot_xmin;
            view_xmax = last_plot_xmax;
        }
        fillPlotWindow(view_xmin, view_xmax);

        float full_width = ImGui::GetContentRegionAvail().x;
        float preview_width = 300.0f;
        float plot_width = full_width - preview_width - 12.0f;
        if (plot_width < 320.0f) plot_width = full_width * 0.65f;
        preview_width = full_width - plot_width - 12.0f;

        uint32_t hover_frame = 0;
        bool plot_hovered = false;
        const double thr = (double)spike_threshold_ms;
        std::vector<double> &x = plot_x;
        std::vector<double> &draw = plot_draw;
        std::vector<double> &delta = plot_delta;
        std::vector<double> &render = plot_render;
        std::vector<double> &lua_wall = plot_lua_wall;
        std::vector<double> &lua_cpu = plot_lua_cpu;
        std::vector<double> &lua_self = plot_lua_self;
        std::vector<double> &c_self = plot_c_self;
        std::vector<double> &lua_outside = plot_lua_outside;
        std::vector<double> &lua_mem = plot_lua_mem;
        std::vector<double> &proc_ws = plot_proc_ws;
        std::vector<double> &proc_private = plot_proc_private;
        bool proc_mem_visible = false;
        double proc_lo = 0.0;
        double proc_hi = 0.0;
        double ws_lo = 0.0;
        double ws_hi = 0.0;
        double priv_lo = 0.0;
        double priv_hi = 0.0;
        bool ws_seen = false;
        bool priv_seen = false;
        for (size_t i = 0; i < proc_ws.size(); i++) {
            if (proc_ws[i] > 0.0) {
                if (!ws_seen) { ws_lo = ws_hi = proc_ws[i]; ws_seen = true; }
                else {
                    if (proc_ws[i] < ws_lo) ws_lo = proc_ws[i];
                    if (proc_ws[i] > ws_hi) ws_hi = proc_ws[i];
                }
            }
            if (proc_private[i] > 0.0) {
                if (!priv_seen) { priv_lo = priv_hi = proc_private[i]; priv_seen = true; }
                else {
                    if (proc_private[i] < priv_lo) priv_lo = proc_private[i];
                    if (proc_private[i] > priv_hi) priv_hi = proc_private[i];
                }
            }
        }
        if (ws_seen || priv_seen) {
            proc_mem_visible = true;
            proc_lo = ws_seen ? ws_lo : priv_lo;
            proc_hi = ws_seen ? ws_hi : priv_hi;
            if (priv_seen && priv_lo < proc_lo) proc_lo = priv_lo;
            if (priv_seen && priv_hi > proc_hi) proc_hi = priv_hi;
        }

        /* Visible-window summaries use v7 Body for slow frames and Lua wall for Lua heat. */
        int over_count = 0;
        double worst_ms = 0.0;
        uint32_t worst_frame = 0;
        double hottest_lua_ms = 0.0;
        uint32_t hottest_lua_frame = 0;
        for (size_t i = 0; i < x.size(); i++) {
            const FrameSnapshot &frame = history[plot_begin_index + i];
            if (lua_cpu[i] > hottest_lua_ms) {
                hottest_lua_ms = lua_cpu[i];
                hottest_lua_frame = frame.lua_frame_index;
            }
            if (draw[i] <= thr) continue;
            over_count++;
            if (draw[i] > worst_ms) {
                worst_ms = draw[i];
                worst_frame = frame.lua_frame_index;
            }
        }
        if (show_spike_hints) {
            if (over_count > 0) {
                ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.2f, 1.0f),
                                   "Slow DrawScene: %d frames  |  worst #%u %.2f ms  (limit %.2f)",
                                   over_count, (unsigned)worst_frame, worst_ms, spike_threshold_ms);
            } else {
                ImGui::TextDisabled("DrawScene within budget (<= %.2f ms)", spike_threshold_ms);
            }
            ImGui::SameLine();
            ImGui::Text(" | Lua hot #%u %.2f ms 总耗时",
                        (unsigned)hottest_lua_frame, hottest_lua_ms);
            if (!history.empty() && history.back().drawscene_body_ms > spike_threshold_ms) {
                ImGui::SameLine();
                ImGui::TextColored(ImVec4(1.0f, 0.25f, 0.2f, 1.0f),
                                   "  latest #%u  %.2f ms",
                                   (unsigned)history.back().lua_frame_index,
                                   history.back().drawscene_body_ms);
            }
        }
        if (proc_mem_visible) {
            if (show_spike_hints) ImGui::SameLine();
            ImGui::Text(show_spike_hints ? " | 进程内存 %.0f–%.0f MB | 专用提交 %.0f–%.0f MB"
                                         : "进程内存 %.0f–%.0f MB | 专用提交 %.0f–%.0f MB",
                        ws_lo, ws_hi, priv_lo, priv_hi);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("进程内存与任务管理器进程列表的「内存」列相同，\n"
                                  "统计的是活动专用工作集。专用提交是私有提交量。");
        }
        if (!lua_mem.empty()) {
            double mem_min = lua_mem[0];
            double mem_max = lua_mem[0];
            for (size_t i = 1; i < lua_mem.size(); i++) {
                if (lua_mem[i] < mem_min) mem_min = lua_mem[i];
                if (lua_mem[i] > mem_max) mem_max = lua_mem[i];
            }
            if (show_spike_hints || proc_mem_visible) ImGui::SameLine();
            ImGui::Text(show_spike_hints || proc_mem_visible
                            ? " | Lua 内存 %.1f–%.1f MB"
                            : "Lua 内存 %.1f–%.1f MB",
                        mem_min, mem_max);
        }

        ImGui::BeginChild("##plot_pane", ImVec2(plot_width, 300), false);
        if (ImPlot::BeginPlot("Real-time performance", ImVec2(-1, -1))) {
            ImPlot::SetupAxes("frame", "ms");
            /* Process memory owns the inner right axis. Lua stays on an outer
               axis so its ~170 MB scale is not what gets read as process RAM. */
            ImPlot::SetupAxis(ImAxis_Y2,
                              proc_mem_visible ? "进程 MB" : "MB",
                              ImPlotAxisFlags_AuxDefault);
            ImPlot::SetupAxisFormat(ImAxis_Y2, proc_mem_visible ? "%.0f" : "%.1f");
            if (proc_mem_visible) {
                ImPlot::SetupAxis(ImAxis_Y3, "Lua MB", ImPlotAxisFlags_AuxDefault);
                ImPlot::SetupAxisFormat(ImAxis_Y3, "%.1f");
            }
            if (!paused && !x.empty()) {
                double xmax = x.back();
                ImPlot::SetupAxisLimits(ImAxis_X1, std::max(0.0, xmax - frame_window),
                                        std::max((double)frame_window, xmax), ImPlotCond_Always);
            }
            if (!lua_mem.empty()) {
                /* Keep at least an 8 MB span so tiny GC steps don't Autofit
                   into a full-height staircase on the secondary axis. */
                double mem_lo = lua_mem[0];
                double mem_hi = lua_mem[0];
                for (size_t i = 1; i < lua_mem.size(); i++) {
                    if (lua_mem[i] < mem_lo) mem_lo = lua_mem[i];
                    if (lua_mem[i] > mem_hi) mem_hi = lua_mem[i];
                }
                double span = mem_hi - mem_lo;
                if (span < 8.0) {
                    double mid = 0.5 * (mem_lo + mem_hi);
                    mem_lo = mid - 4.0;
                    mem_hi = mid + 4.0;
                    if (mem_lo < 0.0) {
                        mem_hi -= mem_lo;
                        mem_lo = 0.0;
                    }
                } else {
                    double pad = span * 0.1;
                    mem_lo -= pad;
                    mem_hi += pad;
                    if (mem_lo < 0.0) mem_lo = 0.0;
                }
                ImPlot::SetupAxisLimits(proc_mem_visible ? ImAxis_Y3 : ImAxis_Y2,
                                        mem_lo, mem_hi,
                                        paused ? ImPlotCond_Once : ImPlotCond_Always);
            }
            if (proc_mem_visible) {
                /* Keep at least a 64 MB span so small allocator noise does
                   not Autofit into a full-height line on the process axis. */
                double axis_lo = proc_lo;
                double axis_hi = proc_hi;
                double span = axis_hi - axis_lo;
                if (span < 64.0) {
                    double mid = 0.5 * (axis_lo + axis_hi);
                    axis_lo = mid - 32.0;
                    axis_hi = mid + 32.0;
                    if (axis_lo < 0.0) {
                        axis_hi -= axis_lo;
                        axis_lo = 0.0;
                    }
                } else {
                    double pad = span * 0.1;
                    axis_lo -= pad;
                    axis_hi += pad;
                    if (axis_lo < 0.0) axis_lo = 0.0;
                }
                ImPlot::SetupAxisLimits(ImAxis_Y2, axis_lo, axis_hi,
                                        paused ? ImPlotCond_Once : ImPlotCond_Always);
            }
            if (!x.empty()) {
                ImPlot::PlotLine("DrawScene", &x[0], &draw[0], (int)x.size());
                ImPlot::PlotLine("DeltaTime", &x[0], &delta[0], (int)x.size());
                if (rendererHasSamples())
                    ImPlot::PlotLine("render", &x[0], &render[0], (int)x.size());
                if (show_cpp_hooks_plot) {
                    for (size_t s = 0; s < plot_cpp_series.size() && s < plot_cpp_labels.size();
                         s++) {
                        if (plot_cpp_series[s].empty()) continue;
                        ImPlot::PlotLine(plot_cpp_labels[s].c_str(), &x[0],
                                         &plot_cpp_series[s][0], (int)x.size());
                    }
                }
                ImPlot::PlotLine("Lua wall", &x[0], &lua_wall[0], (int)x.size());
                ImPlot::PlotLine("Lua 总耗时", &x[0], &lua_cpu[0], (int)x.size());
                ImPlot::PlotLine("Lua 脚本", &x[0], &lua_self[0], (int)x.size());
                ImPlot::PlotLine("C", &x[0], &c_self[0], (int)x.size());
                ImPlot::PlotLine("Lua outside", &x[0], &lua_outside[0], (int)x.size());
                if (proc_mem_visible) {
                    ImPlot::SetAxes(ImAxis_X1, ImAxis_Y2);
                    ImPlot::PlotLine("进程内存", &x[0], &proc_ws[0], (int)x.size(), {
                        ImPlotProp_LineColor, ImVec4(0.35f, 0.85f, 0.95f, 1.0f)
                    });
                    ImPlot::PlotLine("专用提交", &x[0], &proc_private[0], (int)x.size(), {
                        ImPlotProp_LineColor, ImVec4(0.75f, 0.45f, 0.95f, 1.0f)
                    });
                    ImPlot::SetAxes(ImAxis_X1, ImAxis_Y3);
                } else {
                    ImPlot::SetAxes(ImAxis_X1, ImAxis_Y2);
                }
                ImPlot::PlotLine("Lua 内存", &x[0], &lua_mem[0], (int)x.size(), {
                    ImPlotProp_LineColor, ImVec4(0.95f, 0.85f, 0.25f, 1.0f)
                });
                ImPlot::SetAxes(ImAxis_X1, ImAxis_Y1);
            }
            {
                ImPlotRect limits = ImPlot::GetPlotLimits();
                last_plot_xmin = limits.X.Min;
                last_plot_xmax = limits.X.Max;
                view_xmin = limits.X.Min;
                view_xmax = limits.X.Max;
            }

            if (show_spike_hints && thr > 0.0) {
                double budget = thr;
                ImPlot::PlotInfLines("budget", &budget, 1, {
                    ImPlotProp_Flags, ImPlotInfLinesFlags_Horizontal | ImPlotItemFlags_NoLegend,
                    ImPlotProp_LineColor, ImVec4(1.0f, 0.35f, 0.15f, 0.85f),
                    ImPlotProp_LineWeight, 1.5f
                });
                ImPlot::TagY(budget, ImVec4(1.0f, 0.35f, 0.15f, 1.0f), "%.2f", budget);

                struct Spike {
                    size_t index;
                    double excess;
                };
                std::vector<Spike> spikes;
                spikes.reserve(64);
                for (size_t i = 0; i < x.size(); i++) {
                    if (x[i] < view_xmin || x[i] > view_xmax) continue;
                    if (draw[i] <= thr) continue;
                    /* Prefer local peaks so dense overruns don't flood annotations. */
                    bool peak = true;
                    if (i > 0 && x[i - 1] >= view_xmin && draw[i] < draw[i - 1]) peak = false;
                    if (i + 1 < x.size() && x[i + 1] <= view_xmax && draw[i] < draw[i + 1])
                        peak = false;
                    if (!peak) continue;
                    Spike s;
                    s.index = i;
                    s.excess = draw[i] - thr;
                    spikes.push_back(s);
                }
                std::sort(spikes.begin(), spikes.end(),
                          [](const Spike &a, const Spike &b) { return a.excess > b.excess; });
                const int kMaxMarks = 10;
                int mark_n = (int)spikes.size() < kMaxMarks ? (int)spikes.size() : kMaxMarks;
                if (mark_n > 0) {
                    std::vector<double> sx((size_t)mark_n), sy((size_t)mark_n);
                    for (int k = 0; k < mark_n; k++) {
                        size_t i = spikes[(size_t)k].index;
                        sx[(size_t)k] = x[i];
                        sy[(size_t)k] = draw[i];
                        ImPlot::Annotation(x[i], draw[i],
                                           ImVec4(1.0f, 0.35f, 0.15f, 1.0f),
                                           ImVec2(0, -12), true,
                                           "#%u  %.1f",
                                           (unsigned)x[i],
                                           draw[i]);
                    }
                    ImPlot::PlotScatter("over budget", &sx[0], &sy[0], mark_n, {
                        ImPlotProp_Marker, ImPlotMarker_Circle,
                        ImPlotProp_MarkerSize, 5.0f,
                        ImPlotProp_MarkerFillColor, ImVec4(1.0f, 0.3f, 0.15f, 0.95f),
                        ImPlotProp_MarkerLineColor, ImVec4(1.0f, 0.85f, 0.4f, 1.0f),
                        ImPlotProp_Flags, ImPlotItemFlags_NoLegend
                    });
                }
            }

            plot_hovered = ImPlot::IsPlotHovered();
            if (plot_hovered && !x.empty()) {
                double mouse_x = ImPlot::GetPlotMousePos().x;
                size_t best = 0;
                double best_distance = 1e100;
                for (size_t i = 0; i < x.size(); i++) {
                    double distance = x[i] > mouse_x ? x[i] - mouse_x : mouse_x - x[i];
                    if (distance < best_distance) { best = i; best_distance = distance; }
                }
                hover_frame = (uint32_t)x[best];
                if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) &&
                    plot_begin_index + best < history.size())
                    selected_sequence = history[plot_begin_index + best].publish_sequence;
                /* Hover tip when cursor is on an over-budget sample. */
                if (show_spike_hints && draw[best] > thr) {
                    ImGui::BeginTooltip();
                    ImGui::Text("Frame #%u Body over budget",
                                (unsigned)x[best]);
                    ImGui::Text("Body %.2f ms  (limit %.2f)",
                                draw[best], spike_threshold_ms);
                    ImGui::Text("Lua wall %.2f  总耗时 %.2f  脚本 %.2f  C %.2f",
                                lua_wall[best], lua_cpu[best],
                                lua_self[best], c_self[best]);
                    {
                        double prev_mb = (best > 0) ? lua_mem[best - 1] : lua_mem[best];
                        ImGui::Text("Lua 内存 %.1f MB (%+.0f KB)",
                                    lua_mem[best],
                                    (lua_mem[best] - prev_mb) * 1024.0);
                    }
                    {
                        double prev_ws = (best > 0) ? proc_ws[best - 1] : proc_ws[best];
                        double prev_priv = (best > 0) ? proc_private[best - 1] : proc_private[best];
                        ImGui::Text("进程内存 %.1f MB (%+.0f KB)  专用提交 %.1f MB (%+.0f KB)",
                                    proc_ws[best],
                                    (proc_ws[best] - prev_ws) * 1024.0,
                                    proc_private[best],
                                    (proc_private[best] - prev_priv) * 1024.0);
                    }
                    ImGui::EndTooltip();
                }
            }
            ImPlot::EndPlot();
        }
        ImGui::EndChild();

        ImGui::SameLine();
        ImGui::BeginChild("##preview_pane", ImVec2(preview_width, 300), true);
        {
            const FramePreviewEntry *entry = NULL;
            const char *mode = "live";
            if (plot_hovered && hover_frame != 0) {
                entry = g_preview.findNearest(hover_frame);
                mode = "hover";
            } else if (!paused) {
                entry = g_preview.latest();
                mode = "live";
            } else {
                const FrameSnapshot *selected = selectedFrame();
                if (selected) {
                    entry = g_preview.findNearest(selected->lua_frame_index);
                    mode = "paused";
                } else {
                    entry = NULL;
                    mode = "paused";
                }
            }

            ImGui::Text("Preview (%s)", mode);
            if (entry && entry->texture) {
                ImGui::Text("frame #%u", (unsigned)entry->frame_index);
                float avail_w = ImGui::GetContentRegionAvail().x;
                float avail_h = ImGui::GetContentRegionAvail().y - 4.0f;
                float scale = 1.0f;
                if (entry->width > 0 && entry->height > 0) {
                    float sx = avail_w / (float)entry->width;
                    float sy = avail_h / (float)entry->height;
                    scale = sx < sy ? sx : sy;
                    if (scale > 1.0f) scale = 1.0f;
                }
                ImVec2 size((float)entry->width * scale, (float)entry->height * scale);
                ImGui::Image((ImTextureID)entry->texture, size);
            } else {
                ImGui::TextDisabled("no capture");
            }
        }
        ImGui::EndChild();
    }

    static bool containsInsensitive(const std::string &text, const char *needle) {
        if (!needle || !needle[0]) return true;
        std::string lhs = text, rhs = needle;
        std::transform(lhs.begin(), lhs.end(), lhs.begin(), ::tolower);
        std::transform(rhs.begin(), rhs.end(), rhs.begin(), ::tolower);
        return lhs.find(rhs) != std::string::npos;
    }

    static const char *luaMethodKindName(uint16_t kind) {
        switch (kind) {
        case COCOS_DEBUG_IPC_LUA_METHOD_LUA: return "Lua";
        case COCOS_DEBUG_IPC_LUA_METHOD_C: return "C";
        case COCOS_DEBUG_IPC_LUA_METHOD_MAIN: return "main";
        default: return "未知";
        }
    }

    static std::string frameFlagsText(uint32_t flags) {
        struct FlagName { uint32_t bit; const char *name; };
        static const FlagName names[] = {
            {COCOS_DEBUG_IPC_FRAME_EVENT_DROPPED, "事件被丢弃"},
            {COCOS_DEBUG_IPC_FRAME_METHOD_LIMITED, "方法数到上限"},
            {COCOS_DEBUG_IPC_FRAME_THREAD_LIMITED, "线程数到上限"},
            {COCOS_DEBUG_IPC_FRAME_LINE_LIMITED, "行采样到上限"},
            {COCOS_DEBUG_IPC_FRAME_LINE_CAPTURED, "已采集行"},
            {COCOS_DEBUG_IPC_FRAME_STACK_DESYNC, "栈不同步"},
            {COCOS_DEBUG_IPC_FRAME_STATE_CONCURRENT, "状态并发"},
            {COCOS_DEBUG_IPC_FRAME_INTERVAL_LIMITED, "区间数到上限"}
        };
        std::string text;
        uint32_t unknown = flags;
        for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
            if (!(flags & names[i].bit)) continue;
            if (!text.empty()) text += " | ";
            text += names[i].name;
            unknown &= ~names[i].bit;
        }
        if (unknown) {
            char value[24];
            _snprintf(value, sizeof(value), "%s0x%X",
                      text.empty() ? "" : " | ", (unsigned)unknown);
            value[sizeof(value) - 1] = 0;
            text += value;
        }
        return text.empty() ? "-" : text;
    }

    void renderFrameDetails() {
        const FrameSnapshot *frame = selectedFrame();
        if (!frame) return;
        ImGui::SeparatorText("选中帧");
        ImGui::Text("Lua 帧 %u | 渲染帧 %u | 帧间隔 %.3f ms | 主体 %.3f ms | "
                    "事件 %u | 跟踪事件 %u",
                    frame->lua_frame_index, frame->render_frame_sequence,
                    frame->frame_period_ms, frame->drawscene_body_ms,
                    frame->hook_events, (unsigned)frame->trace_events.size());
        ImGui::Text("Lua 墙钟 %.3f ms | 总耗时 %.3f ms | 脚本 %.3f ms | C %.3f ms | "
                    "窗口外 %.3f ms",
                    frame->lua_wall_ms, frame->lua_cpu_ms,
                    frame->lua_script_self_ms, frame->lua_c_self_ms,
                    frame->lua_outside_ms);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("总耗时 = 脚本 + C（由构造保证）。\n"
                              "脚本 = Lua/main 独占；C = 从 Lua 调入的 C 函数\n"
                              "以及 lua_pcall/lua_call/lua_resume 空栈上的 VM 开销。\n"
                              "墙钟是多线程忙区间并集，只作参考。\n"
                              "窗口外 = 窗口外 Lua，不进总耗时。");
        {
            /* Lua memory is the only GC-pressure signal Lua 5.1 gives us
               (no generational stats, no pause/step counters): a sawtooth is
               normal, a monotone climb is a leak, and a jump inside one frame
               is a table/string storm. */
            size_t index = frameIndexOf(frame);
            float previous_mem = 0.0f;
            bool has_previous = index != (size_t)-1 && index > 0;
            if (has_previous) previous_mem = history[index - 1].lua_mem_kb;
            ImGui::Text("Lua 内存 %.1f KB（%+.1f KB/帧）| 观察者开销约 %.3f ms "
                        "（%u 次 hook）",
                        frame->lua_mem_kb,
                        has_previous ? frame->lua_mem_kb - previous_mem : 0.0f,
                        (double)frame->hook_events * kHookOverheadMsPerEvent,
                        frame->hook_events);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("观察者开销是分析器自己的 hook 成本\n"
                                  "（每次事件约 0.23 微秒）。它算在主体里，不算进任何\n"
                                  "Lua 指标。先扣掉它，再判断是不是 Lua 的问题。");
        }
        {
            size_t index = frameIndexOf(frame);
            float prev_ws = 0.0f;
            float prev_priv = 0.0f;
            bool has_previous = index != (size_t)-1 && index > 0;
            if (has_previous) {
                prev_ws = history[index - 1].proc_ws_kb;
                prev_priv = history[index - 1].proc_private_kb;
            }
            ImGui::Text("进程内存 %.1f MB（%+.1f KB）| 专用提交 %.1f MB（%+.1f KB）",
                        frame->proc_ws_kb / 1024.0f,
                        has_previous ? frame->proc_ws_kb - prev_ws : 0.0f,
                        frame->proc_private_kb / 1024.0f,
                        has_previous ? frame->proc_private_kb - prev_priv : 0.0f);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("进程内存与任务管理器进程列表的「内存」列相同，\n"
                                  "统计的是活动专用工作集。专用提交是私有提交量，\n"
                                  "纹理和 C++ 分配会在这里变化。");
        }
        ImGui::TextDisabled("帧时间 %.6f → %.6f 秒",
                            frame->frame_begin_time_seconds,
                            frame->frame_end_time_seconds);
        if (show_spike_hints && frame->drawscene_body_ms > spike_threshold_ms) {
            ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.15f, 1.0f),
                               "主体超出预算 %.2f ms（上限 %.2f）",
                               frame->drawscene_body_ms - spike_threshold_ms,
                               spike_threshold_ms);
        }
        if (frame->flags) {
            std::string flags = frameFlagsText(frame->flags);
            ImGui::TextColored(ImVec4(1, .45f, .2f, 1),
                               "采集标记：%s（0x%X）",
                               flags.c_str(), frame->flags);
        }
        if (ImGui::CollapsingHeader("Lua 热点行##frame_details",
                frame->lua_lines.empty() ? 0 : ImGuiTreeNodeFlags_DefaultOpen)) {
            if (frame->lua_lines.empty()) {
                ImGui::TextDisabled("本帧没有行采样。点击 Capture Lua details，或等一次 Lua 尖刺。");
            } else {
                std::vector<size_t> line_order;
                uint64_t total_line_samples = 0;
                double total_line_ms = 0.0;
                for (size_t i = 0; i < frame->lua_lines.size(); i++)
                {
                    line_order.push_back(i);
                    total_line_samples += frame->lua_lines[i].samples;
                    total_line_ms += (double)frame->lua_lines[i].line_ms;
                }
                if (ImGui::BeginTable("lua_lines", 6,
                        ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                        ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable |
                        ImGuiTableFlags_Sortable,
                        ImVec2(-1, 180))) {
                    ImGui::TableSetupColumn("源文件");
                    ImGui::TableSetupColumn("行");
                    ImGui::TableSetupColumn("耗时 ms", ImGuiTableColumnFlags_DefaultSort |
                                                       ImGuiTableColumnFlags_PreferSortDescending);
                    ImGui::TableSetupColumn("采样次数");
                    ImGui::TableSetupColumn("占比");
                    ImGui::TableSetupColumn("函数");
                    ImGui::TableHeadersRow();
                    ImGuiTableSortSpecs *sort_specs = ImGui::TableGetSortSpecs();
                    int sort_column = 2;
                    bool ascending = false;
                    if (sort_specs && sort_specs->SpecsCount > 0) {
                        sort_column = sort_specs->Specs[0].ColumnIndex;
                        ascending = sort_specs->Specs[0].SortDirection == ImGuiSortDirection_Ascending;
                    }
                    std::sort(line_order.begin(), line_order.end(),
                              [frame, sort_column, ascending](size_t a, size_t b) {
                        const LuaLineSnapshot &lhs = frame->lua_lines[a];
                        const LuaLineSnapshot &rhs = frame->lua_lines[b];
                        int comparison = 0;
                        switch (sort_column) {
                        case 0: comparison = cocos_debug_snapshot_text(lhs.source).compare(
                                cocos_debug_snapshot_text(rhs.source)); break;
                        case 1: comparison = lhs.line < rhs.line ? -1 : lhs.line > rhs.line; break;
                        case 2: comparison = lhs.line_ms < rhs.line_ms ? -1 : lhs.line_ms > rhs.line_ms; break;
                        case 3: comparison = lhs.samples < rhs.samples ? -1 : lhs.samples > rhs.samples; break;
                        case 4: comparison = lhs.samples < rhs.samples ? -1 : lhs.samples > rhs.samples; break;
                        default: comparison = cocos_debug_snapshot_text(lhs.function_name).compare(
                                cocos_debug_snapshot_text(rhs.function_name)); break;
                        }
                        return ascending ? comparison < 0 : comparison > 0;
                    });
                    ImGuiListClipper clipper;
                    clipper.Begin((int)line_order.size());
                    while (clipper.Step()) {
                        for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; row++) {
                            const LuaLineSnapshot &item = frame->lua_lines[line_order[row]];
                            const std::string &item_source = cocos_debug_snapshot_text(item.source);
                            ImGui::TableNextRow();
                            ImGui::TableNextColumn();
                            ImGui::TextUnformatted(item_source.c_str());
                            if (ImGui::IsItemHovered())
                                ImGui::SetTooltip("%s", item_source.c_str());
                            ImGui::TableNextColumn(); ImGui::Text("%d", (int)item.line);
                            ImGui::TableNextColumn(); ImGui::Text("%.3f", item.line_ms);
                            ImGui::TableNextColumn(); ImGui::Text("%u", item.samples);
                            ImGui::TableNextColumn(); ImGui::Text(
                                "%.1f%%", total_line_ms > 0.0
                                    ? 100.0 * (double)item.line_ms / total_line_ms
                                    : (total_line_samples
                                        ? 100.0 * (double)item.samples /
                                          (double)total_line_samples
                                        : 0.0));
                            ImGui::TableNextColumn(); ImGui::TextUnformatted(
                                cocos_debug_snapshot_text(item.function_name).c_str());
                        }
                    }
                    ImGui::EndTable();
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("耗时 ms 是这一行的墙钟时间，包含从这一行调进 C 函数的时间。\n"
                                          "采样次数按虚拟机指令批计数（每 4000 条指令一次），看不到 C 里的时间。\n"
                                          "按耗时排序，不要按采样次数。");
                }
            }
        }
        ImGui::InputTextWithHint("##method_filter", "过滤方法",
                                 method_filter, sizeof(method_filter));
        ImGui::SameLine();
        ImGui::RadioButton("全部", &method_kind_filter, 0);
        ImGui::SameLine();
        ImGui::RadioButton("Lua 脚本", &method_kind_filter, 1);
        ImGui::SameLine();
        ImGui::RadioButton("C", &method_kind_filter, 2);
        std::vector<size_t> order;
        for (size_t i = 0; i < frame->lua_methods.size(); i++) {
            uint16_t kind = frame->lua_methods[i].kind;
            if (method_kind_filter == 1 &&
                kind == COCOS_DEBUG_IPC_LUA_METHOD_C)
                continue;
            if (method_kind_filter == 2 &&
                kind != COCOS_DEBUG_IPC_LUA_METHOD_C)
                continue;
            if (containsInsensitive(cocos_debug_snapshot_text(frame->lua_methods[i].name),
                                    method_filter))
                order.push_back(i);
        }
        if (ImGui::BeginTable("lua_methods", 9,
                ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable |
                ImGuiTableFlags_Sortable | ImGuiTableFlags_ScrollX,
                ImVec2(-1, 230))) {
            ImGui::TableSetupColumn("方法");
            ImGui::TableSetupColumn("类型");
            ImGui::TableSetupColumn("此帧总耗时", ImGuiTableColumnFlags_DefaultSort |
                                                   ImGuiTableColumnFlags_PreferSortDescending);
            ImGui::TableSetupColumn("含下级");
            ImGui::TableSetupColumn("单次最大自身");
            ImGui::TableSetupColumn("单次最大含下级");
            ImGui::TableSetupColumn("总调用次数");
            ImGui::TableSetupColumn("已返回");
            ImGui::TableSetupColumn("帧末未返回");
            ImGui::TableHeadersRow();
            ImGuiTableSortSpecs *sort_specs = ImGui::TableGetSortSpecs();
            int sort_column = 2;
            bool ascending = false;
            if (sort_specs && sort_specs->SpecsCount > 0) {
                sort_column = sort_specs->Specs[0].ColumnIndex;
                ascending = sort_specs->Specs[0].SortDirection == ImGuiSortDirection_Ascending;
            }
            std::sort(order.begin(), order.end(), [frame, sort_column, ascending](size_t a, size_t b) {
                const LuaMethodSnapshot &lhs = frame->lua_methods[a];
                const LuaMethodSnapshot &rhs = frame->lua_methods[b];
                int comparison = 0;
                switch (sort_column) {
                case 0: comparison = cocos_debug_snapshot_text(lhs.name).compare(
                        cocos_debug_snapshot_text(rhs.name)); break;
                case 1: comparison = lhs.kind < rhs.kind ? -1 : lhs.kind > rhs.kind; break;
                case 2: comparison = lhs.self_ms < rhs.self_ms ? -1 : lhs.self_ms > rhs.self_ms; break;
                case 3: comparison = lhs.inclusive_ms < rhs.inclusive_ms ? -1 :
                                     lhs.inclusive_ms > rhs.inclusive_ms; break;
                case 4: comparison = lhs.max_self_ms < rhs.max_self_ms ? -1 :
                                     lhs.max_self_ms > rhs.max_self_ms; break;
                case 5: comparison = lhs.max_inclusive_ms < rhs.max_inclusive_ms ? -1 :
                                     lhs.max_inclusive_ms > rhs.max_inclusive_ms; break;
                case 6: comparison = lhs.calls_started < rhs.calls_started ? -1 :
                                     lhs.calls_started > rhs.calls_started; break;
                case 7: comparison = lhs.calls_completed < rhs.calls_completed ? -1 :
                                     lhs.calls_completed > rhs.calls_completed; break;
                default: comparison = lhs.active_at_end < rhs.active_at_end ? -1 :
                                      lhs.active_at_end > rhs.active_at_end; break;
                }
                return ascending ? comparison < 0 : comparison > 0;
            });
            ImGuiListClipper clipper;
            clipper.Begin((int)order.size());
            while (clipper.Step()) {
                for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; row++) {
                    const LuaMethodSnapshot &item = frame->lua_methods[order[row]];
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn(); ImGui::TextUnformatted(
                        cocos_debug_snapshot_text(item.name).c_str());
                    ImGui::TableNextColumn(); ImGui::TextUnformatted(luaMethodKindName(item.kind));
                    ImGui::TableNextColumn(); ImGui::Text("%.3f", item.self_ms);
                    ImGui::TableNextColumn(); ImGui::Text("%.3f", item.inclusive_ms);
                    ImGui::TableNextColumn(); ImGui::Text("%.3f", item.max_self_ms);
                    ImGui::TableNextColumn(); ImGui::Text("%.3f", item.max_inclusive_ms);
                    ImGui::TableNextColumn(); ImGui::Text("%u", item.calls_started);
                    ImGui::TableNextColumn(); ImGui::Text("%u", item.calls_completed);
                    ImGui::TableNextColumn(); ImGui::Text("%u", item.active_at_end);
                }
            }
            ImGui::EndTable();
        }
        if (ImGui::CollapsingHeader("引擎##frame_details", ImGuiTreeNodeFlags_DefaultOpen) &&
            ImGui::BeginTable("engine_metrics", 2, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
            ImGui::TableSetupColumn("指标");
            ImGui::TableSetupColumn("ms");
            ImGui::TableHeadersRow();
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::TextUnformatted("drawScene 主体");
            ImGui::TableNextColumn(); ImGui::Text("%.3f", frame->drawscene_body_ms);
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::TextUnformatted("帧间隔");
            ImGui::TableNextColumn(); ImGui::Text("%.3f", frame->frame_period_ms);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("包含这次 DrawScene 的帧间隔：\n"
                                  "这次主体，加上呈现、垂直同步和空闲，直到下一次\n"
                                  "drawScene 开始。开了垂直同步时，橙色会贴在 16.67 或 33；\n"
                                  "它不该和蓝色的主体锯齿叠在一起。");
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::TextUnformatted("渲染");
            if (frame->scene_render_hook_present || rendererHookConfigured()) {
                ImGui::TableNextColumn(); ImGui::Text("%.3f", sceneRenderMs(*frame));
            } else {
                ImGui::TableNextColumn();
                ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.2f, 1.0f), "未安装");
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("注入时没有解析到 cc.Renderer:render。\n"
                                      "绿色的渲染曲线不是引擎提交时间。");
            }
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::TextUnformatted("Lua 墙钟");
            ImGui::TableNextColumn(); ImGui::Text("%.3f", frame->lua_wall_ms);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("各 Lua 线程忙区间的并集。\n"
                                  "Lua 总量看总耗时（脚本 + C）。");
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::TextUnformatted("Lua 总耗时");
            ImGui::TableNextColumn(); ImGui::Text("%.3f", frame->lua_cpu_ms);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("总耗时 = 脚本自身 + C 自身");
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::TextUnformatted("Lua 脚本");
            ImGui::TableNextColumn(); ImGui::Text("%.3f", frame->lua_script_self_ms);
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::TextUnformatted("C");
            ImGui::TableNextColumn(); ImGui::Text("%.3f", frame->lua_c_self_ms);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("C / 总耗时 是 Lua 时间里花在\n"
                                  "从 Lua 调入的 C 函数（绑定、标准库、迭代器）\n"
                                  "以及空栈 VM 开销上的比例。\n"
                                  "不是 drawScene / Renderer 这类 C++ hook。");
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::TextUnformatted("窗口外 Lua");
            ImGui::TableNextColumn(); ImGui::Text("%.3f", frame->lua_outside_ms);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("这一帧窗口开始之前那段空隙里跑的 Lua。\n"
                                  "不算进 Lua 总耗时，也不记到任何方法上。");
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::TextUnformatted("观察者开销（估计）");
            ImGui::TableNextColumn(); ImGui::Text("%.3f",
                (double)frame->hook_events * kHookOverheadMsPerEvent);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%u 次 hook 事件 × 约 0.23 微秒。这是分析器\n"
                                  "自己的成本，算在主体里面，所以主体减去它\n"
                                  "才是不插桩时游戏本来的开销。",
                                  frame->hook_events);
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::TextUnformatted("Lua 内存");
            {
                size_t index = frameIndexOf(frame);
                float previous_mem = 0.0f;
                bool has_previous = index != (size_t)-1 && index > 0;
                if (has_previous) previous_mem = history[index - 1].lua_mem_kb;
                ImGui::TableNextColumn();
                ImGui::Text("%.1f KB (%+.1f)", frame->lua_mem_kb,
                            has_previous ? frame->lua_mem_kb - previous_mem : 0.0f);
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Lua 5.1 没有分代回收统计，所以 Lua 总字节数\n"
                                  "是唯一的压力信号：锯齿是正常回收，\n"
                                  "一直往上爬是泄漏。");
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::TextUnformatted("进程内存");
            {
                size_t index = frameIndexOf(frame);
                float previous_mem = 0.0f;
                bool has_previous = index != (size_t)-1 && index > 0;
                if (has_previous) previous_mem = history[index - 1].proc_ws_kb;
                ImGui::TableNextColumn();
                ImGui::Text("%.1f MB (%+.1f KB)", frame->proc_ws_kb / 1024.0f,
                            has_previous ? frame->proc_ws_kb - previous_mem : 0.0f);
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("与任务管理器进程列表的「内存」列相同\n"
                                  "（活动专用工作集）。");
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::TextUnformatted("专用提交");
            {
                size_t index = frameIndexOf(frame);
                float previous_mem = 0.0f;
                bool has_previous = index != (size_t)-1 && index > 0;
                if (has_previous) previous_mem = history[index - 1].proc_private_kb;
                ImGui::TableNextColumn();
                ImGui::Text("%.1f MB (%+.1f KB)", frame->proc_private_kb / 1024.0f,
                            has_previous ? frame->proc_private_kb - previous_mem : 0.0f);
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("进程私有提交。纹理和 C++ 分配会在这里变化，\n"
                                  "Lua 堆统计看不到。");
            ImGui::EndTable();
        }

        if (ImGui::CollapsingHeader("C++ hooks##frame_cpp", ImGuiTreeNodeFlags_DefaultOpen)) {
            if (frame->cpp_methods.empty()) {
                ImGui::TextDisabled("本帧没有 C++ hook 样本。"
                                    "若上方 installed ≤ 3，请重启游戏后重新注入。");
            } else if (ImGui::BeginTable("frame_cpp_hooks", 5,
                                         ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                             ImGuiTableFlags_Resizable |
                                             ImGuiTableFlags_Sortable)) {
                ImGui::TableSetupColumn("钩子");
                ImGui::TableSetupColumn("total ms");
                ImGui::TableSetupColumn("max ms");
                ImGui::TableSetupColumn("last ms");
                ImGui::TableSetupColumn("calls");
                ImGui::TableHeadersRow();
                std::vector<const CppMethodSnapshot *> rows;
                rows.reserve(frame->cpp_methods.size());
                for (size_t i = 0; i < frame->cpp_methods.size(); i++)
                    rows.push_back(&frame->cpp_methods[i]);
                if (ImGuiTableSortSpecs *specs = ImGui::TableGetSortSpecs()) {
                    if (specs->SpecsCount > 0) {
                        const ImGuiTableColumnSortSpecs &spec = specs->Specs[0];
                        std::sort(rows.begin(), rows.end(),
                                  [&](const CppMethodSnapshot *lhs,
                                      const CppMethodSnapshot *rhs) {
                                      int comparison = 0;
                                      switch (spec.ColumnIndex) {
                                      case 0:
                                          comparison =
                                              cocos_debug_snapshot_text(lhs->name)
                                                  .compare(cocos_debug_snapshot_text(rhs->name));
                                          break;
                                      case 1:
                                          comparison = (lhs->total_ms > rhs->total_ms) -
                                                       (lhs->total_ms < rhs->total_ms);
                                          break;
                                      case 2:
                                          comparison = (lhs->max_ms > rhs->max_ms) -
                                                       (lhs->max_ms < rhs->max_ms);
                                          break;
                                      case 3:
                                          comparison = (lhs->last_ms > rhs->last_ms) -
                                                       (lhs->last_ms < rhs->last_ms);
                                          break;
                                      default:
                                          comparison = (lhs->calls > rhs->calls) -
                                                       (lhs->calls < rhs->calls);
                                          break;
                                      }
                                      return spec.SortDirection == ImGuiSortDirection_Ascending
                                                 ? comparison < 0
                                                 : comparison > 0;
                                  });
                    }
                }
                for (size_t i = 0; i < rows.size(); i++) {
                    const CppMethodSnapshot &item = *rows[i];
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(cocos_debug_snapshot_text(item.name).c_str());
                    ImGui::TableNextColumn();
                    ImGui::Text("%.3f", item.total_ms);
                    ImGui::TableNextColumn();
                    ImGui::Text("%.3f", item.max_ms);
                    ImGui::TableNextColumn();
                    ImGui::Text("%.3f", item.last_ms);
                    ImGui::TableNextColumn();
                    ImGui::Text("%u", (unsigned)item.calls);
                }
                ImGui::EndTable();
            }
        }

        if (ImGui::CollapsingHeader("getData 按文件##frame_file_io",
                                    ImGuiTreeNodeFlags_DefaultOpen)) {
            float getdata_total = 0.0f;
            for (size_t i = 0; i < frame->cpp_methods.size(); i++) {
                const CppMethodSnapshot &item = frame->cpp_methods[i];
                if (cocos_debug_snapshot_text(item.name).find(
                        "FileUtilsWin32:getData") != std::string::npos) {
                    getdata_total = item.total_ms;
                    break;
                }
            }
            ImGui::Text("本帧 getData 总耗时 %.3f ms", getdata_total);
            if (frame->flags & COCOS_DEBUG_IPC_FRAME_FILE_IO_LIMITED)
                ImGui::TextDisabled("路径数超限，多余路径合并到 <truncated>");
            if (frame->file_io.empty()) {
                ImGui::TextDisabled("本帧没有按文件的 getData 样本。");
            } else if (ImGui::BeginTable("frame_file_io", 5,
                                         ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                             ImGuiTableFlags_Resizable |
                                             ImGuiTableFlags_Sortable)) {
                ImGui::TableSetupColumn("路径");
                ImGui::TableSetupColumn("total ms");
                ImGui::TableSetupColumn("max ms");
                ImGui::TableSetupColumn("last ms");
                ImGui::TableSetupColumn("calls");
                ImGui::TableHeadersRow();
                std::vector<const FileIoSnapshot *> rows;
                rows.reserve(frame->file_io.size());
                for (size_t i = 0; i < frame->file_io.size(); i++)
                    rows.push_back(&frame->file_io[i]);
                if (ImGuiTableSortSpecs *specs = ImGui::TableGetSortSpecs()) {
                    if (specs->SpecsCount > 0) {
                        const ImGuiTableColumnSortSpecs &spec = specs->Specs[0];
                        std::sort(rows.begin(), rows.end(),
                                  [&](const FileIoSnapshot *lhs,
                                      const FileIoSnapshot *rhs) {
                                      int comparison = 0;
                                      switch (spec.ColumnIndex) {
                                      case 0:
                                          comparison =
                                              cocos_debug_snapshot_text(lhs->path)
                                                  .compare(cocos_debug_snapshot_text(
                                                      rhs->path));
                                          break;
                                      case 1:
                                          comparison = (lhs->total_ms > rhs->total_ms) -
                                                       (lhs->total_ms < rhs->total_ms);
                                          break;
                                      case 2:
                                          comparison = (lhs->max_ms > rhs->max_ms) -
                                                       (lhs->max_ms < rhs->max_ms);
                                          break;
                                      case 3:
                                          comparison = (lhs->last_ms > rhs->last_ms) -
                                                       (lhs->last_ms < rhs->last_ms);
                                          break;
                                      default:
                                          comparison = (lhs->calls > rhs->calls) -
                                                       (lhs->calls < rhs->calls);
                                          break;
                                      }
                                      return spec.SortDirection == ImGuiSortDirection_Ascending
                                                 ? comparison < 0
                                                 : comparison > 0;
                                  });
                    }
                }
                for (size_t i = 0; i < rows.size(); i++) {
                    const FileIoSnapshot &item = *rows[i];
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(cocos_debug_snapshot_text(item.path).c_str());
                    ImGui::TableNextColumn();
                    ImGui::Text("%.3f", item.total_ms);
                    ImGui::TableNextColumn();
                    ImGui::Text("%.3f", item.max_ms);
                    ImGui::TableNextColumn();
                    ImGui::Text("%.3f", item.last_ms);
                    ImGui::TableNextColumn();
                    ImGui::Text("%u", (unsigned)item.calls);
                }
                ImGui::EndTable();
            }
        }
    }

    /* ---------- cross-frame analysis: aggregate / spike diff / curve ---------- */

    size_t aggregateBeginIndex() const {
        size_t n = history.size();
        size_t window = aggregate_window > 0 ? (size_t)aggregate_window : n;
        return n > window ? n - window : 0;
    }

    void rebuildAggregates(bool force) {
        DWORD now = GetTickCount();
        size_t n = history.size();
        uint64_t first = history.empty() ? 0 : history.front().publish_sequence;
        if (!force && n == aggregate_cached_size &&
            first == aggregate_cached_first && now - aggregate_last_build < 500)
            return;
        aggregate_cached_size = n;
        aggregate_cached_first = first;
        aggregate_last_build = now;
        aggregates.clear();
        aggregate_index.clear();
        aggregate_frame_count = 0;
        aggregate_spike_count = 0;
        aggregate_median_baseline_ms = 0;
        if (n == 0) {
            ts_dirty = true;
            return;
        }
        float thr = spike_threshold_ms;
        size_t begin = aggregateBeginIndex();
        std::vector<float> baseline_body;
        baseline_body.reserve(n - begin);
        for (size_t i = begin; i < n; i++) {
            const FrameSnapshot &f = history[i];
            bool spike = f.drawscene_body_ms > thr;
            aggregate_frame_count++;
            if (spike)
                aggregate_spike_count++;
            else
                baseline_body.push_back(f.drawscene_body_ms);
            for (size_t j = 0; j < f.lua_methods.size(); j++) {
                const LuaMethodSnapshot &m = f.lua_methods[j];
                if (!m.method_id) continue;
                std::map<uint32_t, size_t>::iterator it =
                    aggregate_index.find(m.method_id);
                size_t slot;
                if (it == aggregate_index.end()) {
                    MethodAggregate agg;
                    agg.method_id = m.method_id;
                    agg.name = m.name;
                    agg.kind = m.kind;
                    slot = aggregates.size();
                    aggregate_index[m.method_id] = slot;
                    aggregates.push_back(agg);
                } else {
                    slot = it->second;
                }
                MethodAggregate &agg = aggregates[slot];
                agg.hit_frames++;
                agg.self_sum += (double)m.self_ms;
                agg.inclusive_sum += (double)m.inclusive_ms;
                agg.calls_started += m.calls_started;
                if ((double)m.self_ms > agg.self_max) agg.self_max = (double)m.self_ms;
                agg.self_samples.push_back(m.self_ms);
                if (spike) {
                    agg.spike_hits++;
                    agg.spike_self_sum += (double)m.self_ms;
                } else {
                    agg.baseline_hits++;
                    agg.baseline_self_sum += (double)m.self_ms;
                }
            }
        }
        if (!baseline_body.empty()) {
            std::sort(baseline_body.begin(), baseline_body.end());
            aggregate_median_baseline_ms =
                (double)baseline_body[baseline_body.size() / 2];
        }
        ts_dirty = true;
    }

    void rebuildTimeSeries() {
        ts_x.clear();
        ts_y.clear();
        ts_body.clear();
        if (!selected_method_id) return;
        size_t begin = aggregateBeginIndex();
        for (size_t i = begin; i < history.size(); i++) {
            const FrameSnapshot &f = history[i];
            double value = 0.0;
            for (size_t j = 0; j < f.lua_methods.size(); j++) {
                if (f.lua_methods[j].method_id == selected_method_id)
                    value += (double)f.lua_methods[j].self_ms;
            }
            ts_x.push_back((double)f.lua_frame_index);
            ts_y.push_back(value);
            ts_body.push_back((double)f.drawscene_body_ms);
        }
        ts_dirty = false;
    }

    void selectMethod(uint32_t method_id, const std::string *name) {
        selected_method_id = method_id;
        selected_method_label = name ? cocos_debug_snapshot_text(name)
                                     : std::string();
        ts_dirty = true;
    }

    void renderCrossFramePanel() {
        if (!ImGui::CollapsingHeader("跨帧分析##cross_frame"))
            return;
        rebuildAggregates(false);
        ImGui::SetNextItemWidth(220.0f);
        ImGui::SliderInt("窗口（帧）##agg_window", &aggregate_window,
                         60, 4000);
        ImGui::SameLine();
        if (ImGui::Button("刷新##agg_refresh"))
            rebuildAggregates(true);
        ImGui::SameLine();
        ImGui::TextDisabled("预算 %.2f ms", spike_threshold_ms);
        ImGui::Text("帧数 %u | 超预算 %u（%.1f%%）| 安静帧主体中位数 %.2f ms",
                    (unsigned)aggregate_frame_count,
                    (unsigned)aggregate_spike_count,
                    aggregate_frame_count
                        ? 100.0 * (double)aggregate_spike_count /
                              (double)aggregate_frame_count : 0.0,
                    aggregate_median_baseline_ms);
        if (history.empty()) {
            ImGui::TextDisabled("还没有帧。");
            return;
        }
        ImGui::InputTextWithHint("##aggregate_filter", "过滤方法",
                                 aggregate_filter, sizeof(aggregate_filter));
        ImGui::SameLine();
        ImGui::RadioButton("全部##agg_kind", &method_kind_filter, 0);
        ImGui::SameLine();
        ImGui::RadioButton("Lua 脚本##agg_kind", &method_kind_filter, 1);
        ImGui::SameLine();
        ImGui::RadioButton("C##agg_kind", &method_kind_filter, 2);

        std::vector<size_t> order;
        for (size_t i = 0; i < aggregates.size(); i++) {
            uint16_t kind = aggregates[i].kind;
            if (method_kind_filter == 1 &&
                kind == COCOS_DEBUG_IPC_LUA_METHOD_C)
                continue;
            if (method_kind_filter == 2 &&
                kind != COCOS_DEBUG_IPC_LUA_METHOD_C)
                continue;
            if (containsInsensitive(cocos_debug_snapshot_text(aggregates[i].name),
                                    aggregate_filter))
                order.push_back(i);
        }

        /* ---- aggregate table ---- */
        if (ImGui::BeginTable("cross_aggregate", 10,
                ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                ImGuiTableFlags_ScrollY | ImGuiTableFlags_ScrollX |
                ImGuiTableFlags_Resizable | ImGuiTableFlags_Sortable,
                ImVec2(-1, 220))) {
            ImGui::TableSetupColumn("方法");
            ImGui::TableSetupColumn("类型");
            ImGui::TableSetupColumn("出现帧数");
            ImGui::TableSetupColumn("出现比例");
            ImGui::TableSetupColumn("自身合计", ImGuiTableColumnFlags_DefaultSort |
                                                ImGuiTableColumnFlags_PreferSortDescending);
            ImGui::TableSetupColumn("每次出现");
            ImGui::TableSetupColumn("p95 自身");
            ImGui::TableSetupColumn("最大自身");
            ImGui::TableSetupColumn("含下级合计");
            ImGui::TableSetupColumn("调用次数");
            ImGui::TableHeadersRow();
            ImGuiTableSortSpecs *sort_specs = ImGui::TableGetSortSpecs();
            int sort_column = 4;
            bool ascending = false;
            if (sort_specs && sort_specs->SpecsCount > 0) {
                sort_column = sort_specs->Specs[0].ColumnIndex;
                ascending = sort_specs->Specs[0].SortDirection ==
                            ImGuiSortDirection_Ascending;
            }
            std::sort(order.begin(), order.end(),
                      [this, sort_column, ascending](size_t a, size_t b) {
                const MethodAggregate &lhs = aggregates[a];
                const MethodAggregate &rhs = aggregates[b];
                int comparison = 0;
                switch (sort_column) {
                case 0: comparison = cocos_debug_snapshot_text(lhs.name).compare(
                            cocos_debug_snapshot_text(rhs.name)); break;
                case 1: comparison = lhs.kind < rhs.kind ? -1 : lhs.kind > rhs.kind; break;
                case 2: comparison = lhs.hit_frames < rhs.hit_frames ? -1 :
                                     lhs.hit_frames > rhs.hit_frames; break;
                case 3: comparison = lhs.hit_frames < rhs.hit_frames ? -1 :
                                     lhs.hit_frames > rhs.hit_frames; break;
                case 4: comparison = lhs.self_sum < rhs.self_sum ? -1 :
                                     lhs.self_sum > rhs.self_sum; break;
                case 5: comparison = lhs.selfPerHit() < rhs.selfPerHit() ? -1 :
                                     lhs.selfPerHit() > rhs.selfPerHit(); break;
                case 6: comparison = lhs.p95Self() < rhs.p95Self() ? -1 :
                                     lhs.p95Self() > rhs.p95Self(); break;
                case 7: comparison = lhs.self_max < rhs.self_max ? -1 :
                                     lhs.self_max > rhs.self_max; break;
                case 8: comparison = lhs.inclusive_sum < rhs.inclusive_sum ? -1 :
                                     lhs.inclusive_sum > rhs.inclusive_sum; break;
                default: comparison = lhs.calls_started < rhs.calls_started ? -1 :
                                      lhs.calls_started > rhs.calls_started; break;
                }
                return ascending ? comparison < 0 : comparison > 0;
            });
            ImGuiListClipper clipper;
            clipper.Begin((int)order.size());
            while (clipper.Step()) {
                for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; row++) {
                    const MethodAggregate &item = aggregates[order[(size_t)row]];
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::PushID((int)item.method_id);
                    if (ImGui::Selectable(cocos_debug_snapshot_text(item.name).c_str(),
                                          item.method_id == selected_method_id,
                                          ImGuiSelectableFlags_SpanAllColumns))
                        selectMethod(item.method_id, item.name);
                    ImGui::PopID();
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(luaMethodKindName(item.kind));
                    ImGui::TableNextColumn(); ImGui::Text("%u", item.hit_frames);
                    ImGui::TableNextColumn(); ImGui::Text("%.1f%%",
                        aggregate_frame_count
                            ? 100.0 * (double)item.hit_frames /
                              (double)aggregate_frame_count : 0.0);
                    ImGui::TableNextColumn(); ImGui::Text("%.2f", item.self_sum);
                    ImGui::TableNextColumn(); ImGui::Text("%.3f", item.selfPerHit());
                    ImGui::TableNextColumn(); ImGui::Text("%.3f", item.p95Self());
                    ImGui::TableNextColumn(); ImGui::Text("%.3f", item.self_max);
                    ImGui::TableNextColumn(); ImGui::Text("%.2f", item.inclusive_sum);
                    ImGui::TableNextColumn(); ImGui::Text("%llu",
                        (unsigned long long)item.calls_started);
                }
            }
            ImGui::EndTable();
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("出现帧数 = 这个方法出现过的帧数。\n"
                              "出现得少但 p95 很高，是周期性尖刺，不是热循环。\n"
                              "点击一行，画出它每一帧的自身耗时。");

        /* ---- spike attribution ---- */
        std::vector<size_t> spike_order;
        for (size_t i = 0; i < aggregates.size(); i++) {
            const MethodAggregate &item = aggregates[i];
            if (!item.spike_hits) continue;
            if (!containsInsensitive(cocos_debug_snapshot_text(item.name),
                                     aggregate_filter))
                continue;
            spike_order.push_back(i);
        }
        std::sort(spike_order.begin(), spike_order.end(),
                  [this](size_t a, size_t b) {
            double lhs = aggregates[a].spikeDelta();
            double rhs = aggregates[b].spikeDelta();
            if (lhs != rhs) return lhs > rhs;
            return aggregates[a].spike_hits > aggregates[b].spike_hits;
        });
        if (ImGui::CollapsingHeader("尖刺归因（相对安静帧的自身耗时增量）##cross_spike",
                                    ImGuiTreeNodeFlags_DefaultOpen)) {
            if (aggregate_spike_count == 0) {
                ImGui::TextDisabled("这个窗口里没有帧超过 %.2f ms。",
                                    spike_threshold_ms);
            } else if (ImGui::BeginTable("cross_spike_table", 6,
                    ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                    ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable |
                    ImGuiTableFlags_Sortable, ImVec2(-1, 200))) {
                ImGui::TableSetupColumn("方法");
                ImGui::TableSetupColumn("自身增量", ImGuiTableColumnFlags_DefaultSort |
                                                    ImGuiTableColumnFlags_PreferSortDescending);
                ImGui::TableSetupColumn("尖刺均值");
                ImGui::TableSetupColumn("安静均值");
                ImGui::TableSetupColumn("尖刺帧数");
                ImGui::TableSetupColumn("覆盖率");
                ImGui::TableHeadersRow();
                ImGuiTableSortSpecs *sort_specs = ImGui::TableGetSortSpecs();
                int sort_column = 1;
                bool ascending = false;
                if (sort_specs && sort_specs->SpecsCount > 0) {
                    sort_column = sort_specs->Specs[0].ColumnIndex;
                    ascending = sort_specs->Specs[0].SortDirection ==
                                ImGuiSortDirection_Ascending;
                }
                if (sort_column != 1 || ascending)
                    std::sort(spike_order.begin(), spike_order.end(),
                              [this, sort_column, ascending](size_t a, size_t b) -> bool {
                        const MethodAggregate &lhs = aggregates[a];
                        const MethodAggregate &rhs = aggregates[b];
                        int comparison = 0;
                        switch (sort_column) {
                        case 0: comparison = cocos_debug_snapshot_text(lhs.name).compare(
                                    cocos_debug_snapshot_text(rhs.name)); break;
                        case 2: comparison = lhs.spikeMean() < rhs.spikeMean() ? -1 :
                                             lhs.spikeMean() > rhs.spikeMean(); break;
                        case 3: comparison = lhs.baselineMean() < rhs.baselineMean() ? -1 :
                                             lhs.baselineMean() > rhs.baselineMean(); break;
                        case 4: comparison = lhs.spike_hits < rhs.spike_hits ? -1 :
                                             lhs.spike_hits > rhs.spike_hits; break;
                        default: comparison = lhs.spike_hits < rhs.spike_hits ? -1 :
                                              lhs.spike_hits > rhs.spike_hits; break;
                        }
                        return ascending ? comparison < 0 : comparison > 0;
                    });
                const size_t kMaxRows = 40;
                ImGuiListClipper clipper;
                clipper.Begin((int)(spike_order.size() < kMaxRows
                                        ? spike_order.size() : kMaxRows));
                while (clipper.Step()) {
                    for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; row++) {
                        const MethodAggregate &item =
                            aggregates[spike_order[(size_t)row]];
                        ImGui::TableNextRow();
                        ImGui::TableNextColumn();
                        ImGui::PushID((int)item.method_id);
                        if (ImGui::Selectable(
                                cocos_debug_snapshot_text(item.name).c_str(),
                                item.method_id == selected_method_id,
                                ImGuiSelectableFlags_SpanAllColumns))
                            selectMethod(item.method_id, item.name);
                        ImGui::PopID();
                        ImGui::TableNextColumn();
                        ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.25f, 1.0f),
                                           "%+.3f", item.spikeDelta());
                        ImGui::TableNextColumn(); ImGui::Text("%.3f", item.spikeMean());
                        ImGui::TableNextColumn(); ImGui::Text("%.3f", item.baselineMean());
                        ImGui::TableNextColumn(); ImGui::Text("%u", item.spike_hits);
                        ImGui::TableNextColumn(); ImGui::Text("%.0f%%",
                            100.0 * (double)item.spike_hits /
                                (double)aggregate_spike_count);
                    }
                }
                ImGui::EndTable();
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("增量 = 超预算帧上的平均自身耗时，减去安静帧上的平均自身耗时。\n"
                                  "覆盖率 = 这个方法出现在多少比例的超预算帧里。\n"
                                  "增量大且覆盖率低 = 间歇性原因；\n"
                                  "增量大且覆盖率 100%% = 固定成本。");
        }

        /* ---- per-method curve ---- */
        if (ts_dirty) rebuildTimeSeries();
        if (selected_method_id && !ts_x.empty()) {
            ImGui::TextUnformatted(selected_method_label.c_str());
            ImGui::SameLine();
            if (ImGui::Button("清除##ts_clear"))
                selectMethod(0, 0);
            if (ImPlot::BeginPlot("方法自身##cross_ts", ImVec2(-1, 200))) {
                ImPlot::SetupAxes("帧", "ms");
                ImPlot::PlotLine("主体", &ts_x[0], &ts_body[0], (int)ts_x.size());
                ImPlot::PlotLine("自身", &ts_x[0], &ts_y[0], (int)ts_x.size());
                ImPlot::EndPlot();
            }
        } else if (!selected_method_id) {
            ImGui::TextDisabled("在上面选一个方法，画出它每一帧的自身耗时。");
        }
    }

    void pollLuaEditorResult() {
        if (!lua_editor_waiting || !session.ipc) return;
        CocosDebugIpcRemoteLua *remote = &session.ipc->remote_lua;
        LONG result_seq = InterlockedCompareExchange((LONG *)&remote->result_seq, 0, 0);
        if (result_seq == lua_editor_wait_result_seq) return;
        LONG status = InterlockedCompareExchange((LONG *)&remote->result_status, 0, 0);
        lua_editor_waiting = false;
        if (status == 0) {
            strncpy(lua_editor_status, "OK", sizeof(lua_editor_status) - 1);
            lua_editor_error[0] = 0;
        } else if (status == 1) {
            strncpy(lua_editor_status, "Compile error", sizeof(lua_editor_status) - 1);
            strncpy(lua_editor_error, remote->result_error, sizeof(lua_editor_error) - 1);
        } else if (status == 3) {
            strncpy(lua_editor_status, "Busy", sizeof(lua_editor_status) - 1);
            strncpy(lua_editor_error, remote->result_error, sizeof(lua_editor_error) - 1);
        } else {
            strncpy(lua_editor_status, "Runtime error", sizeof(lua_editor_status) - 1);
            strncpy(lua_editor_error, remote->result_error, sizeof(lua_editor_error) - 1);
        }
        lua_editor_status[sizeof(lua_editor_status) - 1] = 0;
        lua_editor_error[sizeof(lua_editor_error) - 1] = 0;
    }

    bool sendRemoteLua(const char *script) {
        char err[COCOS_DEBUG_IPC_ERROR_CAP];
        LONG before;
        if (!session.ipc) {
            strncpy(lua_editor_status, "Not connected", sizeof(lua_editor_status) - 1);
            strncpy(lua_editor_error, "Inject and connect first",
                    sizeof(lua_editor_error) - 1);
            lua_editor_waiting = false;
            lua_editor_status[sizeof(lua_editor_status) - 1] = 0;
            lua_editor_error[sizeof(lua_editor_error) - 1] = 0;
            return false;
        }
        before = InterlockedCompareExchange((LONG *)&session.ipc->remote_lua.result_seq, 0, 0);
        err[0] = 0;
        if (!cocos_debug_run_lua_string(&session, script, err, sizeof(err))) {
            strncpy(lua_editor_status, "Send failed", sizeof(lua_editor_status) - 1);
            strncpy(lua_editor_error, err, sizeof(lua_editor_error) - 1);
            lua_editor_waiting = false;
            lua_editor_status[sizeof(lua_editor_status) - 1] = 0;
            lua_editor_error[sizeof(lua_editor_error) - 1] = 0;
            return false;
        }
        lua_editor_wait_result_seq = before;
        lua_editor_waiting = true;
        strncpy(lua_editor_status, "Sent", sizeof(lua_editor_status) - 1);
        lua_editor_error[0] = 0;
        lua_editor_status[sizeof(lua_editor_status) - 1] = 0;
        return true;
    }

    void ensureDebugListLoaded() {
        if (debug_list_loaded) return;
        debug_list_loaded = true;
        if (!cocos_debug_debug_list_load(&debug_list)) {
            debug_list_note = debug_list.error.empty()
                ? "Failed to load DebugList" : debug_list.error;
            return;
        }
        {
            char msg[96];
            _snprintf(msg, sizeof(msg), "Loaded %u scripts from debug_list\\",
                      (unsigned)debug_list.entries.size());
            debug_list_note = msg;
        }
        refreshDebugListGlobalHotkeys();
        if (!debug_list.entries.empty()) {
            debug_list_selected = 0;
            loadDebugListEditor();
        }
    }

    void loadDebugListEditor() {
        const CocosDebugDebugListEntry *entry;
        if (debug_list_selected < 0 ||
            debug_list_selected >= (int)debug_list.entries.size()) {
            debug_list_name[0] = 0;
            debug_list_code[0] = 0;
            return;
        }
        entry = &debug_list.entries[(size_t)debug_list_selected];
        strncpy(debug_list_name, entry->name.c_str(), sizeof(debug_list_name) - 1);
        debug_list_name[sizeof(debug_list_name) - 1] = 0;
        if (entry->code.size() >= sizeof(debug_list_code)) {
            memcpy(debug_list_code, entry->code.data(), sizeof(debug_list_code) - 1);
            debug_list_code[sizeof(debug_list_code) - 1] = 0;
            debug_list_note = "Script truncated to 64KB (remote Lua limit)";
        } else {
            if (!entry->code.empty())
                memcpy(debug_list_code, entry->code.data(), entry->code.size());
            debug_list_code[entry->code.size()] = 0;
        }
    }

    void flushDebugListEditor() {
        CocosDebugDebugListEntry *entry;
        size_t index;
        if (!debug_list_loaded) return;
        if (debug_list_selected < 0 ||
            debug_list_selected >= (int)debug_list.entries.size())
            return;
        index = (size_t)debug_list_selected;
        entry = &debug_list.entries[index];
        if (strcmp(debug_list_name, entry->name.c_str()) != 0) {
            if (!cocos_debug_debug_list_rename(&debug_list, index, debug_list_name)) {
                debug_list_note = debug_list.error;
                strncpy(debug_list_name, entry->name.c_str(), sizeof(debug_list_name) - 1);
                debug_list_name[sizeof(debug_list_name) - 1] = 0;
            } else {
                strncpy(debug_list_name, entry->name.c_str(), sizeof(debug_list_name) - 1);
                debug_list_name[sizeof(debug_list_name) - 1] = 0;
            }
        }
        if (entry->code != debug_list_code) {
            entry->code = debug_list_code;
            entry->dirty = true;
            if (!cocos_debug_debug_list_save_entry(&debug_list, entry))
                debug_list_note = debug_list.error;
            else
                debug_list_note = "Saved " + entry->filename;
        }
    }

    void selectDebugListEntry(int index) {
        if (index == debug_list_selected) return;
        debug_list_capturing_hotkey = false;
        flushDebugListEditor();
        debug_list_selected = index;
        loadDebugListEditor();
    }

    void addDebugListEntry() {
        int idx;
        flushDebugListEditor();
        idx = cocos_debug_debug_list_add(&debug_list, "New script", NULL);
        if (idx < 0) {
            debug_list_note = debug_list.error.empty()
                ? "Failed to add script" : debug_list.error;
            return;
        }
        debug_list_selected = -1;
        selectDebugListEntry(idx);
        debug_list_note = "Added " + debug_list.entries[(size_t)idx].filename;
    }

    void duplicateDebugListEntry() {
        int idx;
        if (debug_list_selected < 0) return;
        flushDebugListEditor();
        idx = cocos_debug_debug_list_duplicate(&debug_list, (size_t)debug_list_selected);
        if (idx < 0) {
            debug_list_note = debug_list.error.empty()
                ? "Failed to duplicate script" : debug_list.error;
            return;
        }
        debug_list_selected = -1;
        selectDebugListEntry(idx);
        debug_list_note = "Duplicated " + debug_list.entries[(size_t)idx].filename;
    }

    void deleteDebugListSelected() {
        int next;
        if (debug_list_selected < 0 ||
            debug_list_selected >= (int)debug_list.entries.size())
            return;
        next = debug_list_selected;
        if (!cocos_debug_debug_list_remove(&debug_list, (size_t)debug_list_selected)) {
            debug_list_note = debug_list.error.empty()
                ? "Failed to delete script" : debug_list.error;
            return;
        }
        if (next >= (int)debug_list.entries.size())
            next = (int)debug_list.entries.size() - 1;
        debug_list_selected = next;
        loadDebugListEditor();
        refreshDebugListGlobalHotkeys();
        debug_list_note = debug_list.entries.empty() ? "List is empty" : "Deleted";
    }

    void moveDebugListSelected(int delta) {
        size_t new_index = 0;
        if (debug_list_selected < 0) return;
        flushDebugListEditor();
        if (!cocos_debug_debug_list_move(&debug_list, (size_t)debug_list_selected, delta,
                                  &new_index)) {
            debug_list_note = debug_list.error;
            return;
        }
        debug_list_selected = (int)new_index;
    }

    void reloadDebugListSelected() {
        if (debug_list_selected < 0) return;
        if (!cocos_debug_debug_list_reload_entry(&debug_list, (size_t)debug_list_selected)) {
            debug_list_note = debug_list.error.empty()
                ? "Failed to reload script" : debug_list.error;
            return;
        }
        loadDebugListEditor();
        debug_list_note = "Reloaded " +
            debug_list.entries[(size_t)debug_list_selected].filename;
    }

    void assignDebugListHotkey(size_t index, int chord) {
        char text[80];
        size_t i;
        if (index >= debug_list.entries.size())
            return;
        if (chord != 0 && !debug_list_hotkey_can_register(chord)) {
            debug_list_note = "Global hotkeys need Ctrl/Shift/Alt, or an F1-F24 key";
            refreshDebugListGlobalHotkeys();
            return;
        }
        debug_list_format_hotkey(chord, text, sizeof(text));
        if (chord != 0) {
            for (i = 0; i < debug_list.entries.size(); i++) {
                if (i == index)
                    continue;
                if (debug_list_parse_hotkey(debug_list.entries[i].hotkey.c_str()) == chord)
                    cocos_debug_debug_list_set_hotkey(&debug_list, i, "");
            }
        }
        if (!cocos_debug_debug_list_set_hotkey(&debug_list, index, text)) {
            debug_list_note = debug_list.error.empty()
                ? "Failed to save hotkey" : debug_list.error;
            return;
        }
        refreshDebugListGlobalHotkeys();
        if (!chord) {
            debug_list_note = "Hotkey cleared";
            return;
        }
        {
            int id = DEBUG_LIST_HOTKEY_ID_BASE + debug_list.entries[index].id;
            bool ok = false;
            size_t k;
            for (k = 0; k < debug_list_os_hotkeys.size(); k++) {
                if (debug_list_os_hotkeys[k] == id)
                    ok = true;
            }
            if (ok)
                debug_list_note = std::string("Global hotkey ") + text;
            else
                debug_list_note = std::string("Saved ") + text +
                    " but Windows refused it (already in use)";
        }
    }

    void captureDebugListHotkey() {
        ImGuiIO &io = ImGui::GetIO();
        ImGuiKey key;
        if (!debug_list_capturing_hotkey)
            return;
        if (debug_list_selected < 0 ||
            debug_list_selected >= (int)debug_list.entries.size()) {
            debug_list_capturing_hotkey = false;
            return;
        }
        ImGui::SetNextFrameWantCaptureKeyboard(true);
        for (key = ImGuiKey_Tab; key <= ImGuiKey_Oem102; key = (ImGuiKey)(key + 1)) {
            int chord;
            if (!ImGui::IsKeyPressed(key, false))
                continue;
            if (key == ImGuiKey_Escape) {
                debug_list_capturing_hotkey = false;
                refreshDebugListGlobalHotkeys();
                debug_list_note = "Hotkey bind cancelled";
                return;
            }
            if (key == ImGuiKey_Backspace || key == ImGuiKey_Delete) {
                assignDebugListHotkey((size_t)debug_list_selected, 0);
                debug_list_capturing_hotkey = false;
                return;
            }
            if (!debug_list_is_hotkey_key(key))
                continue;
            chord = (int)io.KeyMods | (int)key;
            assignDebugListHotkey((size_t)debug_list_selected, chord);
            debug_list_capturing_hotkey = false;
            return;
        }
    }

    void runDebugListEntry(int index) {
        if (index < 0 || index >= (int)debug_list.entries.size())
            return;
        if (index == debug_list_selected)
            flushDebugListEditor();
        if (debug_list.entries[(size_t)index].code.empty()) {
            debug_list_note = "Script is empty";
            return;
        }
        sendRemoteLua(debug_list.entries[(size_t)index].code.c_str());
        debug_list_note = std::string("Ran ") + debug_list.entries[(size_t)index].name +
            " via hotkey";
    }

    void unregisterDebugListGlobalHotkeys() {
        size_t i;
        if (!g_window)
            return;
        for (i = 0; i < debug_list_os_hotkeys.size(); i++)
            UnregisterHotKey(g_window, debug_list_os_hotkeys[i]);
        debug_list_os_hotkeys.clear();
    }

    void refreshDebugListGlobalHotkeys() {
        size_t i;
        std::string failed;
        unregisterDebugListGlobalHotkeys();
        if (!g_window || !IsWindow(g_window))
            return;
        for (i = 0; i < debug_list.entries.size(); i++) {
            int chord = debug_list_parse_hotkey(debug_list.entries[i].hotkey.c_str());
            ImGuiKey key;
            int vk;
            int id;
            if (!chord || !debug_list_hotkey_can_register(chord))
                continue;
            key = (ImGuiKey)(chord & ~ImGuiMod_Mask_);
            vk = debug_list_hotkey_vk(key);
            id = DEBUG_LIST_HOTKEY_ID_BASE + debug_list.entries[i].id;
            if (!RegisterHotKey(g_window, id, debug_list_hotkey_winmods(chord), vk)) {
                if (!failed.empty())
                    failed += ", ";
                failed += debug_list.entries[i].hotkey;
                continue;
            }
            debug_list_os_hotkeys.push_back(id);
        }
        if (!failed.empty())
            debug_list_note = "Global register failed (in use?): " + failed;
    }

    void pollDebugListHotkeys() {
        int pending[16];
        int n;
        int i;
        ensureDebugListLoaded();
        n = g_pending_global_hotkey_count;
        if (n > 16)
            n = 16;
        memcpy(pending, g_pending_global_hotkeys, (size_t)n * sizeof(pending[0]));
        g_pending_global_hotkey_count = 0;
        if (debug_list_capturing_hotkey || lua_editor_waiting)
            return;
        for (i = 0; i < n; i++) {
            int entry_id = pending[i] - DEBUG_LIST_HOTKEY_ID_BASE;
            size_t j;
            for (j = 0; j < debug_list.entries.size(); j++) {
                if (debug_list.entries[j].id != entry_id)
                    continue;
                runDebugListEntry((int)j);
                return;
            }
        }
    }

    void setupFloatingWindow(bool focus, const ImVec2 &size) {
        ImGuiViewport *viewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowSize(size, ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowPos(
            ImVec2(viewport->WorkPos.x + 64.0f, viewport->WorkPos.y + 64.0f),
            ImGuiCond_FirstUseEver);
        if (focus) {
            ImGui::SetNextWindowFocus();
            ImGui::SetNextWindowCollapsed(false);
        }
    }

    void raiseFloatingWindow(bool *focus) {
        ImGuiViewport *viewport;
        ImVec2 pos;
        ImVec2 size;
        if (!focus || !*focus) return;
        ImGui::SetWindowFocus();
        ImGui::SetWindowCollapsed(false);
        viewport = ImGui::GetMainViewport();
        pos = ImGui::GetWindowPos();
        size = ImGui::GetWindowSize();
        if (size.x > viewport->WorkSize.x)
            size.x = viewport->WorkSize.x;
        if (size.y > viewport->WorkSize.y)
            size.y = viewport->WorkSize.y;
        if (pos.x + size.x > viewport->WorkPos.x + viewport->WorkSize.x)
            pos.x = viewport->WorkPos.x + viewport->WorkSize.x - size.x;
        if (pos.y + size.y > viewport->WorkPos.y + viewport->WorkSize.y)
            pos.y = viewport->WorkPos.y + viewport->WorkSize.y - size.y;
        if (pos.x < viewport->WorkPos.x)
            pos.x = viewport->WorkPos.x;
        if (pos.y < viewport->WorkPos.y)
            pos.y = viewport->WorkPos.y;
        ImGui::SetWindowSize(size, ImGuiCond_Always);
        ImGui::SetWindowPos(pos, ImGuiCond_Always);
        *focus = false;
    }

    void renderLuaEditor() {
        bool connected;
        if (!show_lua_editor) return;
        setupFloatingWindow(focus_lua_editor, ImVec2(720, 480));
        if (!ImGui::Begin("Run Lua String", &show_lua_editor)) {
            ImGui::End();
            focus_lua_editor = false;
            return;
        }
        raiseFloatingWindow(&focus_lua_editor);
        ImGui::TextUnformatted("Script runs on the target Lua state via loadbuffer/pcall.");
        ImGui::InputTextMultiline("##lua_editor", lua_editor_text, sizeof(lua_editor_text),
                                  ImVec2(-1, -80),
                                  ImGuiInputTextFlags_AllowTabInput);
        connected = session.ipc != NULL;
        ImGui::BeginDisabled(!connected || lua_editor_waiting);
        if (ImGui::Button("Run"))
            sendRemoteLua(lua_editor_text);
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Close")) show_lua_editor = false;
        ImGui::SameLine();
        ImGui::Text("Status: %s", lua_editor_status);
        if (lua_editor_error[0])
            ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.25f, 1.0f), "%s", lua_editor_error);
        ImGui::End();
    }

    void renderDebugList() {
        bool connected;
        bool has_selection;
        char path[MAX_PATH];
        if (!show_debug_list) {
            debug_list_capturing_hotkey = false;
            return;
        }
        ensureDebugListLoaded();
        setupFloatingWindow(focus_debug_list, ImVec2(860, 520));
        if (!ImGui::Begin("DebugList", &show_debug_list)) {
            ImGui::End();
            focus_debug_list = false;
            if (!show_debug_list)
                flushDebugListEditor();
            return;
        }
        raiseFloatingWindow(&focus_debug_list);
        captureDebugListHotkey();
        if (!debug_list_capturing_hotkey && ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_S))
            flushDebugListEditor();

        connected = session.ipc != NULL;
        has_selection = debug_list_selected >= 0 &&
            debug_list_selected < (int)debug_list.entries.size();

        if (ImGui::Button("Add"))
            addDebugListEntry();
        ImGui::SameLine();
        ImGui::BeginDisabled(!has_selection);
        if (ImGui::Button("Duplicate"))
            duplicateDebugListEntry();
        ImGui::SameLine();
        if (ImGui::Button("Delete"))
            ImGui::OpenPopup("Delete script?");
        ImGui::SameLine();
        if (ImGui::Button("Up"))
            moveDebugListSelected(-1);
        ImGui::SameLine();
        if (ImGui::Button("Down"))
            moveDebugListSelected(1);
        ImGui::EndDisabled();
        if (ImGui::BeginPopupModal("Delete script?", NULL,
                                   ImGuiWindowFlags_AlwaysAutoResize)) {
            const char *name = (debug_list_selected >= 0 &&
                debug_list_selected < (int)debug_list.entries.size())
                ? debug_list.entries[(size_t)debug_list_selected].name.c_str()
                : "";
            ImGui::Text("Delete \"%s\" and its .lua file?", name);
            if (ImGui::Button("Delete", ImVec2(120, 0))) {
                deleteDebugListSelected();
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel", ImVec2(120, 0)))
                ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
        ImGui::Separator();

        ImGui::BeginChild("##debug_list_left", ImVec2(300, 0), true);
        for (size_t i = 0; i < debug_list.entries.size(); i++) {
            const CocosDebugDebugListEntry &entry = debug_list.entries[i];
            char label[192];
            bool selected = (int)i == debug_list_selected;
            if (!entry.hotkey.empty())
                _snprintf(label, sizeof(label), "%s%s  [%s]##%d",
                          entry.name.c_str(),
                          (entry.dirty || (selected && entry.code != debug_list_code))
                              ? " *" : "",
                          entry.hotkey.c_str(), entry.id);
            else
                _snprintf(label, sizeof(label), "%s%s##%d",
                          entry.name.c_str(),
                          (entry.dirty || (selected && entry.code != debug_list_code))
                              ? " *" : "",
                          entry.id);
            label[sizeof(label) - 1] = 0;
            if (ImGui::Selectable(label, selected))
                selectDebugListEntry((int)i);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", entry.filename.c_str());
        }
        if (debug_list.entries.empty())
            ImGui::TextDisabled("No scripts. Click Add.");
        ImGui::EndChild();

        ImGui::SameLine();
        ImGui::BeginChild("##debug_list_right", ImVec2(0, 0), true);
        has_selection = debug_list_selected >= 0 &&
            debug_list_selected < (int)debug_list.entries.size();
        ImGui::BeginDisabled(!has_selection || debug_list_capturing_hotkey);
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Name");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(-1);
        ImGui::InputText("##debug_list_name", debug_list_name, sizeof(debug_list_name));
        if (ImGui::IsItemDeactivatedAfterEdit())
            flushDebugListEditor();
        ImGui::EndDisabled();
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Hotkey");
        ImGui::SameLine();
        if (debug_list_capturing_hotkey) {
            ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.35f, 1.0f),
                               "Press combo (Esc cancel, Backspace clear)");
        } else {
            const char *hotkey_text = (has_selection &&
                !debug_list.entries[(size_t)debug_list_selected].hotkey.empty())
                ? debug_list.entries[(size_t)debug_list_selected].hotkey.c_str()
                : "None";
            ImGui::TextUnformatted(hotkey_text);
        }
        ImGui::SameLine();
        ImGui::BeginDisabled(!has_selection);
        if (ImGui::Button(debug_list_capturing_hotkey ? "Cancel bind" : "Bind")) {
            debug_list_capturing_hotkey = !debug_list_capturing_hotkey;
            if (debug_list_capturing_hotkey)
                unregisterDebugListGlobalHotkeys();
            else
                refreshDebugListGlobalHotkeys();
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(debug_list_capturing_hotkey || !has_selection ||
            (has_selection &&
             debug_list.entries[(size_t)debug_list_selected].hotkey.empty()));
        if (ImGui::Button("Clear hotkey")) {
            assignDebugListHotkey((size_t)debug_list_selected, 0);
            debug_list_capturing_hotkey = false;
        }
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("Global hotkey: works even when DebugView is not focused.\n"
                              "Use Ctrl/Shift/Alt combinations, or F1-F24.\n"
                              "Windows consumes the combo so the target process will not see it.");
        ImGui::BeginDisabled(!has_selection || debug_list_capturing_hotkey);
        ImGui::InputTextMultiline("##debug_list_code", debug_list_code, sizeof(debug_list_code),
                                  ImVec2(-1, -110),
                                  ImGuiInputTextFlags_AllowTabInput);
        if (ImGui::IsItemDeactivatedAfterEdit())
            flushDebugListEditor();
        if (ImGui::Button("Save"))
            flushDebugListEditor();
        ImGui::SameLine();
        if (ImGui::Button("Reload"))
            reloadDebugListSelected();
        ImGui::SameLine();
        ImGui::BeginDisabled(!connected || lua_editor_waiting);
        if (ImGui::Button("Run")) {
            flushDebugListEditor();
            sendRemoteLua(debug_list_code);
        }
        ImGui::EndDisabled();
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Close"))
            show_debug_list = false;
        if (has_selection &&
            cocos_debug_debug_list_entry_path(&debug_list, (size_t)debug_list_selected,
                                       path, sizeof(path))) {
            ImGui::TextWrapped("%s", path);
        } else if (!debug_list.directory.empty()) {
            ImGui::TextWrapped("Folder: %s", debug_list.directory.c_str());
        }
        ImGui::Text("Run status: %s", lua_editor_status);
        if (lua_editor_error[0])
            ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.25f, 1.0f), "%s", lua_editor_error);
        if (!debug_list_note.empty())
            ImGui::TextWrapped("%s", debug_list_note.c_str());
        ImGui::EndChild();
        ImGui::End();
        if (!show_debug_list)
            flushDebugListEditor();
    }

    void render() {
        finishWorkerIfReady();
        pollTelemetry();
        pollLuaEditorResult();
        pollDebugListHotkeys();
        ImGuiViewport *viewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(viewport->WorkPos);
        ImGui::SetNextWindowSize(viewport->WorkSize);
        ImGui::Begin("Cocos2d DebugView", NULL,
                     ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoTitleBar |
                     ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus);
        renderProcessPanel();
        ImGui::Separator();
        renderControls();
        renderPlot();
        renderFrameDetails();
        renderCrossFramePanel();
        ImGui::End();
        renderLuaEditor();
        renderDebugList();
    }
};

int WINAPI WinMain(HINSTANCE instance, HINSTANCE, LPSTR, int show) {
    ImGui_ImplWin32_EnableDpiAwareness();
    WNDCLASSEXA wc = { sizeof(wc), CS_CLASSDC, window_proc, 0, 0, instance,
                       NULL, NULL, NULL, NULL, "Cocos2dDebugView", NULL };
    RegisterClassExA(&wc);
    g_window = CreateWindowA(wc.lpszClassName, "Cocos2d DebugView",
                             WS_OVERLAPPEDWINDOW, 100, 100, 1280, 850,
                             NULL, NULL, instance, NULL);
    if (!create_device(g_window)) {
        cleanup_device();
        UnregisterClassA(wc.lpszClassName, instance);
        return 1;
    }
    g_preview.setDevice(g_device);
    ShowWindow(g_window, show);
    UpdateWindow(g_window);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImPlot::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    if (GetFileAttributesA("C:\\Windows\\Fonts\\msyh.ttc") != INVALID_FILE_ATTRIBUTES)
        io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\msyh.ttc", 17.0f, NULL,
                                     io.Fonts->GetGlyphRangesChineseFull());
    ImGui::StyleColorsDark();
    ImGui_ImplWin32_Init(g_window);
    ImGui_ImplDX9_Init(g_device);

    Application app;
    app.refreshProcesses();
    bool done = false;
    while (!done) {
        MSG message;
        while (PeekMessage(&message, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&message);
            DispatchMessage(&message);
            if (message.message == WM_QUIT) done = true;
        }
        if (done) break;
        if (g_device_lost) {
            HRESULT result = g_device->TestCooperativeLevel();
            if (result == D3DERR_DEVICELOST) { Sleep(10); continue; }
            if (result == D3DERR_DEVICENOTRESET) reset_device();
            g_device_lost = false;
        }
        if (g_resize_width && g_resize_height) {
            g_present.BackBufferWidth = g_resize_width;
            g_present.BackBufferHeight = g_resize_height;
            g_resize_width = g_resize_height = 0;
            reset_device();
        }
        ImGui_ImplDX9_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        app.render();
        ImGui::EndFrame();
        g_device->SetRenderState(D3DRS_ZENABLE, FALSE);
        g_device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
        g_device->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
        g_device->Clear(0, NULL, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER,
                        D3DCOLOR_RGBA(15, 18, 25, 255), 1.0f, 0);
        if (g_device->BeginScene() >= 0) {
            ImGui::Render();
            ImGui_ImplDX9_RenderDrawData(ImGui::GetDrawData());
            g_device->EndScene();
        }
        if (g_device->Present(NULL, NULL, NULL, NULL) == D3DERR_DEVICELOST)
            g_device_lost = true;
    }
    ImGui_ImplDX9_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImPlot::DestroyContext();
    ImGui::DestroyContext();
    g_preview.clear();
    cleanup_device();
    DestroyWindow(g_window);
    UnregisterClassA(wc.lpszClassName, instance);
    return 0;
}

static bool create_device(HWND window) {
    g_d3d = Direct3DCreate9(D3D_SDK_VERSION);
    if (!g_d3d) return false;
    ZeroMemory(&g_present, sizeof(g_present));
    g_present.Windowed = TRUE;
    g_present.SwapEffect = D3DSWAPEFFECT_DISCARD;
    g_present.BackBufferFormat = D3DFMT_UNKNOWN;
    g_present.EnableAutoDepthStencil = TRUE;
    g_present.AutoDepthStencilFormat = D3DFMT_D16;
    g_present.PresentationInterval = D3DPRESENT_INTERVAL_ONE;
    return g_d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, window,
                               D3DCREATE_HARDWARE_VERTEXPROCESSING,
                               &g_present, &g_device) >= 0;
}

static void cleanup_device() {
    if (g_device) { g_device->Release(); g_device = NULL; }
    if (g_d3d) { g_d3d->Release(); g_d3d = NULL; }
}

static void reset_device() {
    ImGui_ImplDX9_InvalidateDeviceObjects();
    g_preview.invalidateDeviceObjects();
    if (g_device->Reset(&g_present) == D3DERR_INVALIDCALL) return;
    ImGui_ImplDX9_CreateDeviceObjects();
    g_preview.setDevice(g_device);
}

static LRESULT WINAPI window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    if (ImGui_ImplWin32_WndProcHandler(window, message, wparam, lparam)) return TRUE;
    switch (message) {
    case WM_HOTKEY:
        if (g_pending_global_hotkey_count < 16)
            g_pending_global_hotkeys[g_pending_global_hotkey_count++] = (int)wparam;
        return 0;
    case WM_SIZE:
        if (wparam != SIZE_MINIMIZED) {
            g_resize_width = LOWORD(lparam);
            g_resize_height = HIWORD(lparam);
        }
        return 0;
    case WM_SYSCOMMAND:
        if ((wparam & 0xFFF0) == SC_KEYMENU) return 0;
        break;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcA(window, message, wparam, lparam);
}
