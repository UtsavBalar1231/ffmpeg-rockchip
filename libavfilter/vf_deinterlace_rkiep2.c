/*
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

/**
 * @file
 * Rockchip IEP2 (Image Enhancement Processor) motion adaptive deinterlacer
 */

#include <rockchip/iep2_api.h>

#include "libavutil/hwcontext.h"
#include "libavutil/hwcontext_rkmpp.h"
#include "libavutil/mem.h"
#include "libavutil/pixdesc.h"

#include "filters.h"
#include "yadif.h"

#define IEP2_MAX_WIDTH  1920
#define IEP2_MAX_HEIGHT 1080

typedef struct RKIEP2Pool {
    MppBufferGroup       group;
    AVDRMFrameDescriptor layout;
    int                  ready;
} RKIEP2Pool;

typedef struct RKIEP2Context {
    YADIFContext yadif;

    iep_com_ctx *iep;
    int          iep_ready;

    RKIEP2Pool  *pool;
    AVBufferRef *in_frames_ref;
    enum IEP2_FMT fmt;

    AVFrame *second;
    int      second_parity;
    int      err;
} RKIEP2Context;

static void rkiep2_free_desc(void *opaque, uint8_t *data)
{
    mpp_buffer_put((MppBuffer)opaque);
    av_free(data);
}

static AVBufferRef *rkiep2_pool_alloc(void *opaque, size_t size)
{
    RKIEP2Pool *p = opaque;
    AVRKMPPDRMFrameDescriptor *desc;
    AVBufferRef *ref;
    MppBuffer buf = NULL;

    if (!p->ready)
        return NULL;

    desc = av_mallocz(sizeof(*desc));
    if (!desc)
        return NULL;

    if (mpp_buffer_get(p->group, &buf, p->layout.objects[0].size) != MPP_OK || !buf) {
        av_free(desc);
        return NULL;
    }

    desc->drm_desc = p->layout;
    desc->drm_desc.objects[0].fd   = mpp_buffer_get_fd(buf);
    desc->drm_desc.objects[0].size = mpp_buffer_get_size(buf);
    desc->buffers[0] = buf;

    ref = av_buffer_create((uint8_t *)desc, sizeof(*desc), rkiep2_free_desc, buf, 0);
    if (!ref)
        rkiep2_free_desc(buf, (uint8_t *)desc);

    return ref;
}

static void rkiep2_pool_free(void *opaque)
{
    RKIEP2Pool *p = opaque;

    if (p->group)
        mpp_buffer_group_put(p->group);
    av_free(p);
}

static void rkiep2_frames_free(AVHWFramesContext *hwfc)
{
    av_buffer_pool_uninit(&hwfc->pool);
}

typedef struct RKIEP2InputRef {
    AVBufferRef *buf;
    AVBufferRef *frames;
} RKIEP2InputRef;

static void rkiep2_free_input_ref(void *opaque, uint8_t *data)
{
    RKIEP2InputRef *r = opaque;

    av_buffer_unref(&r->buf);
    av_buffer_unref(&r->frames);
    av_free(r);
}

/* yadif_common passes progressive and disabled frames through as they are,
 * so every input frame is moved to the output frames context, which shares
 * its layout; the frame keeps its source context alive for its buffer */
static int rkiep2_adopt_frame(AVFilterContext *ctx, AVFrame *frame)
{
    FilterLink *l = ff_filter_link(ctx->outputs[0]);
    RKIEP2InputRef *r;

    if (frame->hw_frames_ctx->data == l->hw_frames_ctx->data)
        return 0;

    r = av_mallocz(sizeof(*r));
    if (!r)
        return AVERROR(ENOMEM);
    r->buf    = frame->buf[0];
    r->frames = frame->hw_frames_ctx;

    frame->buf[0] = av_buffer_create(frame->data[0], r->buf->size,
                                     rkiep2_free_input_ref, r, AV_BUFFER_FLAG_READONLY);
    if (!frame->buf[0]) {
        frame->buf[0] = r->buf;
        av_free(r);
        return AVERROR(ENOMEM);
    }

    frame->hw_frames_ctx = av_buffer_ref(l->hw_frames_ctx);
    return frame->hw_frames_ctx ? 0 : AVERROR(ENOMEM);
}

/* IEP2 takes one luma pitch for all inputs and outputs, so the output
 * frames copy the layout of the input frames */
