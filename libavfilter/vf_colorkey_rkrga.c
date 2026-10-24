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
 * Rockchip RGA (2D Raster Graphic Acceleration) color key
 */

#include <math.h>

#include "libavutil/internal.h"
#include "libavutil/opt.h"
#include "libavutil/pixdesc.h"

#include "filters.h"

#include "rkrga_common.h"

typedef struct RGAColorkeyContext {
    RKRGAContext rga;

    uint8_t colorkey_rgba[4];
    float similarity;

    RKRGAJob jobs[2];
} RGAColorkeyContext;

static int configure_key(AVFilterContext *ctx)
{
    RGAColorkeyContext *s = ctx->priv;
    const RGAFrameInfo *out = &s->rga.out_rga_frame_info;
    const im_rect rect = { 0, 0, out->act_w, out->act_h };
    /* The similarity a pixel has when one channel alone differs by d */
    const int d = s->similarity * 255.0 * sqrt(3.0);
    unsigned min = 0, max = 0xffu << 24;
    int i, ret;

    for (i = 0; i < 3; i++) {
        min |= av_clip_uint8(s->colorkey_rgba[i] - d) << (8 * i);
        max |= av_clip_uint8(s->colorkey_rgba[i] + d) << (8 * i);
    }

    /* Clear the output, then blend the input over it with the key range made transparent */
    s->jobs[0] = (RKRGAJob){
        .usage = IM_COLOR_FILL,
        .src   = RKRGA_JOB_SRC_NONE,
        .rect  = rect,
        .color = 0,
    };
    s->jobs[1] = (RKRGAJob){
        .usage          = IM_ALPHA_COLORKEY_NORMAL,
        .src            = RKRGA_JOB_SRC_MAIN,
        .rect           = rect,
        .colorkey_range = { .max = max, .min = min },
    };

    for (i = 0; i < FF_ARRAY_ELEMS(s->jobs); i++) {
        if ((ret = ff_rkrga_check_job(ctx, &s->jobs[i])) < 0)
            return ret;
    }

    av_log(ctx, AV_LOG_VERBOSE, "key range 0x%08X-0x%08X\n", min, max);

    return 0;
}

static av_cold int rgacolorkey_config_props(AVFilterLink *outlink)
{
    AVFilterContext *ctx = outlink->src;
    AVFilterLink *inlink = ctx->inputs[0];
    FilterLink      *inl = ff_filter_link(inlink);
    AVHWFramesContext *in_frames_ctx;
    RKRGAParam param = { NULL };
    int ret;

    if (!inl->hw_frames_ctx) {
        av_log(ctx, AV_LOG_ERROR, "No hw context provided on input\n");
        return AVERROR(EINVAL);
    }
    in_frames_ctx = (AVHWFramesContext *)inl->hw_frames_ctx->data;

    switch (in_frames_ctx->sw_format) {
    case AV_PIX_FMT_RGBA:
    case AV_PIX_FMT_BGRA:
    case AV_PIX_FMT_ARGB:
    case AV_PIX_FMT_ABGR:
        break;
    default:
        av_log(ctx, AV_LOG_ERROR, "Unsupported input format '%s', the key needs RGB with alpha\n",
               av_get_pix_fmt_name(in_frames_ctx->sw_format));
        return AVERROR(ENOSYS);
    }

    outlink->w = inlink->w;
    outlink->h = inlink->h;
    outlink->sample_aspect_ratio = inlink->sample_aspect_ratio;

    param.out_sw_format = in_frames_ctx->sw_format;
    param.jobs          = 1;

    ret = ff_rkrga_init(ctx, &param);
    if (ret < 0)
        return ret;

    return configure_key(ctx);
}

static int rgacolorkey_filter_frame(AVFilterContext *ctx, AVFrame *in)
{
    RGAColorkeyContext *s = ctx->priv;
    int ret;

    ret = ff_rkrga_filter_frame_jobs(&s->rga, ctx->inputs[0], in, s->jobs, FF_ARRAY_ELEMS(s->jobs), 0);
    av_frame_free(&in);
    return ret;
}

