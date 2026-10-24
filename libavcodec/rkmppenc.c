/*
 * Copyright (c) 2023 Huseyin BIYIK
 * Copyright (c) 2023 NyanMisaka
 *
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
 * Rockchip MPP (Media Process Platform) video encoder
 */

#include "config_components.h"
#include "rkmppenc.h"
#include "startcode.h"

#include <fcntl.h>
#include <unistd.h>

/* SoCs whose H.264/HEVC encoder takes AFBC (v2) input, from MPP's SoC table */
static const char *const afbc_input_socs[] = {
    "rk3528", "rk3566", "rk3567", "rk3568", "rk3588", NULL,
};

/* SoCs whose JPEG encoder is VPU720, which takes YUV input only */
static const char *const jpeg_yuv_only_socs[] = {
    "rk3572", "rk3576", NULL,
};

/* SoCs whose HEVC encoder (VEPU510) has no intra refresh support */
static const char *const hevc_no_refresh_socs[] = {
    "rk3576", NULL,
};

/* SoCs whose H.264/HEVC encoder (VEPU580) ignores MPP's MppEncROICfg */
static const char *const no_roi_cfg_socs[] = {
    "rk3588", NULL,
};

static int rkmpp_soc_is_any(const char *const *socs)
{
    char compat[512];
    ssize_t len;
    int fd = open("/proc/device-tree/compatible", O_RDONLY);

    if (fd < 0)
        return 0;
    len = read(fd, compat, sizeof(compat) - 1);
    close(fd);
    if (len <= 0)
        return 0;
    compat[len] = '\0';

    /* a list of NUL-terminated strings, e.g. "vendor,board\0rockchip,rk3576\0" */
    for (const char *p = compat; p < compat + len; p += strlen(p) + 1) {
        const char *soc;

        if (!av_strstart(p, "rockchip,", &soc))
            continue;
        for (int i = 0; socs[i]; i++)
            if (av_strstart(soc, socs[i], NULL))
                return 1;
    }

    return 0;
}

/*
 * MPP's color tables end before FFmpeg's (at ICtCp, JEDEC P22 and HLG), and
 * one value past them makes MPP reject or drop the whole color config,
 * range included.
 */
static enum AVColorSpace rkmpp_normalize_colorspace(enum AVColorSpace value)
{
    return value == AVCOL_SPC_RESERVED || !av_color_space_name(value) ||
           (int)value > MPP_FRAME_SPC_ICTCP ? AVCOL_SPC_UNSPECIFIED : value;
}

static enum AVColorPrimaries rkmpp_normalize_color_primaries(enum AVColorPrimaries value)
{
    return value == AVCOL_PRI_RESERVED0 || value == AVCOL_PRI_RESERVED ||
           !av_color_primaries_name(value) ||
           (int)value > MPP_FRAME_PRI_JEDEC_P22 ? AVCOL_PRI_UNSPECIFIED : value;
}

static enum AVColorTransferCharacteristic rkmpp_normalize_color_trc(
    enum AVColorTransferCharacteristic value)
{
    return value == AVCOL_TRC_RESERVED0 || value == AVCOL_TRC_RESERVED ||
           !av_color_transfer_name(value) ||
           (int)value > MPP_FRAME_TRC_ARIB_STD_B67 ? AVCOL_TRC_UNSPECIFIED : value;
}

static MppCodingType rkmpp_get_coding_type(AVCodecContext *avctx)
{
    switch (avctx->codec_id) {
    case AV_CODEC_ID_H264:  return MPP_VIDEO_CodingAVC;
    case AV_CODEC_ID_HEVC:  return MPP_VIDEO_CodingHEVC;
    case AV_CODEC_ID_MJPEG: return MPP_VIDEO_CodingMJPEG;
    default:                return MPP_VIDEO_CodingUnused;
    }
}

static MppFrameFormat rkmpp_get_mpp_fmt_h26x(enum AVPixelFormat pix_fmt)
{
    switch (pix_fmt) {
    case AV_PIX_FMT_GRAY8:   return MPP_FMT_YUV400;
    case AV_PIX_FMT_YUVJ420P:
    case AV_PIX_FMT_YUV420P: return MPP_FMT_YUV420P;
    case AV_PIX_FMT_YUVJ422P:
    case AV_PIX_FMT_YUV422P: return MPP_FMT_YUV422P;
    case AV_PIX_FMT_YUVJ444P:
    case AV_PIX_FMT_YUV444P: return MPP_FMT_YUV444P;
    case AV_PIX_FMT_NV12:    return MPP_FMT_YUV420SP;
    case AV_PIX_FMT_NV21:    return MPP_FMT_YUV420SP_VU;
    case AV_PIX_FMT_NV16:    return MPP_FMT_YUV422SP;
    case AV_PIX_FMT_NV24:    return MPP_FMT_YUV444SP;
    case AV_PIX_FMT_YUYV422: return MPP_FMT_YUV422_YUYV;
    case AV_PIX_FMT_YVYU422: return MPP_FMT_YUV422_YVYU;
    case AV_PIX_FMT_UYVY422: return MPP_FMT_YUV422_UYVY;
    case AV_PIX_FMT_RGB24:   return MPP_FMT_RGB888;
    case AV_PIX_FMT_BGR24:   return MPP_FMT_BGR888;
    case AV_PIX_FMT_RGBA:
    case AV_PIX_FMT_RGB0:    return MPP_FMT_RGBA8888;
    case AV_PIX_FMT_BGRA:
    case AV_PIX_FMT_BGR0:    return MPP_FMT_BGRA8888;
    case AV_PIX_FMT_ARGB:
    case AV_PIX_FMT_0RGB:    return MPP_FMT_ARGB8888;
    case AV_PIX_FMT_ABGR:
    case AV_PIX_FMT_0BGR:    return MPP_FMT_ABGR8888;
    default:                 return MPP_FMT_BUTT;
    }
}

static MppFrameFormat rkmpp_get_mpp_fmt_mjpeg(enum AVPixelFormat pix_fmt)
{
    switch (pix_fmt) {
    case AV_PIX_FMT_YUVJ420P:
    case AV_PIX_FMT_YUV420P:   return MPP_FMT_YUV420P;
    case AV_PIX_FMT_YUVJ422P:
    case AV_PIX_FMT_YUV422P:   return MPP_FMT_YUV422P;     /* RK3576+ only */
    case AV_PIX_FMT_YUVJ444P:
    case AV_PIX_FMT_YUV444P:   return MPP_FMT_YUV444P;     /* RK3576+ only */
    case AV_PIX_FMT_NV12:      return MPP_FMT_YUV420SP;
    case AV_PIX_FMT_NV21:      return MPP_FMT_YUV420SP_VU; /* RK3576+ only */
    case AV_PIX_FMT_NV16:      return MPP_FMT_YUV422SP;    /* RK3576+ only */
    case AV_PIX_FMT_NV24:      return MPP_FMT_YUV444SP;    /* RK3576+ only */
    case AV_PIX_FMT_YUYV422:   return MPP_FMT_YUV422_YUYV;
    case AV_PIX_FMT_UYVY422:   return MPP_FMT_YUV422_UYVY;
    case AV_PIX_FMT_YVYU422:   return MPP_FMT_YUV422_YVYU; /* RK3576+ only */

    /* RGB: pre-RK3576 only */
    case AV_PIX_FMT_RGB444BE:  return MPP_FMT_RGB444;
    case AV_PIX_FMT_BGR444BE:  return MPP_FMT_BGR444;
    case AV_PIX_FMT_RGB555BE:  return MPP_FMT_RGB555;
    case AV_PIX_FMT_BGR555BE:  return MPP_FMT_BGR555;
    case AV_PIX_FMT_RGB565BE:  return MPP_FMT_RGB565;
    case AV_PIX_FMT_BGR565BE:  return MPP_FMT_BGR565;
    case AV_PIX_FMT_RGBA:
    case AV_PIX_FMT_RGB0:      return MPP_FMT_RGBA8888;
    case AV_PIX_FMT_BGRA:
    case AV_PIX_FMT_BGR0:      return MPP_FMT_BGRA8888;
    case AV_PIX_FMT_ARGB:
    case AV_PIX_FMT_0RGB:      return MPP_FMT_ARGB8888;
    case AV_PIX_FMT_ABGR:
    case AV_PIX_FMT_0BGR:      return MPP_FMT_ABGR8888;
    case AV_PIX_FMT_X2RGB10BE: return MPP_FMT_RGB101010;
    case AV_PIX_FMT_X2BGR10BE: return MPP_FMT_BGR101010;
    default:                   return MPP_FMT_BUTT;
    }
}

static uint32_t rkmpp_get_drm_afbc_format(MppFrameFormat mpp_fmt)
{
    switch (mpp_fmt & MPP_FRAME_FMT_MASK) {
    case MPP_FMT_YUV420SP: return DRM_FORMAT_YUV420_8BIT;
    case MPP_FMT_YUV422SP: return DRM_FORMAT_YUYV;
    default:               return DRM_FORMAT_INVALID;
    }
}

static MppFrameChromaFormat rkmpp_fix_chroma_fmt(int chroma_fmt,
                                                 enum AVPixelFormat pix_fmt)
{
    const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get(pix_fmt);
    int log2_chroma_sum = desc->log2_chroma_w + desc->log2_chroma_h;
    int is_yuv = !(desc->flags & AV_PIX_FMT_FLAG_RGB) &&
                 desc->nb_components >= 2;

    if (!is_yuv)
        return MPP_CHROMA_UNSPECIFIED;

    switch (chroma_fmt) {
    case -1:
        return log2_chroma_sum == 0 ? MPP_CHROMA_444 :
               log2_chroma_sum == 1 ? MPP_CHROMA_422 :
                                      MPP_CHROMA_UNSPECIFIED;
    case MPP_CHROMA_400:
        return chroma_fmt;
    case MPP_CHROMA_420:
        return log2_chroma_sum <= 2 ?
            chroma_fmt : MPP_CHROMA_UNSPECIFIED;
    case MPP_CHROMA_422:
        return log2_chroma_sum <= 1 ?
            chroma_fmt : MPP_CHROMA_UNSPECIFIED;
    case MPP_CHROMA_444:
        return log2_chroma_sum == 0 ?
            chroma_fmt : MPP_CHROMA_UNSPECIFIED;
    default:
        return MPP_CHROMA_UNSPECIFIED;
    }
}

