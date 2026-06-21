#include "AngleD3D11Backend.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d11.h>
#include <d3d11_4.h>   // ID3D11Multithread
#include <dxgi1_2.h>
#include <GL/gl.h>     // glGetString / GL_VERSION (which renderer GD frames go through)
#include <algorithm>
#include <cstring>
#include <Geode/loader/Log.hpp>

namespace gdr {

namespace {
    static const char* kEglDllNames[] = { "libEGL.dll", nullptr };

    // RAII guard for ANGLE's immediate-context multithread lock. No-op if the
    // context could not be put into protected mode.
    struct MtGuard {
        ID3D11Multithread* m;
        explicit MtGuard(ID3D11Multithread* mt) : m(mt) { if (m) m->Enter(); }
        ~MtGuard() { if (m) m->Leave(); }
        MtGuard(const MtGuard&) = delete;
        MtGuard& operator=(const MtGuard&) = delete;
    };

    DXGI_FORMAT stagingFormatFor(DXGI_FORMAT src, bool& bgra) {
        switch (src) {
            case DXGI_FORMAT_B8G8R8A8_UNORM:
            case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
            case DXGI_FORMAT_B8G8R8A8_TYPELESS:
                bgra = true;  return DXGI_FORMAT_B8G8R8A8_UNORM;
            case DXGI_FORMAT_R8G8B8A8_UNORM:
            case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
            case DXGI_FORMAT_R8G8B8A8_TYPELESS:
                bgra = false; return DXGI_FORMAT_R8G8B8A8_UNORM;
            default:
                // Unknown family — assume BGRA byte order and copy raw.
                bgra = true;  return src;
        }
    }
}

AngleD3D11Backend::~AngleD3D11Backend() { shutdown(); }

bool AngleD3D11Backend::isBGRA() const { return m_isBGRA; }

bool AngleD3D11Backend::init() {
    if (m_device) return true;

    // GATE: only take the ANGLE/D3D11 path if GD's CURRENT GL context is actually
    // ANGLE (GLES-over-D3D11). libEGL.dll can be loaded in-process by another mod /
    // overlay while the game still renders through NATIVE OpenGL (nvoglv64). In that
    // case eglGetCurrentDisplay() succeeds and we'd grab an ANGLE D3D11 device that
    // is NOT the game's real output surface -> wrong/garbage capture and the
    // "nvoglv64 crash logged as GDR Angle" confusion. The GL_VERSION string is the
    // ground truth for which renderer the game frames go through.
    {
        const char* glv = reinterpret_cast<const char*>(glGetString(GL_VERSION));
        bool isAngle = glv && (std::strstr(glv, "ANGLE") || std::strstr(glv, "OpenGL ES"));
        if (!isAngle) {
            geode::log::info("GDR Angle: native GL context ({}), using GL readback",
                             glv ? glv : "unknown");
            return false;   // -> CaptureScheduler falls back to OpenGLBlockingBackend
        }
    }

    for (const char** d = kEglDllNames; *d; ++d) { m_eglModule = LoadLibraryA(*d); if (m_eglModule) break; }
    if (!m_eglModule) { geode::log::warn("GDR Angle: libEGL.dll not found"); return false; }

    eglGetCurrentDisplay = (pfn_eglGetCurrentDisplay_t)GetProcAddress((HMODULE)m_eglModule, "eglGetCurrentDisplay");
    eglGetProcAddress    = (pfn_eglGetProcAddress_t)   GetProcAddress((HMODULE)m_eglModule, "eglGetProcAddress");
    if (!eglGetCurrentDisplay || !eglGetProcAddress) { geode::log::warn("GDR Angle: egl entrypoints missing"); shutdown(); return false; }

    qryDisplayAttrib = (decltype(qryDisplayAttrib))eglGetProcAddress("eglQueryDisplayAttribEXT");
    qryDeviceAttrib  = (decltype(qryDeviceAttrib)) eglGetProcAddress("eglQueryDeviceAttribEXT");
    if (!qryDisplayAttrib || !qryDeviceAttrib) { geode::log::warn("GDR Angle: egl device-query ext unavailable"); shutdown(); return false; }

    void* dsp = eglGetCurrentDisplay();
    if (!dsp) { geode::log::warn("GDR Angle: no current EGL display"); shutdown(); return false; }

    long long eglDev = 0;
    if (!qryDisplayAttrib(dsp, EGL_DEVICE_EXT, &eglDev) || !eglDev) { geode::log::warn("GDR Angle: EGL_DEVICE_EXT query failed"); shutdown(); return false; }

    long long d3dDev = 0;
    if (!qryDeviceAttrib((void*)eglDev, EGL_D3D11_DEVICE_ANGLE, &d3dDev) || !d3dDev) {
        geode::log::warn("GDR Angle: not a D3D11 ANGLE device"); shutdown(); return false;
    }
    m_device = (ID3D11Device*)d3dDev;
    m_device->AddRef();
    m_device->GetImmediateContext(&m_context);
    if (!m_context) { geode::log::warn("GDR Angle: no immediate context"); shutdown(); return false; }

    // Try to enable multithread protection so the capture worker can touch the
    // immediate context safely. If unavailable, we run synchronously instead.
    if (SUCCEEDED(m_context->QueryInterface(__uuidof(ID3D11Multithread), (void**)&m_mt)) && m_mt) {
        m_mt->SetMultithreadProtected(TRUE);
        m_threadSafe = true;
    } else {
        m_mt = nullptr;
        m_threadSafe = false;
        geode::log::warn("GDR Angle: ID3D11Multithread unavailable, capture stays on render thread");
    }

    geode::log::info("GDR Angle: D3D11 device {:p}, threaded={}", (void*)m_device, m_threadSafe);
    return true;
}

bool AngleD3D11Backend::findSwapChain() {
    if (m_swapChain) return true;
    if (!m_device || !m_context) return false;
    ID3D11RenderTargetView* r = nullptr;
    m_context->OMGetRenderTargets(1, &r, nullptr);
    if (!r) return false;
    ID3D11Resource* rs = nullptr; r->GetResource(&rs); r->Release();
    if (!rs) return false;
    IDXGIResource* dx = nullptr;
    if (SUCCEEDED(rs->QueryInterface(__uuidof(IDXGIResource), (void**)&dx))) {
        IDXGISwapChain* sc = nullptr;
        if (SUCCEEDED(dx->GetParent(__uuidof(IDXGISwapChain), (void**)&sc)) && sc) m_swapChain = sc;
        dx->Release();
    }
    rs->Release();
    return m_swapChain != nullptr;
}

// Caller must hold the MtGuard (touches the immediate context).
ID3D11Texture2D* AngleD3D11Backend::acquireBackbuffer(int& w, int& h, unsigned int& fmt) {
    ID3D11Texture2D* result = nullptr;
    DXGI_FORMAT srcFmt = DXGI_FORMAT_UNKNOWN;

    ID3D11RenderTargetView* rtv = nullptr;
    m_context->OMGetRenderTargets(1, &rtv, nullptr);
    if (rtv) {
        ID3D11Resource* rs = nullptr; rtv->GetResource(&rs); rtv->Release();
        if (rs) {
            ID3D11Texture2D* t = nullptr;
            if (SUCCEEDED(rs->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&t)) && t) {
                D3D11_TEXTURE2D_DESC x = {}; t->GetDesc(&x);
                w = (int)x.Width; h = (int)x.Height; srcFmt = x.Format;
                if (x.SampleDesc.Count > 1) {
                    // Reuse a cached single-sample resolve target instead of
                    // allocating one every frame (per-frame CreateTexture2D was a
                    // source of capture-swap jitter). Recreate only on dim/fmt change.
                    if (!m_resolveTex || m_resolveW != (int)x.Width ||
                        m_resolveH != (int)x.Height || m_resolveFmt != (unsigned)srcFmt) {
                        if (m_resolveTex) { m_resolveTex->Release(); m_resolveTex = nullptr; }
                        D3D11_TEXTURE2D_DESC rd = x;
                        rd.SampleDesc.Count = 1; rd.SampleDesc.Quality = 0;
                        rd.Usage = D3D11_USAGE_DEFAULT; rd.CPUAccessFlags = 0; rd.BindFlags = 0;
                        if (SUCCEEDED(m_device->CreateTexture2D(&rd, nullptr, &m_resolveTex)) && m_resolveTex) {
                            m_resolveW = (int)x.Width; m_resolveH = (int)x.Height; m_resolveFmt = (unsigned)srcFmt;
                        } else { m_resolveTex = nullptr; }
                    }
                    if (m_resolveTex) {
                        m_context->ResolveSubresource(m_resolveTex, 0, t, 0, srcFmt);
                        t->Release(); t = m_resolveTex; t->AddRef();  // caller Releases
                    } else { t->Release(); t = nullptr; }
                }
                result = t;
            }
            rs->Release();
        }
    }
    if (!result) {
        if (!m_swapChain) findSwapChain();
        if (m_swapChain) {
            ID3D11Texture2D* b = nullptr;
            if (SUCCEEDED(m_swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&b)) && b) {
                D3D11_TEXTURE2D_DESC x = {}; b->GetDesc(&x);
                w = (int)x.Width; h = (int)x.Height; srcFmt = x.Format; result = b;
            } else { m_swapChain->Release(); m_swapChain = nullptr; }
        }
    }
    fmt = (unsigned int)srcFmt;
    return result;
}

