#ifndef COCOS_DEBUG_MINGW_COMPAT_H
#define COCOS_DEBUG_MINGW_COMPAT_H

#include <stdint.h>
#include <string.h>
#include <time.h>

#if defined(__MINGW32__) && (__GNUC__ < 8)
static inline time_t cocos_debug_mkgmtime(struct tm *value) {
    /* ImPlot's calendar-time axis is not used by DebugView. */
    return mktime(value);
}
static inline int cocos_debug_gmtime_s(struct tm *out, const time_t *value) {
    struct tm *result = gmtime(value);
    if (!result) return 1;
    *out = *result;
    return 0;
}
static inline int cocos_debug_localtime_s(struct tm *out, const time_t *value) {
    struct tm *result = localtime(value);
    if (!result) return 1;
    *out = *result;
    return 0;
}
#define _mkgmtime cocos_debug_mkgmtime
#define gmtime_s cocos_debug_gmtime_s
#define localtime_s cocos_debug_localtime_s
#endif

#ifndef TME_NONCLIENT
#define TME_NONCLIENT 0x00000010
#endif
#ifndef GET_XBUTTON_WPARAM
#define GET_XBUTTON_WPARAM(wParam) (HIWORD(wParam))
#endif
#ifndef WM_IME_COMPOSITION
#define WM_IME_COMPOSITION 0x010F
#endif
#ifndef WM_IME_CHAR
#define WM_IME_CHAR 0x0286
#endif
#ifndef GCS_RESULTSTR
#define GCS_RESULTSTR 0x0800
#endif
#ifndef RTL_OSVERSIONINFOEXW
#define RTL_OSVERSIONINFOEXW OSVERSIONINFOEXW
#endif

#endif
