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
 * Rockchip VDPP (Video Display Post Processor) enhancement, scaling and
 * luma histogram through MPP's hwpq interface
 */

#include <rockchip/hwpq_vdpp_proc_api.h>

#include "libavutil/bprint.h"
#include "libavutil/hwcontext.h"
#include "libavutil/hwcontext_rkmpp.h"
#include "libavutil/intreadwrite.h"
#include "libavutil/opt.h"
#include "libavutil/pixdesc.h"

#include "filters.h"
#include "scale_eval.h"
#include "video.h"

/* The counter samples every second column, so the histogram gives the
 * distribution of the frame but not its extremes.
 * The packed histogram holds 16x16 blocks of 16 18-bit bins, then 256
 * 32-bit global bins */
#define VDPP_GLOBAL_HIST_OFFSET (16 * 16 * 16 * 18 / 8)
#define VDPP_GLOBAL_HIST_BINS   256

#define VDPP_VEP_MAX_WIDTH  1920
#define VDPP_VEP_MAX_HEIGHT 2048
#define VDPP_HIST_MAX_SIZE  4096
#define VDPP_MIN_SIZE       128

typedef struct VDPPContext {
    const AVClass *class;

    HwpqVdppContext vdpp;
    int vdpp_ready;
    unsigned frame_idx;
    uint8_t hist_buf[VDPP_HIST_LENGTH];

    char *w_expr, *h_expr;
    enum AVPixelFormat format;
    enum AVPixelFormat in_format;

    int dmsr, dmsr_pri, dmsr_sec, dmsr_damp;
    int es, es_gain;
    int dering;
    int sharp, sharp_gain;
    int shoot, shoot_over, shoot_under;
    int hist, hist_bins, hist_only;
} VDPPContext;

static int vdpp_img_info(AVFilterContext *ctx, const AVFrame *frame,
                         HwpqVdppImgInfo *info, HwpqVdppFormat fmt, int set_fd)
{
    const AVDRMFrameDescriptor *desc = (const AVDRMFrameDescriptor *)frame->data[0];
    const AVDRMLayerDescriptor *layer = &desc->layers[0];
    int pitch, uv_off;

    if (desc->nb_objects != 1 || desc->nb_layers != 1 || layer->nb_planes != 2 ||
        layer->planes[0].offset || layer->planes[1].object_index) {
        av_log(ctx, AV_LOG_ERROR, "Only single object semi-planar frames are supported\n");
        return AVERROR(ENOSYS);
    }
    if (desc->objects[0].format_modifier != DRM_FORMAT_MOD_LINEAR &&
        desc->objects[0].format_modifier != DRM_FORMAT_MOD_INVALID) {
        av_log(ctx, AV_LOG_ERROR, "Compressed (AFBC/RFBC) frames are not supported\n");
        return AVERROR(ENOSYS);
    }

    pitch  = layer->planes[0].pitch;
    uv_off = layer->planes[1].offset;
    if (pitch <= 0 || uv_off < pitch * frame->height) {
        av_log(ctx, AV_LOG_ERROR, "Unsupported frame layout: pitch %d, chroma offset %d\n",
               pitch, uv_off);
        return AVERROR(ENOSYS);
    }

    memset(info, 0, sizeof(*info));
    info->img_fmt = fmt;
    info->img_yrgb.fd    = set_fd ? desc->objects[0].fd : 0;
    info->img_yrgb.w_vld = frame->width;
    info->img_yrgb.h_vld = frame->height;
    info->img_yrgb.w_vir = pitch;
    info->img_yrgb.h_vir = uv_off / pitch;
    info->img_cbcr        = info->img_yrgb;
    info->img_cbcr.offset = uv_off;

    return 0;
}

static HwpqVdppFormat vdpp_format(enum AVPixelFormat fmt)
{
    switch (fmt) {
    case AV_PIX_FMT_NV16: return VDPP_FMT_NV16;
    case AV_PIX_FMT_NV24: return VDPP_FMT_NV24;
    default:              return VDPP_FMT_NV12;
    }
}

