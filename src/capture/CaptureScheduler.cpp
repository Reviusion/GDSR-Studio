#include "CaptureScheduler.hpp"
#include "AngleD3D11Backend.hpp"
#include "OpenGLBlockingBackend.hpp"
#include "../ui/StudioState.hpp"

#include <algorithm>
#include <chrono>

#include <Geode/loader/Log.hpp>

namespace gdr {

namespace {
    // Safety ceiling on how long the render thread will block waiting for a free
    // ring slot in "borrow fps" mode. Long enough to fully pace GD down to the
    // pipeline; short enough that a genuinely stalled worker can never hang the
    // game outright (the worker itself gives up a Map after its own budget).
    constexpr int kBorrowCapMs = 500;
}

CaptureScheduler& CaptureScheduler::get() {
    static CaptureScheduler inst;
    return inst;
}

CaptureScheduler::~CaptureScheduler() { shutdown(); }

bool CaptureScheduler::ensureBackend() {
    if (m_backend) return true;
    std::lock_guard<std::mutex> lk(m_initMtx);
    if (m_backend) return true;
    m_initTried = true;

    // Prefer the off-thread ANGLE/D3D11 path; fall back to synchronous GL.
    auto angle = std::make_unique<AngleD3D11Backend>();
    if (angle->init()) {
        m_backend = std::move(angle);
    } else {
        auto gl = std::make_unique<OpenGLBlockingBackend>();
        if (gl->init()) m_backend = std::move(gl);
    }
    if (!m_backend) { geode::log::warn("GDSR Capture: no usable backend"); return false; }

    auto& st = studioState();
    m_throttleMs = std::clamp(st.captureThrottleMs, 0, 50);
    m_borrowFps  = st.borrowFpsFromGame;
    m_threaded   = m_backend->threaded() && st.threadedCapture;
    geode::log::info("GDSR Capture: backend ready, threaded={} ({}{}), borrowFps={}",
                     m_threaded, m_backend->threaded() ? "capable" : "sync-only",
                     st.threadedCapture ? "" : ", user-disabled", m_borrowFps);
    if (m_threaded) startWorker();
    return true;
}

void CaptureScheduler::prepare() {
    if (!ensureBackend() || !m_backend) return;
    // Re-evaluate the user's threading preference (it may have changed since the
    // backend came up). Switching modes is only done here, between recordings.
    auto& st = studioState();
    m_throttleMs = std::clamp(st.captureThrottleMs, 0, 50);
    m_borrowFps  = st.borrowFpsFromGame;
    bool want = m_backend->threaded() && st.threadedCapture;
    if (want != m_threaded) {
        stopWorker();
        m_backend->reset();
        {
            std::lock_guard<std::mutex> lk(m_mtx);
            m_ready.clear(); m_pending = 0; m_haveLast = false; m_lastGood.bytes.clear();
        }
        m_threaded = want;
        if (m_threaded) startWorker();
        geode::log::info("GDSR Capture: threaded mode -> {}", m_threaded);
    }
}

bool CaptureScheduler::isBGRA() {
    ensureBackend();
    return m_backend ? m_backend->isBGRA() : true;
}

bool CaptureScheduler::captureNow(int /*x*/, int /*y*/, int w, int h, double /*tsSeconds*/) {
    if (!ensureBackend()) return false;

    if (m_threaded) {
        int slot = m_backend->issueCopy(w, h);
        if (slot < 0 && (m_borrowFps || m_throttleMs > 0)) {
            // No free ring slot — the worker (Map + memcpy) and/or the encoder is
            // behind. Block the render thread, retrying every couple of ms until a
            // slot frees. This is the "borrow performance from GD" path: GD's
            // swapBuffers slows to the pipeline's pace, which on a weak GPU is what
            // frees GPU time for the staging copies to actually complete — without
            // it the game saturates the GPU and capture throughput collapses to a
            // few real fps. Bounded so a stalled worker can never hang GD.
            using namespace std::chrono;
            const int capMs = m_borrowFps ? kBorrowCapMs : m_throttleMs;
            const auto deadline = steady_clock::now() + milliseconds(capMs);
            do {
                {
                    std::unique_lock<std::mutex> lk(m_mtx);
                    m_cvSlot.wait_for(lk, milliseconds(2));
                }
                slot = m_backend->issueCopy(w, h);
            } while (slot < 0 && m_workerRun.load() && steady_clock::now() < deadline);
        }
        if (slot >= 0) {
            { std::lock_guard<std::mutex> lk(m_mtx); ++m_pending; }
            m_cvWork.notify_one();
        }
        std::lock_guard<std::mutex> lk(m_mtx);
        return m_haveLast || !m_ready.empty() || slot >= 0;
    }

    // Synchronous backend (native GL, or threading disabled / unavailable).
    RawFrame f;
    if (m_backend->captureSync(w, h, f) && !f.bytes.empty()) {
        std::lock_guard<std::mutex> lk(m_mtx);
        m_ready.clear();                       // keep only the freshest frame
        m_ready.push_back(std::move(f));
        return true;
    }
    return false;
}

bool CaptureScheduler::pollFrame(RawFrame& out) {
    std::lock_guard<std::mutex> lk(m_mtx);
    if (!m_ready.empty()) {
        out = std::move(m_ready.front());
        m_ready.pop_front();
        return true;
    }
    // Nothing fresh ready: repeat the last good frame so the wall-clock timeline
    // never develops a gap (prevents the "video plays fast-forward" artifact on
    // a slow GPU). Only the threaded path keeps a last-good frame.
    if (m_haveLast) {
        out.width = m_lastGood.width;
        out.height = m_lastGood.height;
        out.bytes = m_lastGood.bytes;
        return true;
    }
    return false;
}

void CaptureScheduler::endRecording() {
    stopWorker();
    if (m_backend) m_backend->reset();
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        m_ready.clear(); m_pending = 0; m_haveLast = false; m_lastGood.bytes.clear();
    }
    if (m_threaded) startWorker();
}

