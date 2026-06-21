#include "StudioUI.hpp"
#include "StudioState.hpp"
#include "Localization.hpp"
#include "../imgui_hook/ImGuiManager.hpp"
#include "../capture/Recorder.hpp"
#include "../capture/AudioCapture.hpp"

#include <imgui.h>
#include <imgui_internal.h>

#include <windows.h>
#include <shellapi.h>

#include <cmath>
#include <vector>
#include <string>
#include <cstdio>
#include <algorithm>

namespace {
    using Localization::L;

    // Stable dock IDs. ImHashStr resets at "###", so the window identity depends
    // only on the suffix — the visible (localized) label in front can change
    // freely without breaking the saved dock layout.
    constexpr const char* ID_PREVIEW = "###gdsr_preview";
    constexpr const char* ID_MIXER   = "###gdsr_mixer";
    constexpr const char* ID_CTRL    = "###gdsr_ctrl";

    // Build "Localized Label###stable_id" for Begin().
    std::string title(const char* label, const char* id) {
        return std::string(label) + id;
    }

    float clampf(float v, float lo, float hi) {
        return v < lo ? lo : (v > hi ? hi : v);
    }

    int clampi(int v, int lo, int hi) {
        return v < lo ? lo : (v > hi ? hi : v);
    }

    ImVec2 adaptiveMainWindowSize(const ImGuiViewport* vp) {
        float w = clampf(vp->WorkSize.x * 0.86f, 640.0f, 1000.0f);
        float h = clampf(vp->WorkSize.y * 0.82f, 420.0f, 620.0f);
        w = std::min(w, vp->WorkSize.x - 16.0f);
        h = std::min(h, vp->WorkSize.y - ImGui::GetFrameHeight() - 16.0f);
        return ImVec2(std::max(320.0f, w), std::max(240.0f, h));
    }

    ImVec2 adaptivePopupSize(float preferredW, float preferredH) {
        ImGuiViewport* vp = ImGui::GetMainViewport();
        float w = std::min(preferredW, vp->WorkSize.x - 24.0f);
        float h = preferredH <= 0.0f ? 0.0f : std::min(preferredH, vp->WorkSize.y - 48.0f);
        return ImVec2(std::max(300.0f, w), h <= 0.0f ? 0.0f : std::max(180.0f, h));
    }