static int get_byte_stride(const AVDRMObjectDescriptor *object,
                           const AVDRMLayerDescriptor *layer,
                           int is_rgb, int is_planar,
                           int *hs, int *vs)
{
    const AVDRMPlaneDescriptor *plane0, *plane1;
    const int is_packed_fmt = is_rgb || (!is_rgb && !is_planar);

    if (!object || !layer || !hs || !vs)
        return AVERROR(EINVAL);

    plane0 = &layer->planes[0];
    plane1 = &layer->planes[1];

    if (plane0->pitch <= 0 || plane0->pitch > INT_MAX)
        return AVERROR(EINVAL);

    *hs = plane0->pitch;
    *vs = is_packed_fmt ?
        ALIGN_DOWN(object->size / plane0->pitch, is_rgb ? 1 : 2) :
        (plane1->offset / plane0->pitch);

    return (*hs > 0 && *vs > 0) ? 0 : AVERROR(EINVAL);
}

/* MPP imports one buffer and locates the chroma planes from the strides alone. */
static int check_drm_layout(const AVDRMFrameDescriptor *desc,
                            enum AVPixelFormat pix_fmt, int hs, int vs)
{
    const AVPixFmtDescriptor *pix_desc = av_pix_fmt_desc_get(pix_fmt);
    const AVDRMLayerDescriptor *layer = &desc->layers[0];
    const int nb_planes = av_pix_fmt_count_planes(pix_fmt);
    ptrdiff_t c_pitch, c_size;

    if (desc->nb_layers != 1 || layer->nb_planes != nb_planes)
        return AVERROR(EINVAL);
    for (int i = 0; i < nb_planes; i++)
        if (layer->planes[i].object_index)
            return AVERROR(EINVAL);
    if (nb_planes == 1)
        return 0;

    c_pitch = nb_planes == 2 ? ((ptrdiff_t)hs * 2) >> pix_desc->log2_chroma_w
                             : hs >> pix_desc->log2_chroma_w;
    c_size  = c_pitch * (vs >> pix_desc->log2_chroma_h);

    if (layer->planes[1].offset != (ptrdiff_t)hs * vs ||
        layer->planes[1].pitch  != c_pitch)
        return AVERROR(EINVAL);
    if (nb_planes == 3 &&
        (layer->planes[2].offset != layer->planes[1].offset + c_size ||
         layer->planes[2].pitch  != c_pitch))
        return AVERROR(EINVAL);
    if (desc->objects[0].size < (size_t)(layer->planes[nb_planes - 1].offset + c_size))
        return AVERROR(EINVAL);

    return 0;
}

static int get_afbc_byte_stride(const AVPixFmtDescriptor *desc,
                                int *stride, int reverse)
{
    if (!desc || !stride || *stride <= 0)
        return AVERROR(EINVAL);

    if (desc->nb_components == 1 ||
        (desc->flags & AV_PIX_FMT_FLAG_RGB) ||
        (!(desc->flags & AV_PIX_FMT_FLAG_RGB) &&
         !(desc->flags & AV_PIX_FMT_FLAG_PLANAR)))
        return 0;

    if (desc->log2_chroma_w == 1 && desc->log2_chroma_h == 1)
        *stride = reverse ? (*stride * 2 / 3) : (*stride * 3 / 2);
    else if (desc->log2_chroma_w == 1 && !desc->log2_chroma_h)
        *stride = reverse ? (*stride / 2) : (*stride * 2);
    else if (!desc->log2_chroma_w && !desc->log2_chroma_h)
        *stride = reverse ? (*stride / 3) : (*stride * 3);
    else
        return AVERROR(EINVAL);

    return (*stride > 0) ? 0 : AVERROR(EINVAL);
}

static unsigned get_used_frame_count(MPPEncFrame *list)
{
    unsigned count = 0;

    while (list) {
        if (list->queued == 1 &&
            (list->frame || list->mpp_frame))
            ++count;
        list = list->next;
    }

    return count;
}

static void release_frame(MPPEncFrame *frame)
{
    if (frame->mpp_frame)
        mpp_frame_deinit(&frame->mpp_frame);

    av_freep(&frame->mpp_sei_set.datas);
    frame->mpp_sei_set.count = 0;

    av_frame_free(&frame->frame);
    frame->in_mpp = 0;
    frame->queued = 0;
}

/* MPP hands each input frame back in the meta of the packet made from it. */
static int release_input_frame(MPPEncFrame *list, MppPacket mpp_pkt)
{
    MppFrame mpp_frame = NULL;

    if (!mpp_packet_has_meta(mpp_pkt) ||
        mpp_meta_get_frame(mpp_packet_get_meta(mpp_pkt),
                           KEY_INPUT_FRAME, &mpp_frame) != MPP_OK ||
        !mpp_frame)
        return AVERROR(ENOENT);

    for (; list; list = list->next) {
        if (list->in_mpp && list->mpp_frame == mpp_frame) {
            release_frame(list);
            return 0;
        }
    }

    return AVERROR(ENOENT);
}

static void clear_frame_list(MPPEncFrame **list)
{
    while (*list) {
        MPPEncFrame *frame = *list;

        *list = frame->next;

        /* never handed back, so mpp_destroy() has already released it */
        if (frame->in_mpp)
            frame->mpp_frame = NULL;

        release_frame(frame);
        av_freep(&frame);
    }
}

static MPPEncFrame *get_free_frame(MPPEncFrame **list)
{
    MPPEncFrame *out = *list;

    for (; out; out = out->next) {
        if (!out->queued) {
            out->queued = 1;
            break;
        }
    }

    if (!out) {
        out = av_mallocz(sizeof(*out));
        if (!out) {
            av_log(NULL, AV_LOG_ERROR, "Cannot alloc new output frame\n");
            return NULL;
        }
        out->queued = 1;
        out->next   = *list;
        *list       = out;
    }

    return out;
}

static int rkmpp_set_enc_cfg_prep(AVCodecContext *avctx, AVFrame *frame)
{
    RKMPPEncContext *r = avctx->priv_data;
    MppEncCfg cfg = r->mcfg;
    MppFrameFormat mpp_fmt = r->mpp_fmt;
    int ret, is_afbc = 0;
    int hor_stride = 0, ver_stride = 0;
    enum AVColorSpace colorspace = rkmpp_normalize_colorspace(avctx->colorspace);
    enum AVColorPrimaries color_primaries = rkmpp_normalize_color_primaries(avctx->color_primaries);
    enum AVColorTransferCharacteristic color_trc = rkmpp_normalize_color_trc(avctx->color_trc);
    const AVPixFmtDescriptor *pix_desc;
    const AVDRMFrameDescriptor *drm_desc;

    if (r->cfg_init)
        return 0;

    if (!frame)
        return AVERROR(EINVAL);

    drm_desc = (AVDRMFrameDescriptor *)frame->data[0];
    if (drm_desc->objects[0].fd < 0)
        return AVERROR(ENOMEM);

    pix_desc = av_pix_fmt_desc_get(r->pix_fmt);
    is_afbc = drm_is_afbc(drm_desc->objects[0].format_modifier);
    if (!is_afbc &&
        drm_desc->objects[0].format_modifier != DRM_FORMAT_MOD_LINEAR) {
        av_log(avctx, AV_LOG_ERROR, "Only linear and AFBC modifiers are supported\n");
        return AVERROR(ENOSYS);
    }
    if (is_afbc &&
        !(avctx->codec_id == AV_CODEC_ID_H264 ||
          avctx->codec_id == AV_CODEC_ID_HEVC)) {
        av_log(avctx, AV_LOG_ERROR, "AFBC is not supported in codec '%s'\n",
               avcodec_get_name(avctx->codec_id));
        return AVERROR(ENOSYS);
    }
    if (!is_afbc) {
        ret = get_byte_stride(&drm_desc->objects[0],
                              &drm_desc->layers[0],
                              (pix_desc->flags & AV_PIX_FMT_FLAG_RGB),
                              (pix_desc->flags & AV_PIX_FMT_FLAG_PLANAR),
                              &hor_stride, &ver_stride);
        if (ret < 0 || !hor_stride || !ver_stride) {
            av_log(avctx, AV_LOG_ERROR, "Failed to get frame strides\n");
            return AVERROR(EINVAL);
        }

        mpp_enc_cfg_set_s32(cfg, "prep:hor_stride", hor_stride);
        mpp_enc_cfg_set_s32(cfg, "prep:ver_stride", ver_stride);
    }

    mpp_enc_cfg_set_s32(cfg, "prep:width", avctx->width);
    mpp_enc_cfg_set_s32(cfg, "prep:height", avctx->height);

    if (pix_desc->flags & AV_PIX_FMT_FLAG_RGB) /* RGB -> BT709 CSC */
        mpp_enc_cfg_set_s32(cfg, "prep:colorspace", AVCOL_SPC_BT709);
    else
        mpp_enc_cfg_set_s32(cfg, "prep:colorspace", colorspace);

    mpp_enc_cfg_set_s32(cfg, "prep:colorprim", color_primaries);
    mpp_enc_cfg_set_s32(cfg, "prep:colortrc", color_trc);

    mpp_enc_cfg_set_s32(cfg, "prep:colorrange", avctx->color_range);
    if (r->pix_fmt == AV_PIX_FMT_YUVJ420P ||
        r->pix_fmt == AV_PIX_FMT_YUVJ422P ||
        r->pix_fmt == AV_PIX_FMT_YUVJ444P) {
        mpp_enc_cfg_set_s32(cfg, "prep:colorrange", AVCOL_RANGE_JPEG);
    }

    if (avctx->codec_id == AV_CODEC_ID_MJPEG) {
        /* always output full range if the MJPEG encoder supports CSC */
        mpp_enc_cfg_set_s32(cfg, "prep:range_out", AVCOL_RANGE_JPEG);
        mpp_enc_cfg_set_s32(cfg, "prep:format_out", rkmpp_fix_chroma_fmt(r->chroma_fmt, r->pix_fmt));
    }

    if (is_afbc) {
        const AVDRMLayerDescriptor *layer = &drm_desc->layers[0];
        uint32_t drm_afbc_fmt = rkmpp_get_drm_afbc_format(mpp_fmt);

        if (drm_afbc_fmt != layer->format) {
            av_log(avctx, AV_LOG_ERROR, "Input format '%s' with AFBC modifier is not supported\n",
                   av_get_pix_fmt_name(r->pix_fmt));
            return AVERROR(ENOSYS);
        }
        mpp_fmt |= MPP_FRAME_FBC_AFBC_V2;
    }
    mpp_enc_cfg_set_s32(cfg, "prep:format", mpp_fmt);

    if ((ret = r->mapi->control(r->mctx, MPP_ENC_SET_CFG, cfg)) != MPP_OK) {
        av_log(avctx, AV_LOG_ERROR, "Failed to set config with frame: %d\n", ret);
        return AVERROR_EXTERNAL;
    }

    r->cfg_init   = 1;
    r->cfg_afbc   = is_afbc;
    r->hor_stride = hor_stride;
    r->ver_stride = ver_stride;
    av_log(avctx, AV_LOG_VERBOSE, "Configured with size: %dx%d | pix_fmt: %s | sw_pix_fmt: %s\n",
           avctx->width, avctx->height,
           av_get_pix_fmt_name(avctx->pix_fmt), av_get_pix_fmt_name(r->pix_fmt));

    return 0;
}

