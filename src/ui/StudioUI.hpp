#pragma once

// Draws the OBS-like studio interface (menu bar, preview, docked panels,
// status bar). Call once per frame between ImGui::NewFrame and ImGui::Render.
namespace StudioUI {
    void draw(bool* open);
}
