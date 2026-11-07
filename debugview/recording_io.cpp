#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <stdio.h>
#include <string.h>
#include <time.h>

#include <vector>

#include "recording_io.h"

static const char kMagic[8] = {'C', 'C', 'D', 'B', 'R', 'E', 'C', '1'};
/* 5: first shipped layout.
   6: adds FrameSnapshot.lua_outside_ms and LuaLineSnapshot.line_ms (v6 files
      are read by v6 readers; v5 files still load, with both fields 0).
   7: adds actor_x/actor_y/actor_valid after flags (legacy; always 0 now).
      Older files load with actor_valid = 0.
   8: adds proc_ws_kb/proc_private_kb after actor_valid. Older files load
      with both fields 0.
   9: after CppMethod entries, adds file_io_count + per-path getData samples.
      Older files load with file_io empty. */
static const uint32_t kVersion = 9;
static const uint32_t kMinSupportedVersion = 5;

static bool write_raw(FILE *fp, const void *data, size_t size) {
    return fwrite(data, 1, size, fp) == size;
}

static bool read_raw(FILE *fp, void *data, size_t size) {
    return fread(data, 1, size, fp) == size;
}

static bool write_u32(FILE *fp, uint32_t value) {
    return write_raw(fp, &value, sizeof(value));
}

static bool read_u32(FILE *fp, uint32_t *value) {
    return read_raw(fp, value, sizeof(*value));
}

static bool write_u64(FILE *fp, uint64_t value) {
    return write_raw(fp, &value, sizeof(value));
}

static bool read_u64(FILE *fp, uint64_t *value) {
    return read_raw(fp, value, sizeof(*value));
}

static bool write_f32(FILE *fp, float value) {
    return write_raw(fp, &value, sizeof(value));
}

static bool read_f32(FILE *fp, float *value) {
    return read_raw(fp, value, sizeof(*value));
}

static bool write_f64(FILE *fp, double value) {
    return write_raw(fp, &value, sizeof(value));
}

static bool read_f64(FILE *fp, double *value) {
    return read_raw(fp, value, sizeof(*value));
}

static bool write_string(FILE *fp, const std::string &value) {
    if (value.size() > 0x100000u) return false;
    uint32_t length = (uint32_t)value.size();
    if (!write_u32(fp, length)) return false;
    if (length == 0) return true;
    return write_raw(fp, value.data(), length);
}

static bool read_string(FILE *fp, std::string *value) {
    uint32_t length = 0;
    if (!read_u32(fp, &length)) return false;
    if (length > 0x100000u) return false;
    value->assign(length, '\0');
    if (length == 0) return true;
    return read_raw(fp, &(*value)[0], length);
}