static int rkmpp_set_split_cfg(AVCodecContext *avctx, MppEncCfg cfg)
{
    RKMPPEncContext *r = avctx->priv_data;
    MppEncSplitMode mode = MPP_ENC_SPLIT_NONE;
    int arg = 0;

    if (avctx->slices > 1 && r->max_slice_size > 0) {
        av_log(avctx, AV_LOG_ERROR, "slices and max_slice_size cannot be used together\n");
        return AVERROR(EINVAL);
    }

    if (r->max_slice_size > 0) {
        mode = MPP_ENC_SPLIT_BY_BYTE;
        arg  = r->max_slice_size;
        av_log(avctx, AV_LOG_VERBOSE, "Slices are split at %d bytes\n", arg);
    } else if (avctx->slices > 1) {
        /* MPP counts 16x16 MBs for H.264 and CTUs for HEVC */
        int ctu_size = 16, cols, rows, rows_per_slice;

        if (avctx->codec_id == AV_CODEC_ID_HEVC &&
            (mpp_enc_cfg_get_s32(cfg, "h265:lcu_size", &ctu_size) != MPP_OK ||
             ctu_size <= 0))
            ctu_size = 64;

        cols = (avctx->width  + ctu_size - 1) / ctu_size;
        rows = (avctx->height + ctu_size - 1) / ctu_size;
        rows_per_slice = (rows + avctx->slices - 1) / avctx->slices;

        mode = MPP_ENC_SPLIT_BY_CTU;
        arg  = rows_per_slice * cols;
        av_log(avctx, AV_LOG_VERBOSE, "Slices are split every %d rows of %dx%d blocks (%d slices)\n",
               rows_per_slice, ctu_size, ctu_size, (rows + rows_per_slice - 1) / rows_per_slice);
    }

    if (mode != MPP_ENC_SPLIT_NONE) {
        mpp_enc_cfg_set_u32(cfg, "split:mode", mode);
        mpp_enc_cfg_set_u32(cfg, "split:arg", arg);
    }

    return 0;
}

/*
 * Dyadic temporal layers: frame i of a period of 2^(layers - 1) frames has
 * tid layers - 1 - ctz(i) and refers to the latest frame of the layer of
 * frame i - (i & -i). Entry 0 is the IDR; MPP loops over the others.
 * MPP runs out of reconstruction buffers with 4 short-term layers, so 4
 * layers keep layer 0 as long-term references as MPP's tsvc4 preset does:
 * each layer 0 frame is one, and layer 1 refers to it.
 */
static int rkmpp_set_ref_cfg(AVCodecContext *avctx)
{
    RKMPPEncContext *r = avctx->priv_data;
    const int layers = r->temporal_layers;
    const int period = 1 << (layers - 1);
    MppEncRefStFrmCfg st_cfg[(1 << 3) + 1] = { 0 };
    MppEncRefLtFrmCfg lt_cfg = { 0 };
    MppEncRefCfg ref = NULL;
    int ret;

    if (layers <= 1 && !r->ltr_interval)
        return 0;

    if (layers == 4 && r->ltr_interval && r->ltr_interval != period) {
        av_log(avctx, AV_LOG_ERROR, "4 temporal layers use a long-term reference every %d frames, "
               "ltr_interval must be 0 or %d\n", period, period);
        return AVERROR(EINVAL);
    }
    if (r->ltr_interval % period) {
        av_log(avctx, AV_LOG_ERROR, "ltr_interval must be a multiple of %d with %d temporal layers\n",
               period, layers);
        return AVERROR(EINVAL);
    }

    for (int i = 1; i <= period; i++) {
        int tid = i == period ? 0 : layers - 1 - ff_ctz(i);
        int j = i - (i & -i);

        st_cfg[i].temporal_id = tid;
        st_cfg[i].is_non_ref  = layers > 1 && tid == layers - 1;
        st_cfg[i].ref_mode    = REF_TO_TEMPORAL_LAYER;
        st_cfg[i].ref_arg     = j ? layers - 1 - ff_ctz(j) : 0;
        if (layers == 4 && tid == 1)
            st_cfg[i].ref_mode = REF_TO_PREV_LT_REF;
    }
    st_cfg[0].ref_mode = REF_TO_TEMPORAL_LAYER;

    lt_cfg.lt_idx   = 0;
    lt_cfg.ref_mode = REF_TO_PREV_LT_REF;
    lt_cfg.lt_gap   = layers == 4 ? period : r->ltr_interval;

    if (mpp_enc_ref_cfg_init(&ref) != MPP_OK || !ref) {
        av_log(avctx, AV_LOG_ERROR, "Failed to init the reference config\n");
        return AVERROR_EXTERNAL;
    }

    ret = mpp_enc_ref_cfg_set_cfg_cnt(ref, !!lt_cfg.lt_gap, period + 1);
    if (ret == MPP_OK && lt_cfg.lt_gap)
        ret = mpp_enc_ref_cfg_add_lt_cfg(ref, 1, &lt_cfg);
    if (ret == MPP_OK)
        ret = mpp_enc_ref_cfg_add_st_cfg(ref, period + 1, st_cfg);
    if (ret == MPP_OK)
        ret = mpp_enc_ref_cfg_check(ref);
    if (ret == MPP_OK)
        ret = r->mapi->control(r->mctx, MPP_ENC_SET_REF_CFG, ref);
    mpp_enc_ref_cfg_deinit(&ref);

    if (ret != MPP_OK) {
        av_log(avctx, AV_LOG_ERROR, "Failed to set the reference config: %d\n", ret);
        return AVERROR_EXTERNAL;
    }

    av_log(avctx, AV_LOG_VERBOSE, "Temporal layers: %d, long-term reference interval: %d\n",
           layers, lt_cfg.lt_gap);

    return 0;
}