static int rkiep2_check_frame(AVFilterContext *ctx, const AVFrame *frame)
{
    RKIEP2Context *s = ctx->priv;
    const AVDRMFrameDescriptor *desc = (const AVDRMFrameDescriptor *)frame->data[0];
    const AVDRMLayerDescriptor *layer;
    int pitch, h4, chroma_h;
    ptrdiff_t uv_off;

    if (!desc || !frame->hw_frames_ctx)
        return AVERROR(EINVAL);

    layer = &desc->layers[0];
    if (desc->nb_objects != 1 || desc->nb_layers != 1 || layer->nb_planes != 2 ||
        layer->planes[0].object_index || layer->planes[1].object_index) {
        av_log(ctx, AV_LOG_ERROR, "Only single object semi-planar input is supported\n");
        return AVERROR(ENOSYS);
    }
    if (desc->objects[0].format_modifier != DRM_FORMAT_MOD_LINEAR &&
        desc->objects[0].format_modifier != DRM_FORMAT_MOD_INVALID) {
        av_log(ctx, AV_LOG_ERROR, "Compressed (AFBC/RFBC) input is not supported\n");
        return AVERROR(ENOSYS);
    }

    pitch    = layer->planes[0].pitch;
    uv_off   = layer->planes[1].offset;
    h4       = FFALIGN(frame->height, 4);
    chroma_h = s->fmt == IEP2_FMT_YUV420 ? h4 / 2 : h4;
    if (layer->planes[0].offset || layer->planes[1].pitch != pitch ||
        pitch % 16 || pitch < FFALIGN(frame->width, 16) ||
        uv_off < (ptrdiff_t)pitch * h4 ||
        desc->objects[0].size < uv_off + (size_t)pitch * chroma_h) {
        av_log(ctx, AV_LOG_ERROR, "Unsupported input layout: pitch %d, chroma offset %td, size %zu\n",
               pitch, uv_off, desc->objects[0].size);
        return AVERROR(ENOSYS);
    }
    /* MPP passes buffers to the kernel as fd | offset << 10 */
    if (desc->objects[0].fd < 0 || desc->objects[0].fd >= 1024 || uv_off >= (1 << 22)) {
        av_log(ctx, AV_LOG_ERROR, "Buffer fd %d or chroma offset %td out of range\n",
               desc->objects[0].fd, uv_off);
        return AVERROR(ENOSYS);
    }

    if (!s->pool->ready) {
        AVDRMFrameDescriptor *l = &s->pool->layout;

        *l = *desc;
        l->objects[0].fd = -1;
        l->objects[0].format_modifier = DRM_FORMAT_MOD_LINEAR;
        s->pool->ready = 1;
    } else if (pitch != s->pool->layout.layers[0].planes[0].pitch ||
               uv_off != s->pool->layout.layers[0].planes[1].offset) {
        av_log(ctx, AV_LOG_ERROR, "Input frame layout changed\n");
        return AVERROR(ENOSYS);
    }

    return 0;
}

static void rkiep2_set_img(IepImg *img, const AVFrame *frame)
{
    const AVDRMFrameDescriptor *desc = (const AVDRMFrameDescriptor *)frame->data[0];
    uint32_t fd  = desc->objects[0].fd;
    uint32_t off = desc->layers[0].planes[1].offset;

    memset(img, 0, sizeof(*img));
    img->act_w    = frame->width;
    img->act_h    = frame->height;
    img->vir_w    = desc->layers[0].planes[0].pitch;
    img->vir_h    = off / img->vir_w;
    img->format   = IEP_FORMAT_YCbCr_420_SP;
    img->mem_addr = fd;
    img->uv_addr  = fd | (off << 10);
    img->v_addr   = img->uv_addr;
}