bool AngleD3D11Backend::ensureRing(int w, int h, unsigned int fmt) {
    if (m_slots[0].tex && m_ringW == w && m_ringH == h && m_ringFmt == fmt) return true;

    // Need to (re)allocate. Only safe when no slot is in flight.
    for (auto& s : m_slots) if (s.state != SlotState::Free) return false;

    releaseRing();

    bool bgra = true;
    DXGI_FORMAT stageFmt = stagingFormatFor((DXGI_FORMAT)fmt, bgra);

    D3D11_TEXTURE2D_DESC ds = {};
    ds.Width = (UINT)w; ds.Height = (UINT)h; ds.MipLevels = 1; ds.ArraySize = 1;
    ds.Format = stageFmt; ds.SampleDesc.Count = 1;
    ds.Usage = D3D11_USAGE_STAGING; ds.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    for (auto& s : m_slots) {
        if (FAILED(m_device->CreateTexture2D(&ds, nullptr, &s.tex)) || !s.tex) {
            releaseRing();
            return false;
        }
        s.state = SlotState::Free; s.w = w; s.h = h;
    }
    m_ringW = w; m_ringH = h; m_ringFmt = fmt; m_isBGRA = bgra;
    m_copied.clear();
    return true;
}

void AngleD3D11Backend::releaseRing() {
    for (auto& s : m_slots) {
        if (s.tex) { s.tex->Release(); s.tex = nullptr; }
        s.state = SlotState::Free; s.w = s.h = 0;
    }
    m_copied.clear();
    m_ringW = m_ringH = 0; m_ringFmt = 0;
}

