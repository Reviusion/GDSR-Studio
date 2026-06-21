#include "AudioCapture.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <objbase.h>      // CoInitializeEx/CoCreateInstance (WIN32_LEAN_AND_MEAN drops these)
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <mmreg.h>
#include <functiondiscoverykeys_devpkey.h> // PKEY_Device_FriendlyName

#include <Geode/loader/Log.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>

namespace {
    double nowS() {
        return std::chrono::duration<double>(
                   std::chrono::steady_clock::now().time_since_epoch()).count();
    }
    template <class T> void safeRelease(T*& p) { if (p) { p->Release(); p = nullptr; } }

    inline int16_t toS16(float v) {
        if (v >  1.0f) v =  1.0f;
        if (v < -1.0f) v = -1.0f;
        int s = (int)std::lround(v * 32767.0f);
        if (s >  32767) s =  32767;
        if (s < -32768) s = -32768;
        return (int16_t)s;
    }

    std::string wideToUtf8(const wchar_t* w) {
        if (!w) return {};
        int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
        if (n <= 1) return {};
        std::string s(n - 1, 0);
        WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
        return s;
    }
}

AudioCapture& AudioCapture::get() { static AudioCapture inst; return inst; }

// ---------------------------------------------------------------------------
// Device enumeration (called from the UI thread).
// ---------------------------------------------------------------------------
std::vector<AudioDeviceInfo> AudioCapture::listDevices(bool render) {
    std::vector<AudioDeviceInfo> out;
    HRESULT hrCo = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    bool comInit = SUCCEEDED(hrCo);

    IMMDeviceEnumerator* enumr = nullptr;
    IMMDeviceCollection* coll  = nullptr;
    if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                   __uuidof(IMMDeviceEnumerator), (void**)&enumr)) && enumr) {
        if (SUCCEEDED(enumr->EnumAudioEndpoints(render ? eRender : eCapture,
                                                DEVICE_STATE_ACTIVE, &coll)) && coll) {
            UINT count = 0; coll->GetCount(&count);
            for (UINT i = 0; i < count; ++i) {
                IMMDevice* dev = nullptr;
                if (FAILED(coll->Item(i, &dev)) || !dev) continue;
                LPWSTR id = nullptr;
                std::wstring wid;
                if (SUCCEEDED(dev->GetId(&id)) && id) { wid = id; CoTaskMemFree(id); }
                std::string name;
                IPropertyStore* props = nullptr;
                if (SUCCEEDED(dev->OpenPropertyStore(STGM_READ, &props)) && props) {
                    PROPVARIANT pv; PropVariantInit(&pv);
                    if (SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &pv)) && pv.vt == VT_LPWSTR)
                        name = wideToUtf8(pv.pwszVal);
                    PropVariantClear(&pv);
                    props->Release();
                }
                if (name.empty()) name = "Unknown device";
                out.push_back({ name, wid });
                dev->Release();
            }
            coll->Release();
        }
        enumr->Release();
    }
    if (comInit) CoUninitialize();
    return out;
}

bool AudioCapture::openWav(HANDLE& h, const std::wstring& path) {
    h = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                    CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    unsigned char hdr[44] = {0};
    DWORD wrote = 0;
    WriteFile(h, hdr, sizeof(hdr), &wrote, nullptr);   // reserved, patched on stop
    return true;
}

bool AudioCapture::start(const std::wstring& wavBase, int sampleRate, int channels) {
    if (m_running.load()) return true;
    m_rate = sampleRate > 0 ? sampleRate : 48000;
    m_ch   = 2; // mixer always outputs stereo
    m_bytesMix.store(0);
    m_bytesGame.store(0);
    m_bytesMic.store(0);

    size_t ringCap = (size_t)m_rate * 2 * 2; // ~2 s stereo
    m_desktopRing.init(ringCap);
    m_micRing.init(ringCap);
    m_desktopLevel.store(0.0f);
    m_micLevel.store(0.0f);
    m_desktopAvail.store(false);
    m_micAvail.store(false);
    m_desktopThreadStarted = false;
    m_micThreadStarted = false;
    m_pumpThreadStarted = false;

    // Open the three WAVs. Mix is mandatory; game/mic are best-effort (a failed one
    // is just omitted from the mux). All three are written every iteration so the
    // output track layout stays stable.
    if (!openWav(m_wavMix, wavBase + L".mix.wav")) {
        geode::log::warn("GDSR Audio: cannot create mix wav ({})", (unsigned)GetLastError());
        return false;
    }
    if (!openWav(m_wavGame, wavBase + L".game.wav"))
        geode::log::warn("GDSR Audio: cannot create game wav (track omitted)");
    if (!openWav(m_wavMic, wavBase + L".mic.wav"))
        geode::log::warn("GDSR Audio: cannot create mic wav (track omitted)");

    bool wantDesktop = m_desktopEnabled.load();
    bool wantMic     = m_micEnabled.load();
    if (!wantDesktop && !wantMic) wantDesktop = true; // keep a valid (silent) track

    m_running.store(true);
    if (wantDesktop) {
        m_desktopT = std::thread(&AudioCapture::deviceThread, this, true);
        m_desktopThreadStarted = true;
    }
    if (wantMic) {
        m_micT = std::thread(&AudioCapture::deviceThread, this, false);
        m_micThreadStarted = true;
    }
    m_pumpT = std::thread(&AudioCapture::pumpThread, this);
    m_pumpThreadStarted = true;

    geode::log::info("GDSR Audio: started {} Hz stereo (desktop={}, mic={})",
                     m_rate, wantDesktop, wantMic);
    return true;
}

