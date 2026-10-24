/*
 * KMS/DRM output device
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

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include <drm.h>
#include <drm_fourcc.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#include "libavutil/file_open.h"
#include "libavutil/frame.h"
#include "libavutil/hwcontext_drm.h"
#include "libavutil/avstring.h"
#include "libavutil/imgutils.h"
#include "libavutil/log.h"
#include "libavutil/mem.h"
#include "libavutil/opt.h"
#include "libavutil/pixdesc.h"
#include "libavformat/mux.h"
#include "avdevice.h"

#define NB_DUMB 3

enum {
    PLANE_FB_ID, PLANE_CRTC_ID,
    PLANE_SRC_X, PLANE_SRC_Y, PLANE_SRC_W, PLANE_SRC_H,
    PLANE_CRTC_X, PLANE_CRTC_Y, PLANE_CRTC_W, PLANE_CRTC_H,
    NB_PLANE_PROPS
};

static const char *const plane_prop_names[NB_PLANE_PROPS] = {
    "FB_ID", "CRTC_ID", "SRC_X", "SRC_Y", "SRC_W", "SRC_H",
    "CRTC_X", "CRTC_Y", "CRTC_W", "CRTC_H",
};

typedef struct KMSDumb {
    uint32_t handle;
    uint32_t fb_id;
    uint64_t size;
    uint8_t *map;
    uint8_t *data[4];
    int linesize[4];
} KMSDumb;

typedef struct KMSContext {
    const AVClass *class;

    int64_t connector_opt;
    int64_t crtc_opt;
    int64_t plane_opt;
    int scale;

    int fd;
    uint32_t connector_id;
    uint32_t crtc_id;
    int crtc_index;
    drmModeModeInfo mode;
    uint32_t mode_blob;
    int modeset;
    uint32_t conn_crtc_prop;
    uint32_t crtc_mode_prop;
    uint32_t crtc_active_prop;

    uint32_t plane_id;
    uint32_t plane_format;
    uint64_t plane_modifier;
    uint32_t plane_props[NB_PLANE_PROPS];
    uint32_t zpos_prop;
    uint64_t zpos;
    int scale_ok;

    uint32_t shown_fb;
    int shown_fb_owned;
    AVFrame *shown_frame;

    KMSDumb dumb[NB_DUMB];
    int dumb_next;
    int dumb_width, dumb_height;
    enum AVPixelFormat dumb_format;
} KMSContext;

static const struct {
    enum AVPixelFormat pix_fmt;
    uint32_t drm_format;
} kms_formats[] = {
    { AV_PIX_FMT_NV12,     DRM_FORMAT_NV12     },
    { AV_PIX_FMT_NV21,     DRM_FORMAT_NV21     },
    { AV_PIX_FMT_NV16,     DRM_FORMAT_NV16     },
    { AV_PIX_FMT_NV24,     DRM_FORMAT_NV24     },
#ifdef DRM_FORMAT_NV15
    { AV_PIX_FMT_NV15,     DRM_FORMAT_NV15     },
#endif
    { AV_PIX_FMT_P010LE,   DRM_FORMAT_P010     },
    { AV_PIX_FMT_YUV420P,  DRM_FORMAT_YUV420   },
    { AV_PIX_FMT_YUYV422,  DRM_FORMAT_YUYV     },
    { AV_PIX_FMT_UYVY422,  DRM_FORMAT_UYVY     },
    { AV_PIX_FMT_BGR0,     DRM_FORMAT_XRGB8888 },
    { AV_PIX_FMT_BGRA,     DRM_FORMAT_ARGB8888 },
    { AV_PIX_FMT_RGB0,     DRM_FORMAT_XBGR8888 },
    { AV_PIX_FMT_RGBA,     DRM_FORMAT_ABGR8888 },
    { AV_PIX_FMT_RGB565LE, DRM_FORMAT_RGB565   },
    { AV_PIX_FMT_RGB24,    DRM_FORMAT_BGR888   },
    { AV_PIX_FMT_BGR24,    DRM_FORMAT_RGB888   },
};

static uint32_t kms_drm_format(enum AVPixelFormat pix_fmt)
{
    for (int i = 0; i < FF_ARRAY_ELEMS(kms_formats); i++)
        if (kms_formats[i].pix_fmt == pix_fmt)
            return kms_formats[i].drm_format;
    return 0;
}

static uint32_t find_prop(int fd, uint32_t obj_id, uint32_t obj_type,
                          const char *name, uint64_t *value)
{
    drmModeObjectPropertiesPtr props = drmModeObjectGetProperties(fd, obj_id, obj_type);
    uint32_t id = 0;

    if (!props)
        return 0;
    for (uint32_t i = 0; i < props->count_props && !id; i++) {
        drmModePropertyPtr prop = drmModeGetProperty(fd, props->props[i]);

        if (prop && !strcmp(prop->name, name)) {
            id = prop->prop_id;
            if (value)
                *value = props->prop_values[i];
        }
        drmModeFreeProperty(prop);
    }
    drmModeFreeObjectProperties(props);
    return id;
}

static int plane_supports(int fd, const drmModePlane *plane,
                          uint32_t format, uint64_t modifier)
{
    drmModePropertyBlobPtr blob = NULL;
    uint64_t blob_id = 0;
    int ret = 0;

    if (find_prop(fd, plane->plane_id, DRM_MODE_OBJECT_PLANE, "IN_FORMATS", &blob_id) &&
        blob_id && (blob = drmModeGetPropertyBlob(fd, blob_id))) {
        const struct drm_format_modifier_blob *h = blob->data;
        const uint32_t *formats = (const uint32_t *)((const uint8_t *)h + h->formats_offset);
        const struct drm_format_modifier *mods =
            (const struct drm_format_modifier *)((const uint8_t *)h + h->modifiers_offset);

        for (uint32_t i = 0; i < h->count_formats && !ret; i++) {
            if (formats[i] != format)
                continue;
            for (uint32_t j = 0; j < h->count_modifiers && !ret; j++)
                ret = mods[j].modifier == modifier && i >= mods[j].offset &&
                      i < mods[j].offset + 64 &&
                      (mods[j].formats >> (i - mods[j].offset)) & 1;
        }
        drmModeFreePropertyBlob(blob);
        return ret;
    }

    if (modifier != DRM_FORMAT_MOD_LINEAR)
        return 0;
    for (uint32_t i = 0; i < plane->count_formats; i++)
        if (plane->formats[i] == format)
            return 1;
    return 0;
}

static int kms_select_plane(AVFormatContext *avctx, uint32_t format, uint64_t modifier)
{
    KMSContext *k = avctx->priv_data;
    drmModePlaneResPtr res = drmModeGetPlaneResources(k->fd);
    uint32_t found = 0;
    uint64_t found_type = DRM_PLANE_TYPE_PRIMARY;
    int ret;

    if (!res) {
        ret = AVERROR(errno);
        av_log(avctx, AV_LOG_ERROR, "Cannot get the plane resources: %s\n", av_err2str(ret));
        return ret;
    }

    /* prefer the primary plane, then an overlay plane */
    for (int pass = 0; pass < 2 && !found; pass++) {
        for (uint32_t i = 0; i < res->count_planes && !found; i++) {
            drmModePlanePtr plane = drmModeGetPlane(k->fd, res->planes[i]);
            uint64_t type = DRM_PLANE_TYPE_OVERLAY;

            if (!plane)
                continue;
            find_prop(k->fd, plane->plane_id, DRM_MODE_OBJECT_PLANE, "type", &type);
            if (k->plane_opt ? plane->plane_id == k->plane_opt
                             : type == (pass ? DRM_PLANE_TYPE_OVERLAY : DRM_PLANE_TYPE_PRIMARY)) {
                if ((plane->possible_crtcs & (1u << k->crtc_index)) &&
                    plane_supports(k->fd, plane, format, modifier)) {
                    found      = plane->plane_id;
                    found_type = type;
                }
                else if (k->plane_opt)
                    pass = 2;
            }
            drmModeFreePlane(plane);
        }
    }
    drmModeFreePlaneResources(res);

    if (!found) {
        av_log(avctx, AV_LOG_ERROR, "No plane on CRTC %u can show %.4s with modifier "
               "0x%"PRIx64"\n", k->crtc_id, (const char *)&format, modifier);
        return AVERROR(EINVAL);
    }

    for (int i = 0; i < NB_PLANE_PROPS; i++) {
        k->plane_props[i] = find_prop(k->fd, found, DRM_MODE_OBJECT_PLANE,
                                      plane_prop_names[i], NULL);
        if (!k->plane_props[i]) {
            av_log(avctx, AV_LOG_ERROR, "Plane %u has no %s property\n",
                   found, plane_prop_names[i]);
            return AVERROR(EINVAL);
        }
    }

    /* an overlay must go above the primary plane, which may still show the console */
    k->zpos_prop = 0;
    if (found_type != DRM_PLANE_TYPE_PRIMARY &&
        (k->zpos_prop = find_prop(k->fd, found, DRM_MODE_OBJECT_PLANE, "zpos", NULL))) {
        drmModePropertyPtr prop = drmModeGetProperty(k->fd, k->zpos_prop);

        if (prop && !(prop->flags & DRM_MODE_PROP_IMMUTABLE) &&
            (prop->flags & DRM_MODE_PROP_RANGE) && prop->count_values == 2)
            k->zpos = prop->values[1];
        else
            k->zpos_prop = 0;
        drmModeFreeProperty(prop);
    }

    if (k->plane_id != found)
        av_log(avctx, AV_LOG_VERBOSE, "Using plane %u for %.4s\n", found, (const char *)&format);
    k->plane_id       = found;
    k->plane_format   = format;
    k->plane_modifier = modifier;
    k->scale_ok       = k->scale;
    return 0;
}

