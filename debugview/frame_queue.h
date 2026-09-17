#ifndef COCOS_DEBUG_DEBUGVIEW_FRAME_QUEUE_H
#define COCOS_DEBUG_DEBUGVIEW_FRAME_QUEUE_H

/* Requires FrameSnapshot to be defined before this header is included. */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <deque>
#include <vector>

class FrameQueue {
public:
    FrameQueue() {
        InitializeCriticalSection(&lock_);
    }

    ~FrameQueue() {
        DeleteCriticalSection(&lock_);
    }

    void push(const FrameSnapshot &frame) {
        EnterCriticalSection(&lock_);
        queue_.push_back(frame);
        LeaveCriticalSection(&lock_);
    }

    size_t popBatch(std::vector<FrameSnapshot> *out, size_t max_n) {
        size_t n = 0;
        if (!out || max_n == 0) return 0;
        EnterCriticalSection(&lock_);
        while (!queue_.empty() && n < max_n) {
            out->push_back(queue_.front());
            queue_.pop_front();
            n++;
        }
        LeaveCriticalSection(&lock_);
        return n;
    }

    size_t size() const {
        size_t n;
        EnterCriticalSection(&lock_);
        n = queue_.size();
        LeaveCriticalSection(&lock_);
        return n;
    }

    void clear() {
        EnterCriticalSection(&lock_);
        queue_.clear();
        LeaveCriticalSection(&lock_);
    }

private:
    mutable CRITICAL_SECTION lock_;
    std::deque<FrameSnapshot> queue_;
};

#endif
