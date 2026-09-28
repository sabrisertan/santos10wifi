// SPDX-License-Identifier: LGPL-2.1-or-later
// Santos i686 libmix/VDX decoder with NV12 copy-back; explicit opt-in only.
#include <errno.h>
#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <libavcodec/bsf.h>
#include <libavutil/error.h>
#include <hybris/common/binding.h>
#include "mpv_talloc.h"
#include "common/av_common.h"
#include "common/codecs.h"
#include "common/msg.h"
#include "demux/packet.h"
#include "demux/stheader.h"
#include "filters/f_decoder_wrapper.h"
#include "filters/filter_internal.h"
#include "video/mp_image.h"

// Stable i686 prefixes verified against the working tablet vo-player2 ABI.
// Config tail is reserved/zeroed: vendor versions differ in unused native-buffer
// fields. No native graphic buffers or foreign windows cross this interface.
struct vdx_config {
    void *data;
    int32_t size, width, height, surfaces, profile;
    uint32_t flags;
    uint8_t reserved[512];
};
struct vdx_packet {
    void *data;
    int32_t size;
    int64_t pts;
    uint32_t flags, rotation;
    void *ext;
};
struct vdx_raw {
    int32_t width, height, pitch[3], offset[3];
    uint32_t fourcc;
    int32_t size;
    uint8_t *data;
    uint8_t own;
};
struct vdx_render {
    uint32_t surface;
    void *display;
    int32_t scan;
    int64_t pts;
    volatile uint8_t done;
    void *handle;
    int32_t index;
    uint32_t flags;
    volatile uint8_t driver_done;
    struct vdx_raw *raw;
};
_Static_assert(sizeof(void *) == 4, "VDX requires the Santos i686 ABI");
_Static_assert(offsetof(struct vdx_config, flags) == 24, "config ABI");
_Static_assert(sizeof(struct vdx_packet) == 28, "packet ABI");
_Static_assert(offsetof(struct vdx_render, raw) == 40, "render ABI");
_Static_assert(offsetof(struct vdx_raw, data) == 40, "raw ABI");

struct priv {
    struct mp_decoder public;
    struct lavc_state state;
    struct mp_codec_params *codec;
    AVBSFContext *bsf;
    AVPacket *packet;
    void *lib, *decoder;
    void *(*create)(const char *);
    void (*release)(void *);
    int32_t (*start)(void *, struct vdx_config *);
    int32_t (*decode)(void *, struct vdx_packet *);
    struct vdx_render *(*output)(void *, uint8_t, void *);
    void (*stop)(void *);
    void (*flush)(void *);
    bool started, eof, failed;
    unsigned packets, frames, resets;
};

static int fail(struct mp_filter *f, const char *what, int status)
{
    struct priv *p = f->priv;
    MP_ERR(f, "VDX %s failed (%d)\n", what, status);
    p->failed = true;
    mp_filter_internal_mark_failed(f);
    return AVERROR_EXTERNAL;
}

static int send_packet(struct mp_filter *f, struct demux_packet *pkt)
{
    struct priv *p = f->priv;
    if (p->failed)
        return AVERROR_EXTERNAL;
    if (!pkt) {
        // h264_mp4toannexb is a one-packet conversion, with no delayed output.
        p->eof = true;
        return 0;
    }
    if (pkt->len > INT_MAX)
        return fail(f, "packet size", AVERROR_INVALIDDATA);
    av_packet_unref(p->packet);
    int ret = av_new_packet(p->packet, pkt->len);
    if (ret < 0)
        return fail(f, "packet allocation", ret);
    memcpy(p->packet->data, pkt->buffer, pkt->len);
    p->packet->pts = mp_pts_to_av(pkt->pts, NULL);
    p->packet->dts = mp_pts_to_av(pkt->dts, NULL);
    p->packet->flags = pkt->keyframe ? AV_PKT_FLAG_KEY : 0;
    ret = av_bsf_send_packet(p->bsf, p->packet);
    if (ret < 0)
        return fail(f, "bitstream send", ret);
    ret = av_bsf_receive_packet(p->bsf, p->packet);
    if (ret < 0)
        return fail(f, "bitstream receive", ret);
    struct vdx_packet input = {
        .data = p->packet->data, .size = p->packet->size,
        .pts = p->packet->pts, .flags = 0x02, // HAS_COMPLETE_FRAME
    };
    ret = p->decode(p->decoder, &input);
    if (ret == 2) // format change requests resubmission, as in vo-player2
        ret = p->decode(p->decoder, &input);
    av_packet_unref(p->packet);
    if (ret != 1)
        return fail(f, "decode", ret);
    p->packets++;
    return 0;
}