    std::wstring widenUtf8(const std::string& s) {
        if (s.empty()) return {};
        int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
        std::wstring w(n, 0);
        MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), w.data(), n);
        return w;
    }

    std::string defaultVideosDir() {
        wchar_t user[260] = {};
        if (GetEnvironmentVariableW(L"USERPROFILE", user, 260) > 0) {
            std::wstring w(user);
            int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
            std::string s(n, 0);
            WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), s.data(), n, nullptr, nullptr);
            return s + "\\Videos";
        }
        return ".";
    }

    void openRecordingsFolder() {
        std::string dir = studioState().outputDir.empty() ? defaultVideosDir() : studioState().outputDir;
        ShellExecuteW(nullptr, L"open", widenUtf8(dir).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }

    void buildDefaultLayout(ImGuiID dockspaceId) {
        ImGui::DockBuilderRemoveNode(dockspaceId);
        ImGui::DockBuilderAddNode(dockspaceId, ImGuiDockNodeFlags_DockSpace);
        ImVec2 size = ImGui::GetWindowSize();
        ImGui::DockBuilderSetNodeSize(dockspaceId, size);

        // Preview fills the top; the bottom strip holds the audio mixer and the
        // record controls side by side.
        float previewRatio = size.y < 520.0f ? 0.62f : 0.68f;
        float mixerRatio   = size.x < 860.0f ? 0.55f : 0.60f;

        ImGuiID bottom, center;
        center = ImGui::DockBuilderSplitNode(dockspaceId, ImGuiDir_Up, previewRatio, nullptr, &bottom);

        ImGuiID mixerNode, ctrlNode;
        mixerNode = ImGui::DockBuilderSplitNode(bottom, ImGuiDir_Left, mixerRatio, nullptr, &ctrlNode);

        ImGui::DockBuilderDockWindow(ID_PREVIEW, center);
        ImGui::DockBuilderDockWindow(ID_MIXER,   mixerNode);
        ImGui::DockBuilderDockWindow(ID_CTRL,    ctrlNode);

        ImGui::DockBuilderFinish(dockspaceId);
    }

    void drawMenuBar(bool* open) {
        auto& st = studioState();
        const auto& s = L();
        if (!ImGui::BeginMenuBar()) return;

        if (ImGui::BeginMenu(s.menu_file)) {
            if (ImGui::MenuItem(s.file_show_recordings)) openRecordingsFolder();
            if (ImGui::MenuItem(s.file_settings)) st.showSettings = true;
            ImGui::Separator();
            if (ImGui::MenuItem(s.file_exit)) *open = false;
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu(s.menu_view)) {
            if (ImGui::MenuItem(s.view_fullscreen, "F11", st.fullscreen))
                st.fullscreen = !st.fullscreen;
            if (ImGui::MenuItem(s.view_reset_layout))
                st.resetLayout = true;
            ImGui::Separator();
            if (ImGui::BeginMenu(s.view_panels)) {
                ImGui::MenuItem(s.panel_preview,  nullptr, &st.showPreview);
                ImGui::MenuItem(s.panel_mixer,    nullptr, &st.showMixer);
                ImGui::MenuItem(s.panel_controls, nullptr, &st.showControls);
                ImGui::EndMenu();
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu(s.menu_profile)) {
            if (ImGui::MenuItem(L().perf_potato, nullptr, st.perfProfile == PerfProfile::Potato))   { applyPerfProfile(st, PerfProfile::Potato);   st.savePending = true; }
            if (ImGui::MenuItem(L().perf_low, nullptr, st.perfProfile == PerfProfile::Low))         { applyPerfProfile(st, PerfProfile::Low);      st.savePending = true; }
            if (ImGui::MenuItem(L().perf_balanced, nullptr, st.perfProfile == PerfProfile::Balanced)) { applyPerfProfile(st, PerfProfile::Balanced); st.savePending = true; }
            if (ImGui::MenuItem(L().perf_quality, nullptr, st.perfProfile == PerfProfile::Quality)) { applyPerfProfile(st, PerfProfile::Quality);  st.savePending = true; }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu(s.menu_tools)) {
            if (ImGui::MenuItem(s.tools_wizard)) st.showSettings = true;
            if (ImGui::MenuItem(s.view_reset_layout)) st.resetLayout = true;
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu(s.menu_help)) {
            if (ImGui::MenuItem(s.help_about)) st.showAbout = true;
            ImGui::EndMenu();
        }
        ImGui::EndMenuBar();
    }

    void drawPreview() {
        if (ImGui::Begin(title(L().panel_preview, ID_PREVIEW).c_str())) {
            ImVec2 avail = ImGui::GetContentRegionAvail();
            if (avail.x < 32.0f || avail.y < 32.0f) {
                ImGui::Dummy(ImVec2(std::max(1.0f, avail.x), std::max(1.0f, avail.y)));
                ImGui::End();
                return;
            }
            float targetW = avail.x, targetH = avail.x * 9.0f / 16.0f;
            if (targetH > avail.y) { targetH = avail.y; targetW = avail.y * 16.0f / 9.0f; }
            ImVec2 cursor = ImGui::GetCursorScreenPos();
            ImVec2 p0(cursor.x + (avail.x - targetW) * 0.5f, cursor.y + (avail.y - targetH) * 0.5f);
            ImVec2 p1(p0.x + targetW, p0.y + targetH);

            ImDrawList* dl = ImGui::GetWindowDrawList();

            bool recording = Recorder::get().isRecording();
            auto& mgr = ImGuiManager::get();
            if (mgr.gameTexture() != 0) {
                // Live game frame. Framebuffer origin is bottom-left, so flip V.
                dl->AddImage((ImTextureID)mgr.gameTexture(), p0, p1,
                             ImVec2(0, 1), ImVec2(1, 0));
                dl->AddRect(p0, p1, IM_COL32(0, 0, 0, 255));
                // Red recording indicator when active.
                if (recording) {
                    dl->AddRect(ImVec2(p0.x + 2, p0.y + 2), ImVec2(p1.x - 2, p1.y - 2),
                                IM_COL32(220, 40, 40, 200));
                }

                char res[64];
                std::snprintf(res, sizeof(res), "%d x %d", mgr.gameTexW(), mgr.gameTexH());
                dl->AddText(ImVec2(p0.x + 6, p0.y + 6), IM_COL32(150, 154, 160, 255), res);
            } else {
                dl->AddRectFilled(p0, p1, IM_COL32(12, 13, 15, 255));
                dl->AddRect(p0, p1, IM_COL32(0, 0, 0, 255));

                const char* label = "GEOMETRY DASH  \xE2\x80\xA2  PREVIEW";
                ImVec2 ts = ImGui::CalcTextSize(label);
                dl->AddText(ImVec2((p0.x + p1.x - ts.x) * 0.5f, (p0.y + p1.y - ts.y) * 0.5f),
                            IM_COL32(120, 124, 130, 255), label);

                char res[64];
                std::snprintf(res, sizeof(res), "%.0f x %.0f", targetW, targetH);
                dl->AddText(ImVec2(p0.x + 6, p0.y + 6), IM_COL32(150, 154, 160, 255), res);
            }
        }
        ImGui::End();
    }

    // OBS-style compact vertical mixer channel using stable native ImGui layout.
    void drawMixerChannel(const char* id, const char* label,
                          bool* enabled, bool* muted, float* vol, float level,
                          void (*applyLive)(), ImVec2 channelSize) {
        ImGui::PushID(id);
        bool changed = false;

        ImGuiWindowFlags childFlags = ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;
        if (ImGui::BeginChild("##channel", channelSize, true, childFlags)) {
            ImDrawList* dl = ImGui::GetWindowDrawList();

            if (ImGui::Checkbox("##en", enabled)) changed = true;
            ImGui::SameLine(0.0f, 4.0f);
            ImGui::TextUnformatted(label);

            float db = (*muted || !*enabled || *vol <= 0.0001f) ? -60.0f : (20.0f * std::log10(*vol));
            char dbText[24];
            if (db <= -60.0f) std::snprintf(dbText, sizeof(dbText), "-\u221E dB");
            else              std::snprintf(dbText, sizeof(dbText), "%.1f dB", db);
            ImGui::TextDisabled("%s", dbText);

            const float meterH = 96.0f;
            const float meterW = 12.0f;
            const float sliderW = 18.0f;
            const float controlGap = 10.0f;
            const float totalControlsW = meterW + controlGap + sliderW;
            const float startX = std::max(0.0f, (ImGui::GetContentRegionAvail().x - totalControlsW) * 0.5f);

            float topY = ImGui::GetCursorPosY() + 2.0f;

            // Peak meter (left)
            ImGui::SetCursorPos(ImVec2(startX, topY));
            ImGui::InvisibleButton("##meter-space", ImVec2(meterW, meterH));
            ImRect meterRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax());

            dl->AddRectFilled(meterRect.Min, meterRect.Max, IM_COL32(24, 26, 30, 255), 2.0f);
            float peak = (*muted || !*enabled) ? 0.0f : clampf(level, 0.0f, 1.0f);
            if (peak > 0.001f) {
                float fillY = meterRect.Max.y - peak * meterH;
                ImRect fillRect(ImVec2(meterRect.Min.x + 1.0f, fillY), ImVec2(meterRect.Max.x - 1.0f, meterRect.Max.y - 1.0f));
                const float warnY = meterRect.Max.y - 0.75f * meterH;
                const float hotY  = meterRect.Max.y - 0.90f * meterH;
                if (fillRect.Max.y > warnY)
                    dl->AddRectFilled(ImVec2(fillRect.Min.x, std::max(fillRect.Min.y, warnY)), fillRect.Max, IM_COL32(60, 190, 90, 255), 1.0f);
                if (fillRect.Min.y < warnY && fillRect.Max.y > hotY)
                    dl->AddRectFilled(ImVec2(fillRect.Min.x, std::max(fillRect.Min.y, hotY)), ImVec2(fillRect.Max.x, warnY), IM_COL32(210, 190, 60, 255), 1.0f);
                if (fillRect.Min.y < hotY)
                    dl->AddRectFilled(fillRect.Min, ImVec2(fillRect.Max.x, std::min(fillRect.Max.y, hotY)), IM_COL32(220, 60, 60, 255), 1.0f);
            }
            dl->AddRect(meterRect.Min, meterRect.Max, IM_COL32(70, 74, 80, 220), 2.0f);

            auto drawTick = [&](float dbTick, const char* txt) {
                float t = (dbTick + 60.0f) / 69.5f;
                float y = meterRect.Max.y - t * meterH;
                dl->AddLine(ImVec2(meterRect.Max.x + 3.0f, y), ImVec2(meterRect.Max.x + 7.0f, y), IM_COL32(150, 154, 160, 160));
                dl->AddText(ImVec2(meterRect.Max.x + 10.0f, y - ImGui::GetTextLineHeight() * 0.5f), IM_COL32(150, 154, 160, 200), txt);
            };
            drawTick(-60.0f, "-60");
            drawTick(-30.0f, "-30");
            drawTick(-20.0f, "-20");
            drawTick(-10.0f, "-10");
            drawTick(0.0f, "0");
            drawTick(6.0f, "+6");

            // Stable native vertical slider (right)
            ImGui::SetCursorPos(ImVec2(startX + meterW + controlGap, topY));
            float sliderDb = db <= -60.0f ? -60.0f : db;
            ImGui::PushStyleColor(ImGuiCol_FrameBg, IM_COL32(34, 36, 40, 255));
            ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, IM_COL32(42, 45, 52, 255));
            ImGui::PushStyleColor(ImGuiCol_FrameBgActive, IM_COL32(50, 54, 60, 255));
            ImGui::PushStyleColor(ImGuiCol_SliderGrab, IM_COL32(218, 218, 218, 255));
            ImGui::PushStyleColor(ImGuiCol_SliderGrabActive, IM_COL32(255, 255, 255, 255));
            if (ImGui::VSliderFloat("##vol", ImVec2(sliderW, meterH), &sliderDb, -60.0f, 9.5f, "")) {
                *vol = std::pow(10.0f, sliderDb / 20.0f);
                *vol = clampf(*vol, 0.0f, 3.0f);
                changed = true;
            }
            ImGui::PopStyleColor(5);
            if (ImGui::IsItemHovered() || ImGui::IsItemActive()) {
                ImGui::BeginTooltip();
                ImGui::Text("%.0f%%", *vol * 100.0f);
                ImGui::EndTooltip();
            }

            // Zero dB marker on the slider track
            ImRect sliderRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
            float zeroT = (0.0f - (-60.0f)) / 69.5f;
            float zeroY = sliderRect.Max.y - zeroT * sliderRect.GetHeight();
            dl->AddLine(ImVec2(sliderRect.Min.x - 3.0f, zeroY), ImVec2(sliderRect.Max.x + 3.0f, zeroY), IM_COL32(220, 220, 220, 140));

            ImGui::Dummy(ImVec2(1.0f, 8.0f));
            if (ImGui::Button(*muted ? Localization::L().mixer_unmute : Localization::L().mixer_mute, ImVec2(-1.0f, 0.0f))) {
                *muted = !*muted;
                changed = true;
            }
        }
        ImGui::EndChild();

        if (changed) {
            studioState().savePending = true;
            applyLive();
        }
        ImGui::PopID();
    }

    void drawMixer() {
        auto& st = studioState();
        if (ImGui::Begin(title(L().panel_mixer, ID_MIXER).c_str())) {
            float spacing = 12.0f;
            float availW = std::max(140.0f, ImGui::GetContentRegionAvail().x);
            float channelW = std::max(120.0f, (availW - spacing) * 0.5f);
            ImVec2 channelSize(channelW, 180.0f);

            drawMixerChannel("desktop", L().audio_desktop,
                &st.audioDesktopEnabled, &st.audioDesktopMuted, &st.audioDesktopVol,
                AudioCapture::get().desktopLevel(),
                []{ auto& s = studioState(); auto& a = AudioCapture::get();
                    a.setDesktopEnabled(s.audioDesktopEnabled); a.setDesktopMuted(s.audioDesktopMuted);
                    a.setDesktopVolume(s.audioDesktopVol); }, channelSize);

            ImGui::SameLine(0.0f, spacing);

            drawMixerChannel("mic", L().audio_mic,
                &st.audioMicEnabled, &st.audioMicMuted, &st.audioMicVol,
                AudioCapture::get().micLevel(),
                []{ auto& s = studioState(); auto& a = AudioCapture::get();
                    a.setMicEnabled(s.audioMicEnabled); a.setMicMuted(s.audioMicMuted);
                    a.setMicVolume(s.audioMicVol); }, channelSize);
        }
        ImGui::End();
    }

    void drawControls() {
        auto& st = studioState();
        const auto& s = L();
        auto& rec = Recorder::get();
        bool finalizing = rec.isFinalizing();
        if (ImGui::Begin(title(s.panel_controls, ID_CTRL).c_str())) {
            float w = std::max(1.0f, ImGui::GetContentRegionAvail().x);

            // While the previous recording is still being written out, lock the
            // record control so a new session can't race the teardown.
            ImGui::BeginDisabled(finalizing);
            ImVec4 recColor = rec.isRecording() ? ImVec4(0.70f, 0.16f, 0.16f, 1.0f)
                                                : ImVec4(0.18f, 0.47f, 0.87f, 1.0f);
            ImGui::PushStyleColor(ImGuiCol_Button, recColor);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(recColor.x + 0.08f, recColor.y + 0.08f, recColor.z + 0.08f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(recColor.x * 0.85f, recColor.y * 0.85f, recColor.z * 0.85f, 1.0f));
            if (ImGui::Button(rec.isRecording() ? s.ctrl_stop_record : s.ctrl_start_record, ImVec2(w, 0))) {
                if (rec.isRecording()) {
                    rec.stop();
                } else {
                    auto& mgr = ImGuiManager::get();
                    rec.setInputPixelFormat(mgr.pixelBufferIsBGRA());
                    if (rec.start(mgr.gameWindow())) {
                        mgr.showToast(L().notify_recording_started, L().notify_recording_hint, 2600.0);
                        // Maximum recording performance: hide the studio UI while recording.
                        // This also disables preview rendering and prevents the overlay from
                        // competing with the game/render readback path.
                        mgr.setVisible(false);
                    }
                }
            }
            ImGui::PopStyleColor(3);
            ImGui::EndDisabled();

            if (ImGui::Button(s.ctrl_settings, ImVec2(w, 0)))
                st.showSettings = true;

            if (finalizing) {
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.80f, 0.35f, 1.0f));
                int pct = rec.saveProgress();
                if (pct < 0) pct = 0;
                if (pct > 100) pct = 100;
                ImGui::TextWrapped("%s %d%%", s.ctrl_saving, pct);
                ImGui::PopStyleColor();
                ImGui::ProgressBar(pct / 100.0f, ImVec2(w, 0));
            } else if (!rec.isRecording() && !rec.lastError().empty()) {
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.90f, 0.35f, 0.35f, 1.0f));
                ImGui::TextWrapped("%s", rec.lastError().c_str());
                ImGui::PopStyleColor();
            }
        }
        ImGui::End();
    }

    void drawStatusBar() {
        auto& rec = Recorder::get();
        bool recording = rec.isRecording();
        float recSeconds = rec.seconds();
        int mm = (int)recSeconds / 60, ss = (int)recSeconds % 60;

        ImGuiViewport* vp = ImGui::GetMainViewport();
        float h = ImGui::GetFrameHeight();
        ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x, vp->WorkPos.y + vp->WorkSize.y - h));
        ImGui::SetNextWindowSize(ImVec2(vp->WorkSize.x, h));
        ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                 ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoDocking |
                                 ImGuiWindowFlags_NoBringToFrontOnFocus;
        if (ImGui::Begin("##statusbar", nullptr, flags)) {
            ImGui::Text("GDSR Studio v0.1.0");
            ImGui::SameLine();
            ImGui::TextDisabled("|  %s", L().status_toggle_hint);

            char buf[64];
            std::snprintf(buf, sizeof(buf), "REC %02d:%02d", mm, ss);
            float rightW = ImGui::CalcTextSize(buf).x + 220.0f;
            if (ImGui::GetWindowWidth() > rightW + 220.0f) {
                ImGui::SameLine(ImGui::GetWindowWidth() - rightW);

                ImU32 recCol = recording ? IM_COL32(220, 60, 60, 255) : IM_COL32(90, 94, 100, 255);
                ImGui::GetWindowDrawList()->AddCircleFilled(
                    ImVec2(ImGui::GetCursorScreenPos().x + 5, ImGui::GetCursorScreenPos().y + ImGui::GetTextLineHeight() * 0.5f),
                    5.0f, recCol);
                ImGui::Dummy(ImVec2(14, 0)); ImGui::SameLine();
                ImGui::Text("%s", buf);
                ImGui::SameLine();
                ImGui::TextDisabled("|");
                ImGui::SameLine();
                ImGui::Text("%.0f FPS", ImGui::GetIO().Framerate);
            }
        }
        ImGui::End();
    }

    // Device pickers + audio bitrate. Enumeration touches COM, so the lists are
    // cached and only refreshed on demand (first open / Refresh button).
    void drawAudioDeviceSettings() {
        auto& st = studioState();
        static std::vector<AudioDeviceInfo> s_render, s_capture;
        static bool s_loaded = false;
        if (!s_loaded) {
            s_render  = AudioCapture::listDevices(true);
            s_capture = AudioCapture::listDevices(false);
            s_loaded = true;
        }

        // One endpoint combo. `idField` holds the chosen endpoint id (UTF-8);
        // empty = default device. Stores the id back on selection.
        auto deviceCombo = [&](const char* cid, const char* label,
                               const std::vector<AudioDeviceInfo>& devs, std::string* idField) {
            ImGui::PushID(cid);
            ImGui::TextUnformatted(label);
            std::string cur = Localization::L().audio_default_dev;
            for (auto& d : devs) {
                std::string u8(d.id.begin(), d.id.end());
                if (u8 == *idField) { cur = d.name; break; }
            }
            ImGui::SetNextItemWidth(-1);
            if (ImGui::BeginCombo("##dev", cur.c_str())) {
                if (ImGui::Selectable(Localization::L().audio_default_dev, idField->empty())) {
                    idField->clear(); st.savePending = true;
                }
                for (auto& d : devs) {
                    std::string u8(d.id.begin(), d.id.end());
                    if (ImGui::Selectable(d.name.c_str(), u8 == *idField)) {
                        *idField = u8; st.savePending = true;
                    }
                }
                ImGui::EndCombo();
            }
            ImGui::PopID();
        };

        deviceCombo("ddev", Localization::L().audio_desktop, s_render, &st.desktopDeviceId);
        deviceCombo("mdev", Localization::L().audio_mic,     s_capture, &st.micDeviceId);
        if (ImGui::SmallButton(L().audio_refresh_devices)) s_loaded = false;

        ImGui::SetNextItemWidth(160.0f);
        if (ImGui::InputInt(Localization::L().audio_bitrate, &st.audioBitrateKbps, 32, 64)) {
            st.audioBitrateKbps = clampi(st.audioBitrateKbps, 32, 512);
            st.savePending = true;
        }
    }

    void drawRecordingSettings() {
        auto& st = studioState();
        ImGui::SeparatorText(Localization::L().rec_section_video);

        // FPS — самый понятный параметр
        const char* fpsItems[] = { "24", "30", "60" };
        const int   fpsVals[]  = { 24, 30, 60 };
        int fpsIdx = 1;
        for (int i = 0; i < 3; ++i) if (fpsVals[i] == st.recFps) { fpsIdx = i; break; }
        ImGui::Text("%s", Localization::L().recording_fps);
        ImGui::SetNextItemWidth(-1);
        if (ImGui::Combo("##fps", &fpsIdx, fpsItems, 3)) {
            st.recFps = fpsVals[fpsIdx]; st.perfProfile = PerfProfile::Custom; st.savePending = true;
        }

        ImGui::Spacing();

        // Разрешение вывода — просто и понятно
        const char* resItems[] = { Localization::L().rec_res_same, "1920 x 1080", "1280 x 720", "854 x 480" };
        const int   resW[]     = { 0, 1920, 1280, 854 };
        const int   resH[]     = { 0, 1080, 720, 480 };
        int resIdx = 1;
        for (int i = 0; i < 4; ++i)
            if (resW[i] == st.outWidth && resH[i] == st.outHeight) { resIdx = i; break; }
        ImGui::Text("%s", Localization::L().recording_resolution);
        ImGui::SetNextItemWidth(-1);
        if (ImGui::Combo("##outres", &resIdx, resItems, 4)) {
            st.outWidth = resW[resIdx]; st.outHeight = resH[resIdx];
            st.perfProfile = PerfProfile::Custom; st.savePending = true;
        }

        ImGui::Spacing();

        // Качество — от 0 (лучшее) до 51 (худшее, но быстрое)
        ImGui::Text("%s", Localization::L().recording_quality);
        ImGui::SetNextItemWidth(-1);
        if (ImGui::SliderInt("##quality", &st.quality, 10, 45)) {
            st.perfProfile = PerfProfile::Custom; st.savePending = true;
        }
        ImGui::TextDisabled("%s", st.quality < 20 ? Localization::L().rec_quality_best : st.quality < 30 ? Localization::L().rec_quality_good : st.quality < 38 ? Localization::L().rec_quality_medium : Localization::L().rec_quality_fast);

        ImGui::Spacing();

        // Кодек
        ImGui::Text("%s", Localization::L().recording_encoder);
        ImGui::SetNextItemWidth(-1);
        int enc = (int)st.videoEncoder;
        if (ImGui::Combo("##enc", &enc, "x264 (CPU)\0NVENC\0AMF\0QSV\0MJPEG (fast)\0")) {
            st.videoEncoder = (VideoEncoder)enc;
            st.perfProfile = PerfProfile::Custom; st.savePending = true;
        }
        if (st.videoEncoder != VideoEncoder::X264)
            ImGui::TextDisabled("%s", Localization::L().rec_no_hw_encoder);

        ImGui::Spacing();

        // Путь к ffmpeg (продвинутая опция)
        ImGui::Text("%s", Localization::L().recording_ffmpeg_path);
        ImGui::SetNextItemWidth(-1);
        char ffBuf[512]; std::snprintf(ffBuf, sizeof(ffBuf), "%s", st.ffmpegPath.c_str());
        if (ImGui::InputText("##ffpath", ffBuf, sizeof(ffBuf))) {
            st.ffmpegPath = ffBuf; st.savePending = true;
        }

        ImGui::Spacing();
        ImGui::SeparatorText(Localization::L().rec_section_output);

        // Папка для записей
        ImGui::Text("%s", Localization::L().recording_output_dir);
        ImGui::SetNextItemWidth(-1);
        char dirBuf[512]; std::snprintf(dirBuf, sizeof(dirBuf), "%s", st.outputDir.c_str());
        if (ImGui::InputText("##outdir", dirBuf, sizeof(dirBuf))) {
            st.outputDir = dirBuf; st.savePending = true;
        }

        ImGui::Spacing();
        ImGui::SeparatorText(Localization::L().audio_section);
        drawAudioDeviceSettings();

        ImGui::Spacing();
        ImGui::SeparatorText(L().settings_performance);

        if (ImGui::Checkbox(L().settings_threaded_capture, &st.threadedCapture)) {
            st.savePending = true;
        }
        ImGui::TextDisabled("%s", L().settings_threaded_capture_desc);

        if (ImGui::Checkbox(L().settings_borrow_fps, &st.borrowFpsFromGame)) {
            st.savePending = true;
        }
        ImGui::TextDisabled("%s", L().settings_borrow_fps_desc);
    }

    void drawSettingsWindow() {
        auto& st = studioState();
        const auto& s = L();
        if (!st.showSettings) return;

        ImGui::SetNextWindowSize(adaptivePopupSize(420.0f, 520.0f), ImGuiCond_FirstUseEver);
        if (ImGui::Begin(title(s.settings_title, "###gdsr_settings").c_str(), &st.showSettings)) {
            ImGui::SeparatorText(s.settings_appearance);

            // Theme
            ImGui::TextUnformatted(s.settings_theme);
            int themeIdx = (int)st.theme;
            if (ImGui::RadioButton(s.settings_theme_dark, &themeIdx, (int)Theme::Dark)) {
                st.theme = Theme::Dark; st.themeDirty = true; st.savePending = true;
            }
            ImGui::SameLine();
            if (ImGui::RadioButton(s.settings_theme_light, &themeIdx, (int)Theme::Light)) {
                st.theme = Theme::Light; st.themeDirty = true; st.savePending = true;
            }

            ImGui::Spacing();

            // Language
            ImGui::TextUnformatted(s.settings_language);
            ImGui::SetNextItemWidth(-1);
            if (ImGui::BeginCombo("##lang", Localization::nativeName(st.language))) {
                for (int i = 0; i < Localization::count(); ++i) {
                    bool sel = (st.language == i);
                    if (ImGui::Selectable(Localization::nativeName(i), sel)) {
                        st.language = i; st.savePending = true; st.fontDirty = true;
                    }
                    if (sel) ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }

            ImGui::Spacing();

            // Font family
            const char* fonts[] = { "Segoe UI", "Tahoma", "Arial", "Consolas", "ImGui Default" };
            ImGui::TextUnformatted(s.settings_font_family);
            ImGui::SetNextItemWidth(-1);
            if (ImGui::Combo("##fontfam", &st.fontFamily, fonts, IM_ARRAYSIZE(fonts))) {
                st.fontDirty = true; st.savePending = true;
            }

            ImGui::Spacing();

            // Font size
            ImGui::TextUnformatted(s.settings_font_size);
            ImGui::SetNextItemWidth(-1);
            st.fontSize = clampf(st.fontSize, 11.0f, 28.0f);
            if (ImGui::SliderFloat("##fontsize", &st.fontSize, 11.0f, 28.0f, "%.0f px")) {
                st.fontSize = clampf(st.fontSize, 11.0f, 28.0f);
                st.fontDirty = true;
                st.savePending = true;
                st.resetLayout = true;
            }

            drawRecordingSettings();

            ImGui::Spacing();
            ImGui::Separator();
            if (ImGui::Button(s.settings_close))
                st.showSettings = false;
        }
        ImGui::End();
    }

    void drawAboutWindow() {
        auto& st = studioState();
        const auto& s = L();
        if (!st.showAbout) return;

        ImGui::SetNextWindowSize(adaptivePopupSize(440.0f, 0.0f), ImGuiCond_Always);
        if (ImGui::Begin(title(s.about_title, "###gdsr_about").c_str(), &st.showAbout,
                         ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoResize)) {
            ImGui::TextUnformatted("GDSR Studio");
            ImGui::SameLine();
            ImGui::TextDisabled("v0.1.0");
            ImGui::Separator();
            ImGui::TextWrapped("%s", s.about_body);
            ImGui::Spacing();
            ImGui::TextWrapped("%s", s.about_hotkey);
            ImGui::Spacing();
            ImGui::Separator();
            if (ImGui::Button(s.settings_close))
                st.showAbout = false;
        }
        ImGui::End();
    }
}

