#include "frame_preview.h"

#include <stdlib.h>
#include <string.h>

#ifndef PW_RENDERFULLCONTENT
#define PW_RENDERFULLCONTENT 0x00000002
#endif

/* Old MinGW winuser.h may omit PrintWindow. */
extern "C" BOOL WINAPI PrintWindow(HWND hwnd, HDC hdcBlt, UINT nFlags);

FramePreview::FramePreview()
    : device_(NULL), count_(0), write_(0), latest_frame_(0) {
    memset(entries_, 0, sizeof(entries_));
}

FramePreview::~FramePreview() {
    clear();
}

void FramePreview::setDevice(IDirect3DDevice9 *device) {
    device_ = device;
    rebuildTextures();
}

void FramePreview::invalidateDeviceObjects() {
    for (uint32_t i = 0; i < COCOS_DEBUG_PREVIEW_CAP; i++)
        releaseTextureOnly(&entries_[i]);
}

void FramePreview::clear() {
    for (uint32_t i = 0; i < COCOS_DEBUG_PREVIEW_CAP; i++)
        releaseEntry(&entries_[i]);
    count_ = 0;
    write_ = 0;
    latest_frame_ = 0;
}

void FramePreview::releaseTextureOnly(FramePreviewEntry *entry) {
    if (!entry) return;
    if (entry->texture) {
        entry->texture->Release();
        entry->texture = NULL;
    }
}

void FramePreview::releaseEntry(FramePreviewEntry *entry) {
    if (!entry) return;
    releaseTextureOnly(entry);
    free(entry->bgra);
    entry->bgra = NULL;
    entry->frame_index = 0;
    entry->width = 0;
    entry->height = 0;
}

bool FramePreview::rebuildTextures() {
    uint32_t n;
    if (!device_) return false;
    for (n = 0; n < count_; n++) {
        uint32_t i = (write_ + COCOS_DEBUG_PREVIEW_CAP - count_ + n) % COCOS_DEBUG_PREVIEW_CAP;
        FramePreviewEntry *entry = &entries_[i];
        if (!entry->bgra || entry->width <= 0 || entry->height <= 0) continue;
        releaseTextureOnly(entry);
        uploadTexture(entry, entry->bgra, entry->width, entry->height);
    }
    return true;
}

bool FramePreview::captureToBitmap(HWND target, HBITMAP *out_bitmap, int *out_w, int *out_h) {
    RECT client;
    HDC window_dc = NULL;
    HDC mem_dc = NULL;
    HBITMAP bitmap = NULL;
    HGDIOBJ old = NULL;
    int width, height;
    BOOL printed = FALSE;

    if (!out_bitmap || !out_w || !out_h) return false;
    *out_bitmap = NULL;
    *out_w = 0;
    *out_h = 0;
    if (!target || !IsWindow(target) || IsIconic(target)) return false;
    if (!GetClientRect(target, &client)) return false;
    width = client.right - client.left;
    height = client.bottom - client.top;
    if (width <= 1 || height <= 1) return false;

    window_dc = GetDC(target);
    if (!window_dc) return false;
    mem_dc = CreateCompatibleDC(window_dc);
    if (!mem_dc) {
        ReleaseDC(target, window_dc);
        return false;
    }
    bitmap = CreateCompatibleBitmap(window_dc, width, height);
    if (!bitmap) {
        DeleteDC(mem_dc);
        ReleaseDC(target, window_dc);
        return false;
    }
    old = SelectObject(mem_dc, bitmap);
    printed = PrintWindow(target, mem_dc, PW_RENDERFULLCONTENT);
    if (!printed)
        printed = PrintWindow(target, mem_dc, 0);
    if (!printed)
        BitBlt(mem_dc, 0, 0, width, height, window_dc, 0, 0, SRCCOPY);
    SelectObject(mem_dc, old);
    DeleteDC(mem_dc);
    ReleaseDC(target, window_dc);

    *out_bitmap = bitmap;
    *out_w = width;
    *out_h = height;
    return true;
}