static int rkmpp_set_enc_cfg(AVCodecContext *avctx)
{
    RKMPPEncContext *r = avctx->priv_data;
    MppEncCfg cfg = r->mcfg;
    const AVPixFmtDescriptor *pix_desc;
    RK_U32 rc_mode, fps_num, fps_den;
    MppEncHeaderMode header_mode;
    MppEncSeiMode sei_mode;
    int64_t target_bps = FFMIN(avctx->bit_rate, INT_MAX);
    int64_t max_bps = FFMIN(avctx->rc_max_rate, INT_MAX);
    int64_t min_bps = FFMIN(avctx->rc_min_rate, INT_MAX);
    int qp_init, qp_max, qp_min, qp_max_i, qp_min_i;
    int ret;
    enum AVColorSpace colorspace = rkmpp_normalize_colorspace(avctx->colorspace);
    enum AVColorPrimaries color_primaries = rkmpp_normalize_color_primaries(avctx->color_primaries);
    enum AVColorTransferCharacteristic color_trc = rkmpp_normalize_color_trc(avctx->color_trc);

    mpp_enc_cfg_set_s32(cfg, "prep:width", avctx->width);
    mpp_enc_cfg_set_s32(cfg, "prep:height", avctx->height);
    mpp_enc_cfg_set_s32(cfg, "prep:hor_stride", FFALIGN(avctx->width, 64));
    mpp_enc_cfg_set_s32(cfg, "prep:ver_stride", FFALIGN(avctx->height, 64));
    mpp_enc_cfg_set_s32(cfg, "prep:format", r->mpp_fmt);
    mpp_enc_cfg_set_s32(cfg, "prep:mirroring", 0);
    mpp_enc_cfg_set_s32(cfg, "prep:rotation", 0);
    mpp_enc_cfg_set_s32(cfg, "prep:flip", 0);

    pix_desc = av_pix_fmt_desc_get(r->pix_fmt);
    if (pix_desc->flags & AV_PIX_FMT_FLAG_RGB) /* RGB -> BT709 CSC */
        mpp_enc_cfg_set_s32(cfg, "prep:colorspace", AVCOL_SPC_BT709);
    else
        mpp_enc_cfg_set_s32(cfg, "prep:colorspace", colorspace);

    mpp_enc_cfg_set_s32(cfg, "prep:colorprim", color_primaries);
    mpp_enc_cfg_set_s32(cfg, "prep:colortrc", color_trc);

    mpp_enc_cfg_set_s32(cfg, "prep:colorrange", avctx->color_range);
    if (r->pix_fmt == AV_PIX_FMT_YUVJ420P ||
        r->pix_fmt == AV_PIX_FMT_YUVJ422P ||
        r->pix_fmt == AV_PIX_FMT_YUVJ444P) {
        mpp_enc_cfg_set_s32(cfg, "prep:colorrange", AVCOL_RANGE_JPEG);
    }

    if (avctx->codec_id == AV_CODEC_ID_MJPEG) {
        /* always output full range if the MJPEG encoder supports CSC */
        mpp_enc_cfg_set_s32(cfg, "prep:range_out", AVCOL_RANGE_JPEG);
        mpp_enc_cfg_set_s32(cfg, "prep:format_out", rkmpp_fix_chroma_fmt(r->chroma_fmt, r->pix_fmt));
    }

    /* without a frame rate keep MPP's default; the time base is not a rate */
    if (avctx->framerate.den > 0 && avctx->framerate.num > 0) {
        av_reduce(&fps_num, &fps_den, avctx->framerate.num, avctx->framerate.den, 65535);

        mpp_enc_cfg_set_s32(cfg, "rc:fps_in_flex", 0);
        mpp_enc_cfg_set_s32(cfg, "rc:fps_in_num", fps_num);
        mpp_enc_cfg_set_s32(cfg, "rc:fps_in_denom", fps_den);
        mpp_enc_cfg_set_s32(cfg, "rc:fps_in_denorm", fps_den);
        mpp_enc_cfg_set_s32(cfg, "rc:fps_out_flex", 0);
        mpp_enc_cfg_set_s32(cfg, "rc:fps_out_num", fps_num);
        mpp_enc_cfg_set_s32(cfg, "rc:fps_out_denom", fps_den);
        mpp_enc_cfg_set_s32(cfg, "rc:fps_out_denorm", fps_den);
    }

    mpp_enc_cfg_set_s32(cfg, "rc:gop", FFMAX(avctx->gop_size, 1));

    rc_mode = avctx->codec_id == AV_CODEC_ID_MJPEG ? MPP_ENC_RC_MODE_FIXQP : r->rc_mode;
    if (rc_mode == MPP_ENC_RC_MODE_BUTT) {
        if (r->qp_init >= 0)
            rc_mode = MPP_ENC_RC_MODE_FIXQP;
        else if (max_bps > 0)
            rc_mode = MPP_ENC_RC_MODE_VBR;
        else
            rc_mode = MPP_ENC_RC_MODE_CBR;
    }

    switch (rc_mode) {
    case MPP_ENC_RC_MODE_VBR:
        av_log(avctx, AV_LOG_VERBOSE, "Rate Control mode is set to VBR\n"); break;
    case MPP_ENC_RC_MODE_CBR:
        av_log(avctx, AV_LOG_VERBOSE, "Rate Control mode is set to CBR\n"); break;
    case MPP_ENC_RC_MODE_FIXQP:
        av_log(avctx, AV_LOG_VERBOSE, "Rate Control mode is set to CQP\n"); break;
    case MPP_ENC_RC_MODE_AVBR:
        av_log(avctx, AV_LOG_VERBOSE, "Rate Control mode is set to AVBR\n"); break;
    }
    mpp_enc_cfg_set_u32(cfg, "rc:mode", rc_mode);

    switch (rc_mode) {
    case MPP_ENC_RC_MODE_FIXQP:
        /* do not setup bitrate on FIXQP mode */
        break;
    case MPP_ENC_RC_MODE_VBR:
    case MPP_ENC_RC_MODE_AVBR:
        /* VBR mode has wide bound */
        max_bps = (max_bps > 0 && max_bps >= target_bps)
                  ? max_bps : (target_bps * 17 / 16);
        min_bps = (min_bps > 0 && min_bps <= target_bps)
                  ? min_bps : (target_bps * 1 / 16);
        break;
    case MPP_ENC_RC_MODE_CBR:
    default:
        /* CBR mode has narrow bound */
        max_bps = target_bps * 17 / 16;
        min_bps = target_bps * 15 / 16;
        break;
    }
    max_bps = FFMIN(max_bps, INT_MAX);
    if (rc_mode == MPP_ENC_RC_MODE_CBR ||
        rc_mode == MPP_ENC_RC_MODE_VBR ||
        rc_mode == MPP_ENC_RC_MODE_AVBR) {
        mpp_enc_cfg_set_s32(cfg, "rc:bps_target", (int32_t)target_bps);
        mpp_enc_cfg_set_s32(cfg, "rc:bps_max", (int32_t)max_bps);
        mpp_enc_cfg_set_s32(cfg, "rc:bps_min", (int32_t)min_bps);
        av_log(avctx, AV_LOG_VERBOSE, "Bitrate Target/Min/Max is set to %"PRId32"/%"PRId32"/%"PRId32"\n",
               (int32_t)target_bps, (int32_t)min_bps, (int32_t)max_bps);
    }

    if (avctx->rc_buffer_size > 0 && max_bps > 0 &&
        (rc_mode == MPP_ENC_RC_MODE_CBR ||
         rc_mode == MPP_ENC_RC_MODE_VBR ||
         rc_mode == MPP_ENC_RC_MODE_AVBR)) {
        int stats_time_in_sec = avctx->rc_buffer_size / max_bps;
        if (stats_time_in_sec > 0) {
            mpp_enc_cfg_set_u32(cfg, "rc:stats_time", stats_time_in_sec);
            av_log(avctx, AV_LOG_VERBOSE, "Stats time is set to %d\n", stats_time_in_sec);
        }
    }

    mpp_enc_cfg_set_u32(cfg, "rc:drop_mode", MPP_ENC_RC_DROP_FRM_DISABLED);

    switch (avctx->codec_id) {
    case AV_CODEC_ID_H264:
    case AV_CODEC_ID_HEVC:
        {
            switch (rc_mode) {
            case MPP_ENC_RC_MODE_FIXQP:
                /* the frame QP stays qp_init; the bounds only clamp ROI QPs */
                qp_init = r->qp_init >= 0 ? r->qp_init : 26;
                qp_max   = r->qp_max   >= 0 ? FFMAX(r->qp_max,   qp_init) : qp_init;
                qp_min   = r->qp_min   >= 0 ? FFMIN(r->qp_min,   qp_init) : qp_init;
                qp_max_i = r->qp_max_i >= 0 ? FFMAX(r->qp_max_i, qp_init) : qp_max;
                qp_min_i = r->qp_min_i >= 0 ? FFMIN(r->qp_min_i, qp_init) : qp_min;
                mpp_enc_cfg_set_s32(cfg, "rc:qp_ip", 0);
                break;
            case MPP_ENC_RC_MODE_CBR:
            case MPP_ENC_RC_MODE_VBR:
            case MPP_ENC_RC_MODE_AVBR:
                qp_max = r->qp_max >= 0 ? r->qp_max : 48;
                qp_min = FFMIN(r->qp_min >= 0 ? r->qp_min : 0, qp_max);
                qp_max_i = r->qp_max_i >= 0 ? r->qp_max_i : 48;
                qp_min_i = FFMIN(r->qp_min_i >= 0 ? r->qp_min_i : 0, qp_max_i);
                qp_init = FFMIN3(r->qp_init >= 0 ? r->qp_init : 26, qp_max, qp_max_i);
                mpp_enc_cfg_set_s32(cfg, "rc:qp_ip", 2);
                break;
            default:
                return AVERROR(EINVAL);
            }
            mpp_enc_cfg_set_s32(cfg, "rc:qp_init", qp_init);
            mpp_enc_cfg_set_s32(cfg, "rc:qp_max", qp_max);
            mpp_enc_cfg_set_s32(cfg, "rc:qp_min", qp_min);
            mpp_enc_cfg_set_s32(cfg, "rc:qp_max_i",qp_max_i);
            mpp_enc_cfg_set_s32(cfg, "rc:qp_min_i", qp_min_i);

            /* Intra Refresh / GDR */
            if (r->intra_refresh && r->refresh_num) {
                mpp_enc_cfg_set_u32(cfg, "rc:refresh_en", 1);
                mpp_enc_cfg_set_u32(cfg, "rc:refresh_mode", r->refresh_mode);
                mpp_enc_cfg_set_u32(cfg, "rc:refresh_num", r->refresh_num);
                av_log(avctx, AV_LOG_VERBOSE, "Requested to use Intra Refresh, "
                       "Mode/Num is set to %d/%d\n", r->refresh_mode, r->refresh_num);
            }

            if ((ret = rkmpp_set_split_cfg(avctx, cfg)) < 0)
                return ret;
        }
        break;
    case AV_CODEC_ID_MJPEG:
        {
            int quality = r->qp_init;

            if (quality < 1 && avctx->global_quality > 0)
                quality = (avctx->flags & AV_CODEC_FLAG_QSCALE)
                          ? avctx->global_quality / FF_QP2LAMBDA : avctx->global_quality;
            qp_init = quality >= 1 ? av_clip(quality, 1, 99) : 80;
            qp_max = r->qp_max >= 1 ? r->qp_max : 99;
            qp_min = r->qp_min >= 1 ? r->qp_min : 1;
            qp_max_i = qp_min_i = 0;

            if (avctx->chroma_intra_matrix && !avctx->intra_matrix) {
                av_log(avctx, AV_LOG_ERROR, "chroma_intra_matrix needs intra_matrix\n");
                return AVERROR(EINVAL);
            }
            if (avctx->intra_matrix) {
                /* both in raster order, as MPP's tables */
                const uint16_t *luma   = avctx->intra_matrix;
                const uint16_t *chroma = avctx->chroma_intra_matrix ?
                                         avctx->chroma_intra_matrix : luma;
                uint8_t qtable_y[64], qtable_c[64];

                for (int i = 0; i < 64; i++) {
                    qtable_y[i] = av_clip(luma[i],   1, 255);
                    qtable_c[i] = av_clip(chroma[i], 1, 255);
                }
                /* MPP takes the tables only when no quality factor is set */
                if (mpp_enc_cfg_set_st(cfg, "jpeg:qtable_y", qtable_y) != MPP_OK ||
                    mpp_enc_cfg_set_st(cfg, "jpeg:qtable_u", qtable_c) != MPP_OK ||
                    mpp_enc_cfg_set_st(cfg, "jpeg:qtable_v", qtable_c) != MPP_OK) {
                    av_log(avctx, AV_LOG_ERROR, "Failed to set the quantization tables\n");
                    return AVERROR_EXTERNAL;
                }
                av_log(avctx, AV_LOG_VERBOSE, "Using the given quantization tables\n");
            } else {
                /* jpeg use special codec config to control qtable */
                mpp_enc_cfg_set_s32(cfg, "jpeg:q_factor", qp_init);
                mpp_enc_cfg_set_s32(cfg, "jpeg:qf_max", qp_max);
                mpp_enc_cfg_set_s32(cfg, "jpeg:qf_min", qp_min);
            }

            /* MPP writes a DRI and has the encoder emit RSTn every split_arg MCUs */
            if (r->restart_interval > 0) {
                mpp_enc_cfg_set_u32(cfg, "split:mode", MPP_ENC_SPLIT_BY_CTU);
                mpp_enc_cfg_set_u32(cfg, "split:arg", r->restart_interval);
                av_log(avctx, AV_LOG_VERBOSE, "Restart interval is set to %d MCUs\n",
                       r->restart_interval);
            }
        }
        break;
    default:
        return AVERROR(EINVAL);
    }

    av_log(avctx, AV_LOG_VERBOSE, "QP Init/Max/Min/Max_I/Min_I is set to %d/%d/%d/%d/%d\n",
           qp_init, qp_max, qp_min, qp_max_i, qp_min_i);

    switch (avctx->codec_id) {
    case AV_CODEC_ID_H264:
        {
            avctx->profile = r->profile;
            /* 4:0:0 needs chroma_format_idc, which only High and above carry */
            if (r->pix_fmt == AV_PIX_FMT_GRAY8 && avctx->profile != AV_PROFILE_H264_HIGH) {
                av_log(avctx, AV_LOG_WARNING, "Monochrome input needs the High profile, using it\n");
                avctx->profile = AV_PROFILE_H264_HIGH;
            }
            avctx->level = r->level;
            mpp_enc_cfg_set_s32(cfg, "h264:profile", avctx->profile);
            mpp_enc_cfg_set_s32(cfg, "h264:level", avctx->level);
            mpp_enc_cfg_set_s32(cfg, "h264:cabac_en", r->coder);
            mpp_enc_cfg_set_s32(cfg, "h264:cabac_idc", 0);
            mpp_enc_cfg_set_s32(cfg, "h264:trans8x8",
                                (r->dct8x8 && avctx->profile == AV_PROFILE_H264_HIGH));

            mpp_enc_cfg_set_s32(cfg, "h264:prefix_mode", r->prefix_mode);

            switch (avctx->profile) {
            case AV_PROFILE_H264_BASELINE:
                av_log(avctx, AV_LOG_VERBOSE, "Profile is set to BASELINE\n"); break;
            case AV_PROFILE_H264_MAIN:
                av_log(avctx, AV_LOG_VERBOSE, "Profile is set to MAIN\n"); break;
            case AV_PROFILE_H264_HIGH:
                av_log(avctx, AV_LOG_VERBOSE, "Profile is set to HIGH\n");
                if (r->dct8x8)
                    av_log(avctx, AV_LOG_VERBOSE, "8x8 Transform is enabled\n");
                break;
            }
            av_log(avctx, AV_LOG_VERBOSE, "Level is set to %d\n", avctx->level);
            av_log(avctx, AV_LOG_VERBOSE, "Coder is set to %s\n", r->coder ? "CABAC" : "CAVLC");
        }
        break;
    case AV_CODEC_ID_HEVC:
        {
            avctx->profile = r->pix_fmt == AV_PIX_FMT_GRAY8
                ? AV_PROFILE_HEVC_REXT : AV_PROFILE_HEVC_MAIN;
            avctx->level = r->level;
            mpp_enc_cfg_set_s32(cfg, "h265:profile", avctx->profile);
            mpp_enc_cfg_set_s32(cfg, "h265:level", avctx->level);
            if (avctx->level >= 120) {
                mpp_enc_cfg_set_s32(cfg, "h265:tier", r->tier);
                av_log(avctx, AV_LOG_VERBOSE, "Tier is set to %d\n", r->tier);
            }

            switch (avctx->profile) {
            case AV_PROFILE_HEVC_MAIN:
                av_log(avctx, AV_LOG_VERBOSE, "Profile is set to MAIN\n"); break;
            case AV_PROFILE_HEVC_REXT:
                av_log(avctx, AV_LOG_VERBOSE, "Profile is set to REXT\n"); break;
            }
            av_log(avctx, AV_LOG_VERBOSE, "Level is set to %d\n", avctx->level / 3);
        }
        break;
    case AV_CODEC_ID_MJPEG:
        break;
    default:
        return AVERROR(EINVAL);
    }

    if ((ret = r->mapi->control(r->mctx, MPP_ENC_SET_CFG, cfg)) != MPP_OK) {
        av_log(avctx, AV_LOG_ERROR, "Failed to set config: %d\n", ret);
        return AVERROR_EXTERNAL;
    }

    if (avctx->codec_id == AV_CODEC_ID_H264 ||
        avctx->codec_id == AV_CODEC_ID_HEVC) {
        if ((ret = rkmpp_set_ref_cfg(avctx)) < 0)
            return ret;

        /* user data SEI from the frame meta is written in any mode */
        sei_mode = (r->intra_refresh && r->refresh_num)
                   ? MPP_ENC_SEI_MODE_ONE_FRAME : MPP_ENC_SEI_MODE_DISABLE;
        if ((ret = r->mapi->control(r->mctx, MPP_ENC_SET_SEI_CFG, &sei_mode)) != MPP_OK) {
            av_log(avctx, AV_LOG_ERROR, "Failed to set SEI config: %d\n", ret);
            return AVERROR_EXTERNAL;
        }

        header_mode = (avctx->flags & AV_CODEC_FLAG_GLOBAL_HEADER)
                      ? MPP_ENC_HEADER_MODE_DEFAULT : MPP_ENC_HEADER_MODE_EACH_IDR;
        if ((ret = r->mapi->control(r->mctx, MPP_ENC_SET_HEADER_MODE, &header_mode)) != MPP_OK) {
            av_log(avctx, AV_LOG_ERROR, "Failed to set header mode: %d\n", ret);
            return AVERROR_EXTERNAL;
        }
    }

    return 0;
}

