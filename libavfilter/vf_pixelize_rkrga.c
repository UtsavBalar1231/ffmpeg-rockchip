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
 * Rockchip RGA (2D Raster Graphic Acceleration) mosaic
 */

#include "libavutil/eval.h"
#include "libavutil/internal.h"
#include "libavutil/opt.h"
#include "libavutil/pixdesc.h"

#include "filters.h"

#include "rkrga_common.h"

static const char *const var_names[] = {
    "in_w", "iw",
    "in_h", "ih",
    NULL
};

enum var_name {
    VAR_IN_W, VAR_IW,
    VAR_IN_H, VAR_IH,
    VARS_NB
};

typedef struct RGAPixelizeContext {
    RKRGAContext rga;

    int block_w, block_h;
    char *x_expr, *y_expr;
    char *w_expr, *h_expr;

    RKRGAJob job;
    int nb_jobs;
} RGAPixelizeContext;

static int eval_int(AVFilterContext *ctx, const char *expr, int *out)
{
    AVFilterLink *inlink = ctx->inputs[0];
    double var_values[VARS_NB], res;
    int ret;

    var_values[VAR_IN_W] = var_values[VAR_IW] = inlink->w;
    var_values[VAR_IN_H] = var_values[VAR_IH] = inlink->h;

    ret = av_expr_parse_and_eval(&res, expr, var_names, var_values,
                                 NULL, NULL, NULL, NULL, NULL, 0, ctx);
    if (ret < 0) {
        av_log(ctx, AV_LOG_ERROR, "Error when evaluating the expression '%s'.\n", expr);
        return ret;
    }
    *out = res;
    return 0;
}

static int configure_mosaic(AVFilterContext *ctx)
{
    RGAPixelizeContext *s = ctx->priv;
    const RGAFrameInfo *out = &s->rga.out_rga_frame_info;
    int x, y, w, h, mode, ret;

    if (s->block_w != s->block_h) {
        av_log(ctx, AV_LOG_ERROR, "RGA only draws square blocks, got %dx%d\n",
               s->block_w, s->block_h);
        return AVERROR(EINVAL);
    }
    switch (s->block_w) {
    case 8:   mode = IM_MOSAIC_8;   break;
    case 16:  mode = IM_MOSAIC_16;  break;
    case 32:  mode = IM_MOSAIC_32;  break;
    case 64:  mode = IM_MOSAIC_64;  break;
    case 128: mode = IM_MOSAIC_128; break;
    default:
        av_log(ctx, AV_LOG_ERROR, "RGA block size must be 8, 16, 32, 64 or 128, got %d\n",
               s->block_w);
        return AVERROR(EINVAL);
    }

    if ((ret = eval_int(ctx, s->x_expr, &x)) < 0 ||
        (ret = eval_int(ctx, s->y_expr, &y)) < 0 ||
        (ret = eval_int(ctx, s->w_expr, &w)) < 0 ||
        (ret = eval_int(ctx, s->h_expr, &h)) < 0)
        return ret;

    w = FFMIN((int64_t)x + w, out->act_w) - FFMAX(x, 0);
    h = FFMIN((int64_t)y + h, out->act_h) - FFMAX(y, 0);
    x = FFMAX(x, 0);
    y = FFMAX(y, 0);

    if (!(out->pix_desc->flags & AV_PIX_FMT_FLAG_RGB)) {
        x = ALIGN_DOWN(x, RK_RGA_YUV_ALIGN);
        y = ALIGN_DOWN(y, RK_RGA_YUV_ALIGN);
        w = ALIGN_DOWN(w, RK_RGA_YUV_ALIGN);
        h = ALIGN_DOWN(h, RK_RGA_YUV_ALIGN);
    }

    s->nb_jobs = 0;
    if (w < 2 || h < 2) {
        av_log(ctx, AV_LOG_VERBOSE, "Region is outside the frame, passing the input through\n");
        return 0;
    }

    s->job = (RKRGAJob){
        .usage       = IM_MOSAIC,
        .src         = RKRGA_JOB_SRC_DST,
        .rect        = { x, y, w, h },
        .mosaic_mode = mode,
    };
    if ((ret = ff_rkrga_check_job(ctx, &s->job)) < 0)
        return ret;
    s->nb_jobs = 1;

    av_log(ctx, AV_LOG_VERBOSE, "x:%d y:%d w:%d h:%d block:%d\n", x, y, w, h, s->block_w);

    return 0;
}

static av_cold int rgapixelize_config_props(AVFilterLink *outlink)
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

    outlink->w = inlink->w;
    outlink->h = inlink->h;
    outlink->sample_aspect_ratio = inlink->sample_aspect_ratio;

    param.out_sw_format = in_frames_ctx->sw_format;
    param.jobs          = 1;

    ret = ff_rkrga_init(ctx, &param);
    if (ret < 0)
        return ret;

    return configure_mosaic(ctx);
}

static int rgapixelize_filter_frame(AVFilterContext *ctx, AVFrame *in)
{
    RGAPixelizeContext *s = ctx->priv;
    int ret;

    ret = ff_rkrga_filter_frame_jobs(&s->rga, ctx->inputs[0], in, &s->job, s->nb_jobs, 1);
    av_frame_free(&in);
    return ret;
}

