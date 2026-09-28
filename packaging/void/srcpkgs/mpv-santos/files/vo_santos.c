/* vo_santos.c: mpv video output via PowerVR HWC (santos10wifi / Void).
 * Path: mp_image (SW NV12/420P) -> GLES BT.601 -> HWC commit -> panel.
 * Provenance: vo-player2 (HWC flow) + santos-video-vo.vsync (NV12 shader).
 * v1: no OSD/subtitles, no hwdec, no vsync-throttle (swap paces). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <hardware/hardware.h>
#include <hardware/hwcomposer.h>
#include <hybris/hwcomposerwindow/hwcomposer.h>
#include <sync/sync.h>
#include <EGL/egl.h>
#include <GLES2/gl2.h>

#include "video/out/vo.h"
#include "video/mp_image.h"
#include "video/mp_image_pool.h"
#include "osdep/threads.h"

struct priv {
    hwc_composer_device_1_t *hcolors_dev;
    hwc_display_contents_1_t **hwcContents;
    hwc_layer_1_t *fblayer;
    struct ANativeWindow *win;
    EGLDisplay dpy;
    EGLSurface surf;
    EGLContext ctx;
    GLuint prog, tex_y, tex_uv;
    GLint a_pos, a_tc, u_y, u_uv;
    int dw, dh, vx, vy, vw, vh;
};

static const char vsrc[] =
    "attribute vec4 position;\nattribute vec4 texcoords;\nvarying vec2 tc;\n"
    "void main(){gl_Position=position;tc=texcoords.xy;}\n";
static const char fsrc[] =
    "varying highp vec2 tc;\nuniform sampler2D tY;\nuniform sampler2D tUV;\n"
    "void main(){highp float y=texture2D(tY,tc).r;highp vec2 uv=texture2D(tUV,tc).ra;"
    "y=1.164*(y-0.0625);highp float u=uv.r-0.5;highp float v=uv.g-0.5;"
    "gl_FragColor=vec4(y+1.596*v,y-0.391*u-0.813*v,y+2.018*u,1.0);}\n";
static const float qp[8] = { -1,-1, 1,-1, -1,1, 1,1 };
static const float qt[8] = { 0,0, 1,0, 0,1, 1,1 };

static void santos_default_env(void) {
    setenv("LIBEGL", "/system/vendor/lib/egl/libEGL_POWERVR_SGX544_115.so", 0);
    setenv("LIBGLESV2", "/system/vendor/lib/egl/libGLESv2_POWERVR_SGX544_115.so", 0);
    const char *ld = getenv("LD_LIBRARY_PATH");
    if (!ld || !strstr(ld, "santos-hybris")) {
        char buf[1024];
        snprintf(buf, sizeof(buf), "/usr/lib/santos-hybris:%s", ld ? ld : "/usr/lib:/lib");
        setenv("LD_LIBRARY_PATH", buf, 1);
    }
    setenv("HYBRIS_ANDROID_SDK_VERSION", "25", 0);
    setenv("HYBRIS_LINKER_DIR", "/usr/lib/santos-hybris/libhybris/linker", 0);
    setenv("HYBRIS_EGLPLATFORM", "hwcomposer", 0);
    setenv("EGL_PLATFORM", "hwcomposer", 0);
    setenv("HYBRIS_EGLPLATFORM_DIR", "/usr/lib/santos-hybris/libhybris", 0);
    setenv("ANDROID_ROOT", "/system", 0);
    setenv("ANDROID_DATA", "/data", 0);
    setenv("LD_SHIM_LIBS", "/system/vendor/lib/libmultidisplay.so|/system/lib/libshim_mds.so:/system/vendor/lib/libsepdrm.so|/system/lib/libshim_drm.so", 0);
}

static void present_cb(void *ud, struct ANativeWindow *win, struct ANativeWindowBuffer *buf) {
    struct vo *vo = ud;
    struct priv *p = vo->priv;
    int oldretire = p->hwcContents[0]->retireFenceFd;
    p->hwcContents[0]->retireFenceFd = -1;
    p->fblayer->handle = buf->handle;
    p->fblayer->acquireFenceFd = HWCNativeBufferGetFence(buf);
    p->fblayer->releaseFenceFd = -1;
    if (p->hcolors_dev->prepare(p->hcolors_dev, HWC_NUM_DISPLAY_TYPES, p->hwcContents))
        return;
    p->hcolors_dev->set(p->hcolors_dev, HWC_NUM_DISPLAY_TYPES, p->hwcContents);
    HWCNativeBufferSetFence(buf, p->fblayer->releaseFenceFd);
    if (oldretire != -1) { sync_wait(oldretire, -1); close(oldretire); }
}

static GLuint compile_one(struct vo *vo, GLenum type, const char *src) {
    GLuint h = glCreateShader(type);
    glShaderSource(h, 1, &src, 0);
    glCompileShader(h);
    GLint ok = 0;
    glGetShaderiv(h, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        MP_ERR(vo, "santos: shader compile failed\n");
        return 0;
    }
    return h;
}

static int preinit(struct vo *vo) {
    struct priv *p = vo->priv;
    santos_default_env();

    const hw_module_t *mod = NULL;
    if (hw_get_module("hwcomposer", &mod) || !mod) {
        MP_ERR(vo, "santos: hwcomposer module missing\n");
        return -1;
    }
    hwc_composer_device_1_t *dev = NULL;
    if (hwc_open_1(mod, &dev) || !dev) {
        MP_ERR(vo, "santos: hwc open failed\n");
        return -1;
    }
    p->hcolors_dev = dev;
    if (dev->blank)
        dev->blank(dev, 0, 0);
    uint32_t cfgs[5];
    size_t n = 5;
    dev->getDisplayConfigs(dev, 0, cfgs, &n);
    int32_t av[2];
    const uint32_t at[] = { 2, 3, 0 }; /* WIDTH, HEIGHT, NO_ATTRIBUTE */
    dev->getDisplayAttributes(dev, 0, cfgs[0], at, av);
    p->dw = av[0]; p->dh = av[1];
    MP_INFO(vo, "santos: HWC %dx%d\n", p->dw, p->dh);

    size_t sz = sizeof(hwc_display_contents_1_t) + 2 * sizeof(hwc_layer_1_t);
    hwc_display_contents_1_t *list = calloc(1, sz);
    p->hwcContents = calloc(HWC_NUM_DISPLAY_TYPES, sizeof(void *));
    for (int i = 0; i < HWC_NUM_DISPLAY_TYPES; i++)
        p->hwcContents[i] = NULL;
    p->hwcContents[0] = list;
    hwc_rect_t r = { 0, 0, p->dw, p->dh };
    hwc_layer_1_t *l0 = &list->hwLayers[0];
    memset(l0, 0, sizeof(*l0));
    l0->compositionType = HWC_FRAMEBUFFER;
    l0->blending = HWC_BLENDING_NONE;
    l0->displayFrame = r;
    l0->visibleRegionScreen.numRects = 1;
    l0->visibleRegionScreen.rects = &l0->displayFrame;
    l0->acquireFenceFd = -1;
    l0->releaseFenceFd = -1;
    p->fblayer = &list->hwLayers[1];
    memset(p->fblayer, 0, sizeof(*p->fblayer));
    p->fblayer->compositionType = HWC_FRAMEBUFFER_TARGET;
    p->fblayer->blending = HWC_BLENDING_NONE;
    p->fblayer->sourceCropf.top = 0;
    p->fblayer->sourceCropf.left = 0;
    p->fblayer->sourceCropf.bottom = (float)p->dh;
    p->fblayer->sourceCropf.right = (float)p->dw;
    p->fblayer->displayFrame = r;
    p->fblayer->visibleRegionScreen.numRects = 1;
    p->fblayer->visibleRegionScreen.rects = &p->fblayer->displayFrame;
    p->fblayer->acquireFenceFd = -1;
    p->fblayer->releaseFenceFd = -1;
    list->retireFenceFd = -1;
    list->flags = HWC_GEOMETRY_CHANGED;
    list->numHwLayers = 2;

    p->dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    EGLint maj, min;
    if (!eglInitialize(p->dpy, &maj, &min)) {
        MP_ERR(vo, "santos: egl init failed\n");
        return -1;
    }
    p->win = HWCNativeWindowCreate(p->dw, p->dh, HAL_PIXEL_FORMAT_RGBA_8888, present_cb, vo);
    if (!p->win) {
        MP_ERR(vo, "santos: native window failed\n");
        return -1;
    }
    EGLint cfa[] = { EGL_SURFACE_TYPE, EGL_WINDOW_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
                     EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_NONE };
    EGLConfig cfg;
    EGLint nc = 0;
    eglChooseConfig(p->dpy, cfa, &cfg, 1, &nc);
    p->surf = eglCreateWindowSurface(p->dpy, cfg, (EGLNativeWindowType)p->win, 0);
    if (p->surf == EGL_NO_SURFACE) {
        MP_ERR(vo, "santos: window surface failed\n");
        return -1;
    }
    EGLint cxa[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
    p->ctx = eglCreateContext(p->dpy, cfg, EGL_NO_CONTEXT, cxa);
    eglMakeCurrent(p->dpy, p->surf, p->surf, p->ctx);
    GLuint vs = compile_one(vo, GL_VERTEX_SHADER, vsrc);
    GLuint fs = compile_one(vo, GL_FRAGMENT_SHADER, fsrc);
    if (!vs || !fs)
        return -1;
    GLuint prog = glCreateProgram();
    glAttachShader(prog, vs);
    glAttachShader(prog, fs);
    glLinkProgram(prog);
    GLint lk = 0;
    glGetProgramiv(prog, GL_LINK_STATUS, &lk);
    if (!lk) {
        MP_ERR(vo, "santos: program link failed\n");
        return -1;
    }
    p->prog = prog;
    p->a_pos = glGetAttribLocation(prog, "position");
    p->a_tc = glGetAttribLocation(prog, "texcoords");
    p->u_y = glGetUniformLocation(prog, "tY");
    p->u_uv = glGetUniformLocation(prog, "tUV");
    glGenTextures(1, &p->tex_y);
    glGenTextures(1, &p->tex_uv);
    MP_INFO(vo, "santos: preinit OK\n");
    return 0;
}

