/*
 * Freeze guard for inline-hook patching inside the target process.
 *
 * A 5-byte jmp patch is two writes (opcode, then rel32). If another thread
 * fetches instructions from the patched region between those writes it can
 * execute a torn instruction and crash. patch_freeze_begin() suspends every
 * other thread in this process; patch_freeze_end() resumes exactly the
 * threads that were suspended.
 *
 * Rules:
 * - Do ALL allocations (VirtualAlloc etc.) BEFORE patch_freeze_begin(); a
 *   suspended thread may hold the heap/loader lock and allocating while
 *   frozen can deadlock.
 * - Keep the frozen section as short as possible (writes + VirtualProtect).
 * - Residual risks, accepted: a thread created after begin() is not frozen;
 *   a thread suspended while its EIP sits inside the patched bytes resumes
 *   into rewritten code (Detours-style EIP adjustment is out of scope).
 *
 * Header-only (static) so every injection TU gets its own copy and the test
 * exes keep linking without extra sources. begin/end must be called from the
 * same thread and are not re-entrant.
 */
#ifndef COCOS_DEBUG_PATCH_FREEZE_H
#define COCOS_DEBUG_PATCH_FREEZE_H

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>

#define PATCH_FREEZE_MAX_THREADS 256

static DWORD patch_freeze_tids[PATCH_FREEZE_MAX_THREADS];
static int patch_freeze_count;

static inline int patch_freeze_begin(void) {
    HANDLE snap;
    THREADENTRY32 te;
    DWORD self = GetCurrentThreadId();
    DWORD pid = GetCurrentProcessId();
    patch_freeze_count = 0;
    snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    memset(&te, 0, sizeof(te));
    te.dwSize = sizeof(te);
    if (Thread32First(snap, &te)) {
        do {
            HANDLE t;
            if (te.th32OwnerProcessID != pid || te.th32ThreadID == self)
                continue;
            if (patch_freeze_count >= PATCH_FREEZE_MAX_THREADS)
                break;
            t = OpenThread(THREAD_SUSPEND_RESUME, FALSE, te.th32ThreadID);
            if (!t)
                continue;
            if (SuspendThread(t) != (DWORD)-1)
                patch_freeze_tids[patch_freeze_count++] = te.th32ThreadID;
            CloseHandle(t);
        } while (Thread32Next(snap, &te));
    }
    CloseHandle(snap);
    return patch_freeze_count;
}

static inline void patch_freeze_end(void) {
    int i;
    for (i = 0; i < patch_freeze_count; i++) {
        HANDLE t = OpenThread(THREAD_SUSPEND_RESUME, FALSE, patch_freeze_tids[i]);
        if (t) {
            ResumeThread(t);
            CloseHandle(t);
        }
    }
    patch_freeze_count = 0;
}

#endif