bool cocos_debug_exe_directory(char *out, size_t cap) {
    char path[MAX_PATH];
    char *slash;
    DWORD n;
    if (!out || cap < 4) return false;
    n = GetModuleFileNameA(NULL, path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return false;
    slash = strrchr(path, '\\');
    if (!slash) slash = strrchr(path, '/');
    if (!slash) return false;
    *slash = 0;
    if (strlen(path) + 1 > cap) return false;
    strcpy(out, path);
    return true;
}

bool cocos_debug_make_capture_path(char *out, size_t cap) {
    char dir[MAX_PATH];
    time_t now;
    struct tm local_tm;
    char stamp[32];
    if (!cocos_debug_exe_directory(dir, sizeof(dir))) return false;
    time(&now);
    local_tm = *localtime(&now);
    strftime(stamp, sizeof(stamp), "%Y%m%d_%H%M%S", &local_tm);
    if (_snprintf(out, (int)cap, "%s\\cocosdbg_capture_%s.cocosdbg", dir, stamp) <= 0)
        return false;
    out[cap - 1] = 0;
    return true;
}

static bool write_frame(FILE *fp, const FrameSnapshot &frame) {
    uint32_t i;
    uint32_t lua_count = (uint32_t)frame.lua_methods.size();
    uint32_t cpp_count = (uint32_t)frame.cpp_methods.size();
    uint32_t line_count = (uint32_t)frame.lua_lines.size();
    uint32_t event_count = (uint32_t)frame.trace_events.size();
    if (!write_u64(fp, frame.publish_sequence)) return false;
    if (!write_f64(fp, frame.frame_begin_time_seconds)) return false;
    if (!write_f64(fp, frame.frame_end_time_seconds)) return false;
    if (!write_u32(fp, frame.lua_frame_index)) return false;
    if (!write_u32(fp, frame.render_frame_sequence)) return false;
    if (!write_f32(fp, frame.frame_period_ms)) return false;
    if (!write_f32(fp, frame.drawscene_body_ms)) return false;
    if (!write_f32(fp, frame.lua_wall_ms)) return false;
    if (!write_f32(fp, frame.lua_cpu_ms)) return false;
    if (!write_f32(fp, frame.lua_script_self_ms)) return false;
    if (!write_f32(fp, frame.lua_c_self_ms)) return false;
    if (!write_f32(fp, frame.lua_outside_ms)) return false;
    if (!write_f32(fp, frame.lua_mem_kb)) return false;
    if (!write_u32(fp, frame.hook_events)) return false;
    if (!write_u32(fp, frame.flags)) return false;
    if (!write_f32(fp, frame.actor_x)) return false;
    if (!write_f32(fp, frame.actor_y)) return false;
    if (!write_u32(fp, frame.actor_valid)) return false;
    if (!write_f32(fp, frame.proc_ws_kb)) return false;
    if (!write_f32(fp, frame.proc_private_kb)) return false;
    if (!write_u32(fp, lua_count)) return false;
    if (!write_u32(fp, cpp_count)) return false;
    if (!write_u32(fp, line_count)) return false;
    if (!write_u32(fp, event_count)) return false;
    for (i = 0; i < lua_count; i++) {
        const LuaMethodSnapshot &m = frame.lua_methods[i];
        if (!write_string(fp, cocos_debug_snapshot_text(m.name))) return false;
        if (!write_u32(fp, m.kind)) return false;
        if (!write_u32(fp, m.flags)) return false;
        if (!write_f32(fp, m.self_ms)) return false;
        if (!write_f32(fp, m.inclusive_ms)) return false;
        if (!write_f32(fp, m.max_self_ms)) return false;
        if (!write_f32(fp, m.max_inclusive_ms)) return false;
        if (!write_u32(fp, m.calls_started)) return false;
        if (!write_u32(fp, m.calls_completed)) return false;
        if (!write_u32(fp, m.active_at_end)) return false;
    }
    for (i = 0; i < cpp_count; i++) {
        const CppMethodSnapshot &m = frame.cpp_methods[i];
        if (!write_string(fp, cocos_debug_snapshot_text(m.name))) return false;
        if (!write_f32(fp, m.total_ms)) return false;
        if (!write_f32(fp, m.max_ms)) return false;
        if (!write_f32(fp, m.last_ms)) return false;
        if (!write_u32(fp, m.calls)) return false;
        if (!write_u32(fp, m.call_sequence)) return false;
    }
    {
        uint32_t file_io_count = (uint32_t)frame.file_io.size();
        if (!write_u32(fp, file_io_count)) return false;
        for (i = 0; i < file_io_count; i++) {
            const FileIoSnapshot &m = frame.file_io[i];
            if (!write_string(fp, cocos_debug_snapshot_text(m.path))) return false;
            if (!write_f32(fp, m.total_ms)) return false;
            if (!write_f32(fp, m.max_ms)) return false;
            if (!write_f32(fp, m.last_ms)) return false;
            if (!write_u32(fp, m.calls)) return false;
        }
    }
    for (i = 0; i < line_count; i++) {
        const LuaLineSnapshot &m = frame.lua_lines[i];
        if (!write_string(fp, cocos_debug_snapshot_text(m.source))) return false;
        if (!write_string(fp, cocos_debug_snapshot_text(m.function_name))) return false;
        if (!write_u32(fp, (uint32_t)m.line)) return false;
        if (!write_u32(fp, m.samples)) return false;
        if (!write_f32(fp, m.line_ms)) return false;
    }
    for (i = 0; i < event_count; i++) {
        const LuaTraceEventSnapshot &event = frame.trace_events[i];
        if (!write_u32(fp, event.event_sequence)) return false;
        if (!write_u64(fp, event.qpc)) return false;
        if (!write_f64(fp, event.time_seconds)) return false;
        if (!write_u64(fp, event.frame_sequence)) return false;
        if (!write_u32(fp, event.lua_thread_id)) return false;
        if (!write_u32(fp, event.os_thread_id)) return false;
        if (!write_u32(fp, event.method_id)) return false;
        if (!write_string(fp, cocos_debug_snapshot_text(event.method_name))) return false;
        if (!write_u32(fp, (uint32_t)(int32_t)event.depth)) return false;
        if (!write_u32(fp, event.type)) return false;
        if (!write_u32(fp, event.flags)) return false;
    }
    return true;
}

static bool read_frame(FILE *fp, FrameSnapshot *frame, uint32_t version) {
    uint32_t i;
    uint32_t lua_count = 0;
    uint32_t cpp_count = 0;
    uint32_t line_count = 0;
    uint32_t event_count = 0;
    if (!read_u64(fp, &frame->publish_sequence)) return false;
    if (!read_f64(fp, &frame->frame_begin_time_seconds)) return false;
    if (!read_f64(fp, &frame->frame_end_time_seconds)) return false;
    if (!read_u32(fp, &frame->lua_frame_index)) return false;
    if (!read_u32(fp, &frame->render_frame_sequence)) return false;
    if (!read_f32(fp, &frame->frame_period_ms)) return false;
    if (!read_f32(fp, &frame->drawscene_body_ms)) return false;
    if (!read_f32(fp, &frame->lua_wall_ms)) return false;
    if (!read_f32(fp, &frame->lua_cpu_ms)) return false;
    if (!read_f32(fp, &frame->lua_script_self_ms)) return false;
    if (!read_f32(fp, &frame->lua_c_self_ms)) return false;
    frame->lua_outside_ms = 0.0f;
    if (version >= 6 && !read_f32(fp, &frame->lua_outside_ms)) return false;
    if (!read_f32(fp, &frame->lua_mem_kb)) return false;
    if (!read_u32(fp, &frame->hook_events)) return false;
    if (!read_u32(fp, &frame->flags)) return false;
    frame->actor_x = 0.0f;
    frame->actor_y = 0.0f;
    frame->actor_valid = 0;
    if (version >= 7) {
        if (!read_f32(fp, &frame->actor_x)) return false;
        if (!read_f32(fp, &frame->actor_y)) return false;
        if (!read_u32(fp, &frame->actor_valid)) return false;
    }
    frame->proc_ws_kb = 0.0f;
    frame->proc_private_kb = 0.0f;
    if (version >= 8) {
        if (!read_f32(fp, &frame->proc_ws_kb)) return false;
        if (!read_f32(fp, &frame->proc_private_kb)) return false;
    }
    if (!read_u32(fp, &lua_count)) return false;
    if (!read_u32(fp, &cpp_count)) return false;
    if (!read_u32(fp, &line_count)) return false;
    if (!read_u32(fp, &event_count)) return false;
    if (lua_count > 1000000u || cpp_count > 100000u ||
        line_count > 1000000u || event_count > 10000000u) return false;
    frame->lua_methods.clear();
    frame->cpp_methods.clear();
    frame->file_io.clear();
    frame->lua_lines.clear();
    frame->trace_events.clear();
    frame->lua_methods.reserve(lua_count);
    frame->cpp_methods.reserve(cpp_count);
    frame->lua_lines.reserve(line_count);
    frame->trace_events.reserve(event_count);
    for (i = 0; i < lua_count; i++) {
        LuaMethodSnapshot m;
        std::string name;
        uint32_t kind;
        uint32_t flags;
        if (!read_string(fp, &name)) return false;
        m.name = cocos_debug_pool_intern(name);
        if (!read_u32(fp, &kind)) return false;
        if (!read_u32(fp, &flags)) return false;
        m.kind = (uint16_t)kind;
        m.flags = (uint16_t)flags;
        if (!read_f32(fp, &m.self_ms)) return false;
        if (!read_f32(fp, &m.inclusive_ms)) return false;
        if (!read_f32(fp, &m.max_self_ms)) return false;
        if (!read_f32(fp, &m.max_inclusive_ms)) return false;
        if (!read_u32(fp, &m.calls_started)) return false;
        if (!read_u32(fp, &m.calls_completed)) return false;
        if (!read_u32(fp, &m.active_at_end)) return false;
        frame->lua_methods.push_back(m);
    }
    for (i = 0; i < cpp_count; i++) {
        CppMethodSnapshot m;
        std::string name;
        if (!read_string(fp, &name)) return false;
        m.name = cocos_debug_pool_intern(name);
        if (!read_f32(fp, &m.total_ms)) return false;
        if (!read_f32(fp, &m.max_ms)) return false;
        if (!read_f32(fp, &m.last_ms)) return false;
        if (!read_u32(fp, &m.calls)) return false;
        if (!read_u32(fp, &m.call_sequence)) return false;
        frame->cpp_methods.push_back(m);
    }
    if (version >= 9) {
        uint32_t file_io_count = 0;
        if (!read_u32(fp, &file_io_count)) return false;
        if (file_io_count > 100000u) return false;
        frame->file_io.reserve(file_io_count);
        for (i = 0; i < file_io_count; i++) {
            FileIoSnapshot m;
            std::string path;
            if (!read_string(fp, &path)) return false;
            m.path = cocos_debug_pool_intern(path);
            if (!read_f32(fp, &m.total_ms)) return false;
            if (!read_f32(fp, &m.max_ms)) return false;
            if (!read_f32(fp, &m.last_ms)) return false;
            if (!read_u32(fp, &m.calls)) return false;
            frame->file_io.push_back(m);
        }
    }
    for (i = 0; i < line_count; i++) {
        LuaLineSnapshot m;
        std::string source;
        std::string function_name;
        uint32_t line;
        if (!read_string(fp, &source)) return false;
        if (!read_string(fp, &function_name)) return false;
        m.source = cocos_debug_pool_intern(source);
        m.function_name = cocos_debug_pool_intern(function_name);
        if (!read_u32(fp, &line)) return false;
        if (!read_u32(fp, &m.samples)) return false;
        m.line_ms = 0.0f;
        if (version >= 6 && !read_f32(fp, &m.line_ms)) return false;
        m.line = (int32_t)line;
        frame->lua_lines.push_back(m);
    }
    for (i = 0; i < event_count; i++) {
        LuaTraceEventSnapshot event;
        uint32_t depth;
        uint32_t type;
        uint32_t flags;
        if (!read_u32(fp, &event.event_sequence)) return false;
        if (!read_u64(fp, &event.qpc)) return false;
        if (!read_f64(fp, &event.time_seconds)) return false;
        if (!read_u64(fp, &event.frame_sequence)) return false;
        if (!read_u32(fp, &event.lua_thread_id)) return false;
        if (!read_u32(fp, &event.os_thread_id)) return false;
        if (!read_u32(fp, &event.method_id)) return false;
        {
            std::string method_name;
            if (!read_string(fp, &method_name)) return false;
            event.method_name = cocos_debug_pool_intern(method_name);
        }
        if (!read_u32(fp, &depth)) return false;
        if (!read_u32(fp, &type)) return false;
        if (!read_u32(fp, &flags)) return false;
        event.depth = (int16_t)(int32_t)depth;
        event.type = (uint8_t)type;
        event.flags = (uint8_t)flags;
        frame->trace_events.push_back(event);
    }
    return true;
}

static bool write_image(FILE *fp, const FramePreviewImage &img) {
    uint32_t bytes;
    if (img.width <= 0 || img.height <= 0) return false;
    if (img.width > 2048 || img.height > 2048) return false;
    bytes = (uint32_t)((size_t)img.width * (size_t)img.height * 4u);
    if (img.bgra.size() != bytes) return false;
    if (!write_u32(fp, img.frame_index)) return false;
    if (!write_u32(fp, (uint32_t)img.width)) return false;
    if (!write_u32(fp, (uint32_t)img.height)) return false;
    if (!write_u32(fp, bytes)) return false;
    return write_raw(fp, img.bgra.data(), bytes);
}

static bool read_image(FILE *fp, FramePreviewImage *img) {
    uint32_t frame_index = 0, width = 0, height = 0, bytes = 0;
    uint64_t expect;
    if (!img) return false;
    if (!read_u32(fp, &frame_index)) return false;
    if (!read_u32(fp, &width)) return false;
    if (!read_u32(fp, &height)) return false;
    if (!read_u32(fp, &bytes)) return false;
    if (width == 0 || height == 0 || width > 2048u || height > 2048u) return false;
    expect = (uint64_t)width * (uint64_t)height * 4ull;
    if (bytes != expect || bytes > 16u * 1024u * 1024u) return false;
    img->frame_index = frame_index;
    img->width = (int)width;
    img->height = (int)height;
    img->bgra.assign(bytes, 0);
    return read_raw(fp, &img->bgra[0], bytes);
}

bool cocos_debug_save_recording(const char *path, const std::deque<FrameSnapshot> &frames,
                         const std::vector<FramePreviewImage> *images,
                         std::string *error) {
    FILE *fp;
    char magic[8];
    uint64_t count;
    uint32_t image_count;
    size_t i;
    if (!path || !path[0]) {
        if (error) *error = "empty path";
        return false;
    }
    fp = fopen(path, "wb");
    if (!fp) {
        if (error) *error = "failed to open file for writing";
        return false;
    }
    memcpy(magic, kMagic, sizeof(magic));
    if (!write_raw(fp, magic, sizeof(magic)) || !write_u32(fp, kVersion)) {
        fclose(fp);
        if (error) *error = "failed to write header";
        return false;
    }
    count = (uint64_t)frames.size();
    if (!write_u64(fp, count)) {
        fclose(fp);
        if (error) *error = "failed to write frame count";
        return false;
    }
    for (i = 0; i < frames.size(); i++) {
        if (!write_frame(fp, frames[i])) {
            fclose(fp);
            if (error) *error = "failed to write frame data";
            return false;
        }
    }
    image_count = images ? (uint32_t)images->size() : 0;
    if (image_count > COCOS_DEBUG_PREVIEW_CAP) image_count = COCOS_DEBUG_PREVIEW_CAP;
    if (!write_u32(fp, image_count)) {
        fclose(fp);
        if (error) *error = "failed to write image count";
        return false;
    }
    for (i = 0; i < image_count; i++) {
        if (!write_image(fp, (*images)[i])) {
            fclose(fp);
            if (error) *error = "failed to write preview image";
            return false;
        }
    }
    fclose(fp);
    return true;
}

bool cocos_debug_load_recording(const char *path, std::deque<FrameSnapshot> *frames,
                         std::vector<FramePreviewImage> *images,
                         std::string *error) {
    FILE *fp;
    char magic[8];
    uint32_t version = 0;
    uint64_t count = 0;
    uint64_t i;
    std::deque<FrameSnapshot> loaded;
    std::vector<FramePreviewImage> loaded_images;
    if (!path || !path[0] || !frames) {
        if (error) *error = "invalid arguments";
        return false;
    }
    if (images) images->clear();
    fp = fopen(path, "rb");
    if (!fp) {
        if (error) *error = "failed to open file for reading";
        return false;
    }
    if (!read_raw(fp, magic, sizeof(magic)) ||
        memcmp(magic, kMagic, sizeof(magic)) != 0) {
        fclose(fp);
        if (error) *error = "bad recording magic";
        return false;
    }
    if (!read_u32(fp, &version) || version < kMinSupportedVersion ||
        version > kVersion) {
        fclose(fp);
        if (error) *error = "unsupported recording version";
        return false;
    }
    if (!read_u64(fp, &count) || count > 50000000ull) {
        fclose(fp);
        if (error) *error = "invalid frame count";
        return false;
    }
    for (i = 0; i < count; i++) {
        FrameSnapshot frame;
        if (!read_frame(fp, &frame, version)) {
            fclose(fp);
            if (error) *error = "failed to read frame data";
            return false;
        }
        cocos_debug_fill_scene_render(&frame);
        loaded.push_back(frame);
    }
    {
        uint32_t image_count = 0;
        uint32_t n;
        if (!read_u32(fp, &image_count) || image_count > 100000u) {
            fclose(fp);
            if (error) *error = "invalid preview image count";
            return false;
        }
        loaded_images.reserve(image_count);
        for (n = 0; n < image_count; n++) {
            FramePreviewImage img;
            if (!read_image(fp, &img)) {
                fclose(fp);
                if (error) *error = "failed to read preview image";
                return false;
            }
            loaded_images.push_back(img);
        }
    }
    fclose(fp);
    frames->swap(loaded);
    if (images) images->swap(loaded_images);
    return true;
}
