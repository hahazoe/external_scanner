#ifndef COCOS_DEBUG_LUA_API_HOOKS_H
#define COCOS_DEBUG_LUA_API_HOOKS_H

#ifdef __cplusplus
extern "C" {
#endif

/* Installs JMP trampolines on the Lua entry points the profiler needs to see
   from the outside.  Returns a bitmask: 1 = lua_resume, 2 = lua_newthread,
   4 = lua_pcall, 8 = lua_call.  lua_resume and lua_pcall are the only places
   a thrown Lua error can be caught, so without 4 the profiler cannot tell
   that Lua has unwound frames behind its back.  lua_call is optional: without
   it, empty-stack VM time on unprotected C→Lua entries is undercounted. */
int cocos_debug_lua_api_hooks_install(void *lua_resume_address,
                               void *lua_newthread_address,
                               void *lua_pcall_address,
                               void *lua_call_address);
void cocos_debug_lua_api_hooks_remove(void);

#ifdef __cplusplus
}
#endif

#endif