static int rkmpp_prepare_udu_sei_data(AVCodecContext *avctx, MPPEncFrame *mpp_enc_frame)
{
    int i, ret, sei_count = 0;

    if (!mpp_enc_frame ||
        !mpp_enc_frame->frame ||
        !mpp_enc_frame->mpp_frame)
        return AVERROR(EINVAL);

    /* user data unregistered SEI of H26X */
    for (i = 0; i < mpp_enc_frame->frame->nb_side_data; i++) {
        MppEncUserDataSet *mpp_sei_set = &mpp_enc_frame->mpp_sei_set;
        AVFrameSideData *sd = mpp_enc_frame->frame->side_data[i];
        uint8_t *user_data = sd->data;
        void *buf = NULL;

        if (sd->type != AV_FRAME_DATA_SEI_UNREGISTERED)
            continue;

        if (sd->size < AV_UUID_LEN) {
            av_log(avctx, AV_LOG_WARNING, "Invalid UDU SEI data: "
                   "(%zu < UUID(%d-bytes)), skipping\n",
                   sd->size, AV_UUID_LEN);
            continue;
        }

        buf = av_fast_realloc(mpp_sei_set->datas,
                              &mpp_sei_set->count,
                              (sei_count + 1) * sizeof(*(mpp_sei_set->datas)));
        if (!buf) {
            av_log(avctx, AV_LOG_ERROR, "Failed to realloc UDU SEI buffer\n");
            return AVERROR(ENOMEM);
        } else {
            mpp_sei_set->datas = (MppEncUserDataFull *)buf;

            mpp_sei_set->datas[sei_count].len   = sd->size - AV_UUID_LEN;
            mpp_sei_set->datas[sei_count].uuid  = (RK_U8 *)user_data;
            mpp_sei_set->datas[sei_count].pdata = &user_data[AV_UUID_LEN];

            mpp_sei_set->count = ++sei_count;
        }
    }

    if (sei_count > 0) {
        MppMeta mpp_meta = mpp_frame_get_meta(mpp_enc_frame->mpp_frame);
        if (!mpp_meta) {
            av_log(avctx, AV_LOG_ERROR, "Failed to get frame meta\n");
            return AVERROR_EXTERNAL;
        }
        if ((ret = mpp_meta_set_ptr(mpp_meta, KEY_USER_DATAS,
                                    &mpp_enc_frame->mpp_sei_set)) != MPP_OK) {
            av_log(avctx, AV_LOG_ERROR, "Failed to set the UDU SEI datas ptr\n");
            return AVERROR_EXTERNAL;
        }
    }

    return 0;
}

/*
 * The H.264/HEVC HALs keep the ROI pointer of the last frame that carried one,
 * so once ROI is used every later frame gets its own config, empty or not.
 */
static int rkmpp_prepare_roi(AVCodecContext *avctx, MPPEncFrame *mpp_enc_frame,
                             const AVFrame *frame)
{
    RKMPPEncContext *r = avctx->priv_data;
    const AVFrameSideData *sd = av_frame_get_side_data(frame, AV_FRAME_DATA_REGIONS_OF_INTEREST);
    MppEncROICfg *roi_cfg = &mpp_enc_frame->roi_cfg;
    MppMeta mpp_meta;

    if (sd && !r->roi_supported) {
        if (!r->roi_warned)
            av_log(avctx, AV_LOG_WARNING, "ROI is not supported by this SoC's encoder, ignoring it\n");
        r->roi_warned = 1;
        return 0;
    }
    if (!sd && !r->roi_active)
        return 0;

    roi_cfg->number  = 0;
    roi_cfg->regions = mpp_enc_frame->roi_regions;

    if (sd) {
        const AVRegionOfInterest *roi = (const AVRegionOfInterest *)sd->data;
        uint32_t roi_size = roi->self_size;
        int nb_roi;

        if (!roi_size || sd->size % roi_size) {
            av_log(avctx, AV_LOG_ERROR, "Invalid ROI side data\n");
            return AVERROR(EINVAL);
        }
        nb_roi = sd->size / roi_size;
        if (nb_roi > ENC_MAX_ROI_REGIONS && !r->roi_warned) {
            av_log(avctx, AV_LOG_WARNING, "More ROIs set than supported (%d > %d)\n",
                   nb_roi, ENC_MAX_ROI_REGIONS);
            r->roi_warned = 1;
        }

        for (int i = 0; i < nb_roi && roi_cfg->number < ENC_MAX_ROI_REGIONS; i++) {
            MppEncROIRegion *region = &mpp_enc_frame->roi_regions[roi_cfg->number];
            int left, top, right, bottom, qp_delta;

            roi = (const AVRegionOfInterest *)(sd->data + roi_size * i);
            if (!roi->qoffset.den) {
                av_log(avctx, AV_LOG_ERROR, "Invalid ROI quantisation offset\n");
                return AVERROR(EINVAL);
            }

            /* MPP takes 16x16 blocks and rejects regions past the frame */
            left   = ALIGN_DOWN(av_clip(roi->left, 0, avctx->width), 16);
            top    = ALIGN_DOWN(av_clip(roi->top,  0, avctx->height), 16);
            right  = FFMIN(FFALIGN(av_clip(roi->right,  0, avctx->width),  16), avctx->width);
            bottom = FFMIN(FFALIGN(av_clip(roi->bottom, 0, avctx->height), 16), avctx->height);
            if (right <= left || bottom <= top)
                continue;

            qp_delta = lrint(av_clipd(av_q2d(roi->qoffset), -1.0, 1.0) * 51);

            *region = (MppEncROIRegion) {
                .x       = left,
                .y       = top,
                .w       = right - left,
                .h       = bottom - top,
                .quality = qp_delta,
            };
            roi_cfg->number++;
            av_log(avctx, AV_LOG_DEBUG, "ROI: (%d,%d)-(%d,%d) -> %+d\n",
                   left, top, right, bottom, qp_delta);
        }
        r->roi_active = 1;
    }

    mpp_meta = mpp_frame_get_meta(mpp_enc_frame->mpp_frame);
    if (!mpp_meta ||
        mpp_meta_set_ptr(mpp_meta, KEY_ROI_DATA, roi_cfg) != MPP_OK) {
        av_log(avctx, AV_LOG_ERROR, "Failed to set the ROI config\n");
        return AVERROR_EXTERNAL;
    }

    return 0;
}

