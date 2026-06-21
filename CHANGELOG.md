# Changelog

## v0.2.1

- Added 120 fps to the recording framerate options (24 / 30 / 60 / 120)

## v0.2.0

Two big things this release: separate audio tracks, and a much steadier framerate while
recording.

**Three audio tracks.** Every recording now writes three separate tracks into the MP4
instead of one mixed blob:

- **game** - desktop audio only
- **mic** - microphone only
- **game + mic** - the full mix

So you can drop the clip into your editor and re-balance the voice, mute the mic, or pull
the game audio on its own - without re-recording. All three are always written (padded
with silence when a source is off), so the track layout never shifts around.

**Steadier framerate while recording.** Recording used to make the framerate swing - fine
one second, dropping the next. A few fixes for that:

- The preview is decimated to a small thumbnail and uploaded on a separate swap instead of
  piling onto the same frame as the capture, which was the main cause of the swing
- The MSAA resolve texture is cached instead of being recreated every single frame
- The record clock resyncs after a big stall (alt-tab, loading spike) instead of emitting a
  catch-up burst

Borrow-FPS and the capture pacing are untouched - this is purely about cutting the per-frame
work that made things jittery.

### Install

Download `reviusion.gdsr-studio.geode` below and drop it in
`%LOCALAPPDATA%\GeometryDash\geode\mods\`
mod browser. Press **F8** in-game to open the studio.

Needs Geode for Geometry Dash 2.2081 on Windows.

---

## v0.1.0

First release. An OBS-style recording studio that lives inside Geometry Dash - live
preview, audio mixer, hardware encoding, and a capture pipeline that runs off the render
thread so the game doesn't stutter. Hit F8 and record.
