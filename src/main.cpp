#include <Geode/Geode.hpp>
#include <Geode/modify/CCEGLView.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>

#include "imgui_hook/ImGuiManager.hpp"

using namespace geode::prelude;

// Render the ImGui overlay right before the game presents the frame.
class $modify(GDSRView, CCEGLView) {
    void swapBuffers() {
        auto fs = this->getFrameSize();
        ImGuiManager::get().renderFrame(static_cast<int>(fs.width),
                                        static_cast<int>(fs.height));
        CCEGLView::swapBuffers();
    }
};

// Swallow gameplay input only while the studio is actively open.
class $modify(GDSRGameLayer, GJBaseGameLayer) {
    void handleButton(bool down, int button, bool isPlayer1) {
        if (ImGuiManager::get().isVisible())
            return;
        GJBaseGameLayer::handleButton(down, button, isPlayer1);
    }
};

$on_mod(Loaded) {
    log::info("GDSR Studio loaded. Press F8 in-game to open the studio window.");
}