static int rkmpp_submit_frame(AVCodecContext *avctx, AVFrame *frame,
                              MPPEncFrame **out)
{
    RKMPPEncContext *r = avctx->priv_data;
    MppFrame mpp_frame = NULL;
    MppBuffer mpp_buf = NULL;
    AVFrame *drm_frame = NULL;
    const AVDRMFrameDescriptor *drm_desc;
    const AVDRMLayerDescriptor *layer;
    const AVDRMPlaneDescriptor *plane0;
    const AVPixFmtDescriptor *pix_desc = av_pix_fmt_desc_get(r->pix_fmt);
    const int is_planar = pix_desc->flags & AV_PIX_FMT_FLAG_PLANAR;
    const int is_rgb = pix_desc->flags & AV_PIX_FMT_FLAG_RGB;
    const int is_yuv = !is_rgb && pix_desc->nb_components >= 2;
    int hor_stride = 0, ver_stride = 0;
    MppBufferInfo buf_info = { 0 };
    MppFrameFormat mpp_fmt = r->mpp_fmt;
    uint64_t modifier;
    int ret, is_afbc = 0;

    MPPEncFrame *mpp_enc_frame = NULL;

    mpp_enc_frame = get_free_frame(&r->frame_list);
    if (!mpp_enc_frame)
        return AVERROR(ENOMEM);

    if (mpp_frame_init(&mpp_frame) != MPP_OK) {
        av_log(avctx, AV_LOG_ERROR, "Failed to init MPP frame\n");
        ret = AVERROR(ENOMEM);
        goto exit;
    }
    mpp_enc_frame->mpp_frame = mpp_frame;

    if (!frame) {
        av_log(avctx, AV_LOG_DEBUG, "End of stream\n");
        mpp_frame_set_eos(mpp_frame, 1);
        *out = mpp_enc_frame;
        return 0;
    }

    if (avctx->pix_fmt == AV_PIX_FMT_DRM_PRIME) {
        drm_frame = mpp_enc_frame->frame = av_frame_clone(frame);
        if (!drm_frame) {
            ret = AVERROR(ENOMEM);
            goto exit;
        }
    } else {
        AVBufferRef *hw_frames_ctx = frame->hw_frames_ctx;

        drm_frame = mpp_enc_frame->frame = av_frame_alloc();
        if (!drm_frame) {
            ret = AVERROR(ENOMEM);
            goto exit;
        }
        if ((ret = av_hwframe_get_buffer(r->hwframe, drm_frame, 0)) < 0) {
            av_log(avctx, AV_LOG_ERROR, "Cannot allocate an internal frame: %d\n", ret);
            goto exit;
        }
        frame->hw_frames_ctx = NULL; /* clear hwfc to avoid HW -> HW transfer */
        if ((ret = av_hwframe_transfer_data(drm_frame, frame, 0)) < 0) {
            av_log(avctx, AV_LOG_ERROR, "av_hwframe_transfer_data failed: %d\n", ret);
            frame->hw_frames_ctx = hw_frames_ctx;
            goto exit;
        }
        if ((ret = av_frame_copy_props(drm_frame, frame)) < 0) {
            av_log(avctx, AV_LOG_ERROR, "av_frame_copy_props failed: %d\n", ret);
            frame->hw_frames_ctx = hw_frames_ctx;
            goto exit;
        }
        frame->hw_frames_ctx = hw_frames_ctx; /* restore hwfc */
    }

    drm_desc = (AVDRMFrameDescriptor *)drm_frame->data[0];
    if (drm_desc->objects[0].fd < 0) {
        ret = AVERROR(EINVAL);
        goto exit;
    }

    /* planar YUV quirks */
    if ((r->pix_fmt == AV_PIX_FMT_YUV420P ||
         r->pix_fmt == AV_PIX_FMT_YUVJ420P ||
         r->pix_fmt == AV_PIX_FMT_YUV422P ||
         r->pix_fmt == AV_PIX_FMT_YUVJ422P ||
         r->pix_fmt == AV_PIX_FMT_NV24) && (drm_frame->width % 2)) {
        av_log(avctx, AV_LOG_ERROR, "Unsupported width '%d', not 2-aligned\n",
               drm_frame->width);
        ret = AVERROR(EINVAL);
        goto exit;
    }
    /* packed RGB/YUV quirks */
    if ((is_rgb || (is_yuv && !is_planar)) &&
        (drm_frame->width % 2 || drm_frame->height % 2)) {
        av_log(avctx, AV_LOG_ERROR, "Unsupported size '%dx%d', not 2-aligned\n",
               drm_frame->width, drm_frame->height);
        ret = AVERROR(EINVAL);
        goto exit;
    }

    mpp_frame_set_pts(mpp_frame, PTS_TO_MPP_PTS(drm_frame->pts, avctx->time_base));
    mpp_frame_set_width(mpp_frame, drm_frame->width);
    mpp_frame_set_height(mpp_frame, drm_frame->height);

    mpp_frame_set_colorspace(mpp_frame, rkmpp_normalize_colorspace(avctx->colorspace));
    mpp_frame_set_color_primaries(mpp_frame, rkmpp_normalize_color_primaries(avctx->color_primaries));
    mpp_frame_set_color_trc(mpp_frame, rkmpp_normalize_color_trc(avctx->color_trc));

    mpp_frame_set_color_range(mpp_frame, avctx->color_range);
    if (r->pix_fmt == AV_PIX_FMT_YUVJ420P ||
        r->pix_fmt == AV_PIX_FMT_YUVJ422P ||
        r->pix_fmt == AV_PIX_FMT_YUVJ444P) {
        mpp_frame_set_color_range(mpp_frame, AVCOL_RANGE_JPEG);
    }

    layer = &drm_desc->layers[0];
    plane0 = &layer->planes[0];

    modifier = drm_desc->objects[0].format_modifier;
    is_afbc = drm_is_afbc(modifier);
    if (drm_is_rfbc(modifier) || (is_afbc && !r->afbc_input)) {
        av_log(avctx, AV_LOG_ERROR, "%s compressed input is not supported by this SoC's "
               "%s encoder, insert scale_rkrga to convert it to linear\n",
               is_afbc ? "AFBC" : "RFBC", avcodec_get_name(avctx->codec_id));
        ret = AVERROR(ENOSYS);
        goto exit;
    }
    if (!is_afbc && modifier != DRM_FORMAT_MOD_LINEAR) {
        av_log(avctx, AV_LOG_ERROR, "Only linear and AFBC modifiers are supported\n");
        ret = AVERROR(ENOSYS);
        goto exit;
    }
    if (is_afbc) {
        uint32_t drm_afbc_fmt = rkmpp_get_drm_afbc_format(mpp_fmt);
        int afbc_offset_y = 0;

        if (drm_afbc_fmt != layer->format) {
            av_log(avctx, AV_LOG_ERROR, "Input format '%s' with AFBC modifier is not supported\n",
                   av_get_pix_fmt_name(r->pix_fmt));
            ret = AVERROR(ENOSYS);
            goto exit;
        }
        mpp_fmt |= MPP_FRAME_FBC_AFBC_V2;

        if (drm_frame->crop_top > 0) {
            afbc_offset_y = drm_frame->crop_top;
            mpp_frame_set_offset_y(mpp_frame, afbc_offset_y);
        }
    }
    mpp_frame_set_fmt(mpp_frame, mpp_fmt);

    if (is_afbc) {
        hor_stride = plane0->pitch;
        if ((ret = get_afbc_byte_stride(pix_desc, &hor_stride, 1)) < 0)
            goto exit;

        if (hor_stride % 16)
            hor_stride = FFALIGN(avctx->width, 16);

        mpp_frame_set_fbc_hdr_stride(mpp_frame, hor_stride);
    } else {
        ret = get_byte_stride(&drm_desc->objects[0],
                              &drm_desc->layers[0],
                              (pix_desc->flags & AV_PIX_FMT_FLAG_RGB),
                              (pix_desc->flags & AV_PIX_FMT_FLAG_PLANAR),
                              &hor_stride, &ver_stride);
        if (ret < 0 || !hor_stride || !ver_stride) {
            av_log(avctx, AV_LOG_ERROR, "Failed to get frame strides\n");
            ret = AVERROR(EINVAL);
            goto exit;
        }
        if ((ret = check_drm_layout(drm_desc, r->pix_fmt, hor_stride, ver_stride)) < 0) {
            av_log(avctx, AV_LOG_ERROR, "Unsupported DRM frame layout, the planes must "
                   "follow each other in one object at the luma pitch\n");
            goto exit;
        }

        mpp_frame_set_hor_stride(mpp_frame, hor_stride);
        mpp_frame_set_ver_stride(mpp_frame, ver_stride);
    }

    /* the prep config, which MPP reads the strides from, is set on the first frame */
    if (r->cfg_init && (is_afbc != r->cfg_afbc ||
                        (!is_afbc && (hor_stride != r->hor_stride ||
                                      ver_stride != r->ver_stride)))) {
        av_log(avctx, AV_LOG_ERROR, "Input frame layout changed mid-stream "
               "(%s %dx%d to %s %dx%d), which is not supported\n",
               r->cfg_afbc ? "AFBC" : "stride", r->hor_stride, r->ver_stride,
               is_afbc ? "AFBC" : "stride", hor_stride, ver_stride);
        ret = AVERROR(EINVAL);
        goto exit;
    }

    buf_info.type  = MPP_BUFFER_TYPE_DRM;
    buf_info.fd    = drm_desc->objects[0].fd;
    buf_info.size  = drm_desc->objects[0].size;

    if ((ret = mpp_buffer_import(&mpp_buf, &buf_info)) != MPP_OK) {
        av_log(avctx, AV_LOG_ERROR, "Failed to import MPP buffer: %d\n", ret);
        ret = AVERROR_EXTERNAL;
        goto exit;
    }
    /* the frame keeps its own reference */
    mpp_frame_set_buffer(mpp_frame, mpp_buf);
    mpp_buffer_put(mpp_buf);
    mpp_frame_set_buf_size(mpp_frame, drm_desc->objects[0].size);

    if (r->udu_sei &&
        (avctx->codec_id == AV_CODEC_ID_H264 ||
         avctx->codec_id == AV_CODEC_ID_HEVC)) {
        ret = rkmpp_prepare_udu_sei_data(avctx, mpp_enc_frame);
        if (ret < 0)
            goto exit;
    }

    if (avctx->codec_id == AV_CODEC_ID_H264 ||
        avctx->codec_id == AV_CODEC_ID_HEVC) {
        ret = rkmpp_prepare_roi(avctx, mpp_enc_frame, frame);
        if (ret < 0)
            goto exit;
    }

    /* request the IDR on this frame, not on whichever MPP dequeues next */
    if ((avctx->codec_id == AV_CODEC_ID_H264 ||
         avctx->codec_id == AV_CODEC_ID_HEVC) &&
        frame->pict_type == AV_PICTURE_TYPE_I) {
        MppMeta mpp_meta = mpp_frame_get_meta(mpp_frame);

        if (!mpp_meta ||
            mpp_meta_set_s32(mpp_meta, KEY_INPUT_IDR_REQ, 1) != MPP_OK) {
            av_log(avctx, AV_LOG_ERROR, "Failed to request an IDR frame\n");
            ret = AVERROR_EXTERNAL;
            goto exit;
        }
    }

    *out = mpp_enc_frame;
    return 0;

exit:
    release_frame(mpp_enc_frame);
    return ret;
}

