#ifndef COCOS_DEBUG_LUA_VM_SCAN_H
#define COCOS_DEBUG_LUA_VM_SCAN_H

#include <windows.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*CocosDebugScanProgressFn)(int percent, const char *stage);

/* Snapshot Lua-named modules in the target (skips lua_*.dll dumps). */
int cocos_debug_collect_lua_modules(DWORD pid);
int cocos_debug_lua_module_count(void);
int cocos_debug_address_in_lua_module(uintptr_t address);
/* Non-zero after a successful collect+detect when the Lua module exports LuaJIT. */
int cocos_debug_lua_is_luajit(void);

/*
 * Scan committed RW memory for a live main lua_State.
 * Supports PUC-Rio Lua 5.1 (global_State heuristic) and LuaJIT 2.x
 * (lua_State + mainthref). Preferring the idle main thread (status==0).
 */
int cocos_debug_scan_lua_state(HANDLE process, uint32_t *out_state,
                        CocosDebugScanProgressFn progress);

#ifdef __cplusplus
}
#endif

#endif
