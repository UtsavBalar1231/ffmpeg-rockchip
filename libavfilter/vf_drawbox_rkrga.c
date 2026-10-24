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
 * Rockchip RGA (2D Raster Graphic Acceleration) box drawing
 */

#include "libavutil/eval.h"
#include "libavutil/internal.h"
#include "libavutil/opt.h"
#include "libavutil/parseutils.h"
#include "libavutil/pixdesc.h"

#include "filters.h"

#include "rkrga_common.h"

static const char *const var_names[] = {
    "dar",
    "hsub", "vsub",
    "in_h", "ih",
    "in_w", "iw",
    "sar",
    "x",
    "y",
    "h",
    "w",
    "t",
    "fill",
    NULL
};

enum var_name {
    VAR_DAR,
    VAR_HSUB, VAR_VSUB,
    VAR_IN_H, VAR_IH,
    VAR_IN_W, VAR_IW,
    VAR_SAR,
    VAR_X,
    VAR_Y,
    VAR_H,
    VAR_W,
    VAR_T,
    VAR_MAX,
    VARS_NB
};

typedef struct RGADrawBoxContext {
    RKRGAContext rga;

    char *x_expr, *y_expr;
    char *w_expr, *h_expr;
    char *t_expr;
    char *color_str;
    int replace;

    int x, y, w, h;
    int thickness;
    int color;
    int is_yuv;

    RKRGAJob jobs[4];
    int nb_jobs;
} RGADrawBoxContext;

static int parse_color(AVFilterContext *ctx)
{
    RGADrawBoxContext *s = ctx->priv;
    const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get(s->rga.out_sw_format);
    uint8_t rgba[4];

    if (!strcmp(s->color_str, "invert")) {
        av_log(ctx, AV_LOG_ERROR, "Color 'invert' is not supported\n");
        return AVERROR(ENOSYS);
    }
    if (av_parse_color(rgba, s->color_str, -1, ctx) < 0)
        return AVERROR(EINVAL);

    /* RGA fills replace the pixels; it cannot blend the color onto them */
    if (rgba[3] != 0xff && !(s->replace && (desc->flags & AV_PIX_FMT_FLAG_ALPHA))) {
        av_log(ctx, AV_LOG_ERROR, "Translucent colors need replace=1 and an output format with alpha\n");
        return AVERROR(ENOSYS);
    }

    s->color = rgba[0] | rgba[1] << 8 | rgba[2] << 16 | (unsigned)rgba[3] << 24;
    return 0;
}

static int eval_box(AVFilterContext *ctx)
{
    RGADrawBoxContext *s = ctx->priv;
    AVFilterLink *inlink = ctx->inputs[0];
    const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get(s->rga.out_sw_format);
    double var_values[VARS_NB], res;
    char *expr;
    int i, ret;

    var_values[VAR_IN_H] = var_values[VAR_IH] = inlink->h;
    var_values[VAR_IN_W] = var_values[VAR_IW] = inlink->w;
    var_values[VAR_SAR]  = inlink->sample_aspect_ratio.num ? av_q2d(inlink->sample_aspect_ratio) : 1;
    var_values[VAR_DAR]  = (double)inlink->w / inlink->h * var_values[VAR_SAR];
    var_values[VAR_HSUB] = desc->log2_chroma_w;
    var_values[VAR_VSUB] = desc->log2_chroma_h;
    var_values[VAR_X] = NAN;
    var_values[VAR_Y] = NAN;
    var_values[VAR_H] = NAN;
    var_values[VAR_W] = NAN;
    var_values[VAR_T] = NAN;

    /* Evaluate as vf_drawbox does, so that the expressions may refer to each other */
    for (i = 0; i <= 5; i++) {
        var_values[VAR_MAX] = inlink->w;
        if ((ret = av_expr_parse_and_eval(&res, (expr = s->x_expr), var_names, var_values,
                                          NULL, NULL, NULL, NULL, NULL, 0, ctx)) < 0 && i == 5)
            goto fail;
        s->x = var_values[VAR_X] = res;

        var_values[VAR_MAX] = inlink->h;
        if ((ret = av_expr_parse_and_eval(&res, (expr = s->y_expr), var_names, var_values,
                                          NULL, NULL, NULL, NULL, NULL, 0, ctx)) < 0 && i == 5)
            goto fail;
        s->y = var_values[VAR_Y] = res;

        var_values[VAR_MAX] = inlink->w - s->x;
        if ((ret = av_expr_parse_and_eval(&res, (expr = s->w_expr), var_names, var_values,
                                          NULL, NULL, NULL, NULL, NULL, 0, ctx)) < 0 && i == 5)
            goto fail;
        s->w = var_values[VAR_W] = res;

        var_values[VAR_MAX] = inlink->h - s->y;
        if ((ret = av_expr_parse_and_eval(&res, (expr = s->h_expr), var_names, var_values,
                                          NULL, NULL, NULL, NULL, NULL, 0, ctx)) < 0 && i == 5)
            goto fail;
        s->h = var_values[VAR_H] = res;

        var_values[VAR_MAX] = INT_MAX;
        if ((ret = av_expr_parse_and_eval(&res, (expr = s->t_expr), var_names, var_values,
                                          NULL, NULL, NULL, NULL, NULL, 0, ctx)) < 0 && i == 5)
            goto fail;
        s->thickness = var_values[VAR_T] = res;
    }

    s->w = (s->w > 0) ? s->w : inlink->w;
    s->h = (s->h > 0) ? s->h : inlink->h;

    if (s->w < 0 || s->h < 0) {
        av_log(ctx, AV_LOG_ERROR, "Size values less than 0 are not acceptable.\n");
        return AVERROR(EINVAL);
    }

    return 0;

fail:
    av_log(ctx, AV_LOG_ERROR, "Error when evaluating the expression '%s'.\n", expr);
    return ret;
}