static int rkiep2_run(AVFilterContext *ctx, AVFrame *top, AVFrame *bot, int tff)
{
    RKIEP2Context *s = ctx->priv;
    YADIFContext *y = &s->yadif;
    iep_com_ops *ops = s->iep->ops;
    struct iep2_api_params params = { 0 };
    struct iep2_api_info info = { 0 };
    const AVDRMFrameDescriptor *desc = (const AVDRMFrameDescriptor *)y->cur->data[0];
    IepImg img;
    MPP_RET ret;

    params.ptype = IEP2_PARAM_TYPE_MODE;
    params.param.mode.dil_mode  = IEP2_DIL_MODE_I5O2;
    params.param.mode.out_mode  = IEP2_OUT_MODE_LINE;
    params.param.mode.dil_order = tff ? IEP2_FIELD_ORDER_TFF : IEP2_FIELD_ORDER_BFF;
    ret = ops->control(s->iep->priv, IEP_CMD_SET_DEI_CFG, &params);
    if (ret != MPP_OK)
        goto fail;

    memset(&params, 0, sizeof(params));
    params.ptype = IEP2_PARAM_TYPE_COM;
    params.param.com.sfmt       = s->fmt;
    params.param.com.dfmt       = s->fmt;
    params.param.com.sswap      = IEP2_YUV_SWAP_SP_UV;
    params.param.com.dswap      = IEP2_YUV_SWAP_SP_UV;
    params.param.com.width      = y->cur->width;
    params.param.com.height     = y->cur->height;
    params.param.com.hor_stride = desc->layers[0].planes[0].pitch;
    ret = ops->control(s->iep->priv, IEP_CMD_SET_DEI_CFG, &params);
    if (ret != MPP_OK)
        goto fail;

    rkiep2_set_img(&img, y->cur);
    ops->control(s->iep->priv, IEP_CMD_SET_SRC, &img);
    rkiep2_set_img(&img, y->next);
    ops->control(s->iep->priv, IEP_CMD_SET_DEI_SRC1, &img);
    rkiep2_set_img(&img, y->prev);
    ops->control(s->iep->priv, IEP_CMD_SET_DEI_SRC2, &img);
    rkiep2_set_img(&img, top);
    ops->control(s->iep->priv, IEP_CMD_SET_DST, &img);
    rkiep2_set_img(&img, bot);
    ops->control(s->iep->priv, IEP_CMD_SET_DEI_DST1, &img);

    ret = ops->control(s->iep->priv, IEP_CMD_RUN_SYNC, &info);
    if (ret != MPP_OK)
        goto fail;

    if (info.dil_order != IEP2_FIELD_ORDER_UND &&
        (info.dil_order == IEP2_FIELD_ORDER_TFF) != tff)
        av_log(ctx, AV_LOG_DEBUG, "IEP2 detects %s, confidence %u\n",
               info.dil_order == IEP2_FIELD_ORDER_TFF ? "tff" : "bff",
               info.dil_order_confidence_ratio);

    return 0;

fail:
    av_log(ctx, AV_LOG_ERROR, "IEP2 deinterlace failed: %d\n", ret);
    return AVERROR_EXTERNAL;
}

static void rkiep2_filter(AVFilterContext *ctx, AVFrame *dst, int parity, int tff)
{
    RKIEP2Context *s = ctx->priv;
    YADIFContext *y = &s->yadif;
    AVFrame *other;
    int ret;

    if (s->err < 0)
        return;

    /* the second field of a frame was rendered with the first one */
    if (s->second) {
        if (s->second_parity == parity) {
            av_buffer_unref(&dst->buf[0]);
            dst->buf[0]  = s->second->buf[0];
            dst->data[0] = s->second->data[0];
            s->second->buf[0] = NULL;
        }
        av_frame_free(&s->second);
        if (s->second_parity == parity)
            return;
    }

    other = av_frame_alloc();
    if (!other) {
        s->err = AVERROR(ENOMEM);
        return;
    }
    ret = av_hwframe_get_buffer(dst->hw_frames_ctx, other, 0);
    if (ret < 0) {
        av_frame_free(&other);
        s->err = ret;
        return;
    }

    /* parity 0 keeps the top field */
    ret = parity ? rkiep2_run(ctx, other, dst, tff) : rkiep2_run(ctx, dst, other, tff);
    if (ret < 0) {
        av_frame_free(&other);
        s->err = ret;
        return;
    }

    if (y->mode & 1) {
        s->second = other;
        s->second_parity = !parity;
    } else {
        av_frame_free(&other);
    }

    if (y->current_field == YADIF_FIELD_END)
        y->current_field = YADIF_FIELD_NORMAL;
}

static int rkiep2_filter_frame(AVFilterLink *inlink, AVFrame *frame)
{
    AVFilterContext *ctx = inlink->dst;
    RKIEP2Context *s = ctx->priv;
    int ret;

    if (s->err < 0) {
        av_frame_free(&frame);
        return s->err;
    }

    ret = rkiep2_check_frame(ctx, frame);
    if (ret >= 0)
        ret = rkiep2_adopt_frame(ctx, frame);
    if (ret < 0) {
        av_frame_free(&frame);
        return ret;
    }

    ret = ff_yadif_filter_frame(inlink, frame);
    return s->err < 0 ? s->err : ret;
}

static int rkiep2_request_frame(AVFilterLink *outlink)
{
    RKIEP2Context *s = outlink->src->priv;
    int ret = ff_yadif_request_frame(outlink);

    return s->err < 0 ? s->err : ret;
}