static void vdpp_export_hist(VDPPContext *s, AVFrame *frame)
{
    HwpqVdppOutput res = { 0 };
    uint32_t bins[VDPP_GLOBAL_HIST_BINS];
    uint64_t total = 0, sum = 0, acc = 0;
    int ylow = -1, yhigh = -1;
    int i;

    for (i = 0; i < VDPP_GLOBAL_HIST_BINS; i++) {
        bins[i] = AV_RL32(s->hist_buf + VDPP_GLOBAL_HIST_OFFSET + 4 * i);
        total += bins[i];
        sum   += (uint64_t)bins[i] * i;
    }

    av_dict_set_int(&frame->metadata, "lavfi.vdpp.hist_count", total, 0);
    if (hwpq_vdpp_run_cmd(s->vdpp, HWPQ_VDPP_CMD_GET_HIST_RESULT, &res, sizeof(res), NULL) == 0)
        av_dict_set_int(&frame->metadata, "lavfi.vdpp.luma_avg_u10", res.hist.luma_avg, 0);
    if (!total)
        return;

    for (i = 0; i < VDPP_GLOBAL_HIST_BINS; i++) {
        acc += bins[i];
        if (ylow  < 0 && acc >= (total * 10 + 50) / 100)
            ylow = i;
        if (yhigh < 0 && acc >= (total * 90 + 50) / 100)
            yhigh = i;
    }

    av_dict_set_int(&frame->metadata, "lavfi.vdpp.YLOW",  ylow,  0);
    av_dict_set(&frame->metadata, "lavfi.vdpp.YAVG",
                av_asprintf("%g", (double)sum / total), AV_DICT_DONT_STRDUP_VAL);
    av_dict_set_int(&frame->metadata, "lavfi.vdpp.YHIGH", yhigh, 0);

    if (s->hist_bins) {
        AVBPrint bp;
        char *str;

        av_bprint_init(&bp, 0, AV_BPRINT_SIZE_UNLIMITED);
        for (i = 0; i < VDPP_GLOBAL_HIST_BINS; i++)
            av_bprintf(&bp, "%s%u", i ? "," : "", bins[i]);
        if (av_bprint_finalize(&bp, &str) >= 0)
            av_dict_set(&frame->metadata, "lavfi.vdpp.hist", str, AV_DICT_DONT_STRDUP_VAL);
    }
}

static int vdpp_filter_frame(AVFilterLink *inlink, AVFrame *in)
{
    AVFilterContext *ctx = inlink->dst;
    AVFilterLink *outlink = ctx->outputs[0];
    VDPPContext *s = ctx->priv;
    HwpqVdppParams p = { 0 };
    HwpqVdppConfig *cfg = &p.vdpp_config;
    AVFrame *out = NULL;
    int ret;

    ret = vdpp_img_info(ctx, in, &p.src_img_info, vdpp_format(s->in_format), 1);
    if (ret < 0)
        goto fail;

    if (s->hist_only) {
        /* DCI mode does not write the output, fd 0 keeps it unmapped */
        p.dst_img_info = p.src_img_info;
        p.dst_img_info.img_yrgb.fd = p.dst_img_info.img_cbcr.fd = 0;
        p.dst_img_info.img_cbcr.offset = 0;
        p.hist_mode_en = 1;
    } else {
        out = ff_get_video_buffer(outlink, outlink->w, outlink->h);
        if (!out) {
            ret = AVERROR(ENOMEM);
            goto fail;
        }
        ret = vdpp_img_info(ctx, out, &p.dst_img_info, vdpp_format(s->format), 1);
        if (ret < 0)
            goto fail;
    }

    p.frame_idx  = s->frame_idx;
    p.p_hist_buf = s->hist_buf;
    p.vdpp_config_update_flag = !s->frame_idx;

    cfg->dmsr_en              = s->dmsr;
    cfg->str_pri_y            = s->dmsr_pri;
    cfg->str_sec_y            = s->dmsr_sec;
    cfg->dumping_y            = s->dmsr_damp;
    cfg->es_en                = s->es;
    cfg->es_iWgtGain          = s->es_gain;
    cfg->zme_dering_en        = s->dering;
    cfg->hist_cnt_en          = s->hist || s->hist_only;
    /* limited range makes the counter stretch luma to full range first;
     * full range counts the codes as they are, as signalstats does */
    cfg->hist_csc_range       = VDPP_FULL_RANGE;
    cfg->shp_en               = s->sharp;
    cfg->peaking_gain         = s->sharp_gain;
    cfg->shp_shoot_ctrl_en    = s->shoot;
    cfg->shp_shoot_ctrl_over  = s->shoot_over;
    cfg->shp_shoot_ctrl_under = s->shoot_under;

    if (!s->frame_idx) {
        int mode = hwpq_vdpp_check_work_mode(s->vdpp, &p);

        if (s->hist_only ? mode == VDPP_RUN_MODE_UNSUPPORTED : mode != VDPP_RUN_MODE_VEP) {
            av_log(ctx, AV_LOG_ERROR, "VDPP cannot process %dx%d (pitch %d, %d rows) to %dx%d%s\n",
                   in->width, in->height, p.src_img_info.img_yrgb.w_vir,
                   p.src_img_info.img_yrgb.h_vir, outlink->w, outlink->h,
                   s->hist_only ? "" : ", use hist_only or scale first");
            ret = AVERROR(ENOSYS);
            goto fail;
        }
    }

    if (hwpq_vdpp_proc(s->vdpp, &p)) {
        av_log(ctx, AV_LOG_ERROR, "VDPP processing failed\n");
        ret = AVERROR_EXTERNAL;
        goto fail;
    }
    s->frame_idx++;

    if (out) {
        ret = av_frame_copy_props(out, in);
        if (ret < 0)
            goto fail;
        av_frame_free(&in);
    } else {
        out = in;
        in  = NULL;
    }

    if (cfg->hist_cnt_en)
        vdpp_export_hist(s, out);

    return ff_filter_frame(outlink, out);

fail:
    av_frame_free(&in);
    av_frame_free(&out);
    return ret;
}

