#pragma once

#include <string>

// Shared, persistent-ish UI state for the studio. A single instance is shared
// across translation units via the inline accessor below.

enum class Theme { Dark = 0, Light = 1 };

// Order must match the font path table in ImGuiManager::loadFonts().
enum class FontFamily { SegoeUI = 0, Tahoma = 1, Arial = 2, Consolas = 3, Default = 4 };

// H.264 encoder backend. X264 is software (works everywhere); the rest are
// hardware encoders gated on the matching GPU vendor.
enum class VideoEncoder { X264 = 0, NVENC = 1, AMF = 2, QSV = 3 };

// Rate-control mode. Recorder maps these per-encoder (VBR is "quality+ceiling":
// CRF on x264, ICQ on QSV). QVBR is NVENC-only, CQVBR is AMF-only.
enum class RateControl { CQP = 0, QVBR = 1, CQVBR = 2, CBR = 3, VBR = 4 };

// How the recorder decides the rate: by a constant quality target (CRF / CQ /
// QP — best for a recorder, file size floats) or by a constant bitrate ceiling.
// Maps onto RateControl for the encoder args (Quality -> CQP, Bitrate -> CBR).
enum class RateMode { Quality = 0, Bitrate = 1 };

// Per-codec encode mode. Trades encoder speed for picture quality and applies to
// EVERY codec (x264/NVENC/AMF/QSV):
//   MaxPerformance — fastest preset, keeps high fps (120/240) on weak hardware.
//   Balanced       — middle ground.
//   MaxQuality     — slow/high preset, all quality tools on (best picture).
enum class EncodeMode { MaxPerformance = 0, Balanced = 1, MaxQuality = 2 };

// Audio track layout in the final mp4:
//   Single — desktop + mic mixed into one track (plays everywhere).
//   Dual   — track 1 = full mix (desktop+mic), track 2 = microphone only
//            (so it can be re-balanced / removed in an editor, OBS-style).
enum class AudioTrackMode { Single = 0, Dual = 1 };

// Performance presets. Trade preview/recording fidelity for game framerate.
// Custom leaves the individual fields untouched.
enum class PerfProfile { Potato = 0, Low = 1, Balanced = 2, Quality = 3, Custom = 4 };

struct StudioState {
    Theme theme       = Theme::Dark;
    int   language    = 1;        // index into Localization languages (1=English)
    int   fontFamily  = 0;        // FontFamily
    float fontSize    = 16.0f;
    bool  fullscreen  = false;

    // dockable panel visibility
    bool showPreview  = true;
    bool showMixer    = true;
    bool showControls = true;

    // auxiliary windows
    bool showSettings = false;
    bool showAbout    = false;

    // performance
    PerfProfile perfProfile = PerfProfile::Balanced;
    int   previewFps   = 30;      // cap on live-preview capture rate (0 = every frame)
    int   previewMaxDim = 1280;   // GPU-downscale cap on the longest captured side (0 = full res)

    // capture pipeline
    bool  threadedCapture  = true;  // run the GPU readback on a dedicated capture thread
                                    // (off GD's render thread). Falls back to synchronous
                                    // automatically if the driver can't support it.
    bool  borrowFpsFromGame = true; // when the capture/encode pipeline can't keep up at
                                    // the target fps, block GD's render thread (bounded)
                                    // so the game gives up framerate and every recorded
                                    // frame stays a real, fresh frame (smooth recording)
                                    // instead of dropping/duplicating frames.
    int   captureThrottleMs = 4;    // legacy bounded render-thread wait used only when
                                    // borrowFpsFromGame is off; 0 = never wait.

    // audio (WASAPI capture -> mixed WAV -> muxed onto the video at finalize).
    // Desktop (game/system) audio on by default; microphone off by default.
    bool  audioDesktopEnabled = true;
    bool  audioMicEnabled     = false;
    bool  audioDesktopMuted   = false;
    bool  audioMicMuted       = false;
    float audioDesktopVol     = 1.0f;    // 0..3 linear gain
    float audioMicVol         = 1.0f;
    int   audioBitrateKbps    = 192;     // AAC bitrate for the mux pass
    AudioTrackMode audioTrackMode = AudioTrackMode::Single; // single mixed vs dual (mix + mic)
    std::string desktopDeviceId;         // WASAPI endpoint id; empty = default render
    std::string micDeviceId;             // WASAPI endpoint id; empty = default capture