static int kms_select_output(AVFormatContext *avctx)
{
    KMSContext *k = avctx->priv_data;
    drmModeResPtr res = drmModeGetResources(k->fd);
    drmModeConnectorPtr conn = NULL;
    drmModeEncoderPtr enc = NULL;
    drmModeCrtcPtr crtc = NULL;
    uint32_t cur_crtc = 0, possible = 0;
    int ret = AVERROR(ENODEV);

    if (!res) {
        ret = AVERROR(errno);
        av_log(avctx, AV_LOG_ERROR, "Cannot get the DRM resources: %s\n", av_err2str(ret));
        return ret;
    }

    for (int i = 0; i < res->count_connectors && !conn; i++) {
        conn = drmModeGetConnector(k->fd, res->connectors[i]);
        if (conn && (k->connector_opt ? conn->connector_id == k->connector_opt
                                      : conn->connection == DRM_MODE_CONNECTED &&
                                        conn->count_modes > 0))
            break;
        drmModeFreeConnector(conn);
        conn = NULL;
    }
    if (!conn || conn->connection != DRM_MODE_CONNECTED || !conn->count_modes) {
        av_log(avctx, AV_LOG_ERROR, "No connected connector%s found\n",
               k->connector_opt ? " with that id" : "");
        goto end;
    }
    k->connector_id = conn->connector_id;

    if (conn->encoder_id && (enc = drmModeGetEncoder(k->fd, conn->encoder_id)))
        cur_crtc = enc->crtc_id;

    for (int j = 0; j < conn->count_encoders; j++) {
        drmModeEncoderPtr e = drmModeGetEncoder(k->fd, conn->encoders[j]);
        if (e)
            possible |= e->possible_crtcs;
        drmModeFreeEncoder(e);
    }

    for (int i = 0; i < res->count_crtcs && !k->crtc_id; i++) {
        if (k->crtc_opt ? res->crtcs[i] == k->crtc_opt
                        : cur_crtc ? res->crtcs[i] == cur_crtc
                                   : !!(possible & (1u << i))) {
            if (!(possible & (1u << i))) {
                av_log(avctx, AV_LOG_ERROR, "CRTC %u cannot drive connector %u\n",
                       res->crtcs[i], k->connector_id);
                goto end;
            }
            k->crtc_id    = res->crtcs[i];
            k->crtc_index = i;
        }
    }
    if (!k->crtc_id) {
        av_log(avctx, AV_LOG_ERROR, "No CRTC found for connector %u\n", k->connector_id);
        goto end;
    }

    crtc = drmModeGetCrtc(k->fd, k->crtc_id);
    if (crtc && crtc->mode_valid && cur_crtc == k->crtc_id) {
        k->mode = crtc->mode;
    } else {
        k->mode = conn->modes[0];
        for (int i = 0; i < conn->count_modes; i++)
            if (conn->modes[i].type & DRM_MODE_TYPE_PREFERRED) {
                k->mode = conn->modes[i];
                break;
            }
        k->modeset = 1;
    }

    k->conn_crtc_prop   = find_prop(k->fd, k->connector_id, DRM_MODE_OBJECT_CONNECTOR, "CRTC_ID", NULL);
    k->crtc_mode_prop   = find_prop(k->fd, k->crtc_id, DRM_MODE_OBJECT_CRTC, "MODE_ID", NULL);
    k->crtc_active_prop = find_prop(k->fd, k->crtc_id, DRM_MODE_OBJECT_CRTC, "ACTIVE", NULL);
    if (!k->conn_crtc_prop || !k->crtc_mode_prop || !k->crtc_active_prop) {
        av_log(avctx, AV_LOG_ERROR, "Missing atomic connector or CRTC properties\n");
        goto end;
    }
    if (k->modeset &&
        (ret = drmModeCreatePropertyBlob(k->fd, &k->mode, sizeof(k->mode), &k->mode_blob)) < 0) {
        ret = AVERROR(errno);
        av_log(avctx, AV_LOG_ERROR, "Cannot create the mode blob: %s\n", av_err2str(ret));
        goto end;
    }

    av_log(avctx, AV_LOG_INFO, "Connector %u, CRTC %u, mode %s %dHz%s\n",
           k->connector_id, k->crtc_id, k->mode.name, k->mode.vrefresh,
           k->modeset ? " (mode set)" : "");
    ret = 0;
end:
    drmModeFreeCrtc(crtc);
    drmModeFreeEncoder(enc);
    drmModeFreeConnector(conn);
    drmModeFreeResources(res);
    return ret;
}