static int vdpp_config_output(AVFilterLink *outlink)
{
    AVFilterContext *ctx = outlink->src;
    VDPPContext *s = ctx->priv;
    AVFilterLink *inlink = ctx->inputs[0];
    FilterLink *inl = ff_filter_link(inlink);
    FilterLink *outl = ff_filter_link(outlink);
    AVHWFramesContext *in_frames, *out_frames;
    AVBufferRef *device_ref;
    HwpqVdppQueryInfo query = { 0 };
    int w, h, ret;

    if (!inl->hw_frames_ctx) {
        av_log(ctx, AV_LOG_ERROR, "A hardware frames context is required on the input\n");
        return AVERROR(EINVAL);
    }
    in_frames  = (AVHWFramesContext *)inl->hw_frames_ctx->data;
    device_ref = in_frames->device_ref;

    if (in_frames->sw_format != AV_PIX_FMT_NV12 &&
        !(s->hist_only && (in_frames->sw_format == AV_PIX_FMT_NV16 ||
                           in_frames->sw_format == AV_PIX_FMT_NV24))) {
        av_log(ctx, AV_LOG_ERROR, "Unsupported input format: %s\n",
               av_get_pix_fmt_name(in_frames->sw_format));
        return AVERROR(ENOSYS);
    }

    s->in_format = in_frames->sw_format;

    if (!s->vdpp_ready) {
        if (hwpq_vdpp_init(&s->vdpp)) {
            av_log(ctx, AV_LOG_ERROR, "Failed to init VDPP\n");
            return AVERROR(ENODEV);
        }
        s->vdpp_ready = 1;
    }
    if (hwpq_vdpp_run_cmd(s->vdpp, HWPQ_VDPP_CMD_GET_VERSION, &query, sizeof(query), NULL) ||
        query.version.vdpp_ver < 0x200) {
        av_log(ctx, AV_LOG_ERROR, "VDPP version %#x has no histogram or sharpening\n",
               query.version.vdpp_ver);
        return AVERROR(ENOSYS);
    }
    av_log(ctx, AV_LOG_VERBOSE, "VDPP version %#x\n", query.version.vdpp_ver);

    if (s->es && !s->dmsr && !s->hist_only)
        av_log(ctx, AV_LOG_WARNING, "Edge smoothing has no effect without dmsr\n");

    if (s->hist_only) {
        if (inlink->w > VDPP_HIST_MAX_SIZE || inlink->h > VDPP_HIST_MAX_SIZE ||
            inlink->w < VDPP_MIN_SIZE || inlink->h < VDPP_MIN_SIZE) {
            av_log(ctx, AV_LOG_ERROR, "Histogram input must be %d to %d pixels each way\n",
                   VDPP_MIN_SIZE, VDPP_HIST_MAX_SIZE);
            return AVERROR(ENOSYS);
        }
        outlink->w = inlink->w;
        outlink->h = inlink->h;
        av_buffer_unref(&outl->hw_frames_ctx);
        outl->hw_frames_ctx = av_buffer_ref(inl->hw_frames_ctx);
        return outl->hw_frames_ctx ? 0 : AVERROR(ENOMEM);
    }

    ret = ff_scale_eval_dimensions(s, s->w_expr, s->h_expr, inlink, outlink, &w, &h);
    if (ret < 0)
        return ret;
    ret = ff_scale_adjust_dimensions(inlink, &w, &h, 0, 2, 1.0);
    if (ret < 0)
        return ret;
    w &= ~1;
    h &= ~1;

    if (inlink->w > VDPP_VEP_MAX_WIDTH || inlink->h > VDPP_VEP_MAX_HEIGHT ||
        inlink->w < VDPP_MIN_SIZE || inlink->h < VDPP_MIN_SIZE ||
        w > VDPP_VEP_MAX_WIDTH || h > VDPP_VEP_MAX_HEIGHT ||
        w < VDPP_MIN_SIZE || h < VDPP_MIN_SIZE) {
        av_log(ctx, AV_LOG_ERROR, "VDPP takes and makes %dx%d to %dx%d frames, not %dx%d to %dx%d\n",
               VDPP_MIN_SIZE, VDPP_MIN_SIZE, VDPP_VEP_MAX_WIDTH, VDPP_VEP_MAX_HEIGHT,
               inlink->w, inlink->h, w, h);
        return AVERROR(ENOSYS);
    }

    outlink->w = w;
    outlink->h = h;
    if (inlink->sample_aspect_ratio.num)
        outlink->sample_aspect_ratio = av_mul_q((AVRational){ h * inlink->w, w * inlink->h },
                                                inlink->sample_aspect_ratio);
    else
        outlink->sample_aspect_ratio = inlink->sample_aspect_ratio;

    av_buffer_unref(&outl->hw_frames_ctx);
    outl->hw_frames_ctx = av_hwframe_ctx_alloc(device_ref);
    if (!outl->hw_frames_ctx)
        return AVERROR(ENOMEM);

    out_frames = (AVHWFramesContext *)outl->hw_frames_ctx->data;
    out_frames->format    = AV_PIX_FMT_DRM_PRIME;
    out_frames->sw_format = s->format;
    out_frames->width     = w;
    out_frames->height    = h;

    ret = av_hwframe_ctx_init(outl->hw_frames_ctx);
    if (ret < 0)
        return ret;

    av_log(ctx, AV_LOG_VERBOSE, "%dx%d %s -> %dx%d %s\n",
           inlink->w, inlink->h, av_get_pix_fmt_name(in_frames->sw_format),
           w, h, av_get_pix_fmt_name(s->format));

    return 0;
}

