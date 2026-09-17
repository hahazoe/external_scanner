#ifndef COCOS_DEBUG_LUAC_DUMP_H
#define COCOS_DEBUG_LUAC_DUMP_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* dump_dir: absolute path like ...\build\luac_dump\<process> (created if needed). */
void cocos_debug_luac_dump_init(const char *dump_dir);

/* Install cdecl prologue hooks on luaL_loadbuffer / luaL_loadfile. */
int cocos_debug_luac_dump_install_hooks(void);

struct lua_State;
/* Call hooked luaL_loadbuffer without running dump logic (for remote eval). */
int cocos_debug_luac_dump_loadbuffer_orig(struct lua_State *L, const char *buff,
                                   size_t size, const char *name);

#ifdef __cplusplus
}
#endif

#endif
