#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <cstddef>

#include <algorithm>
#include <string>
#include <vector>

#include "debug_list.h"
#include "recording_io.h"

static const char kIndexMagic[] = "CocosDebugList 2";
static const char kIndexMagicV1[] = "CocosDebugList 1";
static const char kDefaultCode[] = "print('hello from DebugList')\n";

static void set_error(CocosDebugDebugList *list, const char *message) {
    if (!list) return;
    list->error = message ? message : "";
}

static bool join_path(char *out, size_t cap, const char *dir, const char *file) {
    int n;
    if (!out || !dir || !file || cap < 8) return false;
    n = _snprintf(out, (int)cap, "%s\\%s", dir, file);
    if (n <= 0 || (size_t)n >= cap) return false;
    return true;
}

static bool ensure_dir(const char *dir) {
    if (CreateDirectoryA(dir, NULL)) return true;
    return GetLastError() == ERROR_ALREADY_EXISTS;
}

static bool read_file(const char *path, std::string *out) {
    FILE *fp;
    long size;
    if (!path || !out) return false;
    fp = fopen(path, "rb");
    if (!fp) return false;
    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        return false;
    }
    size = ftell(fp);
    if (size < 0) {
        fclose(fp);
        return false;
    }
    if (fseek(fp, 0, SEEK_SET) != 0) {
        fclose(fp);
        return false;
    }
    out->assign((size_t)size, '\0');
    if (size > 0 && fread(&(*out)[0], 1, (size_t)size, fp) != (size_t)size) {
        fclose(fp);
        return false;
    }
    fclose(fp);
    if (out->size() >= 3 && (unsigned char)(*out)[0] == 0xEF &&
        (unsigned char)(*out)[1] == 0xBB && (unsigned char)(*out)[2] == 0xBF)
        out->erase(0, 3);
    return true;
}

static bool write_file(const char *path, const std::string &data) {
    FILE *fp;
    if (!path) return false;
    fp = fopen(path, "wb");
    if (!fp) return false;
    if (!data.empty() && fwrite(data.data(), 1, data.size(), fp) != data.size()) {
        fclose(fp);
        return false;
    }
    fclose(fp);
    return true;
}

