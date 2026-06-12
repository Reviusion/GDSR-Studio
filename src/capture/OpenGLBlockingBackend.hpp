#pragma once

#include "CaptureScheduler.hpp"

namespace gdr {

// Native desktop-OpenGL capture backend. Used when the process is NOT running
// through ANGLE (or when the ANGLE/D3D11 path could not be set up). glReadPixels
// must run on the thread that owns the GL context — the render thread — so this
// backend is synchronous (threaded()==false). That readback is the deliberate
// "borrow performance from GD" path: it briefly occupies the render thread.
class OpenGLBlockingBackend : public ICaptureBackend {
public:
    bool init() override;
    bool threaded() const override { return false; }
    bool isBGRA() const override { return m_bgra; }

    // Ring path is unused for a synchronous backend.
    int  issueCopy(int, int) override { return -1; }
    int  acquireCopied() override { return -1; }
    bool mapSlot(int, RawFrame&) override { return false; }
    void releaseSlot(int) override {}

    bool captureSync(int reqW, int reqH, RawFrame& out) override;

    void reset() override {}
    void shutdown() override {}

private:
    bool m_bgra = true;
};

} // namespace gdr