static int kms_commit(AVFormatContext *avctx, uint32_t fb_id, const AVFrame *frame)
{
    KMSContext *k = avctx->priv_data;
    int src_x = frame->crop_left, src_y = frame->crop_top;
    int src_w = frame->width  - frame->crop_left - frame->crop_right;
    int src_h = frame->height - frame->crop_top  - frame->crop_bottom;
    int mode_w = k->mode.hdisplay, mode_h = k->mode.vdisplay;
    int ret = 0;

    for (int attempt = 0; attempt < 2; attempt++) {
        drmModeAtomicReqPtr req;
        uint32_t flags = 0;
        int sx = src_x, sy = src_y, sw = src_w, sh = src_h, dw, dh;
        AVRational sar = frame->sample_aspect_ratio.num ? frame->sample_aspect_ratio
                                                        : (AVRational){ 1, 1 };

        if (k->scale_ok) {
            int64_t disp_w = av_rescale(sw, sar.num, sar.den);

            dw = mode_w;
            dh = av_rescale(sh, mode_w, disp_w);
            if (dh > mode_h) {
                dh = mode_h;
                dw = av_rescale(disp_w, mode_h, sh);
            }
            dw = FFMAX(dw & ~1, 2);
            dh = FFMAX(dh & ~1, 2);
        } else {
            dw = FFMIN(sw, mode_w);
            dh = FFMIN(sh, mode_h);
            sx += (sw - dw) / 2;
            sy += (sh - dh) / 2;
            sw = dw;
            sh = dh;
        }

        if (!(req = drmModeAtomicAlloc()))
            return AVERROR(ENOMEM);
        if (k->modeset) {
            drmModeAtomicAddProperty(req, k->connector_id, k->conn_crtc_prop, k->crtc_id);
            drmModeAtomicAddProperty(req, k->crtc_id, k->crtc_mode_prop, k->mode_blob);
            drmModeAtomicAddProperty(req, k->crtc_id, k->crtc_active_prop, 1);
            flags |= DRM_MODE_ATOMIC_ALLOW_MODESET;
        }
        drmModeAtomicAddProperty(req, k->plane_id, k->plane_props[PLANE_FB_ID],   fb_id);
        drmModeAtomicAddProperty(req, k->plane_id, k->plane_props[PLANE_CRTC_ID], k->crtc_id);
        drmModeAtomicAddProperty(req, k->plane_id, k->plane_props[PLANE_SRC_X], (uint64_t)sx << 16);
        drmModeAtomicAddProperty(req, k->plane_id, k->plane_props[PLANE_SRC_Y], (uint64_t)sy << 16);
        drmModeAtomicAddProperty(req, k->plane_id, k->plane_props[PLANE_SRC_W], (uint64_t)sw << 16);
        drmModeAtomicAddProperty(req, k->plane_id, k->plane_props[PLANE_SRC_H], (uint64_t)sh << 16);
        drmModeAtomicAddProperty(req, k->plane_id, k->plane_props[PLANE_CRTC_X], (mode_w - dw) / 2);
        drmModeAtomicAddProperty(req, k->plane_id, k->plane_props[PLANE_CRTC_Y], (mode_h - dh) / 2);
        drmModeAtomicAddProperty(req, k->plane_id, k->plane_props[PLANE_CRTC_W], dw);
        drmModeAtomicAddProperty(req, k->plane_id, k->plane_props[PLANE_CRTC_H], dh);
        if (k->zpos_prop)
            drmModeAtomicAddProperty(req, k->plane_id, k->zpos_prop, k->zpos);

        ret = drmModeAtomicCommit(k->fd, req, flags, NULL) < 0 ? AVERROR(errno) : 0;
        drmModeAtomicFree(req);

        if (!ret) {
            k->modeset = 0;
            return 0;
        }
        if (ret == AVERROR(EACCES) || ret == AVERROR(EPERM)) {
            av_log(avctx, AV_LOG_ERROR, "Atomic commit refused: this process is not "
                   "the DRM master\n");
            return ret;
        }
        if (!k->scale_ok)
            break;
        av_log(avctx, AV_LOG_WARNING, "Plane %u cannot scale %dx%d to %dx%d (%s), "
               "showing the frame unscaled\n", k->plane_id, sw, sh, dw, dh, av_err2str(ret));
        k->scale_ok = 0;
    }

    av_log(avctx, AV_LOG_ERROR, "Atomic commit failed: %s\n", av_err2str(ret));
    return ret;
}