static int rkiep2_config_input(AVFilterLink *inlink)
{
    AVFilterContext *ctx = inlink->dst;
    RKIEP2Context *s = ctx->priv;
    FilterLink *l = ff_filter_link(inlink);
    AVHWFramesContext *in_frames;

    if (!l->hw_frames_ctx) {
        av_log(ctx, AV_LOG_ERROR, "A hardware frames context is required on the input\n");
        return AVERROR(EINVAL);
    }

    in_frames = (AVHWFramesContext *)l->hw_frames_ctx->data;
    switch (in_frames->sw_format) {
    case AV_PIX_FMT_NV12: s->fmt = IEP2_FMT_YUV420; break;
    case AV_PIX_FMT_NV16: s->fmt = IEP2_FMT_YUV422; break;
    default:
        av_log(ctx, AV_LOG_ERROR, "Unsupported input format: %s\n",
               av_get_pix_fmt_name(in_frames->sw_format));
        return AVERROR(ENOSYS);
    }

    if (inlink->w > IEP2_MAX_WIDTH || inlink->h > IEP2_MAX_HEIGHT) {
        av_log(ctx, AV_LOG_ERROR, "Input size %dx%d exceeds %dx%d\n",
               inlink->w, inlink->h, IEP2_MAX_WIDTH, IEP2_MAX_HEIGHT);
        return AVERROR(ENOSYS);
    }

    av_buffer_unref(&s->in_frames_ref);
    s->in_frames_ref = av_buffer_ref(l->hw_frames_ctx);
    return s->in_frames_ref ? 0 : AVERROR(ENOMEM);
}

static int rkiep2_init_iep(AVFilterContext *ctx)
{
    RKIEP2Context *s = ctx->priv;

    if (s->iep_ready)
        return 0;

    s->iep = get_iep_ctx();
    if (!s->iep) {
        av_log(ctx, AV_LOG_ERROR, "No IEP device found\n");
        return AVERROR(ENODEV);
    }
    if (s->iep->ver != 2) {
        av_log(ctx, AV_LOG_ERROR, "IEP version %d is not IEP2\n", s->iep->ver);
        return AVERROR(ENODEV);
    }
    if (s->iep->ops->init(&s->iep->priv) != MPP_OK) {
        av_log(ctx, AV_LOG_ERROR, "Failed to init IEP2\n");
        return AVERROR_EXTERNAL;
    }
    s->iep_ready = 1;

    return 0;
}

static int rkiep2_config_output(AVFilterLink *outlink)
{
    AVFilterContext *ctx = outlink->src;
    RKIEP2Context *s = ctx->priv;
    YADIFContext *y = &s->yadif;
    FilterLink *l = ff_filter_link(outlink);
    AVHWFramesContext *in_frames = (AVHWFramesContext *)s->in_frames_ref->data;
    AVBufferRef *device_ref = in_frames->device_ref;
    AVHWFramesContext *out_frames;
    AVRKMPPDeviceContext *dev_hwctx;
    RKIEP2Pool *pool;
    int ret;

    if (in_frames->device_ctx->type != AV_HWDEVICE_TYPE_RKMPP) {
        if (ctx->hw_device_ctx &&
            ((AVHWDeviceContext *)ctx->hw_device_ctx->data)->type == AV_HWDEVICE_TYPE_RKMPP) {
            device_ref = ctx->hw_device_ctx;
        } else {
            av_log(ctx, AV_LOG_ERROR, "No RKMPP hardware context provided\n");
            return AVERROR(EINVAL);
        }
    }
    dev_hwctx = ((AVHWDeviceContext *)device_ref->data)->hwctx;

    ret = rkiep2_init_iep(ctx);
    if (ret < 0)
        return ret;

    av_buffer_unref(&l->hw_frames_ctx);
    l->hw_frames_ctx = av_hwframe_ctx_alloc(device_ref);
    if (!l->hw_frames_ctx)
        return AVERROR(ENOMEM);

    out_frames = (AVHWFramesContext *)l->hw_frames_ctx->data;
    out_frames->format    = AV_PIX_FMT_DRM_PRIME;
    out_frames->sw_format = in_frames->sw_format;
    out_frames->width     = ctx->inputs[0]->w;
    out_frames->height    = ctx->inputs[0]->h;

    pool = av_mallocz(sizeof(*pool));
    if (!pool)
        return AVERROR(ENOMEM);
    if (mpp_buffer_group_get_internal(&pool->group,
                                      MPP_BUFFER_TYPE_DRM | dev_hwctx->flags) != MPP_OK) {
        av_free(pool);
        return AVERROR_EXTERNAL;
    }
    out_frames->pool = av_buffer_pool_init2(sizeof(AVRKMPPDRMFrameDescriptor), pool,
                                            rkiep2_pool_alloc, rkiep2_pool_free);
    if (!out_frames->pool) {
        rkiep2_pool_free(pool);
        return AVERROR(ENOMEM);
    }
    out_frames->free = rkiep2_frames_free;
    s->pool = pool;

    ret = av_hwframe_ctx_init(l->hw_frames_ctx);
    if (ret < 0)
        return ret;

    ret = ff_yadif_config_output_common(outlink);
    if (ret < 0)
        return ret;

    y->csp    = av_pix_fmt_desc_get(in_frames->sw_format);
    y->filter = rkiep2_filter;

    return 0;
}

