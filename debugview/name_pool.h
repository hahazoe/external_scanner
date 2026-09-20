#ifndef COCOS_DEBUG_DEBUGVIEW_NAME_POOL_H
#define COCOS_DEBUG_DEBUGVIEW_NAME_POOL_H

#include <string>

/*
 * Global string pool for frame snapshots.
 *
 * Frame snapshots repeat the same method/source names for every frame;
 * storing one std::string per method per frame costs gigabytes at the
 * 10k-frame history cap. Snapshots therefore hold `const std::string *`
 * into this pool. The pool is append-only and never cleared on purpose:
 * snapshot pointers must stay valid for the lifetime of the history, and
 * the number of distinct names is bounded (IPC dictionary cap + whatever a
 * recording file adds).
 */
const std::string *cocos_debug_pool_intern(const char *text);
const std::string *cocos_debug_pool_intern(const std::string &text);

/* Dereference helper for snapshot name fields (NULL -> shared empty). */
inline const std::string &cocos_debug_snapshot_text(const std::string *p) {
    static const std::string kEmpty;
    return p ? *p : kEmpty;
}

#endif