    // video / recording settings
    VideoEncoder videoEncoder = VideoEncoder::X264;
    RateControl  rateControl  = RateControl::CQP;
    EncodeMode   encodeMode   = EncodeMode::MaxQuality; // quality/perf preset per codec
    RateMode     rateMode     = RateMode::Quality;      // constant quality vs constant bitrate
    bool  fullColorRange = false; // encode full-range (pc) instead of limited (tv) —
                                  // preserves GD's vivid colours / less banding
    bool  highChroma444  = false; // 4:4:4 chroma (x264 only) — razor-sharp coloured
                                  // edges/text; bigger files, less player compatibility
    int   quality     = 20;       // QP / CQ / CRF, 0-51 (lower = better)
    int   bitrateKbps = 12000;    // used for CBR and as ceiling for QVBR/CQVBR
    int   recFps      = 60;
    int   outWidth    = 0;        // 0 = match capture (no scale)
    int   outHeight   = 0;
    std::string outputDir;        // empty -> %USERPROFILE%/Videos
    std::string ffmpegPath;       // empty -> bundled getResourcesDir()/ffmpeg.exe

    // one-shot signals consumed by ImGuiManager / StudioUI
    bool themeDirty   = true;     // re-apply style colors
    bool fontDirty    = true;     // reload font atlas (family changed)
    bool resetLayout  = false;    // rebuild the default dock layout
    bool savePending  = false;    // persist settings to disk
};

inline StudioState& studioState() {
    static StudioState state;
    return state;
}

// Apply a performance preset to the recording/preview fields. Custom is a no-op.
// All presets keep the safe software encoder (x264) so they never crash on
// machines whose GPU encoder is unavailable; the user can opt into NVENC/AMF/QSV
// afterward. Lower presets cut preview rate, record fps and output resolution —
// the three things that actually cost game framerate.
inline void applyPerfProfile(StudioState& st, PerfProfile p) {
    st.perfProfile  = p;
    st.videoEncoder = VideoEncoder::X264;
    st.rateControl  = RateControl::CQP;
    st.rateMode     = RateMode::Quality; // constant quality (CRF) — best for a recorder
    switch (p) {
        // Potato  — 480p@30fps, fast encode, lossy. Prioritises game framerate.
        case PerfProfile::Potato:
            st.previewFps = 10; st.recFps = 30; st.quality = 28;
            st.outWidth = 854; st.outHeight = 480; st.bitrateKbps = 2500;
            st.previewMaxDim = 480; st.encodeMode = EncodeMode::MaxPerformance; break;
        // Low  — 720p@60fps, fast encode. Smooth gameplay, modest quality.
        case PerfProfile::Low:
            st.previewFps = 15; st.recFps = 60; st.quality = 24;
            st.outWidth = 1280; st.outHeight = 720; st.bitrateKbps = 6000;
            st.previewMaxDim = 640; st.encodeMode = EncodeMode::MaxPerformance; break;
        // Balanced  — 1080p@60fps, balanced preset. Good picture, stable framerate.
        case PerfProfile::Balanced:
            st.previewFps = 30; st.recFps = 60; st.quality = 21;
            st.outWidth = 1920; st.outHeight = 1080; st.bitrateKbps = 16000;
            st.previewMaxDim = 1280; st.encodeMode = EncodeMode::Balanced; break;
        // Quality  — 1080p@60fps, slow high-quality preset, near-lossless picture.
        case PerfProfile::Quality:
            st.previewFps = 30; st.recFps = 60; st.quality = 17;
            st.outWidth = 1920; st.outHeight = 1080; st.bitrateKbps = 40000;
            st.previewMaxDim = 1280; st.encodeMode = EncodeMode::MaxQuality; break;
        case PerfProfile::Custom:
        default: break;
    }
}