static av_cold void rkiep2_uninit(AVFilterContext *ctx)
{
    RKIEP2Context *s = ctx->priv;

    av_frame_free(&s->second);
    ff_yadif_uninit(ctx);
    av_buffer_unref(&s->in_frames_ref);

    if (s->iep) {
        if (s->iep_ready)
            s->iep->ops->deinit(s->iep->priv);
        put_iep_ctx(s->iep);
        s->iep = NULL;
    }
}

#define OFFSET(x) offsetof(YADIFContext, x)
#define FLAGS (AV_OPT_FLAG_VIDEO_PARAM | AV_OPT_FLAG_FILTERING_PARAM)

#define CONST(name, help, val, u) { name, help, 0, AV_OPT_TYPE_CONST, { .i64 = val }, INT_MIN, INT_MAX, FLAGS, .unit = u }

static const AVOption rkiep2_options[] = {
    { "mode", "specify the interlacing mode", OFFSET(mode), AV_OPT_TYPE_INT, { .i64 = YADIF_MODE_SEND_FRAME }, 0, 1, FLAGS, .unit = "mode" },
    CONST("send_frame", "send one frame for each frame", YADIF_MODE_SEND_FRAME, "mode"),
    CONST("send_field", "send one frame for each field", YADIF_MODE_SEND_FIELD, "mode"),

    { "parity", "specify the assumed picture field parity", OFFSET(parity), AV_OPT_TYPE_INT, { .i64 = YADIF_PARITY_AUTO }, -1, 1, FLAGS, .unit = "parity" },
    CONST("tff",  "assume top field first",    YADIF_PARITY_TFF,  "parity"),
    CONST("bff",  "assume bottom field first", YADIF_PARITY_BFF,  "parity"),
    CONST("auto", "auto detect parity",        YADIF_PARITY_AUTO, "parity"),

    { "deint", "specify which frames to deinterlace", OFFSET(deint), AV_OPT_TYPE_INT, { .i64 = YADIF_DEINT_ALL }, 0, 1, FLAGS, .unit = "deint" },
    CONST("all",        "deinterlace all frames",                       YADIF_DEINT_ALL,        "deint"),
    CONST("interlaced", "only deinterlace frames marked as interlaced", YADIF_DEINT_INTERLACED, "deint"),

    { NULL }
};

AVFILTER_DEFINE_CLASS(rkiep2);

static const AVFilterPad rkiep2_inputs[] = {
    {
        .name         = "default",
        .type         = AVMEDIA_TYPE_VIDEO,
        .filter_frame = rkiep2_filter_frame,
        .config_props = rkiep2_config_input,
    },
};

static const AVFilterPad rkiep2_outputs[] = {
    {
        .name          = "default",
        .type          = AVMEDIA_TYPE_VIDEO,
        .request_frame = rkiep2_request_frame,
        .config_props  = rkiep2_config_output,
    },
};

const FFFilter ff_vf_deinterlace_rkiep2 = {
    .p.name         = "deinterlace_rkiep2",
    .p.description  = NULL_IF_CONFIG_SMALL("Rockchip IEP2 (Image Enhancement Processor) deinterlacer"),
    .p.priv_class   = &rkiep2_class,
    .p.flags        = AVFILTER_FLAG_HWDEVICE | AVFILTER_FLAG_SUPPORT_TIMELINE_INTERNAL,
    .priv_size      = sizeof(RKIEP2Context),
    .uninit         = rkiep2_uninit,
    FILTER_INPUTS(rkiep2_inputs),
    FILTER_OUTPUTS(rkiep2_outputs),
    FILTER_SINGLE_PIXFMT(AV_PIX_FMT_DRM_PRIME),
    .flags_internal = FF_FILTER_FLAG_HWFRAME_AWARE,
};
