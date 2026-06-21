# GDSR Studio

Record Geometry Dash like you're in OBS — without ever leaving the game.

I got tired of alt-tabbing into OBS every time I wanted to record a decent clip for YouTube. Black screen, broken audio, lag — the usual. So I built GDSR Studio: a full recording studio rendered directly inside GD using Dear ImGui.

No OBS. No stream keys. No Game Capture wizard nonsense. Just hit F8 and you've got an OBS-style panel right inside the game — preview window, audio mixer, encoder settings, and a big fat record button.

---

## What it does

- **Live preview** — see exactly what's being recorded, right in the studio panel
- **Video recording** — H.264 (software x264 or hardware NVENC / AMF / QSV) or MJPEG
- **Three audio tracks** — every recording gets three separate tracks in the MP4: **game**, **mic**, and **game + mic** mixed. Edit them independently, mute the mic, re-balance the voice — no re-recording
- **Audio mixer** — separate channels for desktop and mic, with gain, mute, and peak meters
- **Performance presets** — Potato / Low / Balanced / Quality
- **Smooth while recording** — frame capture runs on a dedicated thread via D3D11 staging textures, and the preview is decimated and kept off the capture swap, so the game's framerate stays steady instead of swinging
- **Borrow FPS** — when the encoder falls behind, the game itself slows down just enough so every recorded frame is a real frame (no dropped/duplicated frames). Essential on weaker machines.
- **15 languages** — English, Русский, Español, Português, Deutsch, 简体中文, Français, 日本語, 한국어, Italiano, Polski, Türkçe, Tiếng Việt, Bahasa Indonesia, Українська
- **Dark/light theme**, dockable panels, F11 for fullscreen
- **Recommended: 720p at 60 fps** — the sweet spot for YouTube-ready recordings without hammering your GPU

---

## Installation

You need [Geode](https://geode-sdk.org) for Geometry Dash 2.2081 on Windows.

1. Download `reviusion.gdsr-studio.geode` from the [Releases page](../../releases)
2. Drop it into `%LOCALAPPDATA%\GeometryDash\geode\mods\`
3. Launch GD — the mod loads automatically

Press **F8** to open the studio.

---

## How to use

1. Hit F8 → the studio window opens
2. Go to Settings → pick your output folder, FPS, quality, encoder
3. Click **Start Recording** — the UI hides itself to free up resources
4. Press F8 mid-recording to check the live preview
5. Hit **Stop Recording** → file saves to `%USERPROFILE%\Videos\GDSR_timestamp.mp4`

Hotkeys:
| Key | Action |
|-----|--------|
| F8 | Toggle studio |
| F11 | Fullscreen |

---

## Performance & hardware

Frame capture happens entirely off GD's render thread:

```
Game renders a frame
  → issueCopy()         — render thread: fast D3D11 CopyResource
    → mapSlot()         — worker thread: Map + memcpy (heavy, but off-render)
      → submitFrame()   — encoder thread: pipe to ffmpeg
```

**"Borrow FPS from game"** (on by default) — when the pipeline can't keep up, GD's render thread blocks briefly so every recorded frame stays real. On low-end hardware this is the difference between smooth recording and a slideshow.

**Recommended baseline: 720p @ 60 fps** — hits the YouTube sweet spot, looks clean, and runs fine on anything with a dedicated GPU from the last decade. If your machine struggles, try the presets below.

**If you're on a toaster:**

| Setting | Value |
|---------|-------|
| FPS | 30 |
| Resolution | 1280×720 |
| Encoder | x264 (CPU) |
| Quality | 28–35 |
| Preset | Low or Potato |

---

## Building from source

**Requirements:**
- Windows 10/11
- Visual Studio 2022 Build Tools
- CMake 3.21+
- [Geode SDK](https://docs.geode-sdk.org/getting-started/ide-setup) (`GEODE_SDK` env variable set)
- ffmpeg.exe placed in `resources/` ([download here](https://www.gyan.dev/ffmpeg/builds/))

**Clone with submodules:**
```bat
git clone --recurse-submodules https://github.com/Reviusion/GDSR-Studio.git
cd GDSR-Studio
```

**Build:**
```bat
cmake -B build
cmake --build build --config RelWithDebInfo
```

The built `.geode` package auto-installs into your running GD instance.

---

## Project layout

```
src/
├── capture/
│   ├── AngleD3D11Backend   — reads ANGLE/GLES framebuffer via D3D11 staging textures
│   ├── AudioCapture        — WASAPI loopback + mic → 48kHz stereo WAV
│   ├── CaptureScheduler    — orchestrates the dedicated capture thread
│   ├── OpenGLBlockingBackend — fallback glReadPixels for native desktop GL
│   └── Recorder            — ffmpeg pipe + video/audio mux to mp4
├── imgui_hook/
│   └── ImGuiManager        — swapBuffers hook, preview texture, ImGui render loop
└── ui/
    ├── Localization        — 15-language string table
    ├── StudioState         — shared settings / state
    └── StudioUI            — ImGui panel layout
third_party/
└── imgui                   — Dear ImGui (docking branch, git submodule)
resources/
└── ffmpeg.exe              — not included in the repo, grab it yourself
```

---

## Troubleshooting

| Problem | Likely cause |
|---------|-------------|
| Preview is black | First frame while pipeline primes — wait a second or two. If it stays black, check the Geode log for `GDSR:` errors |
| `mux: ffmpeg code 1` | Rare, on 20+ min recordings with slow HDDs. Mux timeout scales with video length — retry usually works |
| No audio in recording | Check the mixer — desktop audio is on by default. Verify your device in Settings → Audio |
| Game stutters while recording | Expected on weak hardware. Turn on Borrow FPS, lower recFps and resolution |

---

## License

Source code — personal / educational use. Don't redistribute modified versions under the same name.

ffmpeg.exe is bundled under [LGPL 2.1](https://ffmpeg.org/legal.html) — not included in this repository.
