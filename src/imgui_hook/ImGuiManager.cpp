#ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
    #define NOMINMAX
#endif
#include <windows.h>
#include <d3d11.h>
#include <wincodec.h>

#include "ImGuiManager.hpp"
#include "../ui/StudioUI.hpp"
#include "../ui/StudioState.hpp"
#include "../ui/Localization.hpp"
#include "../capture/CaptureScheduler.hpp"
#include "../capture/Recorder.hpp"

#include <imgui.h>
#include <backends/imgui_impl_win32.h>
#include <backends/imgui_impl_opengl3.h>

#include <Geode/loader/Log.hpp>
#include <Geode/loader/Mod.hpp>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <utility>

#ifndef USER_DEFAULT_SCREEN_DPI
    #define USER_DEFAULT_SCREEN_DPI 96
#endif

#ifndef GL_BGRA
    #define GL_BGRA 0x80E1
#endif

#ifndef GL_RGBA
    #define GL_RGBA 0x1908
#endif

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace {
    double nowMs() {
        using namespace std::chrono;
        return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
    }

    HWND     g_hwnd        = nullptr;
    WNDPROC  g_origWndProc = nullptr;

    struct EnumData { DWORD pid; HWND found; };
    BOOL CALLBACK enumProc(HWND hwnd, LPARAM lparam) {
        auto* data = reinterpret_cast<EnumData*>(lparam);
        DWORD pid = 0;
        GetWindowThreadProcessId(hwnd, &pid);
        if (pid == data->pid && IsWindowVisible(hwnd) && GetWindow(hwnd, GW_OWNER) == nullptr) {
            char title[256] = {0};
            GetWindowTextA(hwnd, title, sizeof(title));
            if (title[0] != '\0') { data->found = hwnd; return FALSE; }
        }
        return TRUE;
    }
    HWND findGameWindow() {
        EnumData data{ GetCurrentProcessId(), nullptr };
        EnumWindows(enumProc, reinterpret_cast<LPARAM>(&data));
        return data.found;
    }

    bool isGles2OrAngle() {
        const char* glVersion = reinterpret_cast<const char*>(glGetString(GL_VERSION));
        return glVersion && (std::strstr(glVersion, "OpenGL ES 2") || std::strstr(glVersion, "ANGLE"));
    }

    // DPI is constant per window — cache it once.
    static UINT g_cachedDpi = 0;
    static HWND g_cachedDpiHwnd = nullptr;
    UINT getCachedDpi(HWND hwnd) {
        if (g_cachedDpiHwnd == hwnd && g_cachedDpi != 0) return g_cachedDpi;
        auto user32 = GetModuleHandleW(L"user32.dll");
        auto fn = user32 ? reinterpret_cast<UINT(WINAPI*)(HWND)>(GetProcAddress(user32, "GetDpiForWindow")) : nullptr;
        g_cachedDpi = fn ? fn(hwnd) : USER_DEFAULT_SCREEN_DPI;
        g_cachedDpiHwnd = hwnd;
        return g_cachedDpi;
    }

    LRESULT CALLBACK wndProcHook(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
        if (msg == WM_KEYDOWN && wParam == VK_F8 && !(lParam & 0x40000000)) {
            ImGuiManager::get().toggleVisible();
            return 0;
        }
        if (msg == WM_KEYDOWN && wParam == VK_F11 && !(lParam & 0x40000000)) {
            auto& st = studioState();
            st.fullscreen = !st.fullscreen;
            st.resetLayout = true;
            return 0;
        }
        auto& mgr = ImGuiManager::get();
        if (mgr.isInitialized() && mgr.isVisible()) {
            ImGui_ImplWin32_WndProcHandler(hwnd, msg, wParam, lParam);
            switch (msg) {
                case WM_MOUSEMOVE: case WM_LBUTTONDOWN: case WM_LBUTTONUP:
                case WM_RBUTTONDOWN: case WM_RBUTTONUP: case WM_MBUTTONDOWN:
                case WM_MBUTTONUP: case WM_MOUSEWHEEL: case WM_MOUSEHWHEEL:
                case WM_KEYDOWN: case WM_KEYUP: case WM_CHAR:
                    return 0;
                default: break;
            }
        }
        return CallWindowProcW(g_origWndProc, hwnd, msg, wParam, lParam);
    }
}

ImGuiManager& ImGuiManager::get() {
    static ImGuiManager inst;
    return inst;
}