void AudioCapture::stop() {
    if (!m_running.load() && m_wavMix == INVALID_HANDLE_VALUE &&
        m_wavGame == INVALID_HANDLE_VALUE && m_wavMic == INVALID_HANDLE_VALUE) return;
    m_running.store(false);

    if (m_desktopThreadStarted && m_desktopT.joinable()) m_desktopT.join();
    if (m_micThreadStarted && m_micT.joinable())         m_micT.join();
    if (m_pumpThreadStarted && m_pumpT.joinable())       m_pumpT.join();
    m_desktopThreadStarted = false;
    m_micThreadStarted = false;
    m_pumpThreadStarted = false;

    // Pump joined: the handles are ours to finalize + close.
    finalizeWav(m_wavMix,  m_bytesMix.load());
    finalizeWav(m_wavGame, m_bytesGame.load());
    finalizeWav(m_wavMic,  m_bytesMic.load());
    m_desktopLevel.store(0.0f);
    m_micLevel.store(0.0f);
}

// Patch the RIFF/data sizes into one WAV's header and close it.
void AudioCapture::finalizeWav(HANDLE& wav, long long dataBytes) {
    if (wav == INVALID_HANDLE_VALUE) return;
    uint32_t dataSize = (uint32_t)std::min<long long>(dataBytes, 0xFFFFFFFFll - 44);
    uint32_t byteRate = (uint32_t)(m_rate * m_ch * 2);

    unsigned char h[44];
    auto put32 = [](unsigned char* p, uint32_t v) { p[0]=v&0xFF; p[1]=(v>>8)&0xFF; p[2]=(v>>16)&0xFF; p[3]=(v>>24)&0xFF; };
    auto put16 = [](unsigned char* p, uint16_t v) { p[0]=v&0xFF; p[1]=(v>>8)&0xFF; };
    memcpy(h + 0,  "RIFF", 4);  put32(h + 4,  36 + dataSize);
    memcpy(h + 8,  "WAVE", 4);
    memcpy(h + 12, "fmt ", 4);  put32(h + 16, 16);
    put16(h + 20, 1);                       // PCM
    put16(h + 22, (uint16_t)m_ch);
    put32(h + 24, (uint32_t)m_rate);
    put32(h + 28, byteRate);
    put16(h + 32, (uint16_t)(m_ch * 2));    // block align
    put16(h + 34, 16);                       // bits per sample
    memcpy(h + 36, "data", 4);  put32(h + 40, dataSize);

    SetFilePointer(wav, 0, nullptr, FILE_BEGIN);
    DWORD wrote = 0;
    WriteFile(wav, h, sizeof(h), &wrote, nullptr);
    CloseHandle(wav);
    wav = INVALID_HANDLE_VALUE;
}