namespace StudioUI {
    void draw(bool* open) {
        auto& st = studioState();
        Localization::setLanguage(st.language);

        ImGuiViewport* vp = ImGui::GetMainViewport();
        float statusH = ImGui::GetFrameHeight();

        ImGuiWindowFlags hostFlags = ImGuiWindowFlags_MenuBar | ImGuiWindowFlags_NoDocking;

        if (st.fullscreen) {
            ImGui::SetNextWindowPos(vp->WorkPos);
            ImGui::SetNextWindowSize(ImVec2(vp->WorkSize.x, vp->WorkSize.y - statusH));
            ImGui::SetNextWindowViewport(vp->ID);
            hostFlags |= ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse |
                         ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                         ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus;
        } else {
            ImVec2 hostSize = adaptiveMainWindowSize(vp);
            ImGui::SetNextWindowSize(hostSize, ImGuiCond_FirstUseEver);
            ImGui::SetNextWindowSizeConstraints(ImVec2(320.0f, 240.0f), ImVec2(vp->WorkSize.x, vp->WorkSize.y - statusH));
            ImVec2 pos(vp->WorkPos.x + std::min(40.0f, std::max(8.0f, vp->WorkSize.x * 0.04f)),
                       vp->WorkPos.y + std::min(40.0f, std::max(8.0f, vp->WorkSize.y * 0.04f)));
            ImGui::SetNextWindowPos(pos, ImGuiCond_FirstUseEver);
        }

        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
        bool hostOpen = ImGui::Begin("GDSR Studio###gdr_main", open, hostFlags);
        ImGui::PopStyleVar();

        // Do not scale the whole style every frame. The old code multiplied
        // all paddings and border sizes on every draw call, then tried to undo
        // it. With font-size changes this caused drift, broken docking and ugly
        // buttons. Font size is handled only by the font atlas in ImGuiManager.
        if (hostOpen) {
            drawMenuBar(open);

            auto& mgr = ImGuiManager::get();
            if (mgr.logoTexture() != 0 && mgr.logoTexW() > 0 && mgr.logoTexH() > 0) {
                ImVec2 logoPos = ImGui::GetWindowPos() + ImVec2(10.0f, 30.0f);
                float targetH = 20.0f;
                float targetW = targetH * ((float)mgr.logoTexW() / (float)mgr.logoTexH());
                ImGui::GetWindowDrawList()->AddImage(
                    (ImTextureID)mgr.logoTexture(),
                    logoPos,
                    ImVec2(logoPos.x + targetW, logoPos.y + targetH),
                    ImVec2(0, 0), ImVec2(1, 1));
            }

            ImGuiID dockspaceId = ImGui::GetID("GDRDockSpace");
            if (ImGui::DockBuilderGetNode(dockspaceId) == nullptr || st.resetLayout) {
                buildDefaultLayout(dockspaceId);
                st.resetLayout = false;
            }
            ImGui::DockSpace(dockspaceId, ImVec2(0, 0), ImGuiDockNodeFlags_None);
        }
        ImGui::End();

        if (hostOpen) {
            if (st.showPreview)  drawPreview();
            if (st.showMixer)    drawMixer();
            if (st.showControls) drawControls();
        }

        drawStatusBar();
        drawSettingsWindow();
        drawAboutWindow();
    }
}