static void scaleNearestBGRA(const unsigned char *src, int src_w, int src_h,
                             unsigned char *dst, int dst_w, int dst_h) {
    int y, x;
    for (y = 0; y < dst_h; y++) {
        int sy = y * src_h / dst_h;
        const unsigned char *src_row = src + (size_t)sy * (size_t)src_w * 4u;
        unsigned char *dst_row = dst + (size_t)y * (size_t)dst_w * 4u;
        for (x = 0; x < dst_w; x++) {
            int sx = x * src_w / dst_w;
            memcpy(dst_row + x * 4, src_row + sx * 4, 4);
        }
    }
}

bool FramePreview::uploadTexture(FramePreviewEntry *entry, const void *bgra,
                                 int width, int height) {
    HRESULT hr;
    D3DLOCKED_RECT locked;
    int y;
    if (!device_ || !entry || !bgra || width <= 0 || height <= 0) return false;
    releaseTextureOnly(entry);
    hr = device_->CreateTexture(width, height, 1, 0, D3DFMT_A8R8G8B8,
                                D3DPOOL_MANAGED, &entry->texture, NULL);
    if (FAILED(hr) || !entry->texture) return false;
    hr = entry->texture->LockRect(0, &locked, NULL, 0);
    if (FAILED(hr)) {
        releaseTextureOnly(entry);
        return false;
    }
    for (y = 0; y < height; y++) {
        memcpy((unsigned char *)locked.pBits + y * locked.Pitch,
               (const unsigned char *)bgra + (size_t)y * (size_t)width * 4u,
               (size_t)width * 4u);
    }
    entry->texture->UnlockRect(0);
    entry->width = width;
    entry->height = height;
    return true;
}

bool FramePreview::capture(HWND target, uint32_t frame_index) {
    HBITMAP bitmap = NULL;
    BITMAPINFOHEADER bi;
    unsigned char *src_bits = NULL;
    unsigned char *scaled = NULL;
    HDC screen_dc = NULL;
    int src_w = 0, src_h = 0;
    int dst_w, dst_h;
    int max_edge;
    FramePreviewEntry *entry;
    bool ok = false;
    int y;
    size_t bytes;

    if (!device_) return false;
    if (!captureToBitmap(target, &bitmap, &src_w, &src_h)) return false;

    memset(&bi, 0, sizeof(bi));
    bi.biSize = sizeof(bi);
    bi.biWidth = src_w;
    bi.biHeight = -src_h; /* top-down */
    bi.biPlanes = 1;
    bi.biBitCount = 32;
    bi.biCompression = BI_RGB;
    src_bits = (unsigned char *)malloc((size_t)src_w * (size_t)src_h * 4u);
    if (!src_bits) {
        DeleteObject(bitmap);
        return false;
    }
    screen_dc = GetDC(NULL);
    if (!screen_dc ||
        !GetDIBits(screen_dc, bitmap, 0, src_h, src_bits, (BITMAPINFO *)&bi,
                   DIB_RGB_COLORS)) {
        if (screen_dc) ReleaseDC(NULL, screen_dc);
        free(src_bits);
        DeleteObject(bitmap);
        return false;
    }
    ReleaseDC(NULL, screen_dc);
    DeleteObject(bitmap);

    max_edge = src_w > src_h ? src_w : src_h;
    if (max_edge > COCOS_DEBUG_PREVIEW_MAX_EDGE) {
        dst_w = src_w * COCOS_DEBUG_PREVIEW_MAX_EDGE / max_edge;
        dst_h = src_h * COCOS_DEBUG_PREVIEW_MAX_EDGE / max_edge;
        if (dst_w < 1) dst_w = 1;
        if (dst_h < 1) dst_h = 1;
    } else {
        dst_w = src_w;
        dst_h = src_h;
    }

    /* GDI BGRA already matches D3DFMT_A8R8G8B8 channel order on little-endian. */
    if (dst_w != src_w || dst_h != src_h) {
        scaled = (unsigned char *)malloc((size_t)dst_w * (size_t)dst_h * 4u);
        if (!scaled) {
            free(src_bits);
            return false;
        }
        scaleNearestBGRA(src_bits, src_w, src_h, scaled, dst_w, dst_h);
        free(src_bits);
        src_bits = scaled;
        scaled = NULL;
    }

    /* Force opaque alpha. */
    for (y = 0; y < dst_w * dst_h; y++)
        src_bits[y * 4 + 3] = 255;

    entry = &entries_[write_];
    releaseEntry(entry);
    bytes = (size_t)dst_w * (size_t)dst_h * 4u;
    entry->bgra = (unsigned char *)malloc(bytes);
    if (!entry->bgra) {
        free(src_bits);
        return false;
    }
    memcpy(entry->bgra, src_bits, bytes);
    if (uploadTexture(entry, entry->bgra, dst_w, dst_h)) {
        entry->frame_index = frame_index;
        write_ = (write_ + 1) % COCOS_DEBUG_PREVIEW_CAP;
        if (count_ < COCOS_DEBUG_PREVIEW_CAP) count_++;
        latest_frame_ = frame_index;
        ok = true;
    } else {
        releaseEntry(entry);
    }
    free(src_bits);
    return ok;
}

