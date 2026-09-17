#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d9.h>
#include <psapi.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <deque>
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
    bool follow_target;
    float frame_window;
    float spike_threshold_ms;
    bool show_spike_hints;
    char method_filter[128];
    std::string status_message;
    DWORD last_dock_tick;
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
          was_recording(false), follow_target(true), frame_window(180.0f),
          spike_threshold_ms(16.67f), show_spike_hints(true),
          last_dock_tick(0), last_preview_tick(0),
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
            while (!pending_trace_events.empty()) {
                LuaTraceEventSnapshot event = pending_trace_events.front();
                pending_trace_events.pop_front();
                bool attached = false;
                if (event.frame_sequence != 0) {
                    for (std::deque<FrameSnapshot>::reverse_iterator it = history.rbegin();
                         it != history.rend(); ++it) {
                        if (it->lua_frame_index == event.frame_sequence) {
                            it->trace_events.push_back(event);
                            attached = true;
                            break;
                        }
                    }
                }
                if (!attached && event.frame_sequence != 0)
                    still_pending.push_back(event);
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

    void dockWindow() {
        DWORD now = GetTickCount();
        if (!follow_target || !session.target_window || now - last_dock_tick < 100) return;
        last_dock_tick = now;
        if (!IsWindow(session.target_window)) return;
        RECT rect;
        if (!GetWindowRect(session.target_window, &rect) || IsIconic(session.target_window)) return;
        int width = 980;
        int height = std::max(500L, rect.bottom - rect.top);
        SetWindowPos(g_window, HWND_TOP, rect.right + 8, rect.top, width, height,
                     SWP_NOACTIVATE);
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
        ImGui::Checkbox("Follow target window", &follow_target);
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
                if (method.name.find("Renderer:render") != std::string::npos ||
                    method.name.find("Scene:render") != std::string::npos)
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

        /* Visible-window summaries use v7 Body for slow frames and Lua wall for Lua heat. */
        int over_count = 0;
        double worst_ms = 0.0;
        uint32_t worst_frame = 0;
        double hottest_lua_ms = 0.0;
        uint32_t hottest_lua_frame = 0;
        for (size_t i = 0; i < x.size(); i++) {
            const FrameSnapshot &frame = history[plot_begin_index + i];
            if (lua_wall[i] > hottest_lua_ms) {
                hottest_lua_ms = lua_wall[i];
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
            ImGui::Text(" | Lua hot #%u %.2f ms wall",
                        (unsigned)hottest_lua_frame, hottest_lua_ms);
            if (!history.empty() && history.back().drawscene_body_ms > spike_threshold_ms) {
                ImGui::SameLine();
                ImGui::TextColored(ImVec4(1.0f, 0.25f, 0.2f, 1.0f),
                                   "  latest #%u  %.2f ms",
                                   (unsigned)history.back().lua_frame_index,
                                   history.back().drawscene_body_ms);
            }
        }

        ImGui::BeginChild("##plot_pane", ImVec2(plot_width, 300), false);
        if (ImPlot::BeginPlot("Real-time performance", ImVec2(-1, -1))) {
            ImPlot::SetupAxes("frame", "ms");
            if (!paused && !x.empty()) {
                double xmax = x.back();
                ImPlot::SetupAxisLimits(ImAxis_X1, std::max(0.0, xmax - frame_window),
                                        std::max((double)frame_window, xmax), ImPlotCond_Always);
            }
            if (!x.empty()) {
                ImPlot::PlotLine("DrawScene", &x[0], &draw[0], (int)x.size());
                ImPlot::PlotLine("DeltaTime", &x[0], &delta[0], (int)x.size());
                if (rendererHasSamples())
                    ImPlot::PlotLine("render", &x[0], &render[0], (int)x.size());
                ImPlot::PlotLine("Lua wall", &x[0], &lua_wall[0], (int)x.size());
                ImPlot::PlotLine("Lua CPU", &x[0], &lua_cpu[0], (int)x.size());
                ImPlot::PlotLine("Lua self", &x[0], &lua_self[0], (int)x.size());
                ImPlot::PlotLine("C self", &x[0], &c_self[0], (int)x.size());
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
            if (plot_hovered) {
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
                    ImGui::Text("Lua wall %.2f  CPU %.2f  Lua self %.2f  C self %.2f",
                                lua_wall[best], lua_cpu[best],
                                lua_self[best], c_self[best]);
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

    static std::string relativePathFromChunk(std::string value) {
        std::string out;
        size_t i = 0;
        if (!value.empty() && (value[0] == '@' || value[0] == '='))
            i = 1;
        for (; i < value.size() && out.size() + 1 < 200; i++) {
            unsigned char c = (unsigned char)value[i];
            if (c == '/' || c == '\\' || c == ':') {
                if (!out.empty() && out.back() != '\\') out.push_back('\\');
                continue;
            }
            if (std::isalnum(c) || c == '_' || c == '-' || c == '.')
                out.push_back((char)c);
            else if (!out.empty() && out.back() != '\\')
                out.push_back('_');
        }
        while (!out.empty() && (out.back() == '\\' || out.back() == '.')) out.pop_back();
        auto ends_with_ci = [](const std::string &s, const char *ext) {
            size_t n = strlen(ext);
            if (s.size() < n) return false;
            return _stricmp(s.c_str() + (s.size() - n), ext) == 0;
        };
        if (ends_with_ci(out, ".luac")) out.resize(out.size() - 5);
        if (ends_with_ci(out, ".lua")) out.resize(out.size() - 4);
        while (!out.empty() && (out.back() == '\\' || out.back() == '.')) out.pop_back();
        return out.empty() ? "chunk" : out;
    }

    std::string findDumpFileForSource(const std::string &source) const {
        char path[MAX_PATH];
        std::string rel;
        if (!session.ipc || !session.ipc->config.luac_dump_dir[0] || source.empty())
            return std::string();
        rel = relativePathFromChunk(source);
        _snprintf(path, sizeof(path), "%s\\%s.lua",
                  session.ipc->config.luac_dump_dir, rel.c_str());
        path[sizeof(path) - 1] = 0;
        if (GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES)
            return path;
        _snprintf(path, sizeof(path), "%s\\%s.luac.txt",
                  session.ipc->config.luac_dump_dir, rel.c_str());
        path[sizeof(path) - 1] = 0;
        if (GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES)
            return path;
        _snprintf(path, sizeof(path), "%s\\%s.luac",
                  session.ipc->config.luac_dump_dir, rel.c_str());
        path[sizeof(path) - 1] = 0;
        if (GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES)
            return path;
        return std::string();
    }

    static const char *luaMethodKindName(uint16_t kind) {
        switch (kind) {
        case COCOS_DEBUG_IPC_LUA_METHOD_LUA: return "Lua";
        case COCOS_DEBUG_IPC_LUA_METHOD_C: return "C";
        case COCOS_DEBUG_IPC_LUA_METHOD_MAIN: return "main";
        default: return "unknown";
        }
    }

    static std::string luaMethodFlagsText(uint16_t flags) {
        std::string text;
        uint16_t unknown = flags;
        if (flags & COCOS_DEBUG_IPC_LUA_METHOD_INTERNAL) {
            text = "internal";
            unknown &= (uint16_t)~COCOS_DEBUG_IPC_LUA_METHOD_INTERNAL;
        }
        if (unknown) {
            char value[24];
            _snprintf(value, sizeof(value), "%s0x%X",
                      text.empty() ? "" : "|", (unsigned)unknown);
            value[sizeof(value) - 1] = 0;
            text += value;
        }
        return text.empty() ? "-" : text;
    }

    static const char *luaEventTypeName(uint8_t type) {
        switch (type) {
        case COCOS_DEBUG_IPC_LUA_EVENT_CALL: return "call";
        case COCOS_DEBUG_IPC_LUA_EVENT_RETURN: return "return";
        case COCOS_DEBUG_IPC_LUA_EVENT_TAIL_RETURN: return "tail return";
        case COCOS_DEBUG_IPC_LUA_EVENT_RESUME: return "resume";
        case COCOS_DEBUG_IPC_LUA_EVENT_SUSPEND: return "suspend";
        case COCOS_DEBUG_IPC_LUA_EVENT_FRAME_BEGIN: return "frame begin";
        case COCOS_DEBUG_IPC_LUA_EVENT_FRAME_END: return "frame end";
        default: return "unknown";
        }
    }

    static std::string frameFlagsText(uint32_t flags) {
        struct FlagName { uint32_t bit; const char *name; };
        static const FlagName names[] = {
            {COCOS_DEBUG_IPC_FRAME_EVENT_DROPPED, "event dropped"},
            {COCOS_DEBUG_IPC_FRAME_METHOD_LIMITED, "method limited"},
            {COCOS_DEBUG_IPC_FRAME_THREAD_LIMITED, "thread limited"},
            {COCOS_DEBUG_IPC_FRAME_LINE_LIMITED, "line limited"},
            {COCOS_DEBUG_IPC_FRAME_LINE_CAPTURED, "line captured"},
            {COCOS_DEBUG_IPC_FRAME_STACK_DESYNC, "stack desync"},
            {COCOS_DEBUG_IPC_FRAME_STATE_CONCURRENT, "state concurrent"},
            {COCOS_DEBUG_IPC_FRAME_INTERVAL_LIMITED, "interval limited"}
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
        ImGui::SeparatorText("Selected frame");
        ImGui::Text("Lua frame %u | render frame %u | DeltaTime %.3f ms | Body %.3f ms | "
                    "events %u | trace events %u",
                    frame->lua_frame_index, frame->render_frame_sequence,
                    frame->frame_period_ms, frame->drawscene_body_ms,
                    frame->hook_events, (unsigned)frame->trace_events.size());
        ImGui::Text("Lua wall %.3f ms | CPU %.3f ms | Lua self %.3f ms | C self %.3f ms",
                    frame->lua_wall_ms, frame->lua_cpu_ms,
                    frame->lua_script_self_ms, frame->lua_c_self_ms);
        ImGui::TextDisabled("frame time %.6f -> %.6f s",
                            frame->frame_begin_time_seconds,
                            frame->frame_end_time_seconds);
        if (show_spike_hints && frame->drawscene_body_ms > spike_threshold_ms) {
            ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.15f, 1.0f),
                               "Body over budget by %.2f ms (limit %.2f)",
                               frame->drawscene_body_ms - spike_threshold_ms,
                               spike_threshold_ms);
        }
        if (frame->flags) {
            std::string flags = frameFlagsText(frame->flags);
            ImGui::TextColored(ImVec4(1, .45f, .2f, 1),
                               "Capture flags: %s (0x%X)",
                               flags.c_str(), frame->flags);
        }
        if (ImGui::CollapsingHeader("Lua Hot Lines##frame_details",
                frame->lua_lines.empty() ? 0 : ImGuiTreeNodeFlags_DefaultOpen)) {
            if (frame->lua_lines.empty()) {
                ImGui::TextDisabled("No line samples in this frame. Click Capture Lua details or wait for a Lua spike.");
            } else {
                std::vector<size_t> line_order;
                uint64_t total_line_samples = 0;
                for (size_t i = 0; i < frame->lua_lines.size(); i++)
                {
                    line_order.push_back(i);
                    total_line_samples += frame->lua_lines[i].samples;
                }
                if (ImGui::BeginTable("lua_lines", 5,
                        ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                        ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable |
                        ImGuiTableFlags_Sortable,
                        ImVec2(-1, 180))) {
                    ImGui::TableSetupColumn("Source");
                    ImGui::TableSetupColumn("Line");
                    ImGui::TableSetupColumn("Samples", ImGuiTableColumnFlags_DefaultSort |
                                                      ImGuiTableColumnFlags_PreferSortDescending);
                    ImGui::TableSetupColumn("Share");
                    ImGui::TableSetupColumn("Function");
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
                        case 0: comparison = lhs.source.compare(rhs.source); break;
                        case 1: comparison = lhs.line < rhs.line ? -1 : lhs.line > rhs.line; break;
                        case 2: comparison = lhs.samples < rhs.samples ? -1 : lhs.samples > rhs.samples; break;
                        case 3: comparison = lhs.samples < rhs.samples ? -1 : lhs.samples > rhs.samples; break;
                        default: comparison = lhs.function_name.compare(rhs.function_name); break;
                        }
                        return ascending ? comparison < 0 : comparison > 0;
                    });
                    ImGuiListClipper clipper;
                    clipper.Begin((int)line_order.size());
                    while (clipper.Step()) {
                        for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; row++) {
                            const LuaLineSnapshot &item = frame->lua_lines[line_order[row]];
                            ImGui::TableNextRow();
                            ImGui::TableNextColumn();
                            ImGui::TextUnformatted(item.source.c_str());
                            if (ImGui::IsItemHovered()) {
                                std::string dump = findDumpFileForSource(item.source);
                                if (!dump.empty())
                                    ImGui::SetTooltip("%s\nDump: %s", item.source.c_str(), dump.c_str());
                                else
                                    ImGui::SetTooltip("%s", item.source.c_str());
                            }
                            ImGui::TableNextColumn(); ImGui::Text("%d", (int)item.line);
                            ImGui::TableNextColumn(); ImGui::Text("%u", item.samples);
                            ImGui::TableNextColumn(); ImGui::Text(
                                "%.1f%%", total_line_samples
                                    ? 100.0 * (double)item.samples /
                                      (double)total_line_samples
                                    : 0.0);
                            ImGui::TableNextColumn(); ImGui::TextUnformatted(item.function_name.c_str());
                        }
                    }
                    ImGui::EndTable();
                }
            }
        }
        ImGui::InputTextWithHint("##method_filter", "Filter Lua methods",
                                 method_filter, sizeof(method_filter));
        std::vector<size_t> order;
        for (size_t i = 0; i < frame->lua_methods.size(); i++)
            if (containsInsensitive(frame->lua_methods[i].name, method_filter)) order.push_back(i);
        if (ImGui::BeginTable("lua_methods", 10,
                ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable |
                ImGuiTableFlags_Sortable | ImGuiTableFlags_ScrollX,
                ImVec2(-1, 230))) {
            ImGui::TableSetupColumn("Method");
            ImGui::TableSetupColumn("Kind");
            ImGui::TableSetupColumn("Flags");
            ImGui::TableSetupColumn("Self ms", ImGuiTableColumnFlags_DefaultSort |
                                               ImGuiTableColumnFlags_PreferSortDescending);
            ImGui::TableSetupColumn("Inclusive ms");
            ImGui::TableSetupColumn("Max self");
            ImGui::TableSetupColumn("Max inclusive");
            ImGui::TableSetupColumn("Started");
            ImGui::TableSetupColumn("Completed");
            ImGui::TableSetupColumn("Active end");
            ImGui::TableHeadersRow();
            ImGuiTableSortSpecs *sort_specs = ImGui::TableGetSortSpecs();
            int sort_column = 3;
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
                case 0: comparison = lhs.name.compare(rhs.name); break;
                case 1: comparison = lhs.kind < rhs.kind ? -1 : lhs.kind > rhs.kind; break;
                case 2: comparison = lhs.flags < rhs.flags ? -1 : lhs.flags > rhs.flags; break;
                case 3: comparison = lhs.self_ms < rhs.self_ms ? -1 : lhs.self_ms > rhs.self_ms; break;
                case 4: comparison = lhs.inclusive_ms < rhs.inclusive_ms ? -1 :
                                     lhs.inclusive_ms > rhs.inclusive_ms; break;
                case 5: comparison = lhs.max_self_ms < rhs.max_self_ms ? -1 :
                                     lhs.max_self_ms > rhs.max_self_ms; break;
                case 6: comparison = lhs.max_inclusive_ms < rhs.max_inclusive_ms ? -1 :
                                     lhs.max_inclusive_ms > rhs.max_inclusive_ms; break;
                case 7: comparison = lhs.calls_started < rhs.calls_started ? -1 :
                                     lhs.calls_started > rhs.calls_started; break;
                case 8: comparison = lhs.calls_completed < rhs.calls_completed ? -1 :
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
                    std::string method_flags = luaMethodFlagsText(item.flags);
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn(); ImGui::TextUnformatted(item.name.c_str());
                    ImGui::TableNextColumn(); ImGui::TextUnformatted(luaMethodKindName(item.kind));
                    ImGui::TableNextColumn(); ImGui::TextUnformatted(method_flags.c_str());
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
        if (ImGui::CollapsingHeader("C++ hooks##frame_details") &&
            ImGui::BeginTable("cpp_methods", 6,
                ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                ImGuiTableFlags_Resizable)) {
            ImGui::TableSetupColumn("Target");
            ImGui::TableSetupColumn("Total ms");
            ImGui::TableSetupColumn("Max ms");
            ImGui::TableSetupColumn("Last ms");
            ImGui::TableSetupColumn("Calls");
            ImGui::TableSetupColumn("Call sequence");
            ImGui::TableHeadersRow();
            for (size_t i = 0; i < frame->cpp_methods.size(); i++) {
                const CppMethodSnapshot &item = frame->cpp_methods[i];
                ImGui::TableNextRow();
                ImGui::TableNextColumn(); ImGui::TextUnformatted(item.name.c_str());
                ImGui::TableNextColumn(); ImGui::Text("%.3f", item.total_ms);
                ImGui::TableNextColumn(); ImGui::Text("%.3f", item.max_ms);
                ImGui::TableNextColumn(); ImGui::Text("%.3f", item.last_ms);
                ImGui::TableNextColumn(); ImGui::Text("%u", item.calls);
                ImGui::TableNextColumn(); ImGui::Text("%u", item.call_sequence);
            }
            ImGui::EndTable();
        }
        if (ImGui::CollapsingHeader("Raw Lua trace events##frame_details",
                frame->trace_events.empty() ? 0 : ImGuiTreeNodeFlags_DefaultOpen)) {
            if (frame->trace_events.empty()) {
                ImGui::TextDisabled("No raw events for this frame. Use Trace Start while profiling.");
            } else if (ImGui::BeginTable("lua_trace_events", 11,
                    ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                    ImGuiTableFlags_ScrollY | ImGuiTableFlags_ScrollX |
                    ImGuiTableFlags_Resizable, ImVec2(-1, 240))) {
                ImGui::TableSetupColumn("Event seq");
                ImGui::TableSetupColumn("QPC");
                ImGui::TableSetupColumn("Time s");
                ImGui::TableSetupColumn("Frame seq");
                ImGui::TableSetupColumn("Type");
                ImGui::TableSetupColumn("Lua thread");
                ImGui::TableSetupColumn("OS thread");
                ImGui::TableSetupColumn("Depth");
                ImGui::TableSetupColumn("Flags");
                ImGui::TableSetupColumn("Method id");
                ImGui::TableSetupColumn("Method");
                ImGui::TableHeadersRow();
                ImGuiListClipper clipper;
                clipper.Begin((int)frame->trace_events.size());
                while (clipper.Step()) {
                    for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; row++) {
                        const LuaTraceEventSnapshot &event = frame->trace_events[(size_t)row];
                        ImGui::TableNextRow();
                        ImGui::TableNextColumn(); ImGui::Text("%u", event.event_sequence);
                        ImGui::TableNextColumn(); ImGui::Text("%llu",
                            (unsigned long long)event.qpc);
                        ImGui::TableNextColumn(); ImGui::Text("%.6f", event.time_seconds);
                        ImGui::TableNextColumn(); ImGui::Text("%llu",
                            (unsigned long long)event.frame_sequence);
                        ImGui::TableNextColumn(); ImGui::TextUnformatted(
                            luaEventTypeName(event.type));
                        ImGui::TableNextColumn(); ImGui::Text("%u", event.lua_thread_id);
                        ImGui::TableNextColumn(); ImGui::Text("%u", event.os_thread_id);
                        ImGui::TableNextColumn(); ImGui::Text("%d", (int)event.depth);
                        ImGui::TableNextColumn(); ImGui::Text("0x%02X", event.flags);
                        ImGui::TableNextColumn(); ImGui::Text("%u", event.method_id);
                        ImGui::TableNextColumn(); ImGui::TextUnformatted(
                            event.method_name.c_str());
                    }
                }
                ImGui::EndTable();
            }
        }
        if (ImGui::CollapsingHeader("Engine##frame_details", ImGuiTreeNodeFlags_DefaultOpen) &&
            ImGui::BeginTable("engine_metrics", 2, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
            ImGui::TableSetupColumn("Metric");
            ImGui::TableSetupColumn("ms");
            ImGui::TableHeadersRow();
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::TextUnformatted("drawScene body");
            ImGui::TableNextColumn(); ImGui::Text("%.3f", frame->drawscene_body_ms);
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::TextUnformatted("DeltaTime");
            ImGui::TableNextColumn(); ImGui::Text("%.3f", frame->frame_period_ms);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Frame interval that contains this DrawScene:\n"
                                  "this body + present/vsync/idle until the next\n"
                                  "drawScene begins. Orange sits on 16.67/33 with vsync;\n"
                                  "it is not supposed to overlay the blue Body sawtooth.");
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::TextUnformatted("render");
            if (frame->scene_render_hook_present || rendererHookConfigured()) {
                ImGui::TableNextColumn(); ImGui::Text("%.3f", sceneRenderMs(*frame));
            } else {
                ImGui::TableNextColumn();
                ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.2f, 1.0f), "not installed");
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("cc.Renderer:render was not resolved at inject time.\n"
                                      "The green render series is not engine submit time.");
            }
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::TextUnformatted("Lua wall");
            ImGui::TableNextColumn(); ImGui::Text("%.3f", frame->lua_wall_ms);
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::TextUnformatted("Lua CPU");
            ImGui::TableNextColumn(); ImGui::Text("%.3f", frame->lua_cpu_ms);
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::TextUnformatted("Lua script self");
            ImGui::TableNextColumn(); ImGui::Text("%.3f", frame->lua_script_self_ms);
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::TextUnformatted("Lua C self");
            ImGui::TableNextColumn(); ImGui::Text("%.3f", frame->lua_c_self_ms);
            ImGui::EndTable();
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
        dockWindow();
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