static int query_format(struct vo *vo, int format) {
    (void)vo;
    return (format == IMGFMT_NV12 || format == IMGFMT_420P) ? 1 : 0;
}

static int reconfig(struct vo *vo, struct mp_image_params *params) {
    struct priv *p = vo->priv;
    int vw = params->w, vh = params->h;
    p->vw = vw; p->vh = vh;
    p->vx = 0; p->vy = 0;
    int dw = p->dw, dh = p->dh;
    int w = dw, h = dw * vh / vw;
    if (h > dh) { h = dh; w = dh * vw / vh; }
    p->vx = (dw - w) / 2; p->vy = (dh - h) / 2;
    p->vw = w; p->vh = h;
    vo->dwidth = dw;
    vo->dheight = dh;
    MP_INFO(vo, "santos: video %dx%d -> viewport %d,%d %dx%d\n", vw, vh, p->vx, p->vy, w, h);
    return 0;
}

static void upload_nv12(struct vo *vo, const unsigned char *y, int y_stride,
                        const unsigned char *uv, int uv_stride, int w, int h) {
    struct priv *p = vo->priv;
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, p->tex_y);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    if (y_stride == w) {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE, w, h, 0, GL_LUMINANCE, GL_UNSIGNED_BYTE, y);
    } else {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE, w, h, 0, GL_LUMINANCE, GL_UNSIGNED_BYTE, NULL);
        for (int r = 0; r < h; r++)
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, r, w, 1, GL_LUMINANCE, GL_UNSIGNED_BYTE, y + r * y_stride);
    }
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, p->tex_uv);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    if (uv_stride == w) {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE_ALPHA, w / 2, h / 2, 0,
                     GL_LUMINANCE_ALPHA, GL_UNSIGNED_BYTE, uv);
    } else {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE_ALPHA, w / 2, h / 2, 0,
                     GL_LUMINANCE_ALPHA, GL_UNSIGNED_BYTE, NULL);
        for (int r = 0; r < h / 2; r++)
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, r, w / 2, 1,
                            GL_LUMINANCE_ALPHA, GL_UNSIGNED_BYTE, uv + r * uv_stride);
    }
    glViewport(p->vx, p->vy, p->vw, p->vh);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glUseProgram(p->prog);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, p->tex_y);
    glUniform1i(p->u_y, 0);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, p->tex_uv);
    glUniform1i(p->u_uv, 1);
    glVertexAttribPointer(p->a_pos, 2, GL_FLOAT, GL_FALSE, 0, qp);
    glVertexAttribPointer(p->a_tc, 2, GL_FLOAT, GL_FALSE, 0, qt);
    glEnableVertexAttribArray(p->a_pos);
    glEnableVertexAttribArray(p->a_tc);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glDisableVertexAttribArray(p->a_pos);
    glDisableVertexAttribArray(p->a_tc);
}