int AngleD3D11Backend::issueCopy(int /*reqW*/, int /*reqH*/) {
    if (!m_device || !m_context) return -1;

    // Cheap early-out when the ring is already full: avoid taking the ANGLE
    // immediate-context lock + acquireBackbuffer just to discover there's no free
    // slot. The borrow-wait loop re-calls this every couple of ms, and grabbing
    // the lock there contends with the worker's Map — slowing the very thread that
    // frees slots. (Does NOT change borrow-fps; only makes the "full" case cheap.)
    {
        std::lock_guard<std::mutex> rg(m_ringMtx);
        if (m_slots[0].tex) {
            bool anyFree = false;
            for (auto& s : m_slots) if (s.state == SlotState::Free) { anyFree = true; break; }
            if (!anyFree) return -1;
        }
    }

    MtGuard lock(m_mt);

    int sw = 0, sh = 0; unsigned int fmt = 0;
    ID3D11Texture2D* bb = acquireBackbuffer(sw, sh, fmt);
    if (!bb) return -1;
    if (sw <= 0 || sh <= 0) { bb->Release(); return -1; }

    std::lock_guard<std::mutex> rg(m_ringMtx);
    if (!ensureRing(sw, sh, fmt)) { bb->Release(); return -1; }

    int id = -1;
    for (int i = 0; i < kSlots; ++i) if (m_slots[i].state == SlotState::Free) { id = i; break; }
    if (id < 0) { bb->Release(); return -1; }   // no free slot — pipeline behind

    m_context->CopyResource(m_slots[id].tex, bb);
    bb->Release();
    m_slots[id].state = SlotState::Copied;
    m_slots[id].w = sw; m_slots[id].h = sh;
    m_copied.push_back(id);
    return id;
}

int AngleD3D11Backend::acquireCopied() {
    std::lock_guard<std::mutex> rg(m_ringMtx);
    if (m_copied.empty()) return -1;
    int id = m_copied.front(); m_copied.pop_front();
    m_slots[id].state = SlotState::Mapping;
    return id;
}

