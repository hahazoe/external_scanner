#ifndef COCOS_DEBUG_DEBUG_LIST_H
#define COCOS_DEBUG_DEBUG_LIST_H

#include <string>
#include <vector>

struct CocosDebugDebugListEntry {
    int id;
    std::string name;
    std::string filename;
    std::string hotkey;
    std::string code;
    bool dirty;
};

struct CocosDebugDebugList {
    std::string directory;
    std::vector<CocosDebugDebugListEntry> entries;
    int next_id;
    std::string error;
};

bool cocos_debug_debug_list_load(CocosDebugDebugList *list);
bool cocos_debug_debug_list_save_index(CocosDebugDebugList *list);
bool cocos_debug_debug_list_save_entry(CocosDebugDebugList *list, CocosDebugDebugListEntry *entry);
bool cocos_debug_debug_list_flush_all(CocosDebugDebugList *list);
int cocos_debug_debug_list_add(CocosDebugDebugList *list, const char *name, const char *code);
int cocos_debug_debug_list_duplicate(CocosDebugDebugList *list, size_t index);
bool cocos_debug_debug_list_remove(CocosDebugDebugList *list, size_t index);
bool cocos_debug_debug_list_rename(CocosDebugDebugList *list, size_t index, const char *name);
bool cocos_debug_debug_list_set_hotkey(CocosDebugDebugList *list, size_t index, const char *hotkey);
bool cocos_debug_debug_list_move(CocosDebugDebugList *list, size_t index, int delta, size_t *new_index);
bool cocos_debug_debug_list_reload_entry(CocosDebugDebugList *list, size_t index);
bool cocos_debug_debug_list_entry_path(const CocosDebugDebugList *list, size_t index,
                                char *out, size_t cap);

#endif