static bool plane_valid(const struct vdx_raw *r, int plane, int rows, int bytes)
{
    if (r->offset[plane] < 0 || r->pitch[plane] < bytes || rows <= 0)
        return false;
    uint64_t end = (uint64_t)r->offset[plane] +
                   (uint64_t)(rows - 1) * r->pitch[plane] + bytes;
    return r->size > 0 && end <= (uint64_t)r->size;
}

static int receive_frame(struct mp_filter *f, struct mp_frame *out)
{
    struct priv *p = f->priv;
    if (p->failed)
        return AVERROR_EOF;
    struct vdx_render *rb = p->output(p->decoder, p->eof, NULL);
    if (!rb)
        return p->eof ? AVERROR_EOF : AVERROR(EAGAIN);
    if (rb->scan != 0) { // VA_FRAME_PICTURE; field order is not exposed by this ABI.
        rb->done = 1;
        return fail(f, "interlaced output is not supported", rb->scan);
    }
    struct vdx_raw *r = rb->raw;
    if (!r || !r->data || r->width <= 0 || r->height <= 0 ||
        r->width > 1920 || r->height > 1088 || (r->width & 1) || (r->height & 1) ||
        (r->fourcc != 0x3231564e && r->fourcc != 0x4e563132) ||
        !plane_valid(r, 0, r->height, r->width) ||
        !plane_valid(r, 1, r->height / 2, r->width)) {
        rb->done = 1;
        return fail(f, "raw NV12 layout", AVERROR_INVALIDDATA);
    }
    struct mp_image *img = mp_image_alloc(IMGFMT_NV12, r->width, r->height);
    if (!img) {
        rb->done = 1;
        return fail(f, "image allocation", AVERROR(ENOMEM));
    }
    for (int plane = 0; plane < 2; plane++) {
        int rows = plane ? r->height / 2 : r->height;
        for (int y = 0; y < rows; y++)
            memcpy(img->planes[plane] + y * img->stride[plane],
                   r->data + r->offset[plane] + (size_t)y * r->pitch[plane], r->width);
    }
    img->pts = mp_pts_from_av(rb->pts, NULL);
    img->params.p_w = p->codec->par_w;
    img->params.p_h = p->codec->par_h;
    img->params.color = p->codec->color;
    img->params.repr = p->codec->repr;
    img->params.chroma_location = p->codec->chroma_location;
    img->params.rotate = p->codec->rotate;
    img->params.stereo3d = p->codec->stereo_mode;
    // rawData already applies decoder cropping; do not crop the copy twice.
    mp_image_params_guess_csp(&img->params);
    rb->done = 1; // copy owns its pixels before returning vendor surface
    if (++p->frames <= 3 || p->frames % 120 == 0)
        MP_INFO(f, "VDX frame=%u pts=%.6f %dx%d packets=%u\n",
                p->frames, img->pts, img->w, img->h, p->packets);
    *out = MAKE_FRAME(MP_FRAME_VIDEO, img);
    return 0;
}