ImGuiManager::~ImGuiManager() {
    unhookWindowProc();
    if (m_logoTex != 0) {
        glDeleteTextures(1, &m_logoTex);
        m_logoTex = 0;
    }
}

void ImGuiManager::showToast(const char* title, const char* body, double durationMs) {
    m_toastTitle = title ? title : "";
    m_toastBody = body ? body : "";
    m_toastDurationMs = durationMs > 0.0 ? durationMs : 2800.0;
    m_toastStart = 0.0;
    m_showToast = true;
}

void ImGuiManager::unhookWindowProc() {
    if (g_hwnd && g_origWndProc) {
        SetWindowLongPtrW(g_hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(g_origWndProc));
        g_origWndProc = nullptr;
    }
}

void ImGuiManager::ensureLogoTexture() {
    if (m_logoTex != 0) return;

    HRESULT hrCo = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    bool comInit = SUCCEEDED(hrCo);

    auto loadPng = [&](const std::wstring& file) -> bool {
        IWICImagingFactory* factory = nullptr;
        IWICBitmapDecoder* decoder = nullptr;
        IWICBitmapFrameDecode* frame = nullptr;
        IWICFormatConverter* conv = nullptr;

        HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                      IID_PPV_ARGS(&factory));
        if (FAILED(hr) || !factory) return false;

        hr = factory->CreateDecoderFromFilename(file.c_str(), nullptr, GENERIC_READ,
                                                WICDecodeMetadataCacheOnLoad, &decoder);
        if (FAILED(hr) || !decoder) { factory->Release(); return false; }

        hr = decoder->GetFrame(0, &frame);
        if (FAILED(hr) || !frame) { decoder->Release(); factory->Release(); return false; }

        hr = factory->CreateFormatConverter(&conv);
        if (FAILED(hr) || !conv) { frame->Release(); decoder->Release(); factory->Release(); return false; }

        hr = conv->Initialize(frame, GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone,
                              nullptr, 0.0, WICBitmapPaletteTypeCustom);
        if (FAILED(hr)) {
            conv->Release(); frame->Release(); decoder->Release(); factory->Release();
            return false;
        }

        UINT w = 0, h = 0;
        conv->GetSize(&w, &h);
        if (w == 0 || h == 0) {
            conv->Release(); frame->Release(); decoder->Release(); factory->Release();
            return false;
        }

        std::vector<unsigned char> pixels((size_t)w * (size_t)h * 4);
        hr = conv->CopyPixels(nullptr, w * 4, (UINT)pixels.size(), pixels.data());
        if (FAILED(hr)) {
            conv->Release(); frame->Release(); decoder->Release(); factory->Release();
            return false;
        }

        glGenTextures(1, &m_logoTex);
        glBindTexture(GL_TEXTURE_2D, m_logoTex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, (GLsizei)w, (GLsizei)h, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
        m_logoW = (int)w;
        m_logoH = (int)h;

        conv->Release();
        frame->Release();
        decoder->Release();
        factory->Release();
        return m_logoTex != 0;
    };

    auto tryFile = [&](const std::filesystem::path& p) -> bool {
        std::error_code ec;
        return std::filesystem::exists(p, ec) && loadPng(p.wstring());
    };

    auto modDir = geode::Mod::get()->getResourcesDir();
    if (!tryFile(modDir / "logo.png"))
        tryFile(std::filesystem::path(L"C:\\Geometry Dash\\gdrm-studio\\logo.png"));

    if (comInit) CoUninitialize();
}