static void add_rect(RGADrawBoxContext *s, int64_t x0, int64_t y0, int64_t x1, int64_t y1)
{
    const int fw = s->rga.out_rga_frame_info.act_w;
    const int fh = s->rga.out_rga_frame_info.act_h;
    RKRGAJob *job;

    x0 = FFMAX(x0, 0);
    y0 = FFMAX(y0, 0);
    x1 = FFMIN(x1, fw);
    y1 = FFMIN(y1, fh);
    if (x1 <= x0 || y1 <= y0)
        return;

    /* Cover the chroma samples that vf_drawbox writes, and keep RGA's 2x2 minimum */
    if (s->is_yuv) {
        x0 &= ~1;
        y0 &= ~1;
        x1 = FFMIN(FFALIGN(x1, 2), fw);
        y1 = FFMIN(FFALIGN(y1, 2), fh);
    }
    if (x1 - x0 < 2) {
        if (x1 < fw) x1++; else x0--;
    }
    if (y1 - y0 < 2) {
        if (y1 < fh) y1++; else y0--;
    }

    job = &s->jobs[s->nb_jobs++];
    *job = (RKRGAJob){
        .usage = IM_COLOR_FILL,
        .src   = RKRGA_JOB_SRC_NONE,
        .rect  = { x0, y0, x1 - x0, y1 - y0 },
        .color = s->color,
    };
}

static void build_jobs(RGADrawBoxContext *s)
{
    const int64_t x = s->x, y = s->y, w = s->w, h = s->h, t = s->thickness;

    s->nb_jobs = 0;
    if (t <= 0 || w <= 0 || h <= 0)
        return;

    if (2 * t >= w || 2 * t >= h) {
        add_rect(s, x, y, x + w, y + h);
        return;
    }

    add_rect(s, x,         y,         x + w, y + t);
    add_rect(s, x,         y + h - t, x + w, y + h);
    add_rect(s, x,         y + t,     x + t, y + h - t);
    add_rect(s, x + w - t, y + t,     x + w, y + h - t);
}

static int configure_box(AVFilterContext *ctx)
{
    RGADrawBoxContext *s = ctx->priv;
    int i, ret;

    if ((ret = parse_color(ctx)) < 0 ||
        (ret = eval_box(ctx)) < 0)
        return ret;

    build_jobs(s);
    for (i = 0; i < s->nb_jobs; i++) {
        if ((ret = ff_rkrga_check_job(ctx, &s->jobs[i])) < 0)
            return ret;
    }

    av_log(ctx, AV_LOG_VERBOSE, "x:%d y:%d w:%d h:%d t:%d color:0x%08X in %d fills\n",
           s->x, s->y, s->w, s->h, s->thickness, (unsigned)s->color, s->nb_jobs);

    return 0;
}

static av_cold int rgadrawbox_config_props(AVFilterLink *outlink)
{
    AVFilterContext *ctx = outlink->src;
    RGADrawBoxContext *s = ctx->priv;
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

    s->is_yuv = !(av_pix_fmt_desc_get(param.out_sw_format)->flags & AV_PIX_FMT_FLAG_RGB);

    return configure_box(ctx);
}

static int rgadrawbox_filter_frame(AVFilterContext *ctx, AVFrame *in)
{
    RGADrawBoxContext *s = ctx->priv;
    int ret;

    ret = ff_rkrga_filter_frame_jobs(&s->rga, ctx->inputs[0], in, s->jobs, s->nb_jobs, 1);
    av_frame_free(&in);
    return ret;
}

static int rgadrawbox_activate(AVFilterContext *ctx)
{
    return ff_rkrga_activate_jobs(ctx, rgadrawbox_filter_frame);
}

static int rgadrawbox_process_command(AVFilterContext *ctx, const char *cmd, const char *args,
                                      char *res, int res_len, int flags)
{
    RGADrawBoxContext *s = ctx->priv;
    RKRGAJob old_jobs[4];
    int old_nb_jobs = s->nb_jobs;
    int old_x = s->x, old_y = s->y, old_w = s->w, old_h = s->h;
    int old_t = s->thickness, old_color = s->color;
    int ret;

    memcpy(old_jobs, s->jobs, sizeof(old_jobs));

    ret = ff_filter_process_command(ctx, cmd, args, res, res_len, flags);
    if (ret < 0)
        return ret;

    ret = configure_box(ctx);
    if (ret < 0) {
        s->x = old_x;
        s->y = old_y;
        s->w = old_w;
        s->h = old_h;
        s->thickness = old_t;
        s->color = old_color;
        s->nb_jobs = old_nb_jobs;
        memcpy(s->jobs, old_jobs, sizeof(old_jobs));
    }

    return ret;
}