static bool file_exists(const char *path) {
    DWORD attr;
    if (!path || !path[0]) return false;
    attr = GetFileAttributesA(path);
    return attr != INVALID_FILE_ATTRIBUTES &&
           (attr & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

static std::string sanitize_name(const char *name) {
    std::string out;
    size_t start;
    size_t end;
    if (name) {
        for (const char *p = name; *p; ++p) {
            char c = *p;
            if (c == '\t' || c == '\r' || c == '\n')
                c = ' ';
            out += c;
        }
    }
    start = 0;
    while (start < out.size() && (out[start] == ' ' || out[start] == '.'))
        start++;
    end = out.size();
    while (end > start && (out[end - 1] == ' ' || out[end - 1] == '.'))
        end--;
    out = out.substr(start, end - start);
    if (out.empty())
        out = "script";
    if (out.size() > 80)
        out.resize(80);
    return out;
}

static std::string make_slug(const char *name) {
    std::string slug;
    const unsigned char *p = (const unsigned char *)(name ? name : "");
    for (; *p; ++p) {
        char c;
        if ((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') ||
            (*p >= '0' && *p <= '9')) {
            c = (char)*p;
        } else if (*p == ' ' || *p == '-' || *p == '_') {
            c = '_';
        } else {
            continue;
        }
        if (c == '_' && (slug.empty() || slug[slug.size() - 1] == '_'))
            continue;
        slug += c;
        if (slug.size() >= 40)
            break;
    }
    while (!slug.empty() && slug[slug.size() - 1] == '_')
        slug.resize(slug.size() - 1);
    if (slug.empty())
        slug = "script";
    return slug;
}

static std::string make_filename(int id, const char *name) {
    char buf[96];
    _snprintf(buf, sizeof(buf), "%d_%s.lua", id, make_slug(name).c_str());
    buf[sizeof(buf) - 1] = 0;
    return buf;
}

static bool entry_path(const CocosDebugDebugList *list, const CocosDebugDebugListEntry &entry,
                       char *out, size_t cap) {
    if (!list || list->directory.empty() || entry.filename.empty())
        return false;
    return join_path(out, cap, list->directory.c_str(), entry.filename.c_str());
}

static bool load_entry_code(CocosDebugDebugList *list, CocosDebugDebugListEntry *entry) {
    char path[MAX_PATH];
    if (!entry_path(list, *entry, path, sizeof(path))) {
        set_error(list, "Invalid script path");
        return false;
    }
    if (!file_exists(path)) {
        entry->code.clear();
        entry->dirty = false;
        return true;
    }
    if (!read_file(path, &entry->code)) {
        set_error(list, "Failed to read script file");
        return false;
    }
    entry->dirty = false;
    return true;
}

static int parse_id_from_filename(const char *filename) {
    int id = 0;
    const char *p;
    if (!filename || filename[0] < '1' || filename[0] > '9')
        return 0;
    p = filename;
    while (*p >= '0' && *p <= '9') {
        id = id * 10 + (*p - '0');
        p++;
    }
    if (*p != '_')
        return 0;
    return id;
}

static std::string name_from_filename(const char *filename) {
    std::string stem;
    const char *dot;
    const char *p;
    if (!filename)
        return "script";
    stem = filename;
    if (stem.size() > 4 && _stricmp(stem.c_str() + stem.size() - 4, ".lua") == 0)
        stem.resize(stem.size() - 4);
    p = stem.c_str();
    while (*p >= '0' && *p <= '9')
        p++;
    if (*p == '_' && p != stem.c_str())
        p++;
    if (*p)
        return sanitize_name(p);
    dot = strrchr(filename, '.');
    if (dot && dot != filename)
        return sanitize_name(std::string(filename, dot).c_str());
    return sanitize_name(filename);
}

static bool filename_in_list(const CocosDebugDebugList *list, const char *filename) {
    size_t i;
    if (!list || !filename)
        return false;
    for (i = 0; i < list->entries.size(); i++) {
        if (_stricmp(list->entries[i].filename.c_str(), filename) == 0)
            return true;
    }
    return false;
}

static bool id_in_use(const CocosDebugDebugList *list, int id) {
    size_t i;
    if (!list)
        return false;
    for (i = 0; i < list->entries.size(); i++) {
        if (list->entries[i].id == id)
            return true;
    }
    return false;
}

static std::string sanitize_hotkey(const char *hotkey) {
    std::string out;
    if (hotkey) {
        for (const char *p = hotkey; *p; ++p) {
            if (*p == '\t' || *p == '\r' || *p == '\n')
                continue;
            out += *p;
        }
    }
    if (out == "-")
        out.clear();
    if (out.size() > 64)
        out.resize(64);
    return out;
}

static bool parse_index_line(const std::string &line, int version,
                             CocosDebugDebugListEntry *entry) {
    size_t tab1;
    size_t tab2;
    size_t tab3;
    if (line.empty() || line[0] == '#')
        return false;
    tab1 = line.find('\t');
    if (tab1 == std::string::npos)
        return false;
    tab2 = line.find('\t', tab1 + 1);
    entry->id = atoi(line.c_str());
    if (entry->id <= 0)
        return false;
    entry->hotkey.clear();
    if (tab2 == std::string::npos) {
        entry->filename = line.substr(tab1 + 1);
        entry->name = name_from_filename(entry->filename.c_str());
    } else if (version >= 2) {
        tab3 = line.find('\t', tab2 + 1);
        entry->filename = line.substr(tab1 + 1, tab2 - tab1 - 1);
        if (tab3 == std::string::npos) {
            entry->name = sanitize_name(line.c_str() + tab2 + 1);
        } else {
            entry->hotkey = sanitize_hotkey(line.substr(tab2 + 1, tab3 - tab2 - 1).c_str());
            entry->name = sanitize_name(line.c_str() + tab3 + 1);
        }
    } else {
        entry->filename = line.substr(tab1 + 1, tab2 - tab1 - 1);
        entry->name = sanitize_name(line.c_str() + tab2 + 1);
    }
    if (entry->filename.empty())
        return false;
    entry->code.clear();
    entry->dirty = false;
    return true;
}

static bool write_index_unlocked(CocosDebugDebugList *list) {
    std::string text;
    char path[MAX_PATH];
    size_t i;
    if (!join_path(path, sizeof(path), list->directory.c_str(), "index.txt")) {
        set_error(list, "Index path is too long");
        return false;
    }
    text = kIndexMagic;
    text += "\n";
    for (i = 0; i < list->entries.size(); i++) {
        const CocosDebugDebugListEntry &entry = list->entries[i];
        char prefix[32];
        _snprintf(prefix, sizeof(prefix), "%d\t", entry.id);
        text += prefix;
        text += entry.filename;
        text += "\t";
        text += entry.hotkey.empty() ? "-" : entry.hotkey;
        text += "\t";
        text += entry.name;
        text += "\n";
    }
    if (!write_file(path, text)) {
        set_error(list, "Failed to write index.txt");
        return false;
    }
    return true;
}

static void refresh_next_id(CocosDebugDebugList *list) {
    int max_id = 0;
    size_t i;
    for (i = 0; i < list->entries.size(); i++) {
        if (list->entries[i].id > max_id)
            max_id = list->entries[i].id;
    }
    list->next_id = max_id + 1;
    if (list->next_id < 1)
        list->next_id = 1;
}

static bool import_orphan_files(CocosDebugDebugList *list) {
    char pattern[MAX_PATH];
    WIN32_FIND_DATAA found;
    HANDLE handle;
    if (!join_path(pattern, sizeof(pattern), list->directory.c_str(), "*.lua"))
        return false;
    handle = FindFirstFileA(pattern, &found);
    if (handle == INVALID_HANDLE_VALUE)
        return true;
    do {
        CocosDebugDebugListEntry entry;
        int parsed_id;
        if (found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            continue;
        if (filename_in_list(list, found.cFileName))
            continue;
        parsed_id = parse_id_from_filename(found.cFileName);
        if (parsed_id > 0 && !id_in_use(list, parsed_id))
            entry.id = parsed_id;
        else
            entry.id = list->next_id++;
        if (entry.id >= list->next_id)
            list->next_id = entry.id + 1;
        entry.filename = found.cFileName;
        entry.name = name_from_filename(found.cFileName);
        entry.hotkey.clear();
        entry.dirty = false;
        if (!load_entry_code(list, &entry)) {
            FindClose(handle);
            return false;
        }
        list->entries.push_back(entry);
    } while (FindNextFileA(handle, &found));
    FindClose(handle);
    return true;
}

bool cocos_debug_debug_list_load(CocosDebugDebugList *list) {
    char exe_dir[MAX_PATH];
    char dir[MAX_PATH];
    char index_path[MAX_PATH];
    std::string index_text;
    if (!list) return false;
    list->entries.clear();
    list->directory.clear();
    list->next_id = 1;
    list->error.clear();
    if (!cocos_debug_exe_directory(exe_dir, sizeof(exe_dir))) {
        set_error(list, "Failed to resolve executable directory");
        return false;
    }
    if (!join_path(dir, sizeof(dir), exe_dir, "debug_list")) {
        set_error(list, "debug_list path is too long");
        return false;
    }
    if (!ensure_dir(dir)) {
        set_error(list, "Failed to create debug_list directory");
        return false;
    }
    list->directory = dir;
    if (!join_path(index_path, sizeof(index_path), dir, "index.txt")) {
        set_error(list, "Index path is too long");
        return false;
    }
    if (file_exists(index_path) && read_file(index_path, &index_text)) {
        size_t start = 0;
        bool header = true;
        int index_version = 1;
        while (start <= index_text.size()) {
            size_t end = index_text.find('\n', start);
            std::string line;
            CocosDebugDebugListEntry entry;
            if (end == std::string::npos)
                end = index_text.size();
            line = index_text.substr(start, end - start);
            if (!line.empty() && line[line.size() - 1] == '\r')
                line.resize(line.size() - 1);
            if (header) {
                header = false;
                if (line == kIndexMagic) {
                    index_version = 2;
                    start = end + 1;
                    continue;
                }
                if (line == kIndexMagicV1 || line.empty() || line[0] == '#') {
                    index_version = 1;
                    start = end + 1;
                    continue;
                }
            }
            if (parse_index_line(line, index_version, &entry)) {
                if (!load_entry_code(list, &entry))
                    return false;
                list->entries.push_back(entry);
            }
            if (end == index_text.size())
                break;
            start = end + 1;
        }
    }
    refresh_next_id(list);
    if (!import_orphan_files(list))
        return false;
    refresh_next_id(list);
    if (!write_index_unlocked(list))
        return false;
    return true;
}

bool cocos_debug_debug_list_save_index(CocosDebugDebugList *list) {
    if (!list || list->directory.empty()) {
        if (list) set_error(list, "DebugList is not loaded");
        return false;
    }
    return write_index_unlocked(list);
}

bool cocos_debug_debug_list_save_entry(CocosDebugDebugList *list, CocosDebugDebugListEntry *entry) {
    char path[MAX_PATH];
    if (!list || !entry) return false;
    if (!entry_path(list, *entry, path, sizeof(path))) {
        set_error(list, "Invalid script path");
        return false;
    }
    if (!write_file(path, entry->code)) {
        set_error(list, "Failed to write script file");
        return false;
    }
    entry->dirty = false;
    return true;
}

bool cocos_debug_debug_list_flush_all(CocosDebugDebugList *list) {
    size_t i;
    if (!list) return false;
    for (i = 0; i < list->entries.size(); i++) {
        if (!list->entries[i].dirty)
            continue;
        if (!cocos_debug_debug_list_save_entry(list, &list->entries[i]))
            return false;
    }
    return cocos_debug_debug_list_save_index(list);
}

int cocos_debug_debug_list_add(CocosDebugDebugList *list, const char *name, const char *code) {
    CocosDebugDebugListEntry entry;
    std::string cleaned = sanitize_name(name);
    if (!list || list->directory.empty()) {
        if (list) set_error(list, "DebugList is not loaded");
        return -1;
    }
    entry.id = list->next_id++;
    entry.name = cleaned;
    entry.filename = make_filename(entry.id, entry.name.c_str());
    entry.hotkey.clear();
    entry.code = code ? code : kDefaultCode;
    entry.dirty = true;
    if (!cocos_debug_debug_list_save_entry(list, &entry))
        return -1;
    list->entries.push_back(entry);
    write_index_unlocked(list);
    return (int)list->entries.size() - 1;
}

int cocos_debug_debug_list_duplicate(CocosDebugDebugList *list, size_t index) {
    std::string copy_name;
    if (!list || index >= list->entries.size()) {
        if (list) set_error(list, "No script selected");
        return -1;
    }
    copy_name = list->entries[index].name + " copy";
    return cocos_debug_debug_list_add(list, copy_name.c_str(),
                               list->entries[index].code.c_str());
}

bool cocos_debug_debug_list_remove(CocosDebugDebugList *list, size_t index) {
    char path[MAX_PATH];
    if (!list || index >= list->entries.size()) {
        if (list) set_error(list, "No script selected");
        return false;
    }
    if (entry_path(list, list->entries[index], path, sizeof(path)) && file_exists(path))
        DeleteFileA(path);
    list->entries.erase(list->entries.begin() + (std::ptrdiff_t)index);
    return write_index_unlocked(list);
}

bool cocos_debug_debug_list_rename(CocosDebugDebugList *list, size_t index, const char *name) {
    CocosDebugDebugListEntry *entry;
    std::string cleaned = sanitize_name(name);
    std::string new_filename;
    char old_path[MAX_PATH];
    char new_path[MAX_PATH];
    if (!list || index >= list->entries.size()) {
        if (list) set_error(list, "No script selected");
        return false;
    }
    entry = &list->entries[index];
    new_filename = make_filename(entry->id, cleaned.c_str());
    if (cleaned == entry->name && new_filename == entry->filename)
        return true;
    if (!entry_path(list, *entry, old_path, sizeof(old_path))) {
        set_error(list, "Invalid script path");
        return false;
    }
    entry->name = cleaned;
    if (new_filename != entry->filename) {
        if (!join_path(new_path, sizeof(new_path), list->directory.c_str(),
                       new_filename.c_str())) {
            set_error(list, "Renamed path is too long");
            return false;
        }
        if (file_exists(new_path) && _stricmp(old_path, new_path) != 0) {
            set_error(list, "A script file with that name already exists");
            return false;
        }
        if (file_exists(old_path) && _stricmp(old_path, new_path) != 0) {
            if (!MoveFileA(old_path, new_path)) {
                set_error(list, "Failed to rename script file");
                return false;
            }
        }
        entry->filename = new_filename;
    }
    return write_index_unlocked(list);
}

bool cocos_debug_debug_list_set_hotkey(CocosDebugDebugList *list, size_t index, const char *hotkey) {
    if (!list || index >= list->entries.size()) {
        if (list) set_error(list, "No script selected");
        return false;
    }
    list->entries[index].hotkey = sanitize_hotkey(hotkey);
    return write_index_unlocked(list);
}

bool cocos_debug_debug_list_move(CocosDebugDebugList *list, size_t index, int delta,
                          size_t *new_index) {
    size_t target;
    if (!list || index >= list->entries.size() || delta == 0) {
        if (list && delta == 0) {
            if (new_index) *new_index = index;
            return true;
        }
        if (list) set_error(list, "Cannot move that script");
        return false;
    }
    if (delta < 0) {
        if (index == 0) {
            if (new_index) *new_index = index;
            return true;
        }
        target = index - 1;
    } else {
        if (index + 1 >= list->entries.size()) {
            if (new_index) *new_index = index;
            return true;
        }
        target = index + 1;
    }
    std::swap(list->entries[index], list->entries[target]);
    if (new_index)
        *new_index = target;
    return write_index_unlocked(list);
}

bool cocos_debug_debug_list_reload_entry(CocosDebugDebugList *list, size_t index) {
    if (!list || index >= list->entries.size()) {
        if (list) set_error(list, "No script selected");
        return false;
    }
    return load_entry_code(list, &list->entries[index]);
}

bool cocos_debug_debug_list_entry_path(const CocosDebugDebugList *list, size_t index,
                                char *out, size_t cap) {
    if (!list || index >= list->entries.size())
        return false;
    return entry_path(list, list->entries[index], out, cap);
}