// ---- Minimal capture init (NO ImGui, NO context creation) ----
// Called on the first frame to set up glReadPixels without any ImGui overhead.
void ImGuiManager::ensureCaptureReady() {
    if (m_captureReady) return;
    g_hwnd = findGameWindow();
    m_hwnd = g_hwnd;
    if (!g_hwnd) return;

    // Detect GL pixel format (BGRA vs RGBA) — one-time GL probe.
    // Internal texture format is always GL_RGBA regardless of memory layout;
    // GL_BGRA as internal format is not valid in core GL 3.x and produces a
    // black texture on some drivers (observed on ANGLE D3D11 in certain configs).
    m_glReadFormat = GL_BGRA; m_glTexFormat = GL_BGRA;
    m_pixbufIsBGRA = true;

    auto tryFmt = [&](unsigned int fmt) -> bool {
        unsigned char tmp[4] = {};
        GLint prev = 4; glGetIntegerv(GL_PACK_ALIGNMENT, &prev);
        (void)glGetError();
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glReadPixels(0, 0, 1, 1, fmt, GL_UNSIGNED_BYTE, tmp);
        glPixelStorei(GL_PACK_ALIGNMENT, prev);
        return glGetError() == GL_NO_ERROR;
    };
    if (!tryFmt(GL_BGRA)) {
        if (tryFmt(GL_RGBA)) { m_glReadFormat = GL_RGBA; m_glTexFormat = GL_RGBA; m_pixbufIsBGRA = false; }
    }

    m_captureReady = true;
    geode::log::info("GDSR Studio: capture-ready (hwnd={}, fmt={})", (void*)g_hwnd, m_pixbufIsBGRA ? "BGRA" : "RGBA");
}

// ---- Full ImGui init (only when user opens the studio) ----
void ImGuiManager::lazyInit() {
    ensureCaptureReady();
    if (!g_hwnd) return;

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable | ImGuiConfigFlags_NavEnableKeyboard;
    io.IniFilename = nullptr;

    loadSettings();
    loadFonts();
    ensureLogoTexture();
    applyTheme();

    if (!ImGui_ImplWin32_Init(g_hwnd)) { geode::log::error("GDSR Studio: ImGui_ImplWin32_Init failed"); return; }
    const char* glslVersion = isGles2OrAngle() ? "#version 100" : "#version 130";
    if (!ImGui_ImplOpenGL3_Init(glslVersion)) { geode::log::error("GDSR Studio: ImGui_ImplOpenGL3_Init failed"); return; }

    m_initialized = true;
    geode::log::info("GDSR Studio: ImGui initialized (hwnd={})", (void*)g_hwnd);
}