static int rkmpp_send_frame(AVCodecContext *avctx, MPPEncFrame *mpp_enc_frame)
{
    RKMPPEncContext *r = avctx->priv_data;
    AVFrame *frame = mpp_enc_frame->frame;
    MppFrame mpp_frame = mpp_enc_frame->mpp_frame;
    int ret;

    if (frame && (ret = rkmpp_set_enc_cfg_prep(avctx, frame)) < 0)
        goto exit;

    if ((ret = r->mapi->encode_put_frame(r->mctx, mpp_frame)) != MPP_OK) {
        int log_level = (ret == MPP_NOK) ? AV_LOG_DEBUG : AV_LOG_ERROR;
        ret = (ret == MPP_NOK) ? AVERROR(EAGAIN) : AVERROR_EXTERNAL;
        av_log(avctx, log_level, "Failed to put frame to encoder input queue: %d\n", ret);
        goto exit;
    }
    mpp_enc_frame->in_mpp = 1;
    av_log(avctx, AV_LOG_DEBUG, "Wrote %ld bytes to encoder\n",
           mpp_frame_get_buf_size(mpp_frame));

exit:
    return ret;
}

/*
 * MPP's HEVC HALs patch nuh_temporal_id_plus1 from the syntax of the frame
 * prepared last, which with two frames in flight is the next one. The
 * packet meta carries the frame's own temporal id.
 */
static void rkmpp_fix_hevc_temporal_id(uint8_t *buf, int size, int tid)
{
    const uint8_t *p = buf, *end = buf + size;
    uint32_t state = UINT32_MAX;

    while (p < end) {
        p = avpriv_find_start_code(p, end, &state);
        if ((state & 0xffffff00) != 0x100 || p >= end)
            break;
        if (((state >> 1) & 0x3f) < 32)
            buf[p - buf] = (p[0] & 0xf8) | (tid + 1);
    }
}

static int rkmpp_get_packet(AVCodecContext *avctx, AVPacket *packet, int timeout)
{
    RKMPPEncContext *r = avctx->priv_data;
    MppPacket mpp_pkt = NULL;
    MppMeta mpp_meta = NULL;
    int key_frame = 0;
    int avg_qp = -1;
    int tid = -1;
    int ret;

    if ((ret = r->mapi->control(r->mctx, MPP_SET_OUTPUT_TIMEOUT, (MppParam)&timeout)) != MPP_OK) {
        av_log(avctx, AV_LOG_ERROR, "Failed to set output timeout: %d\n", ret);
        return AVERROR_EXTERNAL;
    }

    if ((ret = r->mapi->encode_get_packet(r->mctx, &mpp_pkt)) != MPP_OK) {
        int again = ret == MPP_NOK || ret == MPP_ERR_TIMEOUT;
        av_log(avctx, again ? AV_LOG_DEBUG : AV_LOG_ERROR,
               "Failed to get packet from encoder output queue: %d\n", ret);
        return again ? AVERROR(EAGAIN) : AVERROR_EXTERNAL;
    }
    if (!mpp_pkt)
        return AVERROR(ENOMEM);

    ret = release_input_frame(r->frame_list, mpp_pkt);

    if (mpp_packet_get_eos(mpp_pkt)) {
        av_log(avctx, AV_LOG_DEBUG, "Received an EOS packet\n");
        ret = AVERROR_EOF;
        goto exit;
    }
    av_log(avctx, AV_LOG_DEBUG, "Received a packet\n");

    if (ret < 0) {
        av_log(avctx, AV_LOG_ERROR, "Failed to get key input frame from packet meta\n");
        ret = AVERROR_EXTERNAL;
        goto exit;
    }

    /* a frame that failed, and each P frame MPP drops until the next IDR */
    if (!mpp_packet_get_length(mpp_pkt)) {
        av_log(avctx, AV_LOG_WARNING, "MPP failed to encode a frame, dropping its empty packet\n");
        ret = AVERROR(EAGAIN);
        goto exit;
    }

    /* freeing MppPacket data in buffer callbacks is not supported in async mode */
    {
        size_t mpp_pkt_length = mpp_packet_get_length(mpp_pkt);

        if ((ret = ff_get_encode_buffer(avctx, packet, mpp_pkt_length, 0)) < 0) {
            av_log(avctx, AV_LOG_ERROR, "ff_get_encode_buffer failed: %d\n", ret);
            goto exit;
        }
        memcpy(packet->data, mpp_packet_get_data(mpp_pkt), mpp_pkt_length);
    }

    packet->time_base.num = avctx->time_base.num;
    packet->time_base.den = avctx->time_base.den;
    packet->pts = MPP_PTS_TO_PTS(mpp_packet_get_pts(mpp_pkt), avctx->time_base);
    packet->dts = packet->pts;

    mpp_meta = mpp_packet_get_meta(mpp_pkt);
    if (!mpp_meta || !mpp_packet_has_meta(mpp_pkt)) {
        av_log(avctx, AV_LOG_ERROR, "Failed to get packet meta\n");
        ret = AVERROR_EXTERNAL;
        goto exit;
    }

    if (avctx->codec_id == AV_CODEC_ID_HEVC &&
        mpp_meta_get_s32(mpp_meta, KEY_TEMPORAL_ID, &tid) == MPP_OK &&
        tid >= 0 && tid < 7)
        rkmpp_fix_hevc_temporal_id(packet->data, packet->size, tid);

    mpp_meta_get_s32(mpp_meta, KEY_OUTPUT_INTRA, &key_frame);
    if (key_frame)
        packet->flags |= AV_PKT_FLAG_KEY;

    mpp_meta_get_s32(mpp_meta, KEY_ENC_AVERAGE_QP, &avg_qp);
    if (avg_qp >= 0)
        ff_encode_add_stats_side_data(packet, avg_qp * FF_QP2LAMBDA, NULL, 0,
                                      (packet->flags & AV_PKT_FLAG_KEY) ?
                                      AV_PICTURE_TYPE_I : AV_PICTURE_TYPE_P);

    mpp_packet_deinit(&mpp_pkt);
    return 0;

exit:
    if (mpp_pkt)
        mpp_packet_deinit(&mpp_pkt);

    return ret;
}

static int rkmpp_wait_packet(AVCodecContext *avctx, AVPacket *packet)
{
    int64_t deadline = av_gettime_relative() + ENC_WAIT_TIMEOUT_MS * 1000LL;
    int64_t left_ms = ENC_WAIT_TIMEOUT_MS;
    int ret;

    do {
        ret = rkmpp_get_packet(avctx, packet, FFMAX(left_ms, 1));
        left_ms = (deadline - av_gettime_relative()) / 1000;
    } while (ret == AVERROR(EAGAIN) && left_ms > 0);

    if (ret == AVERROR(EAGAIN)) {
        av_log(avctx, AV_LOG_ERROR, "Timed out waiting for the encoder output\n");
        ret = AVERROR(ETIMEDOUT);
    }

    return ret;
}

static int rkmpp_encode_frame(AVCodecContext *avctx, AVPacket *packet,
                              const AVFrame *frame, int *got_packet)
{
    RKMPPEncContext *r = avctx->priv_data;
    MPPEncFrame *mpp_enc_frame = NULL;
    int ret;
    int timeout = (avctx->codec_id == AV_CODEC_ID_H264 ||
                   avctx->codec_id == AV_CODEC_ID_HEVC ||
                   avctx->codec_id == AV_CODEC_ID_MJPEG) &&
                   !(avctx->flags & AV_CODEC_FLAG_LOW_DELAY)
                   ? MPP_TIMEOUT_NON_BLOCK : MPP_TIMEOUT_BLOCK;

    if (!r->eos_sent) {
        int64_t deadline = av_gettime_relative() + ENC_WAIT_TIMEOUT_MS * 1000LL;

        if ((ret = rkmpp_submit_frame(avctx, (AVFrame *)frame, &mpp_enc_frame)) < 0)
            return ret;

        while ((ret = rkmpp_send_frame(avctx, mpp_enc_frame)) == AVERROR(EAGAIN)) {
            if (av_gettime_relative() > deadline) {
                av_log(avctx, AV_LOG_ERROR, "Timed out putting a frame to the encoder\n");
                ret = AVERROR(ETIMEDOUT);
                break;
            }
            av_usleep(1000);
        }
        if (ret) {
            release_frame(mpp_enc_frame);
            return ret;
        }
        if (!frame)
            r->eos_sent = 1;
    }

    if (!frame || get_used_frame_count(r->frame_list) > r->async_frames)
        ret = rkmpp_wait_packet(avctx, packet);
    else
        ret = rkmpp_get_packet(avctx, packet, timeout);
    if (ret == AVERROR_EOF ||
        ret == AVERROR(EAGAIN))
        *got_packet = 0;
    else if (ret)
        return ret;
    else
        *got_packet = 1;

    return 0;
}

/* After a reset every frame still inside MPP sits in a packet on its output list. */
static void rkmpp_release_output(RKMPPEncContext *r)
{
    int timeout = MPP_TIMEOUT_NON_BLOCK;
    MppPacket mpp_pkt = NULL;

    if (r->mapi->control(r->mctx, MPP_SET_OUTPUT_TIMEOUT, (MppParam)&timeout) != MPP_OK)
        return;

    while (r->mapi->encode_get_packet(r->mctx, &mpp_pkt) == MPP_OK && mpp_pkt) {
        release_input_frame(r->frame_list, mpp_pkt);
        mpp_packet_deinit(&mpp_pkt);
    }
}

static av_cold int rkmpp_encode_close(AVCodecContext *avctx)
{
    RKMPPEncContext *r = avctx->priv_data;

    r->cfg_init = 0;
    r->async_frames = 0;

    if (r->mcfg) {
        mpp_enc_cfg_deinit(r->mcfg);
        r->mcfg = NULL;
    }

    if (r->mapi) {
        r->mapi->reset(r->mctx);
        rkmpp_release_output(r);
        mpp_destroy(r->mctx);
        r->mctx = NULL;
    }

    clear_frame_list(&r->frame_list);

    if (r->hwframe)
        av_buffer_unref(&r->hwframe);
    if (r->hwdevice)
        av_buffer_unref(&r->hwdevice);

    return 0;
}