static void kms_gem_close(int fd, uint32_t handle)
{
    struct drm_gem_close arg = { .handle = handle };

    drmIoctl(fd, DRM_IOCTL_GEM_CLOSE, &arg);
}

static int kms_prime_fb(AVFormatContext *avctx, const AVFrame *frame, uint32_t *fb_id,
                        uint32_t *format, uint64_t *modifier)
{
    KMSContext *k = avctx->priv_data;
    const AVDRMFrameDescriptor *desc = (const AVDRMFrameDescriptor *)frame->data[0];
    uint32_t handles[AV_DRM_MAX_PLANES] = { 0 }, obj_handles[AV_DRM_MAX_PLANES] = { 0 };
    uint32_t pitches[4] = { 0 }, offsets[4] = { 0 };
    uint64_t modifiers[4] = { 0 };
    int n = 0, ret = 0;

    if (desc->nb_layers != 1 || desc->nb_objects < 1 || desc->nb_objects > AV_DRM_MAX_PLANES) {
        av_log(avctx, AV_LOG_ERROR, "Unsupported DRM frame with %d layers\n", desc->nb_layers);
        return AVERROR(EINVAL);
    }

    *format   = desc->layers[0].format;
    *modifier = desc->objects[0].format_modifier;
    if (*modifier == DRM_FORMAT_MOD_INVALID)
        *modifier = DRM_FORMAT_MOD_LINEAR;

    for (int i = 0; i < desc->nb_objects; i++) {
        if (drmPrimeFDToHandle(k->fd, desc->objects[i].fd, &obj_handles[i]) < 0) {
            ret = AVERROR(errno);
            av_log(avctx, AV_LOG_ERROR, "Cannot import DMA-BUF %d: %s\n",
                   desc->objects[i].fd, av_err2str(ret));
            goto end;
        }
    }
    for (int i = 0; i < desc->layers[0].nb_planes && n < 4; i++, n++) {
        const AVDRMPlaneDescriptor *p = &desc->layers[0].planes[i];

        if (p->object_index >= desc->nb_objects) {
            ret = AVERROR(EINVAL);
            goto end;
        }
        handles[n]   = obj_handles[p->object_index];
        pitches[n]   = p->pitch;
        offsets[n]   = p->offset;
        modifiers[n] = *modifier;
    }

    if (drmModeAddFB2WithModifiers(k->fd, frame->width, frame->height, *format,
                                   handles, pitches, offsets,
                                   *modifier != DRM_FORMAT_MOD_LINEAR ? modifiers : NULL,
                                   fb_id,
                                   *modifier != DRM_FORMAT_MOD_LINEAR ? DRM_MODE_FB_MODIFIERS : 0) < 0) {
        ret = AVERROR(errno);
        av_log(avctx, AV_LOG_ERROR, "Cannot add a %dx%d %.4s framebuffer: %s\n",
               frame->width, frame->height, (const char *)format, av_err2str(ret));
    }

end:
    /* the framebuffer keeps the buffers referenced */
    for (int i = 0; i < desc->nb_objects; i++) {
        int dup = 0;
        for (int j = 0; j < i; j++)
            dup |= obj_handles[j] == obj_handles[i];
        if (obj_handles[i] && !dup)
            kms_gem_close(k->fd, obj_handles[i]);
    }
    return ret;
}

