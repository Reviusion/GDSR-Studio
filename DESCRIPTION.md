# GDSR Studio — a recording studio inside Geometry Dash

A full OBS-style recording studio that lives directly inside GD. No external software, no Game Capture, no alt-tabbing. Just hit F8 and you're in the studio.

---

## Overview

GDSR Studio isn't just another screenshot mod. It's a complete recorder with a live preview, audio mixer, hardware encoding, and a frame capture pipeline that doesn't tank your game's framerate.

**Video:** H.264 via x264 (software) or NVENC/AMF/QSV (hardware). Three encode modes per codec — Max Performance, Balanced, Max Quality — that actually map to each encoder's real presets, plus full color range and optional 4:4:4 chroma. Frame capture runs entirely off GD's render thread using D3D11 staging textures — so the game stays smooth even on low-end hardware.

**Audio:** WASAPI loopback captures system audio with zero hassle. Microphone on a separate channel. Record a single mixed track, or two tracks (full mix + mic-only) so you can re-balance the voice in your editor. Everything is encoded to AAC and muxed into a final MP4.

**Controls:** Everything you'd expect from OBS — preview window, audio mixer with peak meters, encoder and resolution settings, performance presets ranging from "potato" to "quality."

---

## Recommended: 720p @ 60 fps

That's the baseline for a clean, YouTube-ready recording. Runs fine on anything with a dedicated GPU from the last decade. If it chugs, drop down to the **Low** or **Balanced** preset.

---

## Performance presets

- **Potato** — 480p@30fps, fast encode. For actual toasters
- **Low** — 720p@60fps, fast encode. Smooth on office laptops
- **Balanced** — 1080p@60fps, balanced preset. The daily driver
- **Quality** — 1080p@60fps, slow preset, near-lossless. When picture matters more than file size

---

## Highlights

- Per-codec encode modes (Max Performance / Balanced / Max Quality) — real encoder presets, not a one-size streaming preset
- Constant-quality or constant-bitrate, full color range, optional 4:4:4 chroma (x264)
- Dual audio tracks — full mix + a separate mic-only track for editing
- Frame capture through D3D11 staging — not glReadPixels, doesn't stall the render thread
- Borrow FPS — when encoding can't keep up, the game slows itself down so every recorded frame is real
- 15 interface languages
- Dark and light themes
- Dockable panels, F11 fullscreen
- Auto-mux of video + audio into MP4 via ffmpeg

---

## Install

1. Download the `.geode` file from the releases page
2. Drop it in `%LOCALAPPDATA%\GeometryDash\geode\mods\`
3. Launch GD
4. Press F8

Requires Geode for GD 2.2081 (Windows).