void AngleD3D11Backend::copyRows(const void* src, int rowPitch, int w, int h,
                                 std::vector<unsigned char>& out) {
    // ANGLE backs GL's default framebuffer with a D3D11 texture that is already
    // laid out bottom-up (GL row 0 = bottom of the image), exactly like the
    // proven D3D11Hook readback. So we copy rows straight through and let
    // ffmpeg's single "vflip" produce the top-down video. Adding a vertical flip
    // here as well would double-flip and record the video upside-down.
    const size_t rb = (size_t)w * 4;
    out.resize(rb * (size_t)h);
    const uint8_t* s = static_cast<const uint8_t*>(src);
    uint8_t* d = out.data();
    if ((size_t)rowPitch == rb) {
        std::memcpy(d, s, rb * (size_t)h);
    } else {
        for (int y = 0; y < h; ++y)
            std::memcpy(d + (size_t)y * rb, s + (size_t)y * rowPitch, rb);
    }
}

bool AngleD3D11Backend::mapSlot(int id, RawFrame& out) {
    if (id < 0 || id >= kSlots || !m_slots[id].tex) return false;
    ID3D11Texture2D* tex = m_slots[id].tex;
    int w = m_slots[id].w, h = m_slots[id].h;

    D3D11_MAPPED_SUBRESOURCE mr = {};
    {
        MtGuard lock(m_mt);
        HRESULT hr = m_context->Map(tex, 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mr);
        if (hr == DXGI_ERROR_WAS_STILL_DRAWING) return false;   // GPU not done — retry
        if (FAILED(hr) || !mr.pData) return false;
    }
    // Lock released: the staging surface stays mapped and is private to us, so
    // the big memcpy never blocks ANGLE's render thread.
    out.width = w; out.height = h;
    copyRows(mr.pData, (int)mr.RowPitch, w, h, out.bytes);
    {
        MtGuard lock(m_mt);
        m_context->Unmap(tex, 0);
    }
    return true;
}

void AngleD3D11Backend::releaseSlot(int id) {
    if (id < 0 || id >= kSlots) return;
    std::lock_guard<std::mutex> rg(m_ringMtx);
    // If it was still queued (sync path / reset), drop it from the FIFO.
    for (auto it = m_copied.begin(); it != m_copied.end(); ++it)
        if (*it == id) { m_copied.erase(it); break; }
    m_slots[id].state = SlotState::Free;
}

bool AngleD3D11Backend::captureSync(int reqW, int reqH, RawFrame& out) {
    int id = issueCopy(reqW, reqH);
    if (id < 0) {
        if (m_hasCache) { out.width = m_cacheW; out.height = m_cacheH; out.bytes = m_cache; return true; }
        return false;
    }
    bool ok = mapSlot(id, out);   // DO_NOT_WAIT; may miss on a cold first frame
    releaseSlot(id);
    if (ok && !out.bytes.empty()) {
        m_cache = out.bytes; m_cacheW = out.width; m_cacheH = out.height; m_hasCache = true;
        return true;
    }
    if (m_hasCache) { out.width = m_cacheW; out.height = m_cacheH; out.bytes = m_cache; return true; }
    return false;
}

void AngleD3D11Backend::reset() {
    std::lock_guard<std::mutex> rg(m_ringMtx);
    for (auto& s : m_slots) s.state = SlotState::Free;
    m_copied.clear();
    m_hasCache = false; m_cache.clear(); m_cacheW = m_cacheH = 0;
}

void AngleD3D11Backend::shutdown() {
    releaseRing();
    if (m_resolveTex) { m_resolveTex->Release(); m_resolveTex = nullptr; m_resolveW = m_resolveH = 0; m_resolveFmt = 0; }
    if (m_mt)        { m_mt->Release();        m_mt = nullptr; }
    if (m_swapChain) { m_swapChain->Release(); m_swapChain = nullptr; }
    if (m_context)   { m_context->Release();   m_context = nullptr; }
    if (m_device)    { m_device->Release();    m_device = nullptr; }
    if (m_eglModule) { FreeLibrary((HMODULE)m_eglModule); m_eglModule = nullptr; }
    m_threadSafe = false;
    m_cache.clear(); m_hasCache = false;
}

} // namespace gdr
