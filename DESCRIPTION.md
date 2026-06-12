# GDSR Studio — a recording studio inside Geometry Dash

A full OBS-style recording studio that lives directly inside GD. No external software, no Game Capture, no alt-tabbing. Just hit F8 and you're in the studio.

---

## Overview

GDSR Studio isn't just another screenshot mod. It's a complete recorder with a live preview, audio mixer, hardware encoding, and a frame capture pipeline that doesn't tank your game's framerate.

**Video:** H.264 via x264/NVENC/AMF/QSV, or MJPEG. Frame capture runs entirely off GD's render thread using D3D11 staging textures — so the game stays smooth even on low-end hardware.

**Audio:** WASAPI loopback captures system audio with zero hassle. Microphone on a separate channel. Both get mixed to AAC and muxed into a final MP4.

**Controls:** Everything you'd expect from OBS — preview window, audio mixer with peak meters, encoder and resolution settings, performance presets ranging from "potato" to "quality."

---

## Performance presets

- **Potato** — 480p@24fps, maximum compression. For actual toasters
- **Low** — 720p@30fps. Playable on office laptops
- **Balanced** — 720p@24fps. The sweet spot for most setups
- **Quality** — 720p@24fps, low CRF. When picture quality matters

---

## Highlights

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