static int rgacolorkey_activate(AVFilterContext *ctx)
{
    return ff_rkrga_activate_jobs(ctx, rgacolorkey_filter_frame);
}

static int rgacolorkey_process_command(AVFilterContext *ctx, const char *cmd, const char *args,
                                       char *res, int res_len, int flags)
{
    int ret = ff_filter_process_command(ctx, cmd, args, res, res_len, flags);

    if (ret < 0)
        return ret;

    return configure_key(ctx);
}

static av_cold void rgacolorkey_uninit(AVFilterContext *ctx)
{
    ff_rkrga_close(ctx);
}

#define OFFSET(x) offsetof(RGAColorkeyContext, x)
#define FLAGS (AV_OPT_FLAG_FILTERING_PARAM | AV_OPT_FLAG_VIDEO_PARAM)
#define RFLAGS (FLAGS | AV_OPT_FLAG_RUNTIME_PARAM)

static const AVOption rgacolorkey_options[] = {
    { "color", "set the colorkey key color", OFFSET(colorkey_rgba), AV_OPT_TYPE_COLOR, { .str = "black" }, 0, 0, RFLAGS },
    { "similarity", "set the colorkey similarity value", OFFSET(similarity), AV_OPT_TYPE_FLOAT, { .dbl = 0.01 }, 0.00001, 1.0, RFLAGS },
    { "core", "Set multicore RGA scheduler core [use with caution]", OFFSET(rga.scheduler_core), AV_OPT_TYPE_FLAGS, { .i64 = 0 }, 0, INT_MAX, FLAGS, .unit = "core" },
        { "default",    NULL, 0, AV_OPT_TYPE_CONST, { .i64 = 0 }, 0, 0, FLAGS, .unit = "core" },
        { "rga2_core0", NULL, 0, AV_OPT_TYPE_CONST, { .i64 = 4 }, 0, 0, FLAGS, .unit = "core" }, /* RGA2_SCHEDULER_CORE0 */
        { "rga2_core1", NULL, 0, AV_OPT_TYPE_CONST, { .i64 = 8 }, 0, 0, FLAGS, .unit = "core" }, /* RGA2_SCHEDULER_CORE1 */
    { "async_depth", "Set the internal parallelization depth", OFFSET(rga.async_depth), AV_OPT_TYPE_INT, { .i64 = 2 }, 0, 4, .flags = FLAGS },
    { NULL },
};

AVFILTER_DEFINE_CLASS(rgacolorkey);

static const AVFilterPad rgacolorkey_inputs[] = {
    {
        .name = "default",
        .type = AVMEDIA_TYPE_VIDEO,
    },
};

static const AVFilterPad rgacolorkey_outputs[] = {
    {
        .name         = "default",
        .type         = AVMEDIA_TYPE_VIDEO,
        .config_props = rgacolorkey_config_props,
    },
};

const FFFilter ff_vf_colorkey_rkrga = {
    .p.name          = "colorkey_rkrga",
    .p.description   = NULL_IF_CONFIG_SMALL("Rockchip RGA (2D Raster Graphic Acceleration) color key"),
    .p.priv_class    = &rgacolorkey_class,
    .p.flags         = AVFILTER_FLAG_HWDEVICE,
    .priv_size       = sizeof(RGAColorkeyContext),
    .uninit          = rgacolorkey_uninit,
    .activate        = rgacolorkey_activate,
    .process_command = rgacolorkey_process_command,
    FILTER_INPUTS(rgacolorkey_inputs),
    FILTER_OUTPUTS(rgacolorkey_outputs),
    FILTER_SINGLE_PIXFMT(AV_PIX_FMT_DRM_PRIME),
    .flags_internal  = FF_FILTER_FLAG_HWFRAME_AWARE,
};