static void kms_free_dumb(KMSContext *k)
{
    for (int i = 0; i < NB_DUMB; i++) {
        KMSDumb *d = &k->dumb[i];

        if (d->fb_id && d->fb_id == k->shown_fb)
            k->shown_fb_owned = 1;
        else if (d->fb_id)
            drmModeRmFB(k->fd, d->fb_id);
        if (d->map)
            munmap(d->map, d->size);
        if (d->handle) {
            struct drm_mode_destroy_dumb arg = { .handle = d->handle };
            drmIoctl(k->fd, DRM_IOCTL_MODE_DESTROY_DUMB, &arg);
        }
        memset(d, 0, sizeof(*d));
    }
}

static int kms_alloc_dumb(AVFormatContext *avctx, const AVFrame *frame, uint32_t format)
{
    KMSContext *k = avctx->priv_data;
    ptrdiff_t linesizes[4];
    size_t sizes[4];
    int ls[4], ret;

    if ((ret = av_image_fill_linesizes(ls, frame->format, frame->width)) < 0)
        return ret;

    for (int i = 0; i < NB_DUMB; i++) {
        KMSDumb *d = &k->dumb[i];
        struct drm_mode_create_dumb create = { .width = FFALIGN(ls[0], 64), .bpp = 8 };
        struct drm_mode_map_dumb map = { 0 };
        uint32_t handles[4] = { 0 }, pitches[4] = { 0 }, offsets[4] = { 0 };
        size_t total = 0;
        int nb_planes = av_pix_fmt_count_planes(frame->format);

        /* first pass sizes the buffer with the likely pitch, the second uses the real one */
        for (int pass = 0; pass < 2; pass++) {
            uint32_t pitch = pass ? create.pitch : create.width;

            for (int p = 0; p < 4; p++)
                linesizes[p] = (ptrdiff_t)ls[p] * pitch / ls[0];
            if ((ret = av_image_fill_plane_sizes(sizes, frame->format, frame->height,
                                                 linesizes)) < 0)
                return ret;
            total = 0;
            for (int p = 0; p < nb_planes; p++)
                total += sizes[p];
            if (!pass) {
                create.height = (total + pitch - 1) / pitch;
                if (drmIoctl(k->fd, DRM_IOCTL_MODE_CREATE_DUMB, &create) < 0) {
                    ret = AVERROR(errno);
                    av_log(avctx, AV_LOG_ERROR, "Cannot create a dumb buffer: %s\n",
                           av_err2str(ret));
                    return ret;
                }
                d->handle = create.handle;
                d->size   = create.size;
            }
        }
        if (total > d->size) {
            av_log(avctx, AV_LOG_ERROR, "Dumb buffer too small for the frame\n");
            return AVERROR(EINVAL);
        }

        map.handle = d->handle;
        if (drmIoctl(k->fd, DRM_IOCTL_MODE_MAP_DUMB, &map) < 0)
            return AVERROR(errno);
        d->map = mmap(NULL, d->size, PROT_READ | PROT_WRITE, MAP_SHARED, k->fd, map.offset);
        if (d->map == MAP_FAILED) {
            d->map = NULL;
            return AVERROR(errno);
        }

        for (int p = 0, off = 0; p < nb_planes; off += sizes[p], p++) {
            d->data[p]     = d->map + off;
            d->linesize[p] = linesizes[p];
            handles[p]     = d->handle;
            pitches[p]     = linesizes[p];
            offsets[p]     = off;
        }
        if (drmModeAddFB2(k->fd, frame->width, frame->height, format,
                          handles, pitches, offsets, &d->fb_id, 0) < 0) {
            ret = AVERROR(errno);
            av_log(avctx, AV_LOG_ERROR, "Cannot add a %dx%d %s framebuffer: %s\n",
                   frame->width, frame->height, av_get_pix_fmt_name(frame->format),
                   av_err2str(ret));
            return ret;
        }
    }

    k->dumb_width  = frame->width;
    k->dumb_height = frame->height;
    k->dumb_format = frame->format;
    return 0;
}

