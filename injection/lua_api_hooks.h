#ifndef COCOS_DEBUG_LUA_API_HOOKS_H
#define COCOS_DEBUG_LUA_API_HOOKS_H

#ifdef __cplusplus
extern "C" {
#endif

int cocos_debug_lua_api_hooks_install(void *lua_resume_address,
                               void *lua_newthread_address);
void cocos_debug_lua_api_hooks_remove(void);

#ifdef __cplusplus
}
#endif

#endif