// ---- Settings / Fonts / Theme (unchanged) ----
void ImGuiManager::loadSettings() {
    auto* mod = geode::Mod::get();
    auto& st = studioState();
    st.theme      = static_cast<Theme>(mod->getSavedValue<int>("theme", (int)Theme::Dark));
    st.language   = mod->getSavedValue<int>("language", 0);
    st.fontFamily = mod->getSavedValue<int>("font-family", 0);
    st.fontSize   = (float)mod->getSavedValue<double>("font-size", 16.0);
    {
        int ve = mod->getSavedValue<int>("video-encoder", (int)VideoEncoder::X264);
        if (ve < 0 || ve > (int)VideoEncoder::QSV) ve = (int)VideoEncoder::X264; // drop removed MJPEG
        st.videoEncoder = static_cast<VideoEncoder>(ve);
    }
    st.rateControl  = static_cast<RateControl>(mod->getSavedValue<int>("rate-control", (int)RateControl::CQP));
    st.encodeMode   = static_cast<EncodeMode>(mod->getSavedValue<int>("encode-mode", (int)EncodeMode::MaxQuality));
    st.rateMode     = static_cast<RateMode>(mod->getSavedValue<int>("rate-mode", (int)RateMode::Quality));
    st.fullColorRange = mod->getSavedValue<bool>("full-color-range", false);
    st.highChroma444  = mod->getSavedValue<bool>("high-chroma-444", false);
    st.perfProfile  = static_cast<PerfProfile>(mod->getSavedValue<int>("perf-profile", (int)PerfProfile::Balanced));
    st.previewFps    = mod->getSavedValue<int>("preview-fps", 30);
    st.previewMaxDim = mod->getSavedValue<int>("preview-maxdim", 1280);
    st.quality      = mod->getSavedValue<int>("rec-quality", 23);
    st.bitrateKbps  = mod->getSavedValue<int>("rec-bitrate", 12000);
    st.recFps       = mod->getSavedValue<int>("rec-fps", 60);
    st.outWidth     = mod->getSavedValue<int>("out-width", 0);
    st.outHeight    = mod->getSavedValue<int>("out-height", 0);
    st.outputDir    = mod->getSavedValue<std::string>("output-dir", "");
    st.ffmpegPath   = mod->getSavedValue<std::string>("ffmpeg-path", "");
    st.threadedCapture   = mod->getSavedValue<bool>("threaded-capture", true);
    st.borrowFpsFromGame = mod->getSavedValue<bool>("borrow-fps", true);
    st.captureThrottleMs = mod->getSavedValue<int>("capture-throttle-ms", 4);
    st.audioDesktopEnabled = mod->getSavedValue<bool>("audio-desktop-enabled", true);
    st.audioMicEnabled     = mod->getSavedValue<bool>("audio-mic-enabled", false);
    st.audioDesktopMuted   = mod->getSavedValue<bool>("audio-desktop-muted", false);
    st.audioMicMuted       = mod->getSavedValue<bool>("audio-mic-muted", false);
    st.audioDesktopVol     = std::clamp((float)mod->getSavedValue<double>("audio-desktop-vol", 1.0), 0.0f, 3.0f);
    st.audioMicVol         = std::clamp((float)mod->getSavedValue<double>("audio-mic-vol", 1.0), 0.0f, 3.0f);
    st.audioBitrateKbps    = mod->getSavedValue<int>("audio-bitrate", 192);
    st.audioTrackMode      = static_cast<AudioTrackMode>(mod->getSavedValue<int>("audio-track-mode", (int)AudioTrackMode::Single));
    st.desktopDeviceId     = mod->getSavedValue<std::string>("audio-desktop-id", "");
    st.micDeviceId         = mod->getSavedValue<std::string>("audio-mic-id", "");
    if (st.perfProfile != PerfProfile::Custom) applyPerfProfile(st, st.perfProfile);
    st.themeDirty = true; st.fontDirty = true;
}
void ImGuiManager::saveSettings() {
    auto* mod = geode::Mod::get();
    auto& st = studioState();
    mod->setSavedValue<int>("theme", (int)st.theme);
    mod->setSavedValue<int>("language", st.language);
    mod->setSavedValue<int>("font-family", st.fontFamily);
    mod->setSavedValue<double>("font-size", (double)st.fontSize);
    mod->setSavedValue<int>("video-encoder", (int)st.videoEncoder);
    mod->setSavedValue<int>("rate-control", (int)st.rateControl);
    mod->setSavedValue<int>("encode-mode", (int)st.encodeMode);
    mod->setSavedValue<int>("rate-mode", (int)st.rateMode);
    mod->setSavedValue<bool>("full-color-range", st.fullColorRange);
    mod->setSavedValue<bool>("high-chroma-444", st.highChroma444);
    mod->setSavedValue<int>("perf-profile", (int)st.perfProfile);
    mod->setSavedValue<int>("preview-fps", st.previewFps);
    mod->setSavedValue<int>("preview-maxdim", st.previewMaxDim);
    mod->setSavedValue<int>("rec-quality", st.quality);
    mod->setSavedValue<int>("rec-bitrate", st.bitrateKbps);
    mod->setSavedValue<int>("rec-fps", st.recFps);
    mod->setSavedValue<int>("out-width", st.outWidth);
    mod->setSavedValue<int>("out-height", st.outHeight);
    mod->setSavedValue<std::string>("output-dir", st.outputDir);
    mod->setSavedValue<std::string>("ffmpeg-path", st.ffmpegPath);
    mod->setSavedValue<bool>("threaded-capture", st.threadedCapture);
    mod->setSavedValue<bool>("borrow-fps", st.borrowFpsFromGame);
    mod->setSavedValue<int>("capture-throttle-ms", st.captureThrottleMs);
    mod->setSavedValue<bool>("audio-desktop-enabled", st.audioDesktopEnabled);
    mod->setSavedValue<bool>("audio-mic-enabled", st.audioMicEnabled);
    mod->setSavedValue<bool>("audio-desktop-muted", st.audioDesktopMuted);
    mod->setSavedValue<bool>("audio-mic-muted", st.audioMicMuted);
    mod->setSavedValue<double>("audio-desktop-vol", (double)st.audioDesktopVol);
    mod->setSavedValue<double>("audio-mic-vol", (double)st.audioMicVol);
    mod->setSavedValue<int>("audio-bitrate", st.audioBitrateKbps);
    mod->setSavedValue<int>("audio-track-mode", (int)st.audioTrackMode);
    mod->setSavedValue<std::string>("audio-desktop-id", st.desktopDeviceId);
    mod->setSavedValue<std::string>("audio-mic-id", st.micDeviceId);
    st.savePending = false;
}
void ImGuiManager::loadFonts() {
    auto& io = ImGui::GetIO();
    auto& st = studioState();
    static const char* kPaths[] = { "C:\\Windows\\Fonts\\segoeui.ttf", "C:\\Windows\\Fonts\\tahoma.ttf",
                                     "C:\\Windows\\Fonts\\arial.ttf", "C:\\Windows\\Fonts\\consola.ttf", nullptr };
    int fam = st.fontFamily;
    if (fam < 0 || fam > 4) fam = 0;
    st.fontSize = st.fontSize < 11.0f ? 11.0f : (st.fontSize > 28.0f ? 28.0f : st.fontSize);

    io.Fonts->Clear();

    ImFontConfig cfg;
    cfg.SizePixels = st.fontSize;
    cfg.OversampleH = 2;
    cfg.OversampleV = 1;
    cfg.PixelSnapH = true;

    ImFontGlyphRangesBuilder builder;
    builder.AddRanges(io.Fonts->GetGlyphRangesDefault());
    builder.AddRanges(io.Fonts->GetGlyphRangesCyrillic());
    builder.AddRanges(io.Fonts->GetGlyphRangesVietnamese());
    ImVector<ImWchar> ranges;
    builder.BuildRanges(&ranges);

    m_font = kPaths[fam] ? io.Fonts->AddFontFromFileTTF(kPaths[fam], st.fontSize, &cfg, ranges.Data) : nullptr;
    if (!m_font) m_font = io.Fonts->AddFontDefault(&cfg);

    // Merge ALL available CJK system fonts so each contributes its own glyphs
    // (e.g. MS Gothic for Japanese Kanji+Kana, Malgun Gothic for Korean Hangul).
    static const char* cjkFontCandidates[] = {
        "C:\\Windows\\Fonts\\msyh.ttc",            // Microsoft YaHei (Chinese Simplified)
        "C:\\Windows\\Fonts\\msgothic.ttc",        // MS Gothic (Japanese)
        "C:\\Windows\\Fonts\\malgun.ttf",          // Malgun Gothic (Korean)
        "C:\\Windows\\Fonts\\simsun.ttc",          // SimSun (Chinese)
        "C:\\Windows\\Fonts\\yugothm.ttc",         // Yu Gothic (Japanese)
        "C:\\Windows\\Fonts\\gulim.ttc",           // Gulim (Korean)
        "C:\\Windows\\Fonts\\mingliub.ttc",        // MingLiU (Chinese Traditional)
        nullptr,
    };
    for (const char** cf = cjkFontCandidates; *cf; ++cf) {
        if (GetFileAttributesA(*cf) != INVALID_FILE_ATTRIBUTES) {
            ImFontConfig mergeCfg;
            mergeCfg.MergeMode = true;
            mergeCfg.SizePixels = st.fontSize;
            mergeCfg.OversampleH = 1;
            mergeCfg.OversampleV = 1;
            mergeCfg.PixelSnapH = true;

            ImFontGlyphRangesBuilder cjkBuilder;
            cjkBuilder.AddRanges(io.Fonts->GetGlyphRangesJapanese());
            cjkBuilder.AddRanges(io.Fonts->GetGlyphRangesKorean());
            cjkBuilder.AddRanges(io.Fonts->GetGlyphRangesChineseSimplifiedCommon());
            ImVector<ImWchar> cjkRanges;
            cjkBuilder.BuildRanges(&cjkRanges);

            io.Fonts->AddFontFromFileTTF(*cf, st.fontSize, &mergeCfg, cjkRanges.Data);
        }
    }

    st.fontDirty = false;
}
void ImGuiManager::applyTheme() {
    auto& st = studioState();
    if (st.theme == Theme::Light) ImGui::StyleColorsLight(); else ImGui::StyleColorsDark();

    ImGuiStyle& s = ImGui::GetStyle();
    s.WindowRounding = 6.0f;
    s.ChildRounding = 4.0f;
    s.FrameRounding = 5.0f;
    s.GrabRounding = 4.0f;
    s.TabRounding = 5.0f;
    s.ScrollbarRounding = 4.0f;
    s.WindowBorderSize = 1.0f;
    s.FrameBorderSize = 1.0f;
    s.WindowPadding = {8.0f, 8.0f};
    s.FramePadding = {9.0f, 5.0f};
    s.ItemSpacing = {8.0f, 6.0f};
    s.ItemInnerSpacing = {6.0f, 5.0f};
    s.DockingSeparatorSize = 2.0f;

    auto accent = ImVec4(0.18f, 0.47f, 0.87f, 1.0f);
    auto accentHover = ImVec4(0.25f, 0.56f, 0.98f, 1.0f);
    auto accentDim = ImVec4(0.18f, 0.47f, 0.87f, 0.62f);
    ImVec4* c = s.Colors;
    if (st.theme == Theme::Dark) {
        c[ImGuiCol_WindowBg] = {0.12f, 0.13f, 0.15f, 1.0f};
        c[ImGuiCol_ChildBg] = {0.10f, 0.11f, 0.13f, 1.0f};
        c[ImGuiCol_PopupBg] = {0.10f, 0.11f, 0.13f, 0.98f};
        c[ImGuiCol_FrameBg] = {0.16f, 0.17f, 0.20f, 1.0f};
        c[ImGuiCol_FrameBgHovered] = {0.20f, 0.22f, 0.26f, 1.0f};
        c[ImGuiCol_FrameBgActive] = {0.24f, 0.27f, 0.32f, 1.0f};
        c[ImGuiCol_TitleBg] = {0.10f, 0.11f, 0.13f, 1.0f};
        c[ImGuiCol_TitleBgActive] = {0.13f, 0.15f, 0.18f, 1.0f};
        c[ImGuiCol_Button] = {0.20f, 0.22f, 0.25f, 1.0f};
        c[ImGuiCol_ButtonHovered] = {0.26f, 0.29f, 0.34f, 1.0f};
        c[ImGuiCol_ButtonActive] = accent;
        c[ImGuiCol_Border] = {0.28f, 0.31f, 0.36f, 0.75f};
        c[ImGuiCol_Separator] = {0.28f, 0.31f, 0.36f, 0.75f};
        c[ImGuiCol_Tab] = c[ImGuiCol_TabUnfocused] = {0.12f, 0.13f, 0.15f, 1.0f};
        c[ImGuiCol_TabActive] = {0.16f, 0.30f, 0.50f, 1.0f};
    }
    c[ImGuiCol_Header] = accentDim;
    c[ImGuiCol_HeaderHovered] = accentHover;
    c[ImGuiCol_HeaderActive] = accent;
    c[ImGuiCol_CheckMark] = accent;
    c[ImGuiCol_SliderGrab] = accent;
    c[ImGuiCol_SliderGrabActive] = accentHover;
    c[ImGuiCol_TabHovered] = accentHover;
    st.themeDirty = false;
}


