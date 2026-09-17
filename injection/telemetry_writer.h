#ifndef COCOS_DEBUG_TELEMETRY_WRITER_H
#define COCOS_DEBUG_TELEMETRY_WRITER_H

#include <windows.h>
#include "../common/profiler_ipc.h"

int cocos_debug_telemetry_open(DWORD pid);
void cocos_debug_telemetry_close(void);
CocosDebugIpcBlock *cocos_debug_telemetry_block(void);
const CocosDebugIpcInjectionConfig *cocos_debug_telemetry_config(void);
void cocos_debug_telemetry_set_state(int state, const char *error);
int cocos_debug_telemetry_poll_command(int *sequence);
void cocos_debug_telemetry_ack_command(int sequence);
int cocos_debug_telemetry_viewer_alive(void);
uint32_t cocos_debug_telemetry_intern_method(const char *key);
CocosDebugIpcFrame *cocos_debug_telemetry_begin_frame(uint64_t publish_sequence);
void cocos_debug_telemetry_end_frame(CocosDebugIpcFrame *frame);
void cocos_debug_telemetry_emit_lua_event(const CocosDebugIpcLuaEvent *event);

#endif
