#ifndef COCOS_DEBUG_DEBUGVIEW_RECORDING_IO_H
#define COCOS_DEBUG_DEBUGVIEW_RECORDING_IO_H

#include <deque>
#include <string>
#include <vector>

#include "telemetry_client.h"
#include "frame_preview.h"

bool cocos_debug_exe_directory(char *out, size_t cap);
bool cocos_debug_make_capture_path(char *out, size_t cap);
bool cocos_debug_save_recording(const char *path, const std::deque<FrameSnapshot> &frames,
                         const std::vector<FramePreviewImage> *images,
                         std::string *error);
bool cocos_debug_load_recording(const char *path, std::deque<FrameSnapshot> *frames,
                         std::vector<FramePreviewImage> *images,
                         std::string *error);

#endif