static int control(struct mp_filter *f, enum dec_ctrl cmd, void *arg)
{
    struct priv *p = f->priv;
    if (cmd == VDCTRL_GET_HWDEC) {
        *(char **)arg = p->frames ? "santos-vdx-copy" : NULL;
        return CONTROL_TRUE;
    }
    return CONTROL_UNKNOWN;
}
static void process(struct mp_filter *f)
{
    struct priv *p = f->priv;
    lavc_process(f, &p->state, send_packet, receive_frame);
}
static void reset(struct mp_filter *f)
{
    struct priv *p = f->priv;
    if (p->started) p->flush(p->decoder);
    if (p->bsf) av_bsf_flush(p->bsf);
    if (p->packet) av_packet_unref(p->packet);
    p->state = (struct lavc_state){0};
    p->eof = false;
    // Fatal decoder errors are not cleared by seeking.
    MP_VERBOSE(f, "VDX flush/reset %u\n", ++p->resets);
}
static void destroy(struct mp_filter *f)
{
    struct priv *p = f->priv;
    MP_INFO(f, "VDX close packets=%u frames=%u resets=%u\n", p->packets, p->frames, p->resets);
    if (p->started) p->stop(p->decoder);
    if (p->decoder) p->release(p->decoder);
    av_packet_free(&p->packet);
    av_bsf_free(&p->bsf);
    if (p->lib) android_dlclose(p->lib);
}
static const struct mp_filter_info filter = {
    .name = "vd_santos_vdx", .priv_size = sizeof(struct priv),
    .process = process, .reset = reset, .destroy = destroy,
};
static struct mp_decoder *create(struct mp_filter *parent,
                                 struct mp_codec_params *codec, const char *name)
{
    if (strcmp(name, "santos_vdx") || !codec->codec || strcmp(codec->codec, "h264"))
        return NULL;
    struct mp_filter *f = mp_filter_create(parent, &filter);
    if (!f) return NULL;
    struct priv *p = f->priv;
    p->codec = codec;
    p->public = (struct mp_decoder){.f = f, .control = control};
    mp_filter_add_pin(f, MP_PIN_IN, "in");
    mp_filter_add_pin(f, MP_PIN_OUT, "out");
    f->log = mp_log_new(f, parent->log, "santos-vdx");
    AVCodecParameters *params = mp_codec_params_to_av(codec);
    if (!params) goto error;
    // Do not guess from missing metadata: a high-bit-depth or interlaced stream
    // can otherwise initialize successfully but fail only after packet input.
    // The device launcher requests lavf stream probing before decoder selection.
    bool supported = params->width > 0 && params->height > 0 &&
        params->width <= 1920 && params->height <= 1088 &&
        params->bits_per_raw_sample <= 8 &&
        params->field_order == AV_FIELD_PROGRESSIVE &&
        (params->profile == AV_PROFILE_H264_BASELINE ||
         params->profile == AV_PROFILE_H264_CONSTRAINED_BASELINE ||
         params->profile == AV_PROFILE_H264_MAIN || params->profile == AV_PROFILE_H264_HIGH);
    const AVBitStreamFilter *bsf = av_bsf_get_by_name("h264_mp4toannexb");
    if (!bsf) {
        avcodec_parameters_free(&params);
        goto error;
    }
    int ret = av_bsf_alloc(bsf, &p->bsf);
    if (ret >= 0) ret = avcodec_parameters_copy(p->bsf->par_in, params);
    avcodec_parameters_free(&params);
    if (!supported || ret < 0) goto error;
    p->bsf->time_base_in = AV_TIME_BASE_Q;
    if (av_bsf_init(p->bsf) < 0) goto error;
    p->packet = av_packet_alloc();
    if (!p->packet) goto error;
    p->lib = android_dlopen("/system/lib/libva_videodecoder.so", 2);
    if (!p->lib) goto error;
#define SYM(field, symbol) do { \
    p->field = android_dlsym(p->lib, symbol); \
    if (!p->field) { MP_ERR(f, "Missing VDX symbol %s\n", symbol); goto error; } \
} while (0)
    SYM(create, "_Z18createVideoDecoderPKc");
    SYM(release, "_Z19releaseVideoDecoderP13IVideoDecoder");
    SYM(start, "_ZN15VideoDecoderAVC5startEP17VideoConfigBuffer");
    SYM(decode, "_ZN15VideoDecoderAVC6decodeEP17VideoDecodeBuffer");
    SYM(output, "_ZN16VideoDecoderBase9getOutputEbP16VideoErrorBuffer");
    SYM(stop, "_ZN15VideoDecoderAVC4stopEv");
    SYM(flush, "_ZN15VideoDecoderAVC5flushEv");
#undef SYM
    p->decoder = p->create("video/avc");
    if (!p->decoder) goto error;
    struct vdx_config config = {.flags = 0x40}; // WANT_RAW_OUTPUT, retain B-frame ordering
    if (p->start(p->decoder, &config) != 1) goto error;
    p->started = true;
    MP_INFO(f, "Using VDX hardware H.264 decoding (NV12 copy-back).\n");
    return &p->public;
error:
    MP_ERR(f, "VDX initialization failed or H.264 profile unsupported.\n");
    talloc_free(f);
    return NULL;
}
static void add_decoders(struct mp_decoder_list *list)
{
    mp_add_decoder(list, "h264", "santos_vdx", "Santos VDX H.264 hardware decoder (NV12 copy)");
}
const struct mp_decoder_fns vd_santos_vdx = {.create = create, .add_decoders = add_decoders};