/* The output name is a device path, or the name of the DRM driver to look for. */
static int kms_open(AVFormatContext *avctx, const char *name, char *path, size_t size)
{
    int ret = AVERROR(ENODEV);

    if (!*name || strchr(name, '/')) {
        av_strlcpy(path, *name ? name : "/dev/dri/card0", size);
        if ((ret = avpriv_open(path, O_RDWR)) < 0) {
            ret = AVERROR(errno);
            av_log(avctx, AV_LOG_ERROR, "Cannot open %s: %s\n", path, av_err2str(ret));
        }
        return ret;
    }

    for (int i = 0; i < 64; i++) {
        drmVersionPtr ver;
        int fd;

        snprintf(path, size, DRM_DEV_NAME, DRM_DIR_NAME, i);
        if ((fd = avpriv_open(path, O_RDWR)) < 0)
            continue;
        ver = drmGetVersion(fd);
        if (ver && !strcmp(ver->name, name)) {
            drmFreeVersion(ver);
            return fd;
        }
        drmFreeVersion(ver);
        close(fd);
    }
    av_log(avctx, AV_LOG_ERROR, "No DRM device with the driver %s\n", name);
    return ret;
}

static int kms_init(AVFormatContext *avctx)
{
    KMSContext *k = avctx->priv_data;

    k->fd = -1;
    return 0;
}

