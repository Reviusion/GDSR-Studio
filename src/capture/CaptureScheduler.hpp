#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace gdr {

// One captured backbuffer image. Pixels are stored BOTTOM-UP (OpenGL
// convention: row 0 is the bottom of the image) in BGRA or RGBA byte order,
// matching ImGuiManager's preview-texture upload and Recorder's "vflip".
struct RawFrame {
    int width  = 0;
    int height = 0;
    std::vector<unsigned char> bytes;
};

// A capture backend turns the live backbuffer into CPU pixels. Two exist:
//   * AngleD3D11Backend    — ANGLE / GLES: pulls the underlying D3D11 device out
//                            of ANGLE and reads the swapchain back through a ring
//                            of staging textures. The heavy Map+copy can run on a
//                            worker thread (threaded() == true).
//   * OpenGLBlockingBackend — native desktop GL: glReadPixels on the calling
//                            (render) thread (threaded() == false).
// The scheduler owns exactly one, chosen at init.
class ICaptureBackend {
public:
    virtual ~ICaptureBackend() = default;

    virtual bool init() = 0;
    // True if resolve()/mapSlot() may run on a thread other than the one calling
    // issueCopy() — i.e. the heavy readback can leave the render thread.
    virtual bool threaded() const = 0;
    // Byte order of produced pixels (true = BGRA, false = RGBA).
    virtual bool isBGRA() const = 0;

    // ---- threaded ring path ----
    // Render thread: snapshot the current backbuffer into a free ring slot and
    // queue the GPU copy. Returns the slot id, or -1 if no slot is free / failed.
    virtual int  issueCopy(int reqW, int reqH) = 0;
    // Worker thread: pop the oldest copied-but-unmapped slot (marks it MAPPING),
    // or return -1 if none. Pair with releaseSlot().
    virtual int  acquireCopied() = 0;
    // Worker thread: non-blocking map of slot `id`. Fills `out` (bottom-up) and
    // returns true on success; returns false if the GPU copy is not ready yet
    // (caller should retry). The slot stays owned until releaseSlot().
    virtual bool mapSlot(int id, RawFrame& out) = 0;
    // Worker thread: return slot `id` to the free pool.
    virtual void releaseSlot(int id) = 0;

    // ---- synchronous fallback path (!threaded) ----
    // Render thread: capture straight into `out`.
    virtual bool captureSync(int reqW, int reqH, RawFrame& out) = 0;

    // Free all ring slots / drop caches (called between recordings while the
    // worker is stopped). Keeps the device alive.
    virtual void reset() = 0;

    virtual void shutdown() = 0;
};

// Drives capture for both the live preview and recording. Designed around the
// exact call pattern in ImGuiManager: captureNow() then pollFrame() each frame
// that wants pixels.
class CaptureScheduler {
public:
    static CaptureScheduler& get();

    // Bring the backend up (call on the render thread, GL/EGL current). Safe to
    // call repeatedly. Used by Recorder::start to learn the pixel format early.
    void prepare();

    // Render thread, once per frame wanting pixels. Issues/refreshes a capture of
    // the backbuffer. `x,y` are reserved (full backbuffer is captured), `w,h` are
    // the desired size, `tsSeconds` is a wall-clock stamp (reserved for pacing).
    // Returns false only when no frame is or will be available (capture down).
    bool captureNow(int x, int y, int w, int h, double tsSeconds);

    // Render thread. Hands back the next available frame (moved into `out`).
    // Once the pipeline is primed this always yields a frame while recording —
    // if nothing newer is ready the last good frame is repeated — so the
    // wall-clock timeline never develops gaps (no "fast-forward" artifact).
    bool pollFrame(RawFrame& out);

    // Render thread. Recording stopped: drain the pipeline and reset.
    void endRecording();

    void shutdown();

    bool isBGRA();

private:
    CaptureScheduler() = default;
    ~CaptureScheduler();
    CaptureScheduler(const CaptureScheduler&) = delete;
    CaptureScheduler& operator=(const CaptureScheduler&) = delete;

    bool ensureBackend();
    void startWorker();
    void stopWorker();
    void workerLoop();

    std::unique_ptr<ICaptureBackend> m_backend;
    bool m_threaded = false;
    bool m_initTried = false;
    std::mutex m_initMtx;

    // Pipeline state (shared between render thread and capture worker).
    std::thread m_worker;
    std::mutex  m_mtx;
    std::condition_variable m_cvWork;   // wakes the worker when a copy is issued
    std::condition_variable m_cvReady;  // (reserved) frame became ready
    std::condition_variable m_cvSlot;   // wakes captureNow when a slot frees
    std::atomic<bool> m_workerRun{false};

    int m_pending = 0;                  // copies issued but not yet acquired
    std::deque<RawFrame> m_ready;       // mapped frames awaiting pollFrame
    size_t m_maxReady = 3;

    RawFrame m_lastGood;                // for timeline-preserving repeats
    bool     m_haveLast = false;

    int m_throttleMs = 4;               // bounded render-thread wait when behind
    bool m_borrowFps = true;            // block the render thread until the
                                        // pipeline can take a frame (pace GD down
                                        // to the capture/encode rate) instead of
                                        // dropping/duplicating frames
};

} // namespace gdr