// ---------------------------------------------------------------------------
// One body for both endpoints.
// ---------------------------------------------------------------------------
void AudioCapture::deviceThread(bool loopback) {
    Ring& ring = loopback ? m_desktopRing : m_micRing;
    std::atomic<float>& levelOut = loopback ? m_desktopLevel : m_micLevel;
    std::atomic<bool>&  availOut = loopback ? m_desktopAvail : m_micAvail;
    const std::wstring& wantId   = loopback ? m_desktopId : m_micId;
    const char* tag = loopback ? "desktop" : "mic";

    HRESULT hrCo = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    bool comInit = SUCCEEDED(hrCo);

    IMMDeviceEnumerator* enumr = nullptr;
    IMMDevice*           dev   = nullptr;
    IAudioClient*        client = nullptr;
    IAudioCaptureClient* cap    = nullptr;
    WAVEFORMATEX*        wf     = nullptr;

    auto cleanup = [&]() {
        if (client) client->Stop();
        if (wf) { CoTaskMemFree(wf); wf = nullptr; }
        safeRelease(cap);
        safeRelease(client);
        safeRelease(dev);
        safeRelease(enumr);
        if (comInit) CoUninitialize();
    };

    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                __uuidof(IMMDeviceEnumerator), (void**)&enumr)) || !enumr) {
        geode::log::warn("GDSR Audio[{}]: MMDeviceEnumerator failed", tag);
        cleanup(); return;
    }
    HRESULT hrDev = wantId.empty()
        ? enumr->GetDefaultAudioEndpoint(loopback ? eRender : eCapture, eConsole, &dev)
        : enumr->GetDevice(wantId.c_str(), &dev);
    if (FAILED(hrDev) || !dev) {
        // Fall back to the default endpoint if a saved device id is gone.
        if (!wantId.empty()) hrDev = enumr->GetDefaultAudioEndpoint(loopback ? eRender : eCapture, eConsole, &dev);
        if (FAILED(hrDev) || !dev) { geode::log::warn("GDSR Audio[{}]: no endpoint", tag); cleanup(); return; }
    }
    if (FAILED(dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&client)) || !client) {
        geode::log::warn("GDSR Audio[{}]: Activate failed", tag);
        cleanup(); return;
    }
    if (FAILED(client->GetMixFormat(&wf)) || !wf) {
        geode::log::warn("GDSR Audio[{}]: GetMixFormat failed", tag);
        cleanup(); return;
    }

    const int srcRate = (int)wf->nSamplesPerSec;
    const int srcCh   = (int)wf->nChannels;
    const int bits    = (int)wf->wBitsPerSample;
    const int blockAlign = (int)wf->nBlockAlign;
    bool isFloat = false;
    if (wf->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) {
        isFloat = true;
    } else if (wf->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
               wf->cbSize >= sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX)) {
        auto* ext = reinterpret_cast<WAVEFORMATEXTENSIBLE*>(wf);
        isFloat = (ext->SubFormat.Data1 == 3); // 3=IEEE_FLOAT, 1=PCM
    }

    const DWORD flags = loopback ? AUDCLNT_STREAMFLAGS_LOOPBACK : 0;
    const REFERENCE_TIME bufDur = 2000000; // 200 ms
    if (FAILED(client->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, bufDur, 0, wf, nullptr))) {
        geode::log::warn("GDSR Audio[{}]: Initialize failed", tag);
        cleanup(); return;
    }
    if (FAILED(client->GetService(__uuidof(IAudioCaptureClient), (void**)&cap)) || !cap) {
        geode::log::warn("GDSR Audio[{}]: GetService failed", tag);
        cleanup(); return;
    }
    if (FAILED(client->Start())) {
        geode::log::warn("GDSR Audio[{}]: Start failed", tag);
        cleanup(); return;
    }

    availOut.store(true);
    geode::log::info("GDSR Audio[{}]: {} Hz {}ch {}-bit {} -> 48k stereo",
                     tag, srcRate, srcCh, bits, isFloat ? "float" : "int");

    const double ratio = (double)srcRate / (double)m_rate;
    double frac = 0.0;
    float  lastL = 0.0f, lastR = 0.0f;
    bool   haveLast = false;

    std::vector<float> out;
    out.reserve((size_t)m_rate / 4);

    auto readSample = [&](const BYTE*& p) -> float {
        float v = 0.0f;
        if (isFloat)         { v = *reinterpret_cast<const float*>(p); }
        else if (bits == 16) { v = *reinterpret_cast<const int16_t*>(p) / 32768.0f; }
        else if (bits == 32) { v = (float)(*reinterpret_cast<const int32_t*>(p) / 2147483648.0); }
        else if (bits == 24) { int s = p[0] | (p[1] << 8) | (p[2] << 16);
                               if (s & 0x800000) s |= ~0xFFFFFF; v = s / 8388608.0f; }
        return v;
    };

    while (m_running.load()) {
        UINT32 packet = 0;
        if (FAILED(cap->GetNextPacketSize(&packet))) { Sleep(5); continue; }
        if (packet == 0) {
            levelOut.store(levelOut.load() * 0.80f);
            Sleep(5);
            continue;
        }

        out.clear();
        float peak = 0.0f;

        while (packet > 0) {
            BYTE* data = nullptr;
            UINT32 frames = 0;
            DWORD  bflags = 0;
            if (FAILED(cap->GetBuffer(&data, &frames, &bflags, nullptr, nullptr))) break;
            const bool silent = (bflags & AUDCLNT_BUFFERFLAGS_SILENT) != 0;

            const BYTE* p = data;
            for (UINT32 f = 0; f < frames; ++f) {
                float curL = 0.0f, curR = 0.0f;
                if (!silent && data) {
                    const BYTE* fp = p;
                    float c0 = readSample(fp);
                    float c1 = (srcCh >= 2) ? readSample(fp) : c0;
                    curL = c0; curR = c1;
                }
                p += blockAlign;

                if (!haveLast) { lastL = curL; lastR = curR; haveLast = true; }
                while (frac < 1.0) {
                    float a = (float)frac;
                    float oL = lastL + (curL - lastL) * a;
                    float oR = lastR + (curR - lastR) * a;
                    out.push_back(oL);
                    out.push_back(oR);
                    float m = std::max(std::fabs(oL), std::fabs(oR));
                    if (m > peak) peak = m;
                    frac += ratio;
                }
                frac -= 1.0;
                lastL = curL; lastR = curR;
            }
            cap->ReleaseBuffer(frames);
            if (FAILED(cap->GetNextPacketSize(&packet))) break;
        }

        if (!out.empty()) ring.push(out.data(), out.size());
        float prev = levelOut.load();
        levelOut.store(std::max(peak, prev * 0.80f));
    }

    cleanup();
    availOut.store(false);
}