static int kms_write_header(AVFormatContext *avctx)
{
    KMSContext *k = avctx->priv_data;
    const AVCodecParameters *par;
    char path[64];
    int ret;

    if (avctx->nb_streams != 1 ||
        avctx->streams[0]->codecpar->codec_type != AVMEDIA_TYPE_VIDEO ||
        avctx->streams[0]->codecpar->codec_id != AV_CODEC_ID_WRAPPED_AVFRAME) {
        av_log(avctx, AV_LOG_ERROR, "Only a single wrapped_avframe video stream is supported\n");
        return AVERROR(EINVAL);
    }
    par = avctx->streams[0]->codecpar;
    if (par->format != AV_PIX_FMT_DRM_PRIME && !kms_drm_format(par->format)) {
        av_log(avctx, AV_LOG_ERROR, "Pixel format %s is not supported, use DRM PRIME "
               "frames or e.g. -pix_fmt nv12\n", av_get_pix_fmt_name(par->format));
        return AVERROR(EINVAL);
    }

    if ((k->fd = kms_open(avctx, avctx->url, path, sizeof(path))) < 0)
        return k->fd;
    if (drmSetClientCap(k->fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1) < 0 ||
        drmSetClientCap(k->fd, DRM_CLIENT_CAP_ATOMIC, 1) < 0) {
        ret = AVERROR(errno);
        av_log(avctx, AV_LOG_ERROR, "%s does not support atomic modesetting: %s\n",
               path, av_err2str(ret));
        return ret;
    }
    if (drmSetMaster(k->fd) < 0) {
        ret = AVERROR(errno);
        av_log(avctx, AV_LOG_ERROR, "Cannot become the DRM master of %s: %s. Another "
               "process, such as a display server, is probably holding it.\n",
               path, av_err2str(ret));
        return ret;
    }

    return kms_select_output(avctx);
}

