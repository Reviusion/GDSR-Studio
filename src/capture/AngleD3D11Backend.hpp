#pragma once

#include "CaptureScheduler.hpp"

#include <cstdint>
#include <deque>
#include <mutex>
#include <vector>

struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11Texture2D;
struct IDXGISwapChain;
struct ID3D11Multithread;

namespace gdr {

// ANGLE / GLES capture backend.
//
// GD on Windows renders through ANGLE, which is a GLES front-end over D3D11.
// We reach the *underlying* ID3D11Device via EGL device queries, then read the
// swapchain backbuffer back through a small ring of D3D11 STAGING textures:
//
//   render thread (issueCopy):  CopyResource(backbuffer -> ring[slot])   [cheap]
//   capture thread (mapSlot):   Map(ring[slot], DO_NOT_WAIT) + memcpy    [heavy]
//
// The two threads share ANGLE's immediate context, so it is put into
// ID3D11Multithread protected mode; if that is unavailable the backend reports
// threaded()==false and the scheduler falls back to captureSync() on the render
// thread (degradation without regression).
class AngleD3D11Backend : public ICaptureBackend {
public:
    AngleD3D11Backend() = default;
    ~AngleD3D11Backend() override;

    bool init() override;
    bool threaded() const override { return m_threadSafe; }
    bool isBGRA() const override;

    int  issueCopy(int reqW, int reqH) override;
    int  acquireCopied() override;
    bool mapSlot(int id, RawFrame& out) override;
    void releaseSlot(int id) override;

    bool captureSync(int reqW, int reqH, RawFrame& out) override;

    void reset() override;
    void shutdown() override;

private:
    static constexpr int kSlots = 6;   // deeper ring absorbs encoder/GPU jitter so
                                       // the render thread blocks on borrow less often

    enum class SlotState { Free, Copied, Mapping };
    struct Slot {
        ID3D11Texture2D* tex = nullptr;
        SlotState state = SlotState::Free;
        int w = 0, h = 0;
    };

    // Returns an AddRef'd backbuffer texture for the just-rendered frame, plus
    // its size and DXGI format. Caller must Release(). nullptr on failure.
    ID3D11Texture2D* acquireBackbuffer(int& w, int& h, unsigned int& fmt);
    // Cached single-sample target for MSAA backbuffers — reused across frames so
    // ResolveSubresource doesn't allocate a texture every captured frame (jitter).
    ID3D11Texture2D* m_resolveTex = nullptr;
    int m_resolveW = 0, m_resolveH = 0; unsigned int m_resolveFmt = 0;
    bool findSwapChain();
    bool ensureRing(int w, int h, unsigned int fmt);
    void releaseRing();
    // Tightly pack a mapped surface (strip the row-pitch padding). ANGLE's
    // default framebuffer is already stored bottom-up (GL convention), so this
    // copies rows straight through — the single flip a top-down video needs is
    // applied once by ffmpeg's "vflip". (Flipping here too would double-flip.)
    void copyRows(const void* src, int rowPitch, int w, int h,
                  std::vector<unsigned char>& out);

    // ---- ANGLE / EGL plumbing ----
    void* m_eglModule = nullptr;
    using pfn_eglGetCurrentDisplay_t = void* (*)();
    using pfn_eglGetProcAddress_t    = void* (*)(const char*);
    pfn_eglGetCurrentDisplay_t eglGetCurrentDisplay = nullptr;
    pfn_eglGetProcAddress_t    eglGetProcAddress    = nullptr;
    bool (*qryDisplayAttrib)(void*, int, long long*) = nullptr;
    bool (*qryDeviceAttrib)(void*, int, long long*)  = nullptr;
    static constexpr int EGL_DEVICE_EXT         = 0x322C;
    static constexpr int EGL_D3D11_DEVICE_ANGLE = 0x33A1;

    ID3D11Device*        m_device    = nullptr;
    ID3D11DeviceContext* m_context   = nullptr;
    IDXGISwapChain*      m_swapChain = nullptr;
    ID3D11Multithread*   m_mt        = nullptr;
    bool                 m_threadSafe = false;

    // Ring + its ordering FIFO, guarded by m_ringMtx (issueCopy on render thread,
    // acquire/release on the capture worker).
    std::mutex      m_ringMtx;
    Slot            m_slots[kSlots];
    std::deque<int> m_copied;          // slot ids in copy order, awaiting map
    int             m_ringW = 0, m_ringH = 0;
    unsigned int    m_ringFmt = 0;     // DXGI_FORMAT (0 = unknown)
    bool            m_isBGRA = true;   // derived from the swapchain format

    // captureSync repeat cache.
    std::vector<unsigned char> m_cache;
    int m_cacheW = 0, m_cacheH = 0;
    bool m_hasCache = false;
};

} // namespace gdr