bool ImGuiManager::captureBackbuffer(bool needPixels) {
    int vw = m_vpW, vh = m_vpH;
    if (vw <= 0 || vh <= 0) {
        GLint vp[4] = {0,0,0,0}; glGetIntegerv(GL_VIEWPORT, vp);
        vw = vp[2]; vh = vp[3];
        if (vw <= 0 || vh <= 0) return false;
        m_vpX = vp[0]; m_vpY = vp[1]; m_vpW = vw; m_vpH = vh;
    }
    if (!needPixels && !m_visible) return false;
    m_gameW = vw; m_gameH = vh;

    auto& scheduler = gdr::CaptureScheduler::get();
    double ts = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    if (!scheduler.captureNow(m_vpX, m_vpY, vw, vh, ts)) return false;

    gdr::RawFrame frame;
    if (!scheduler.pollFrame(frame)) return false;

    m_gameW = frame.width;
    m_gameH = frame.height;
    m_pixbuf = std::move(frame.bytes);
    return !m_pixbuf.empty();
}

void ImGuiManager::captureSize(int vpW, int vpH, int& outW, int& outH, bool forceRecord) const {
    auto& st = studioState();
    bool recording = forceRecord || Recorder::get().isRecording();
    if (recording) {
        outW = vpW; outH = vpH;
        return;
    }
    int cap = st.previewMaxDim;
    if (cap <= 0 || (vpW <= cap && vpH <= cap)) { outW = vpW; outH = vpH; return; }
    if (vpW >= vpH) { outW = cap; outH = (int)((long long)vpH * cap / vpW); }
    else            { outH = cap; outW = (int)((long long)vpW * cap / vpH); }
    if (outW < 2) outW = 2; if (outH < 2) outH = 2;
}

