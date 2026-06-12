#pragma once

#include <string>
#include <vector>

struct ImFont;

class ImGuiManager {
public:
    static ImGuiManager& get();
    ~ImGuiManager();

    void renderFrame(int fbW, int fbH);

    void setVisible(bool visible) { m_visible = visible; }
    void toggleVisible() {
        m_visible = !m_visible;
        // Initialize ImGui only when studio is first opened — not at mod load.
        // This avoids all ImGui/ANGLE overhead during normal gameplay.
        if (m_visible && !m_initialized) lazyInit();
    }
    bool isVisible() const { return m_visible; }
    bool isInitialized() const { return m_initialized; }

    unsigned long long gameTexture() const { return m_gameTex; }
    unsigned long long logoTexture() const { return m_logoTex; }
    int logoTexW() const { return m_logoW; }
    int logoTexH() const { return m_logoH; }
    int gameTexW() const { return m_gameW; }
    int gameTexH() const { return m_gameH; }
    bool pixelBufferIsBGRA() const { return m_pixbufIsBGRA; }

    HWND gameWindow() const { return m_hwnd; }
    void showToast(const char* title, const char* body, double durationMs = 2800.0);

    void plannedRecordSize(int& w, int& h) const;

    // Ensure the mod's capture textures / format are ready (called from
    // Recorder::start before any capture).  Does NOT initialize ImGui —
    // only sets up the minimal GL state needed for glReadPixels.
    void ensureCaptureReady();

private:
    ImGuiManager() = default;

    void lazyInit();
    void applyTheme();
    void loadFonts();
    void ensureLogoTexture();
    void loadSettings();
    void saveSettings();
    void unhookWindowProc();
    bool captureBackbuffer(bool needPixels);
    void uploadPreviewTexture();  // upload m_pixbuf to m_gameTex; m_pixbuf must be non-empty
    void captureSize(int vpW, int vpH, int& outW, int& outH, bool forceRecord = false) const;

    bool    m_initialized = false;
    bool    m_visible     = false;
    bool    m_captureReady = false;  // minimal GL state for capture (no ImGui)
    ImFont* m_font        = nullptr;
    HWND    m_hwnd        = nullptr;
    bool    m_showStartupNotify = true;
    double  m_startupNotifyStart = 0.0;
    bool    m_showToast = false;
    double  m_toastStart = 0.0;
    double  m_toastDurationMs = 2800.0;
    std::string m_toastTitle;
    std::string m_toastBody;

    unsigned int               m_gameTex = 0;
    unsigned int               m_logoTex = 0;
    int                        m_logoW = 0;
    int                        m_logoH = 0;
    int                        m_gameW   = 0;
    int                        m_gameH   = 0;
    int                        m_texW    = 0;
    int                        m_texH    = 0;
    int                        m_vpX     = 0;
    int                        m_vpY     = 0;
    int                        m_vpW     = 0;
    int                        m_vpH     = 0;
    int                        m_lastFbW = 0;  // track framebuffer size for viewport re-query
    int                        m_lastFbH = 0;
    std::vector<unsigned char> m_pixbuf;
    std::vector<unsigned char> m_previewScratch;

    double m_lastCaptureMs = 0.0;
    double m_lastRecordCaptureMs = 0.0;
    bool   m_wasRecording = false;
    int    m_lastLogW      = 0;
    int    m_lastLogH      = 0;

    bool         m_pixbufIsBGRA = true;
    unsigned int m_glReadFormat = 0;
    unsigned int m_glTexFormat = 0;
};
