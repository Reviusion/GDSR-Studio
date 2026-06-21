#pragma once
//
// AudioCapture — OBS-style audio for the recorder.
//
//   * Desktop audio: WASAPI *loopback* on a render endpoint (game/system sound).
//   * Microphone:    WASAPI *capture* on a capture endpoint.
//
// Both are converted to 48 kHz stereo float, mixed (per-source volume + mute),
// converted to interleaved s16le and written to a temporary **WAV file**. The
// recorder muxes that WAV onto the video in a second ffmpeg pass at stop.
//
// Why a WAV file and not a live pipe: the bundled ffmpeg cannot open a Windows
// named pipe ("\\.\pipe\...") as an input, so a real-time second input is not
// possible. Writing a WAV keeps the video path completely independent — the
// video always saves even if audio fails — and the mux is a fast copy pass.
//
// The mixer pump emits a continuous 48 kHz timeline paced by the wall clock
// (silence-padded), so audio length == recording wall-clock length == the
// wall-clock-paced video, keeping A/V in sync.
//
#include <atomic>
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

// A selectable audio endpoint. `id` is the opaque WASAPI endpoint id (pass to
// setDesktopDeviceId/setMicDeviceId); `name` is the friendly UTF-8 label.
struct AudioDeviceInfo {
    std::string  name;
    std::wstring id;
};

class AudioCapture {
public:
    static AudioCapture& get();

    // Enumerate active endpoints. render=true -> playback devices (desktop
    // loopback sources), render=false -> capture devices (microphones).
    static std::vector<AudioDeviceInfo> listDevices(bool render);

    // Begin capture. `wavBase` is a path STEM; three WAVs are written:
    //   <base>.mix.wav (desktop+mic), <base>.game.wav (desktop), <base>.mic.wav (mic).
    // All three are always written (silence-padded) so the output track layout is
    // stable regardless of which sources are enabled. False if the mix can't open.
    bool start(const std::wstring& wavBase, int sampleRate = 48000, int channels = 2);
    void stop();
    bool isRunning() const { return m_running.load(); }

    long long bytesWritten() const { return m_bytesMix.load(); }
    int sampleRate() const { return m_rate; }
    int channels()   const { return m_ch; }

    // ---- device selection (set before start; empty = default endpoint) ----
    void setDesktopDeviceId(const std::wstring& id) { m_desktopId = id; }
    void setMicDeviceId(const std::wstring& id)     { m_micId = id; }

    // ---- mixer controls (thread-safe) ----
    void setDesktopEnabled(bool e) { m_desktopEnabled.store(e); }
    void setMicEnabled(bool e)     { m_micEnabled.store(e); }
    void setDesktopVolume(float v) { m_desktopVol.store(v); }
    void setMicVolume(float v)     { m_micVol.store(v); }
    void setDesktopMuted(bool m)   { m_desktopMuted.store(m); }
    void setMicMuted(bool m)       { m_micMuted.store(m); }

    // 0..1 peak meters (pre-volume source level), for the UI level bars.
    float desktopLevel() const { return m_desktopLevel.load(); }
    float micLevel()     const { return m_micLevel.load(); }

    bool desktopAvailable() const { return m_desktopAvail.load(); }
    bool micAvailable()     const { return m_micAvail.load(); }

private:
    AudioCapture() = default;
    AudioCapture(const AudioCapture&) = delete;
    AudioCapture& operator=(const AudioCapture&) = delete;

    void deviceThread(bool loopback);   // shared body for desktop + mic
    void pumpThread();
    bool openWav(HANDLE& h, const std::wstring& path);   // create + reserve 44-byte header
    void finalizeWav(HANDLE& h, long long dataBytes);    // patch sizes + close one WAV

    // Single-producer / single-consumer float ring (mutex guarded). Overwrites
    // the oldest samples when full so a fast device clock can't grow latency.
    struct Ring {
        std::vector<float> buf;
        size_t cap = 0, head = 0, count = 0;
        std::mutex mtx;
        void init(size_t capSamples) {
            std::lock_guard<std::mutex> lk(mtx);
            buf.assign(capSamples, 0.0f); cap = capSamples; head = 0; count = 0;
        }
        void push(const float* d, size_t n) {
            std::lock_guard<std::mutex> lk(mtx);
            if (cap == 0) return;
            for (size_t i = 0; i < n; ++i) {
                size_t tail = (head + count) % cap;
                buf[tail] = d[i];
                if (count < cap) ++count;
                else head = (head + 1) % cap;
            }
        }
        size_t pop(float* out, size_t n) {
            std::lock_guard<std::mutex> lk(mtx);
            size_t got = 0;
            while (got < n && count > 0) {
                out[got++] = buf[head];
                head = (head + 1) % cap;
                --count;
            }
            return got;
        }
    };

    int m_rate = 48000;
    int m_ch   = 2;

    std::atomic<bool> m_running{false};

    std::wstring m_desktopId;   // empty = default render endpoint
    std::wstring m_micId;       // empty = default capture endpoint

    std::atomic<bool>  m_desktopEnabled{true};
    std::atomic<bool>  m_micEnabled{false};
    std::atomic<bool>  m_desktopMuted{false};
    std::atomic<bool>  m_micMuted{false};
    std::atomic<float> m_desktopVol{1.0f};
    std::atomic<float> m_micVol{1.0f};
    std::atomic<float> m_desktopLevel{0.0f};
    std::atomic<float> m_micLevel{0.0f};
    std::atomic<bool>  m_desktopAvail{false};
    std::atomic<bool>  m_micAvail{false};

    Ring m_desktopRing;
    Ring m_micRing;

    // Three independent output WAVs (mix / game / mic). Opened on the caller
    // thread before the pump launches, then touched only by the pump until join.
    HANDLE m_wavMix  = INVALID_HANDLE_VALUE;
    HANDLE m_wavGame = INVALID_HANDLE_VALUE;
    HANDLE m_wavMic  = INVALID_HANDLE_VALUE;
    std::atomic<long long> m_bytesMix{0};
    std::atomic<long long> m_bytesGame{0};
    std::atomic<long long> m_bytesMic{0};

    std::thread m_desktopT;
    std::thread m_micT;
    std::thread m_pumpT;
    bool m_desktopThreadStarted = false;
    bool m_micThreadStarted = false;
    bool m_pumpThreadStarted = false;
};
