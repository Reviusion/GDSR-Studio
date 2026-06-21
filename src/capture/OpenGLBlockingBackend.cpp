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
#ifndef GL_FRAMEBUFFER_BINDING
#define GL_FRAMEBUFFER_BINDING 0x8CA6
#endif
#ifndef GL_READ_BUFFER
#define GL_READ_BUFFER 0x0C02
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

    // Clamp the read rectangle DOWN to the real current drawable. ImGuiManager's
    // cached viewport is built from CCEGLView::getFrameSize() (logical points) and
    // can exceed the true GL framebuffer (DPI scaling / reduced render scale / a
    // transient resize). Feeding that oversized rect to glReadPixels makes some
    // drivers (notably nvoglv64 on NVIDIA) read past the backbuffer storage and
    // throw an EXCEPTION_ACCESS_VIOLATION — the reported native-GL crash. GL_VIEWPORT
    // after GD's normal draw equals the drawable, so it is the authoritative bound.
    GLint vp[4] = {0, 0, 0, 0};
    (void)glGetError();
    glGetIntegerv(GL_VIEWPORT, vp);
    if (glGetError() == GL_NO_ERROR && vp[2] > 0 && vp[3] > 0) {
        if (reqW > vp[2]) reqW = vp[2];
        if (reqH > vp[3]) reqH = vp[3];
    }
    if (reqW <= 0 || reqH <= 0) return false;

    out.width = reqW; out.height = reqH;
    out.bytes.assign((size_t)reqW * (size_t)reqH * 4, 0);

    GLint prevAlign = 4;        glGetIntegerv(GL_PACK_ALIGNMENT, &prevAlign);
    GLint prevReadBuf = GL_BACK; glGetIntegerv(GL_READ_BUFFER, &prevReadBuf);
    GLint fbo = 0;              glGetIntegerv(GL_FRAMEBUFFER_BINDING, &fbo);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    // Only force the back buffer when the default framebuffer is bound; touching
    // glReadBuffer on a user FBO would be wrong. Fully restored below.
    if (fbo == 0) { (void)glGetError(); glReadBuffer(GL_BACK); (void)glGetError(); }
    (void)glGetError();
    // glReadPixels returns rows bottom-up — exactly the convention the rest of
    // the pipeline (preview UV flip, ffmpeg vflip) expects.
    glReadPixels(0, 0, reqW, reqH, m_bgra ? GL_BGRA : GL_RGBA, GL_UNSIGNED_BYTE, out.bytes.data());
    GLenum err = glGetError();
    if (fbo == 0) glReadBuffer((GLenum)prevReadBuf);
    glPixelStorei(GL_PACK_ALIGNMENT, prevAlign);
    return err == GL_NO_ERROR;
}

} // namespace gdr