static av_cold void rgadrawbox_uninit(AVFilterContext *ctx)
{
    ff_rkrga_close(ctx);
}

#define OFFSET(x) offsetof(RGADrawBoxContext, x)
#define FLAGS (AV_OPT_FLAG_FILTERING_PARAM | AV_OPT_FLAG_VIDEO_PARAM)
#define RFLAGS (FLAGS | AV_OPT_FLAG_RUNTIME_PARAM)

static const AVOption rgadrawbox_options[] = {
    { "x",         "set horizontal position of the left box edge", OFFSET(x_expr),    AV_OPT_TYPE_STRING, { .str = "0" },     0, 0, RFLAGS },
    { "y",         "set vertical position of the top box edge",    OFFSET(y_expr),    AV_OPT_TYPE_STRING, { .str = "0" },     0, 0, RFLAGS },
    { "width",     "set width of the box",                         OFFSET(w_expr),    AV_OPT_TYPE_STRING, { .str = "0" },     0, 0, RFLAGS },
    { "w",         "set width of the box",                         OFFSET(w_expr),    AV_OPT_TYPE_STRING, { .str = "0" },     0, 0, RFLAGS },
    { "height",    "set height of the box",                        OFFSET(h_expr),    AV_OPT_TYPE_STRING, { .str = "0" },     0, 0, RFLAGS },
    { "h",         "set height of the box",                        OFFSET(h_expr),    AV_OPT_TYPE_STRING, { .str = "0" },     0, 0, RFLAGS },
    { "color",     "set color of the box",                         OFFSET(color_str), AV_OPT_TYPE_STRING, { .str = "black" }, 0, 0, RFLAGS },
    { "c",         "set color of the box",                         OFFSET(color_str), AV_OPT_TYPE_STRING, { .str = "black" }, 0, 0, RFLAGS },
    { "thickness", "set the box thickness",                        OFFSET(t_expr),    AV_OPT_TYPE_STRING, { .str = "3" },     0, 0, RFLAGS },
    { "t",         "set the box thickness",                        OFFSET(t_expr),    AV_OPT_TYPE_STRING, { .str = "3" },     0, 0, RFLAGS },
    { "replace",   "replace color & alpha",                        OFFSET(replace),   AV_OPT_TYPE_BOOL,   { .i64 = 0 },       0, 1, RFLAGS },
    { "core", "Set multicore RGA scheduler core [use with caution]", OFFSET(rga.scheduler_core), AV_OPT_TYPE_FLAGS, { .i64 = 0 }, 0, INT_MAX, FLAGS, .unit = "core" },
        { "default",    NULL, 0, AV_OPT_TYPE_CONST, { .i64 = 0 }, 0, 0, FLAGS, .unit = "core" },
        { "rga2_core0", NULL, 0, AV_OPT_TYPE_CONST, { .i64 = 4 }, 0, 0, FLAGS, .unit = "core" }, /* RGA2_SCHEDULER_CORE0 */
        { "rga2_core1", NULL, 0, AV_OPT_TYPE_CONST, { .i64 = 8 }, 0, 0, FLAGS, .unit = "core" }, /* RGA2_SCHEDULER_CORE1 */
    { "async_depth", "Set the internal parallelization depth", OFFSET(rga.async_depth), AV_OPT_TYPE_INT, { .i64 = 2 }, 0, 4, .flags = FLAGS },
    { NULL },
};

AVFILTER_DEFINE_CLASS(rgadrawbox);

static const AVFilterPad rgadrawbox_inputs[] = {
    {
        .name = "default",
        .type = AVMEDIA_TYPE_VIDEO,
    },
};

static const AVFilterPad rgadrawbox_outputs[] = {
    {
        .name         = "default",
        .type         = AVMEDIA_TYPE_VIDEO,
        .config_props = rgadrawbox_config_props,
    },
};

const FFFilter ff_vf_drawbox_rkrga = {
    .p.name          = "drawbox_rkrga",
    .p.description   = NULL_IF_CONFIG_SMALL("Rockchip RGA (2D Raster Graphic Acceleration) box drawing"),
    .p.priv_class    = &rgadrawbox_class,
    .p.flags         = AVFILTER_FLAG_HWDEVICE,
    .priv_size       = sizeof(RGADrawBoxContext),
    .uninit          = rgadrawbox_uninit,
    .activate        = rgadrawbox_activate,
    .process_command = rgadrawbox_process_command,
    FILTER_INPUTS(rgadrawbox_inputs),
    FILTER_OUTPUTS(rgadrawbox_outputs),
    FILTER_SINGLE_PIXFMT(AV_PIX_FMT_DRM_PRIME),
    .flags_internal  = FF_FILTER_FLAG_HWFRAME_AWARE,
};