void ImGuiManager::uploadPreviewTexture() {
    if (m_pixbuf.empty() || m_gameW <= 0 || m_gameH <= 0) return;
    GLint prevTex = 0; glGetIntegerv(GL_TEXTURE_BINDING_2D, &prevTex);
    const size_t bytes = (size_t)m_gameW * (size_t)m_gameH * 4;
    if (m_previewScratch.size() != bytes) m_previewScratch.resize(bytes);
    if (m_pixbufIsBGRA) {
        const unsigned char* src = m_pixbuf.data();
        unsigned char* dst = m_previewScratch.data();
        for (size_t i = 0; i < bytes; i += 4) {
            dst[i + 0] = src[i + 2];
            dst[i + 1] = src[i + 1];
            dst[i + 2] = src[i + 0];
            dst[i + 3] = 255;
        }
    } else {
        std::memcpy(m_previewScratch.data(), m_pixbuf.data(), bytes);
        for (size_t i = 3; i < bytes; i += 4) m_previewScratch[i] = 255;
    }
    if (m_gameTex == 0) {
        glGenTextures(1, &m_gameTex);
        glBindTexture(GL_TEXTURE_2D, m_gameTex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    } else {
        glBindTexture(GL_TEXTURE_2D, m_gameTex);
    }
    // Internal format is always GL_RGBA (valid everywhere); the external format
    // is always GL_RGBA to avoid driver/ANGLE BGRA upload quirks; the scratch
    // buffer is converted into RGBA with an opaque alpha channel above.
    if (m_gameW != m_texW || m_gameH != m_texH) {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, m_gameW, m_gameH, 0,
                     GL_RGBA, GL_UNSIGNED_BYTE, m_previewScratch.data());
        m_texW = m_gameW; m_texH = m_gameH;
    } else {
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, m_gameW, m_gameH,
                        GL_RGBA, GL_UNSIGNED_BYTE, m_previewScratch.data());
    }
    glBindTexture(GL_TEXTURE_2D, (GLuint)prevTex);
}