void FramePreview::exportImages(std::vector<FramePreviewImage> *out) const {
    uint32_t n;
    if (!out) return;
    out->clear();
    if (count_ == 0) return;
    out->reserve(count_);
    for (n = 0; n < count_; n++) {
        uint32_t i = (write_ + COCOS_DEBUG_PREVIEW_CAP - count_ + n) % COCOS_DEBUG_PREVIEW_CAP;
        const FramePreviewEntry *entry = &entries_[i];
        FramePreviewImage img;
        size_t bytes;
        if (!entry->bgra || entry->width <= 0 || entry->height <= 0) continue;
        bytes = (size_t)entry->width * (size_t)entry->height * 4u;
        img.frame_index = entry->frame_index;
        img.width = entry->width;
        img.height = entry->height;
        img.bgra.assign(entry->bgra, entry->bgra + bytes);
        out->push_back(img);
    }
}

bool FramePreview::importImages(const std::vector<FramePreviewImage> &images) {
    size_t i;
    size_t n = images.size();
    clear();
    if (n == 0) return true;
    if (n > COCOS_DEBUG_PREVIEW_CAP) {
        /* Keep the newest images if the file has more than the ring can hold. */
        i = n - COCOS_DEBUG_PREVIEW_CAP;
    } else {
        i = 0;
    }
    for (; i < n; i++) {
        const FramePreviewImage &img = images[i];
        FramePreviewEntry *entry;
        size_t bytes;
        if (img.width <= 0 || img.height <= 0) continue;
        if (img.width > 2048 || img.height > 2048) continue;
        bytes = (size_t)img.width * (size_t)img.height * 4u;
        if (img.bgra.size() != bytes) continue;
        entry = &entries_[write_];
        releaseEntry(entry);
        entry->bgra = (unsigned char *)malloc(bytes);
        if (!entry->bgra) continue;
        memcpy(entry->bgra, img.bgra.data(), bytes);
        entry->frame_index = img.frame_index;
        entry->width = img.width;
        entry->height = img.height;
        if (device_)
            uploadTexture(entry, entry->bgra, img.width, img.height);
        write_ = (write_ + 1) % COCOS_DEBUG_PREVIEW_CAP;
        if (count_ < COCOS_DEBUG_PREVIEW_CAP) count_++;
        latest_frame_ = img.frame_index;
    }
    return count_ > 0 || images.empty();
}

const FramePreviewEntry *FramePreview::latest() const {
    uint32_t i;
    if (count_ == 0) return NULL;
    i = (write_ + COCOS_DEBUG_PREVIEW_CAP - 1) % COCOS_DEBUG_PREVIEW_CAP;
    if (!entries_[i].bgra && !entries_[i].texture) return NULL;
    return &entries_[i];
}

const FramePreviewEntry *FramePreview::findNearest(uint32_t frame_index) const {
    const FramePreviewEntry *best = NULL;
    uint32_t best_distance = 0xFFFFFFFFu;
    uint32_t n;
    if (count_ == 0) return NULL;
    for (n = 0; n < count_; n++) {
        uint32_t i = (write_ + COCOS_DEBUG_PREVIEW_CAP - 1 - n) % COCOS_DEBUG_PREVIEW_CAP;
        const FramePreviewEntry *entry = &entries_[i];
        uint32_t distance;
        if (!entry->bgra && !entry->texture) continue;
        if (entry->frame_index == frame_index) return entry;
        distance = entry->frame_index > frame_index
            ? entry->frame_index - frame_index
            : frame_index - entry->frame_index;
        if (distance < best_distance) {
            best_distance = distance;
            best = entry;
        }
    }
    return best;
}