static int rgapixelize_activate(AVFilterContext *ctx)
{
    return ff_rkrga_activate_jobs(ctx, rgapixelize_filter_frame);
}

static int rgapixelize_process_command(AVFilterContext *ctx, const char *cmd, const char *args,
                                       char *res, int res_len, int flags)
{
    RGAPixelizeContext *s = ctx->priv;
    RKRGAJob old_job = s->job;
    int old_nb_jobs = s->nb_jobs;
    int ret;

    ret = ff_filter_process_command(ctx, cmd, args, res, res_len, flags);
    if (ret < 0)
        return ret;

    ret = configure_mosaic(ctx);
    if (ret < 0) {
        s->job = old_job;
        s->nb_jobs = old_nb_jobs;
    }

    return ret;
}

static av_cold void rgapixelize_uninit(AVFilterContext *ctx)
{
    ff_rkrga_close(ctx);
}

#define OFFSET(x) offsetof(RGAPixelizeContext, x)
#define FLAGS (AV_OPT_FLAG_FILTERING_PARAM | AV_OPT_FLAG_VIDEO_PARAM)
#define RFLAGS (FLAGS | AV_OPT_FLAG_RUNTIME_PARAM)

static const AVOption rgapixelize_options[] = {
    { "width",  "set block width",  OFFSET(block_w), AV_OPT_TYPE_INT, { .i64 = 16 }, 8, 128, RFLAGS },
    { "w",      "set block width",  OFFSET(block_w), AV_OPT_TYPE_INT, { .i64 = 16 }, 8, 128, RFLAGS },
    { "height", "set block height", OFFSET(block_h), AV_OPT_TYPE_INT, { .i64 = 16 }, 8, 128, RFLAGS },
    { "h",      "set block height", OFFSET(block_h), AV_OPT_TYPE_INT, { .i64 = 16 }, 8, 128, RFLAGS },
    { "x",        "set the x position of the region",  OFFSET(x_expr), AV_OPT_TYPE_STRING, { .str = "0" },  0, 0, RFLAGS },
    { "y",        "set the y position of the region",  OFFSET(y_expr), AV_OPT_TYPE_STRING, { .str = "0" },  0, 0, RFLAGS },
    { "region_w", "set the width of the region",       OFFSET(w_expr), AV_OPT_TYPE_STRING, { .str = "iw" }, 0, 0, RFLAGS },
    { "region_h", "set the height of the region",      OFFSET(h_expr), AV_OPT_TYPE_STRING, { .str = "ih" }, 0, 0, RFLAGS },
    { "core", "Set multicore RGA scheduler core [use with caution]", OFFSET(rga.scheduler_core), AV_OPT_TYPE_FLAGS, { .i64 = 0 }, 0, INT_MAX, FLAGS, .unit = "core" },
        { "default",    NULL, 0, AV_OPT_TYPE_CONST, { .i64 = 0 }, 0, 0, FLAGS, .unit = "core" },
        { "rga2_core0", NULL, 0, AV_OPT_TYPE_CONST, { .i64 = 4 }, 0, 0, FLAGS, .unit = "core" }, /* RGA2_SCHEDULER_CORE0 */
        { "rga2_core1", NULL, 0, AV_OPT_TYPE_CONST, { .i64 = 8 }, 0, 0, FLAGS, .unit = "core" }, /* RGA2_SCHEDULER_CORE1 */
    { "async_depth", "Set the internal parallelization depth", OFFSET(rga.async_depth), AV_OPT_TYPE_INT, { .i64 = 2 }, 0, 4, .flags = FLAGS },
    { NULL },
};

AVFILTER_DEFINE_CLASS(rgapixelize);

static const AVFilterPad rgapixelize_inputs[] = {
    {
        .name = "default",
        .type = AVMEDIA_TYPE_VIDEO,
    },
};

static const AVFilterPad rgapixelize_outputs[] = {
    {
        .name         = "default",
        .type         = AVMEDIA_TYPE_VIDEO,
        .config_props = rgapixelize_config_props,
    },
};

const FFFilter ff_vf_pixelize_rkrga = {
    .p.name          = "pixelize_rkrga",
    .p.description   = NULL_IF_CONFIG_SMALL("Rockchip RGA (2D Raster Graphic Acceleration) mosaic"),
    .p.priv_class    = &rgapixelize_class,
    .p.flags         = AVFILTER_FLAG_HWDEVICE,
    .priv_size       = sizeof(RGAPixelizeContext),
    .uninit          = rgapixelize_uninit,
    .activate        = rgapixelize_activate,
    .process_command = rgapixelize_process_command,
    FILTER_INPUTS(rgapixelize_inputs),
    FILTER_OUTPUTS(rgapixelize_outputs),
    FILTER_SINGLE_PIXFMT(AV_PIX_FMT_DRM_PRIME),
    .flags_internal  = FF_FILTER_FLAG_HWFRAME_AWARE,
};