void ImGuiManager::plannedRecordSize(int& w, int& h) const {
    int vw = m_vpW, vh = m_vpH;
    if (vw <= 0 || vh <= 0) { GLint vp[4]; glGetIntegerv(GL_VIEWPORT, vp); vw = vp[2]; vh = vp[3]; }
    if (vw <= 0 || vh <= 0) { w = 1280; h = 720; return; }
    captureSize(vw, vh, w, h, true);
}

// ======================================================================
// MAIN FRAME — called from swapBuffers hook
// ======================================================================
void ImGuiManager::renderFrame(int fbW, int fbH) {
    Recorder::get().tick();
    bool recording = Recorder::get().isRecording();
    // Reset capture timer when a new recording session starts, otherwise
    // (now - 0) / interval = millions of due frames => first frame repeats 120x => video "stuck" at 2fps.
    if (recording && !m_wasRecording) {
        using namespace std::chrono;
        m_lastRecordCaptureMs = (double)duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
    }
    m_wasRecording = recording;

    // ---- ABSOLUTE EARLY-OUT: nothing to do at all ----
    // When idle (no recording, UI hidden, startup toast done) do ZERO work.
    // No GL calls, no Win32 API, no ImGui — nothing.
    if (!recording && !m_visible && !m_showStartupNotify && !m_showToast) {
        return;
    }

    // ---- First frame: minimal capture init (NO ImGui) ----
    // Ensure capture + window proc are set up on the VERY FIRST frame,
    // way before the user presses any keys.  The window proc must be
    // installed before F8 can be intercepted.
    if (!m_captureReady) {
        ensureCaptureReady();
        if (!m_captureReady) return;
        // Install window proc NOW so F8 works.
        if (!g_origWndProc && g_hwnd) {
            g_origWndProc = reinterpret_cast<WNDPROC>(
                SetWindowLongPtrW(g_hwnd, GWLP_WNDPROC,
                    reinterpret_cast<LONG_PTR>(&wndProcHook)));
        }
    }

    // ---- Update viewport cache (only on resize) ----
    if (fbW != m_lastFbW || fbH != m_lastFbH) {
        m_lastFbW = fbW; m_lastFbH = fbH;
        GLint vp[4]; glGetIntegerv(GL_VIEWPORT, vp);
        int cw = 0, ch = 0;
        if (vp[2] > 0 && vp[3] > 0) { cw = vp[2]; ch = vp[3]; }
        if (fbW > cw && fbH > ch) { cw = fbW; ch = fbH; }
        if (cw > 0 && ch > 0) {
            m_vpX = 0; m_vpY = 0; m_vpW = cw; m_vpH = ch;
        }
    }

    // ---- Preview: capture BEFORE ImGui renders ----
    // On ANGLE, ImGui's D3D11 calls corrupt the render target state, making
    // acquireBackbuffer() return null after ImGui draws. Capture the clean game
    // frame first so the preview texture is always valid.
    if (!recording && m_visible) {
        auto& st = studioState();
        int pf = st.previewFps > 0 ? std::min(st.previewFps, 30) : 30;
        double now = nowMs();
        if (now - m_lastCaptureMs >= 1000.0 / pf) {
            m_lastCaptureMs = now;
            if (captureBackbuffer(true) && !m_pixbuf.empty())
                uploadPreviewTexture();
        }
    }

    // ---- Recording: capture when the fps timer fires ----
    if (recording) {
        auto& st = studioState();
        double now = nowMs();
        int rf = (st.recFps > 0) ? st.recFps : 30; if (rf < 1) rf = 1; if (rf > 300) rf = 300;
        double interval = 1000.0 / rf;

        int dueFrames = 0;
        while (now - m_lastRecordCaptureMs >= interval && dueFrames < 300) {
            m_lastRecordCaptureMs += interval;
            ++dueFrames;
        }

        if (dueFrames > 0) {
            if (captureBackbuffer(true) && !m_pixbuf.empty()) {
                if (m_visible && now - m_lastCaptureMs >= 1000.0 / 30.0) {
                    m_lastCaptureMs = now;
                    uploadPreviewTexture();
                }
                int w = m_gameW, h = m_gameH;
                Recorder::get().submitFrame(std::move(m_pixbuf), w, h, dueFrames);
            }
        }
    } else {
        if (m_wasRecording) {
            gdr::CaptureScheduler::get().endRecording();
        }
        m_wasRecording = false;
    }

    // ---- Studio not visible → skip all ImGui ----
    if (!m_visible) {
        if (m_showStartupNotify || m_showToast) {
            // Minimal startup toast (only for first 4 seconds).
            // Skip if ImGui is not initialized yet (avoids ANGLE shader compilation).
            if (m_initialized) {
                double now = nowMs();
                    ImGui_ImplOpenGL3_NewFrame();
                    ImGui_ImplWin32_NewFrame();
                    ImGui::NewFrame();
                auto drawToast = [&](const char* name, const char* title, const char* body, float alpha, float y) {
                    ImGui::SetNextWindowPos(ImVec2(12.0f, y), ImGuiCond_Always);
                    ImGui::SetNextWindowBgAlpha(alpha);
                    if (ImGui::Begin(name, nullptr, ImGuiWindowFlags_NoDecoration |
                            ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoFocusOnAppearing |
                            ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoMove)) {
                        if (title && *title) ImGui::Text("%s", title);
                        if (body && *body) ImGui::TextDisabled("%s", body);
                    }
                    ImGui::End();
                };

                float nextY = 12.0f;
                if (m_showStartupNotify) {
                    if (m_startupNotifyStart == 0.0) m_startupNotifyStart = now;
                    if (now - m_startupNotifyStart < 4000.0) {
                        auto& l = Localization::L();
                        drawToast("##gdsr_toast_startup", l.notify_startup, l.notify_hint, 0.85f, nextY);
                        nextY += 56.0f;
                    } else {
                        m_showStartupNotify = false;
                    }
                }

                if (m_showToast) {
                    if (m_toastStart == 0.0) m_toastStart = now;
                    if (now - m_toastStart < m_toastDurationMs) {
                        drawToast("##gdsr_toast_generic", m_toastTitle.c_str(), m_toastBody.c_str(), 0.90f, nextY);
                    } else {
                        m_showToast = false;
                    }
                }

                    ImGui::Render();
                    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
            } else {
                m_showStartupNotify = false;
                m_showToast = false;
            }
        }
        return;
    }

    // ---- Studio is visible: ensure ImGui is initialized ----
    if (!m_initialized) {
        lazyInit();
        if (!m_initialized) return;
    }

    auto& st = studioState();
    if (st.fontDirty) loadFonts();
    if (st.themeDirty) applyTheme();

    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
    ImGui::PushFont(m_font, st.fontSize);
    StudioUI::draw(&m_visible);
    ImGui::PopFont();
    if (st.savePending) saveSettings();
    ImGui::Render();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
}