static av_cold void vdpp_uninit(AVFilterContext *ctx)
{
    VDPPContext *s = ctx->priv;

    if (s->vdpp_ready)
        hwpq_vdpp_deinit(s->vdpp);
    s->vdpp_ready = 0;
}

#define OFFSET(x) offsetof(VDPPContext, x)
#define FLAGS (AV_OPT_FLAG_FILTERING_PARAM | AV_OPT_FLAG_VIDEO_PARAM)

static const AVOption vdpp_options[] = {
    { "w", "Output video width",  OFFSET(w_expr), AV_OPT_TYPE_STRING, { .str = "iw" }, 0, 0, FLAGS },
    { "h", "Output video height", OFFSET(h_expr), AV_OPT_TYPE_STRING, { .str = "ih" }, 0, 0, FLAGS },
    { "format", "Output pixel format", OFFSET(format), AV_OPT_TYPE_INT, { .i64 = AV_PIX_FMT_NV12 }, 0, INT_MAX, FLAGS, .unit = "format" },
        { "nv12", NULL, 0, AV_OPT_TYPE_CONST, { .i64 = AV_PIX_FMT_NV12 }, 0, 0, FLAGS, .unit = "format" },
        { "nv24", NULL, 0, AV_OPT_TYPE_CONST, { .i64 = AV_PIX_FMT_NV24 }, 0, 0, FLAGS, .unit = "format" },
    { "dmsr",        "Enable de-mosquito, de-ringing and de-shooting", OFFSET(dmsr),      AV_OPT_TYPE_BOOL, { .i64 = 0 },   0, 1,    FLAGS },
    { "dmsr_pri",    "Primary DMSR strength",                       OFFSET(dmsr_pri),  AV_OPT_TYPE_INT,  { .i64 = 10 },  0, 31,   FLAGS },
    { "dmsr_sec",    "Secondary DMSR strength",                     OFFSET(dmsr_sec),  AV_OPT_TYPE_INT,  { .i64 = 4 },   0, 31,   FLAGS },
    { "dmsr_damp",   "DMSR damping",                                OFFSET(dmsr_damp), AV_OPT_TYPE_INT,  { .i64 = 6 },   0, 15,   FLAGS },
    { "es",          "Enable edge smoothing, which acts only with dmsr", OFFSET(es),        AV_OPT_TYPE_BOOL, { .i64 = 0 },   0, 1,    FLAGS },
    { "es_gain",     "Edge smoothing weight gain",                  OFFSET(es_gain),   AV_OPT_TYPE_INT,  { .i64 = 128 }, 1, 255,  FLAGS },
    { "dering",      "Enable scaler de-ringing",                    OFFSET(dering),    AV_OPT_TYPE_BOOL, { .i64 = 1 },   0, 1,    FLAGS },
    { "sharp",       "Enable sharpening",                           OFFSET(sharp),     AV_OPT_TYPE_BOOL, { .i64 = 0 },   0, 1,    FLAGS },
    { "sharp_gain",  "Sharpening peaking gain",                     OFFSET(sharp_gain), AV_OPT_TYPE_INT, { .i64 = 196 }, 0, 1023, FLAGS },
    { "shoot",       "Enable sharpening overshoot control",         OFFSET(shoot),     AV_OPT_TYPE_BOOL, { .i64 = 1 },   0, 1,    FLAGS },
    { "shoot_over",  "Overshoot limit",                             OFFSET(shoot_over),  AV_OPT_TYPE_INT, { .i64 = 8 },  0, 127,  FLAGS },
    { "shoot_under", "Undershoot limit",                            OFFSET(shoot_under), AV_OPT_TYPE_INT, { .i64 = 8 },  0, 127,  FLAGS },
    { "hist",        "Export the input luma histogram as frame metadata", OFFSET(hist), AV_OPT_TYPE_BOOL, { .i64 = 0 }, 0, 1, FLAGS },
    { "hist_bins",   "Also export the 256 histogram bins",          OFFSET(hist_bins), AV_OPT_TYPE_BOOL, { .i64 = 0 },   0, 1,    FLAGS },
    { "hist_only",   "Pass frames through, only export the histogram", OFFSET(hist_only), AV_OPT_TYPE_BOOL, { .i64 = 0 }, 0, 1, FLAGS },
    { NULL },
};

AVFILTER_DEFINE_CLASS(vdpp);

static const AVFilterPad vdpp_inputs[] = {
    {
        .name         = "default",
        .type         = AVMEDIA_TYPE_VIDEO,
        .filter_frame = vdpp_filter_frame,
    },
};

static const AVFilterPad vdpp_outputs[] = {
    {
        .name         = "default",
        .type         = AVMEDIA_TYPE_VIDEO,
        .config_props = vdpp_config_output,
    },
};

const FFFilter ff_vf_vdpp_rkmpp = {
    .p.name         = "vdpp_rkmpp",
    .p.description  = NULL_IF_CONFIG_SMALL("Rockchip VDPP (Video Display Post Processor) enhancement, scaling and histogram"),
    .p.priv_class   = &vdpp_class,
    .p.flags        = AVFILTER_FLAG_HWDEVICE,
    .priv_size      = sizeof(VDPPContext),
    .uninit         = vdpp_uninit,
    FILTER_INPUTS(vdpp_inputs),
    FILTER_OUTPUTS(vdpp_outputs),
    FILTER_SINGLE_PIXFMT(AV_PIX_FMT_DRM_PRIME),
    .flags_internal = FF_FILTER_FLAG_HWFRAME_AWARE,
};
