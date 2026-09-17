#ifndef COCOS_DEBUG_DEBUGVIEW_FRAME_PREVIEW_H
#define COCOS_DEBUG_DEBUGVIEW_FRAME_PREVIEW_H

#include <stdint.h>
#include <vector>
#include <windows.h>
#include <d3d9.h>

#define COCOS_DEBUG_PREVIEW_CAP 600
#define COCOS_DEBUG_PREVIEW_MAX_EDGE 320

struct FramePreviewEntry {
    uint32_t frame_index;
    int width;
    int height;
    IDirect3DTexture9 *texture;
    unsigned char *bgra; /* CPU copy, width*height*4, owned */
};

struct FramePreviewImage {
    uint32_t frame_index;
    int width;
    int height;
    std::vector<unsigned char> bgra;
};

class FramePreview {
public:
    FramePreview();
    ~FramePreview();

    void setDevice(IDirect3DDevice9 *device);
    void invalidateDeviceObjects();
    void clear();

    /* Capture target window client area and bind it to frame_index. */
    bool capture(HWND target, uint32_t frame_index);

    /* Exact or nearest cached frame. Returns NULL if empty. */
    const FramePreviewEntry *findNearest(uint32_t frame_index) const;
    const FramePreviewEntry *latest() const;

    uint32_t count() const { return count_; }

    /* Snapshot CPU images for recording I/O (newest first order not required). */
    void exportImages(std::vector<FramePreviewImage> *out) const;
    /* Replace cache with loaded images and rebuild textures if device is set. */
    bool importImages(const std::vector<FramePreviewImage> &images);

private:
    bool captureToBitmap(HWND target, HBITMAP *out_bitmap, int *out_w, int *out_h);
    bool uploadTexture(FramePreviewEntry *entry, const void *bgra, int width, int height);
    void releaseEntry(FramePreviewEntry *entry);
    void releaseTextureOnly(FramePreviewEntry *entry);
    bool rebuildTextures();

    IDirect3DDevice9 *device_;
    FramePreviewEntry entries_[COCOS_DEBUG_PREVIEW_CAP];
    uint32_t count_;
    uint32_t write_;
    uint32_t latest_frame_;
};

#endif