void CaptureScheduler::startWorker() {
    if (!m_threaded || m_workerRun.load()) return;
    m_workerRun.store(true);
    m_worker = std::thread(&CaptureScheduler::workerLoop, this);
}

void CaptureScheduler::stopWorker() {
    if (m_workerRun.load()) {
        m_workerRun.store(false);
        m_cvWork.notify_all();
    }
    if (m_worker.joinable()) m_worker.join();
}

void CaptureScheduler::workerLoop() {
    using namespace std::chrono;
    while (m_workerRun.load()) {
        {
            std::unique_lock<std::mutex> lk(m_mtx);
            m_cvWork.wait(lk, [this] { return !m_workerRun.load() || m_pending > 0; });
            if (!m_workerRun.load()) break;
            --m_pending;
        }
        int slot = m_backend->acquireCopied();
        if (slot < 0) continue;

        RawFrame f;
        bool ok = false;
        // The copy was issued at least one frame ago, so the GPU is usually
        // done; retry briefly (non-blocking Map) if it is not.
        for (int t = 0; t < 200 && m_workerRun.load(); ++t) {
            if (m_backend->mapSlot(slot, f)) { ok = true; break; }
            std::this_thread::sleep_for(milliseconds(1));
        }
        m_backend->releaseSlot(slot);

        std::lock_guard<std::mutex> lk(m_mtx);
        m_cvSlot.notify_all();                 // a slot just freed
        if (ok && !f.bytes.empty()) {
            if (m_ready.size() >= m_maxReady) m_ready.pop_front();
            m_lastGood.width = f.width; m_lastGood.height = f.height;
            m_lastGood.bytes = f.bytes; m_haveLast = true;
            m_ready.push_back(std::move(f));
            m_cvReady.notify_one();
        }
    }
}

void CaptureScheduler::shutdown() {
    stopWorker();
    if (m_backend) { m_backend->shutdown(); m_backend.reset(); }
    std::lock_guard<std::mutex> lk(m_mtx);
    m_ready.clear(); m_pending = 0; m_haveLast = false; m_lastGood.bytes.clear();
}

} // namespace gdr
