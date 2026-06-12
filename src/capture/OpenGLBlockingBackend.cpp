#include "OpenGLBlockingBackend.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <GL/gl.h>
#include <Geode/loader/Log.hpp>

#ifndef GL_BGRA
#define GL_BGRA 0x80E1
#endif

namespace gdr {

bool OpenGLBlockingBackend::init() {
    // Probe whether glReadPixels accepts GL_BGRA (matches the in-memory layout
    // ffmpeg wants without a channel swap); otherwise fall back to RGBA.
    m_bgra = true;
    unsigned char tmp[4] = {};
    GLint prevAlign = 4; glGetIntegerv(GL_PACK_ALIGNMENT, &prevAlign);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    (void)glGetError();
    glReadPixels(0, 0, 1, 1, GL_BGRA, GL_UNSIGNED_BYTE, tmp);
    if (glGetError() != GL_NO_ERROR) {
        (void)glGetError();
        m_bgra = false;
    }
    glPixelStorei(GL_PACK_ALIGNMENT, prevAlign);
    geode::log::info("GDSR GL: glReadPixels capture, fmt={}", m_bgra ? "BGRA" : "RGBA");
    return true;
}

bool OpenGLBlockingBackend::captureSync(int reqW, int reqH, RawFrame& out) {
    if (reqW <= 0 || reqH <= 0) return false;
    out.width = reqW; out.height = reqH;
    out.bytes.resize((size_t)reqW * reqH * 4);

    GLint prevAlign = 4; glGetIntegerv(GL_PACK_ALIGNMENT, &prevAlign);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    (void)glGetError();
    // glReadPixels returns rows bottom-up — exactly the convention the rest of
    // the pipeline (preview UV flip, ffmpeg vflip) expects.
    glReadPixels(0, 0, reqW, reqH, m_bgra ? GL_BGRA : GL_RGBA, GL_UNSIGNED_BYTE, out.bytes.data());
    GLenum err = glGetError();
    glPixelStorei(GL_PACK_ALIGNMENT, prevAlign);
    return err == GL_NO_ERROR;
}

} // namespace gdr