// ---------------------------------------------------------------------------
// Mixer pump: emit a continuous 48 kHz stereo s16 WAV stream paced by the wall
// clock (silence-padded) so audio length == wall-clock length.
// ---------------------------------------------------------------------------
void AudioCapture::pumpThread() {
    if (m_wavMix == INVALID_HANDLE_VALUE) return;

    const int rate = m_rate;
    const double t0 = nowS();
    long long emitted = 0;

    std::vector<float>   dtmp, mtmp;
    std::vector<int16_t> s16mix, s16game, s16mic;

    // Write `bytes` to one WAV in <=1MB chunks, bounding the file at the 32-bit WAV
    // data limit (~6.2h) so the header can never under-report the real size. A
    // null handle is a no-op; returns false only on a real write failure.
    auto writeAll = [&](HANDLE h, const int16_t* d, size_t bytes,
                        std::atomic<long long>& ctr) -> bool {
        if (h == INVALID_HANDLE_VALUE) return true;
        if (ctr.load() + (long long)bytes > 0xFFFFFFFFll - 44) return true;
        const BYTE* bp = reinterpret_cast<const BYTE*>(d);
        size_t left = bytes;
        while (left > 0) {
            DWORD chunk = (DWORD)std::min<size_t>(left, 1u << 20);
            DWORD w = 0;
            if (!WriteFile(h, bp, chunk, &w, nullptr) || w == 0) return false;
            bp += w; left -= w; ctr.fetch_add(w);
        }
        return true;
    };

    while (m_running.load()) {
        double elapsed = nowS() - t0;
        long long target = (long long)(elapsed * rate);
        long long toEmit = target - emitted;
        if (toEmit <= 0) { Sleep(4); continue; }
        if (toEmit > rate) toEmit = rate;

        const size_t need = (size_t)toEmit * 2;
        if (dtmp.size()    < need) dtmp.resize(need);
        if (mtmp.size()    < need) mtmp.resize(need);
        if (s16mix.size()  < need) s16mix.resize(need);
        if (s16game.size() < need) s16game.resize(need);
        if (s16mic.size()  < need) s16mic.resize(need);

        size_t dN = m_desktopRing.pop(dtmp.data(), need);
        size_t mN = m_micRing.pop(mtmp.data(), need);

        const bool  dOn  = m_desktopEnabled.load() && !m_desktopMuted.load();
        const bool  mOn  = m_micEnabled.load()     && !m_micMuted.load();
        const float dVol = std::clamp(m_desktopVol.load(), 0.0f, 3.0f);
        const float mVol = std::clamp(m_micVol.load(), 0.0f, 3.0f);

        // game = desktop only, mic = mic only, mix = both. Silence where a source
        // is off/absent, so all three tracks stay equal-length.
        for (size_t i = 0; i < need; ++i) {
            float g = (dOn && i < dN) ? dtmp[i] * dVol : 0.0f;
            float m = (mOn && i < mN) ? mtmp[i] * mVol : 0.0f;
            s16game[i] = toS16(g);
            s16mic[i]  = toS16(m);
            s16mix[i]  = toS16(g + m);
        }

        const size_t bytes = need * sizeof(int16_t);
        if (!writeAll(m_wavMix, s16mix.data(), bytes, m_bytesMix)) break;  // mandatory
        writeAll(m_wavGame, s16game.data(), bytes, m_bytesGame);
        writeAll(m_wavMic,  s16mic.data(),  bytes, m_bytesMic);

        emitted += toEmit;
        Sleep(4);
    }
}