static int kms_write_packet(AVFormatContext *avctx, AVPacket *pkt)
{
    KMSContext *k = avctx->priv_data;
    const AVFrame *src = (const AVFrame *)pkt->data;
    AVFrame *frame = NULL;
    uint32_t fb_id = 0, format;
    uint64_t modifier = DRM_FORMAT_MOD_LINEAR;
    int owned = 0, ret;

    if (pkt->size < sizeof(*src))
        return AVERROR(EINVAL);

    if (src->format == AV_PIX_FMT_DRM_PRIME) {
        if ((ret = kms_prime_fb(avctx, src, &fb_id, &format, &modifier)) < 0)
            return ret;
        owned = 1;
        if (!(frame = av_frame_clone(src))) {
            ret = AVERROR(ENOMEM);
            goto fail;
        }
    } else {
        KMSDumb *d;

        if (!(format = kms_drm_format(src->format))) {
            av_log(avctx, AV_LOG_ERROR, "Unsupported frame format %s\n",
                   av_get_pix_fmt_name(src->format));
            return AVERROR(EINVAL);
        }
        if (src->width != k->dumb_width || src->height != k->dumb_height ||
            src->format != k->dumb_format) {
            kms_free_dumb(k);
            k->dumb_width = 0;
            if ((ret = kms_alloc_dumb(avctx, src, format)) < 0) {
                kms_free_dumb(k);
                return ret;
            }
        }
        d = &k->dumb[k->dumb_next];
        k->dumb_next = (k->dumb_next + 1) % NB_DUMB;
        av_image_copy2(d->data, d->linesize, src->data, src->linesize,
                       src->format, src->width, src->height);
        fb_id = d->fb_id;
    }

    if ((!k->plane_id || format != k->plane_format || modifier != k->plane_modifier) &&
        (ret = kms_select_plane(avctx, format, modifier)) < 0)
        goto fail;

    if ((ret = kms_commit(avctx, fb_id, src)) < 0)
        goto fail;

    if (k->shown_fb_owned)
        drmModeRmFB(k->fd, k->shown_fb);
    av_frame_free(&k->shown_frame);
    k->shown_fb       = fb_id;
    k->shown_fb_owned = owned;
    k->shown_frame    = frame;
    return 0;

fail:
    if (owned)
        drmModeRmFB(k->fd, fb_id);
    av_frame_free(&frame);
    return ret;
}

static void kms_deinit(AVFormatContext *avctx)
{
    KMSContext *k = avctx->priv_data;

    if (k->fd < 0)
        return;
    if (k->shown_fb_owned)
        drmModeRmFB(k->fd, k->shown_fb);
    k->shown_fb = 0;
    kms_free_dumb(k);
    av_frame_free(&k->shown_frame);
    if (k->mode_blob)
        drmModeDestroyPropertyBlob(k->fd, k->mode_blob);
    close(k->fd);
    k->fd = -1;
}

#define OFFSET(x) offsetof(KMSContext, x)
#define ENC AV_OPT_FLAG_ENCODING_PARAM
static const AVOption options[] = {
    { "connector_id", "connector to show the video on (0 picks the first connected one)",
      OFFSET(connector_opt), AV_OPT_TYPE_INT64, { .i64 = 0 }, 0, UINT32_MAX, ENC },
    { "crtc_id", "CRTC to drive the connector with (0 picks the active or a usable one)",
      OFFSET(crtc_opt), AV_OPT_TYPE_INT64, { .i64 = 0 }, 0, UINT32_MAX, ENC },
    { "plane_id", "plane to show the video on (0 picks one that supports the format)",
      OFFSET(plane_opt), AV_OPT_TYPE_INT64, { .i64 = 0 }, 0, UINT32_MAX, ENC },
    { "scale", "scale the video to fit the display, keeping the aspect ratio",
      OFFSET(scale), AV_OPT_TYPE_BOOL, { .i64 = 1 }, 0, 1, ENC },
    { NULL },
};

static const AVClass kms_class = {
    .class_name = "KMS outdev",
    .item_name  = av_default_item_name,
    .option     = options,
    .version    = LIBAVUTIL_VERSION_INT,
    .category   = AV_CLASS_CATEGORY_DEVICE_VIDEO_OUTPUT,
};

const FFOutputFormat ff_kms_muxer = {
    .p.name         = "kms",
    .p.long_name    = NULL_IF_CONFIG_SMALL("KMS video output device"),
    .p.audio_codec  = AV_CODEC_ID_NONE,
    .p.video_codec  = AV_CODEC_ID_WRAPPED_AVFRAME,
    .p.flags        = AVFMT_NOFILE | AVFMT_VARIABLE_FPS | AVFMT_NOTIMESTAMPS,
    .p.priv_class   = &kms_class,
    .priv_data_size = sizeof(KMSContext),
    .init           = kms_init,
    .write_header   = kms_write_header,
    .write_packet   = kms_write_packet,
    .deinit         = kms_deinit,
};
