#pragma once
#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

// Encode pipeline. Receives already-captured BGRA/RGBA frames (the heavy GPU
// readback now happens in gdr::CaptureScheduler, off the render thread) and
// streams them to a bundled ffmpeg over an anonymous pipe on a worker thread.
//
// Backpressure: the frame queue is bounded (kMaxQueued). When the encoder
// falls behind, a new frame is NOT pushed — instead the newest queued frame's
// repeat-count is bumped, so memory stays flat and the wall-clock duration is
// preserved (the segment becomes a brief freeze, never a speed-up / crash).
class Recorder {
public:
    static Recorder& get();
    ~Recorder();
    bool start(HWND hwnd);
    void stop();

    bool  isRecording()  const { return m_running.load(); }
    bool  isFinalizing() const { return m_finalizing.load(); }
    int   saveProgress() const { return m_saveProgress.load(); }
    float seconds() const;
    void  tick();
    void  setInputPixelFormat(bool bgra) { m_inputBGRA = bgra; }
    // Copying submit (kept for compatibility). Prefer the move overload below.
    bool  submitFrame(const unsigned char* data, size_t bytes, int w, int h, int repeat = 1);
    // Zero-copy submit: the render thread hands ownership of the pixel buffer
    // straight to the encode queue — no per-frame memcpy on the render thread.
    bool  submitFrame(std::vector<unsigned char>&& data, int w, int h, int repeat = 1);

    bool                 m_fbReady = false;
    std::mutex           m_fbMutex;
    std::vector<unsigned char> m_fbBuf;
    int                  m_fbW = 0, m_fbH = 0;

    std::string lastError() const;
    const std::string& outputFile() const { return m_outFile; }

private:
    Recorder() = default;
    void workerLoop();
    void finalizeStop();
    bool startFfmpeg();
    void stopFfmpeg();
    bool writePipe(const unsigned char* d, size_t bytes);
    void setLastError(std::string error);
    // Second ffmpeg pass: mux the recorded temp video + the captured WAV into the
    // final mp4 (-c:v copy -c:a aac). Returns false on failure (caller then keeps
    // the temp video as a video-only result).
    bool runMux();

    static constexpr size_t kMaxQueued = 8;   // deeper encode queue rides out brief
                                              // encoder stalls without back-pressuring GD

    HWND m_hwnd = nullptr;
    int m_srcX = 0, m_srcY = 0, m_srcW = 0, m_srcH = 0;
    int m_capW = 0, m_capH = 0, m_fps = 60, m_pipeFps = 30;
    size_t m_bgraBytes = 0;
    bool m_inputBGRA = true;
    bool m_borrowFps = true;   // block the render thread when the encoder is
                               // behind (pace GD to the encode rate) instead of
                               // dropping / freezing frames

    struct QueuedFrame {
        std::vector<unsigned char> data;
        int repeat = 1;
    };

    std::thread m_worker;
    std::thread m_finalizeThread;
    std::mutex m_queueMutex;
    std::condition_variable m_queueCv;  // wakes the encoder worker (new frame)
    std::condition_variable m_spaceCv;  // wakes a blocked submit (queue drained)
    std::deque<QueuedFrame> m_frameQueue;
    std::atomic<long long> m_droppedFrames{0};
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_finalizing{false};
    std::atomic<long long> m_writtenFrames{0};
    std::atomic<long long> m_stopTargetFrames{0};
    std::atomic<int> m_saveProgress{0};
    std::atomic<double> m_startTs{0.0};

    std::vector<unsigned char> m_bgraBuf;

    mutable std::mutex m_stateMutex;
    std::string m_lastError;
    std::string m_outFile;        // final muxed mp4 the user gets
    std::string m_videoTmpFile;   // temp video-only mp4 (== m_outFile when no audio)
    std::string m_audioTmpBase;   // stem passed to AudioCapture (empty when no audio)
    std::string m_audioTmpMix;    // <base>.mix.wav  (game+mic, default track)
    std::string m_audioTmpGame;   // <base>.game.wav (desktop only)
    std::string m_audioTmpMic;    // <base>.mic.wav  (mic only)
    bool m_audioOn = false;       // an audio source was armed for this recording
    int  m_audioBitrate = 192;
    mutable std::mutex m_ffmpegMutex;
    void* m_ffmpegProc = nullptr;
    void* m_ffmpegStdinWr = nullptr;
};