static bool draw_frame(struct vo *vo, struct vo_frame *frame) {
    struct mp_image *img = frame->current;
    if (!img)
        return VO_TRUE;
    if (img->imgfmt == IMGFMT_NV12) {
        upload_nv12(vo, img->planes[0], img->stride[0], img->planes[1], img->stride[1], img->w, img->h);
    } else if (img->imgfmt == IMGFMT_420P) {
        /* interleave U/V to NV12 scratch */
        int w = img->w, h = img->h;
        static unsigned char *scratch;
        static int scratch_sz;
        int need = w * h / 2;
        if (need > scratch_sz) {
            free(scratch);
            scratch = malloc(need);
            scratch_sz = need;
        }
        for (int r = 0; r < h / 2; r++) {
            const unsigned char *u = img->planes[1] + r * img->stride[1];
            const unsigned char *v = img->planes[2] + r * img->stride[2];
            unsigned char *d = scratch + r * w;
            for (int c = 0; c < w / 2; c++) {
                d[2 * c] = u[c];
                d[2 * c + 1] = v[c];
            }
        }
        upload_nv12(vo, img->planes[0], img->stride[0], scratch, w, w, h);
    } else {
        return VO_FALSE;
    }
    return VO_TRUE;
}

static void flip_page(struct vo *vo) {
    struct priv *p = vo->priv;
    eglSwapBuffers(p->dpy, p->surf);
}

static int control(struct vo *vo, uint32_t request, void *data) {
    (void)vo; (void)data;
    switch (request) {
    case VOCTRL_SET_PANSCAN:
        return VO_TRUE;
    }
    return VO_NOTIMPL;
}

static void uninit(struct vo *vo) {
    struct priv *p = vo->priv;
    if (!p)
        return;
    /* r0: never destroy the HWC window (HWCNativeWindowDestroy segfaults);
     * process exit reclaims it. Just unbind GL. */
    if (p->dpy)
        eglMakeCurrent(p->dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    free(p->hwcContents);
}

const struct vo_driver video_out_santos = {
    .description = "PowerVR HWC direct (santos10wifi)",
    .name = "santos",
    .caps = VO_CAP_NORETAIN,
    .preinit = preinit,
    .query_format = query_format,
    .reconfig = reconfig,
    .control = control,
    .draw_frame = draw_frame,
    .flip_page = flip_page,
    .uninit = uninit,
    .priv_size = sizeof(struct priv),
};
