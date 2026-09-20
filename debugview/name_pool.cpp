#include "name_pool.h"

#include <deque>
#include <unordered_map>

namespace {
/* deque: push_back never invalidates pointers to existing elements. */
std::deque<std::string> g_pool_strings;
std::unordered_map<std::string, const std::string *> g_pool_index;
}

const std::string *cocos_debug_pool_intern(const char *text) {
    std::string key = text ? text : "";
    std::unordered_map<std::string, const std::string *>::const_iterator it =
        g_pool_index.find(key);
    if (it != g_pool_index.end())
        return it->second;
    g_pool_strings.push_back(key);
    const std::string *pooled = &g_pool_strings.back();
    g_pool_index.insert(std::make_pair(*pooled, pooled));
    return pooled;
}

const std::string *cocos_debug_pool_intern(const std::string &text) {
    return cocos_debug_pool_intern(text.c_str());
}
