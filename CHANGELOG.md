# Changelog

## v0.2.0

This one's mostly about quality. v0.1.0 recorded fine, but every encoder ran on a
single "just get it out the door" preset, so the picture never looked as good as the
hardware could actually do. That's fixed now, plus a few things people asked for.

**Encode modes (per codec).** Every encoder — x264, NVENC, AMF, QSV — now has three
modes instead of one fixed preset:

- **Max Performance** — fastest preset, keeps your framerate up on weak hardware
- **Balanced** — the sensible middle
- **Max Quality** — slow preset with all the quality tools on (look-ahead, AQ, B-frames)

These map to the encoders' *real* presets (NVENC p1→p7, x264 superfast→medium, and so
on), not the streaming "low-latency" preset everything used to be stuck on. Max Quality
in particular looks dramatically better at the same bitrate.

**Better quality controls.**

- Pick constant-quality (CRF/CQ) or constant-bitrate — clearer than the old rate-control dropdown
- Full color range option, so GD's bright colors don't get crushed and you get less banding
- 4:4:4 chroma (x264) for razor-sharp colored edges and text — bigger files, but worth it for some clips

**Dual audio tracks.** You can now record two tracks instead of one: the full mix plus a
separate mic-only track. Drop it into your editor and re-balance or mute your voice
without re-recording. (Single mixed track is still the default.)

**Reworked presets.** They were all 720p before. Now they actually spread out:

| Preset | Resolution | FPS | Encode |
|--------|-----------|-----|--------|
| Potato | 480p | 30 | Max Performance |
| Low | 720p | 60 | Max Performance |
| Balanced | 1080p | 60 | Balanced |
| Quality | 1080p | 60 | Max Quality |

**Other changes.**

- Default quality bumped a little (CRF 23 → 20) — cleaner out of the box
- Removed MJPEG. Nobody was using it and the files were enormous
- More translated strings across all 15 languages for the new settings

### Install

Download `reviusion.gdsr-studio.geode` below and drop it in
`%LOCALAPPDATA%\GeometryDash\geode\mods\`, or just install it from the Geode in-game
mod browser. Press **F8** in-game to open the studio.

Needs Geode for Geometry Dash 2.2081 on Windows.

---

## v0.1.0

First release. An OBS-style recording studio that lives inside Geometry Dash — live
preview, audio mixer, hardware encoding, and a capture pipeline that runs off the render
thread so the game doesn't stutter. Hit F8 and record.