static av_cold int init_hwframes_ctx(AVCodecContext *avctx)
{
    RKMPPEncContext *r = avctx->priv_data;
    AVHWFramesContext *hwfc;
    int ret;

    av_buffer_unref(&r->hwframe);
    r->hwframe = av_hwframe_ctx_alloc(r->hwdevice);
    if (!r->hwframe)
        return AVERROR(ENOMEM);

    hwfc            = (AVHWFramesContext *)r->hwframe->data;
    hwfc->format    = AV_PIX_FMT_DRM_PRIME;
    hwfc->sw_format = avctx->pix_fmt;
    hwfc->width     = avctx->width;
    hwfc->height    = avctx->height;

    ret = av_hwframe_ctx_init(r->hwframe);
    if (ret < 0) {
        av_buffer_unref(&r->hwframe);
        av_log(avctx, AV_LOG_ERROR, "Error creating internal frames_ctx: %d\n", ret);
        return ret;
    }

    return 0;
}

static av_cold int rkmpp_encode_init(AVCodecContext *avctx)
{
    RKMPPEncContext *r = avctx->priv_data;
    enum AVPixelFormat pix_fmt = AV_PIX_FMT_NONE;
    MppFrameFormat mpp_fmt = MPP_FMT_BUTT;
    MppCodingType coding_type = MPP_VIDEO_CodingUnused;
    MppPacket mpp_pkt = NULL;
    int input_timeout = MPP_TIMEOUT_NON_BLOCK;
    int output_timeout = MPP_TIMEOUT_NON_BLOCK;
    int ret;

    r->cfg_init = 0;
    r->async_frames = 0;

    if ((coding_type = rkmpp_get_coding_type(avctx)) == MPP_VIDEO_CodingUnused) {
        av_log(avctx, AV_LOG_ERROR, "Unknown codec id: %d\n", avctx->codec_id);
        return AVERROR(ENOSYS);
    }

    pix_fmt = avctx->pix_fmt == AV_PIX_FMT_DRM_PRIME ? avctx->sw_pix_fmt : avctx->pix_fmt;
    mpp_fmt = avctx->codec_id == AV_CODEC_ID_MJPEG
              ? rkmpp_get_mpp_fmt_mjpeg(pix_fmt) : rkmpp_get_mpp_fmt_h26x(pix_fmt);
    mpp_fmt &= MPP_FRAME_FMT_MASK;

    if (mpp_fmt == MPP_FMT_BUTT) {
        av_log(avctx, AV_LOG_ERROR, "Unsupported input pixel format '%s'\n",
               av_get_pix_fmt_name(pix_fmt));
        return AVERROR(ENOSYS);
    }
    if (avctx->codec_id == AV_CODEC_ID_MJPEG &&
        (av_pix_fmt_desc_get(pix_fmt)->flags & AV_PIX_FMT_FLAG_RGB) &&
        rkmpp_soc_is_any(jpeg_yuv_only_socs)) {
        av_log(avctx, AV_LOG_ERROR, "RGB input is not supported by this SoC's JPEG encoder, "
               "convert it to a YUV format such as nv12 first\n");
        return AVERROR(ENOSYS);
    }

    /* MPP would still suppress the periodic IDRs, leaving one IDR for the whole stream */
    if (avctx->codec_id == AV_CODEC_ID_HEVC && r->intra_refresh &&
        rkmpp_soc_is_any(hevc_no_refresh_socs)) {
        av_log(avctx, AV_LOG_ERROR, "Intra refresh is not supported by this SoC's HEVC encoder\n");
        return AVERROR(ENOSYS);
    }

    r->pix_fmt = pix_fmt;
    r->mpp_fmt = mpp_fmt;

    r->afbc_input = (avctx->codec_id == AV_CODEC_ID_H264 ||
                     avctx->codec_id == AV_CODEC_ID_HEVC) &&
                    rkmpp_soc_is_any(afbc_input_socs);
    r->roi_supported = !rkmpp_soc_is_any(no_roi_cfg_socs);

    if ((ret = mpp_check_support_format(MPP_CTX_ENC, coding_type)) != MPP_OK) {
        av_log(avctx, AV_LOG_ERROR, "MPP doesn't support encoding codec '%s' (%d)\n",
               avcodec_get_name(avctx->codec_id), avctx->codec_id);
        return AVERROR(ENOSYS);
    }

    if ((ret = mpp_create(&r->mctx, &r->mapi)) != MPP_OK) {
        av_log(avctx, AV_LOG_ERROR, "Failed to create MPP context and api: %d\n", ret);
        ret = AVERROR_EXTERNAL;
        goto fail;
    }

    if ((ret = r->mapi->control(r->mctx, MPP_SET_INPUT_TIMEOUT,
                                (MppParam)&input_timeout)) != MPP_OK) {
        av_log(avctx, AV_LOG_ERROR, "Failed to set input timeout: %d\n", ret);
        return AVERROR_EXTERNAL;
    }

    if ((ret = r->mapi->control(r->mctx, MPP_SET_OUTPUT_TIMEOUT,
                                (MppParam)&output_timeout)) != MPP_OK) {
        av_log(avctx, AV_LOG_ERROR, "Failed to set output timeout: %d\n", ret);
        return AVERROR_EXTERNAL;
    }

    if ((ret = mpp_init(r->mctx, MPP_CTX_ENC, coding_type)) != MPP_OK) {
        av_log(avctx, AV_LOG_ERROR, "Failed to init MPP context: %d\n", ret);
        ret = AVERROR_EXTERNAL;
        goto fail;
    }

    if ((ret = mpp_enc_cfg_init(&r->mcfg)) != MPP_OK) {
        av_log(avctx, AV_LOG_ERROR, "Failed to init encoder config: %d\n", ret);
        ret = AVERROR_EXTERNAL;
        goto fail;
    }

    if ((ret = r->mapi->control(r->mctx, MPP_ENC_GET_CFG, r->mcfg)) != MPP_OK) {
        av_log(avctx, AV_LOG_ERROR, "Failed to get encoder config: %d\n", ret);
        ret = AVERROR_EXTERNAL;
        goto fail;
    }

    if ((ret = rkmpp_set_enc_cfg(avctx)) < 0)
        goto fail;

    if (avctx->codec_id == AV_CODEC_ID_H264 ||
        avctx->codec_id == AV_CODEC_ID_HEVC)
        r->async_frames = H26X_ASYNC_FRAMES;
    else if (avctx->codec_id == AV_CODEC_ID_MJPEG)
        r->async_frames = MJPEG_ASYNC_FRAMES;

    if ((avctx->flags & AV_CODEC_FLAG_GLOBAL_HEADER) &&
        (avctx->codec_id == AV_CODEC_ID_H264 ||
         avctx->codec_id == AV_CODEC_ID_HEVC)) {
        RK_U8 enc_hdr_buf[H26X_HEADER_SIZE];
        size_t pkt_len = 0;
        void *pkt_pos = NULL;

        memset(enc_hdr_buf, 0, H26X_HEADER_SIZE);

        if ((ret = mpp_packet_init(&mpp_pkt,
                                   (void *)enc_hdr_buf,
                                   H26X_HEADER_SIZE)) != MPP_OK || !mpp_pkt) {
            av_log(avctx, AV_LOG_ERROR, "Failed to init extra info packet: %d\n", ret);
            ret = AVERROR_EXTERNAL;
            goto fail;
        }

        mpp_packet_set_length(mpp_pkt, 0);
        if ((ret = r->mapi->control(r->mctx, MPP_ENC_GET_HDR_SYNC, mpp_pkt)) != MPP_OK) {
            av_log(avctx, AV_LOG_ERROR, "Failed to get header sync: %d\n", ret);
            ret = AVERROR_EXTERNAL;
            goto fail;
        }

        pkt_pos = mpp_packet_get_pos(mpp_pkt);
        pkt_len = mpp_packet_get_length(mpp_pkt);

        if (avctx->extradata) {
            av_free(avctx->extradata);
            avctx->extradata = NULL;
        }
        avctx->extradata = av_malloc(pkt_len + AV_INPUT_BUFFER_PADDING_SIZE);
        if (!avctx->extradata) {
            ret = AVERROR(ENOMEM);
            goto fail;
        }
        avctx->extradata_size = pkt_len;
        memcpy(avctx->extradata, pkt_pos, pkt_len);
        memset(avctx->extradata + pkt_len, 0, AV_INPUT_BUFFER_PADDING_SIZE);
        mpp_packet_deinit(&mpp_pkt);
    }

    if (avctx->pix_fmt == AV_PIX_FMT_DRM_PRIME)
        return 0;

    if (avctx->hw_frames_ctx || avctx->hw_device_ctx) {
        AVBufferRef *device_ref = avctx->hw_device_ctx;
        AVHWDeviceContext *device_ctx = NULL;
        AVHWFramesContext *hwfc = NULL;

        if (avctx->hw_frames_ctx) {
            hwfc = (AVHWFramesContext *)avctx->hw_frames_ctx->data;
            device_ref = hwfc->device_ref;
        }
        device_ctx = (AVHWDeviceContext *)device_ref->data;

        if (device_ctx && device_ctx->type == AV_HWDEVICE_TYPE_RKMPP) {
            r->hwdevice = av_buffer_ref(device_ref);
            if (r->hwdevice)
                av_log(avctx, AV_LOG_VERBOSE, "Picked up an existing RKMPP hardware device\n");
        }
    }
    if (!r->hwdevice) {
        if ((ret = av_hwdevice_ctx_create(&r->hwdevice,
                                          AV_HWDEVICE_TYPE_RKMPP,
                                          NULL, NULL, 0)) < 0) {
            av_log(avctx, AV_LOG_ERROR, "Failed to create a RKMPP hardware device: %d\n", ret);
            goto fail;
        }
        av_log(avctx, AV_LOG_VERBOSE, "Created a RKMPP hardware device\n");
    }

    ret = init_hwframes_ctx(avctx);
    if (ret < 0)
        goto fail;

    return 0;

fail:
    if (mpp_pkt)
        mpp_packet_deinit(&mpp_pkt);

    return ret;
}

#if CONFIG_H264_RKMPP_ENCODER
DEFINE_RKMPP_ENCODER(h264, H264, h26x)
#endif
#if CONFIG_HEVC_RKMPP_ENCODER
DEFINE_RKMPP_ENCODER(hevc, HEVC, h26x)
#endif
#if CONFIG_MJPEG_RKMPP_ENCODER
DEFINE_RKMPP_ENCODER(mjpeg, MJPEG, mjpeg)
#endif
