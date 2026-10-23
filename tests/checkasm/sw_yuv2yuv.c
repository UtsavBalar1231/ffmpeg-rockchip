/*
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with FFmpeg; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include <string.h>

#include "libavutil/common.h"
#include "libavutil/intreadwrite.h"
#include "libavutil/mem_internal.h"
#include "libavutil/pixdesc.h"

#include "libswscale/swscale.h"
#include "libswscale/swscale_internal.h"

#include "checkasm.h"

#define randomize_buffers(buf, size)      \
    do {                                  \
        for (int j = 0; j < size; j += 4) \
            AV_WN32(buf + j, rnd());      \
    } while (0)

static void check_semiplanar(int dst_pix_fmt)
{
    static const int src_fmts[] = {
        AV_PIX_FMT_NV24,
        AV_PIX_FMT_NV42,
    };
    const AVPixFmtDescriptor *dst_desc = av_pix_fmt_desc_get(dst_pix_fmt);
#define NUM_LINES 4
#define MAX_LINE_SIZE 1920
    static const int input_sizes[] = {8, 128, 1080, MAX_LINE_SIZE};

    declare_func(int, SwsInternal *c, const uint8_t *const src[],
                      const int srcStride[], int srcSliceY, int srcSliceH,
                      uint8_t *const dstParam[], const int dstStride[]);

    LOCAL_ALIGNED_8(uint8_t, src_y,  [MAX_LINE_SIZE * NUM_LINES]);
    LOCAL_ALIGNED_8(uint8_t, src_uv, [MAX_LINE_SIZE * NUM_LINES * 2]);
    const uint8_t *src[4] = { src_y, src_uv };

    LOCAL_ALIGNED_8(uint8_t, dst0_y, [MAX_LINE_SIZE * NUM_LINES]);
    LOCAL_ALIGNED_8(uint8_t, dst0_u, [MAX_LINE_SIZE * NUM_LINES / 2]);
    LOCAL_ALIGNED_8(uint8_t, dst0_v, [MAX_LINE_SIZE * NUM_LINES / 2]);
    uint8_t *dst0[4] = { dst0_y, dst0_u, dst0_v };

    LOCAL_ALIGNED_8(uint8_t, dst1_y, [MAX_LINE_SIZE * NUM_LINES]);
    LOCAL_ALIGNED_8(uint8_t, dst1_u, [MAX_LINE_SIZE * NUM_LINES / 2]);
    LOCAL_ALIGNED_8(uint8_t, dst1_v, [MAX_LINE_SIZE * NUM_LINES / 2]);
    uint8_t *dst1[4] = { dst1_y, dst1_u, dst1_v };

    randomize_buffers(src_y,  MAX_LINE_SIZE * NUM_LINES);
    randomize_buffers(src_uv, MAX_LINE_SIZE * NUM_LINES * 2);

    for (int sfi = 0; sfi < FF_ARRAY_ELEMS(src_fmts); sfi++) {
        int src_pix_fmt = src_fmts[sfi];
        const AVPixFmtDescriptor *src_desc = av_pix_fmt_desc_get(src_pix_fmt);
        for (int isi = 0; isi < FF_ARRAY_ELEMS(input_sizes); isi++) {
            SwsContext *sws;
            SwsInternal *c;
            int log_level;
            int width = input_sizes[isi];
            int srcSliceY = 0;
            int srcSliceH = NUM_LINES;
            int srcStride[4] = {
                MAX_LINE_SIZE,
                MAX_LINE_SIZE * 2,
            };
            int dstStride[4] = {
                MAX_LINE_SIZE,
                MAX_LINE_SIZE >> dst_desc->log2_chroma_w,
                MAX_LINE_SIZE >> dst_desc->log2_chroma_w,
            };

            // override log level to prevent spamming of the message
            // "No accelerated colorspace conversion found from %s to %s"
            log_level = av_log_get_level();
            av_log_set_level(AV_LOG_ERROR);
            sws = sws_getContext(width, srcSliceH, src_pix_fmt,
                                 width, srcSliceH, dst_pix_fmt,
                                 0, NULL, NULL, NULL);
            av_log_set_level(log_level);
            if (!sws)
                fail();

            c = sws_internal(sws);
            if (check_func(c->convert_unscaled, "%s_%s_%d", src_desc->name, dst_desc->name, width)) {
                memset(dst0_y, 0xFF, MAX_LINE_SIZE * NUM_LINES);
                memset(dst0_u, 0xFF, MAX_LINE_SIZE * NUM_LINES / 2);
                memset(dst0_v, 0xFF, MAX_LINE_SIZE * NUM_LINES / 2);
                memset(dst1_y, 0xFF, MAX_LINE_SIZE * NUM_LINES);
                memset(dst1_u, 0xFF, MAX_LINE_SIZE * NUM_LINES / 2);
                memset(dst1_v, 0xFF, MAX_LINE_SIZE * NUM_LINES / 2);

                call_ref(c, src, srcStride, srcSliceY,
                         srcSliceH, dst0, dstStride);
                call_new(c, src, srcStride, srcSliceY,
                         srcSliceH, dst1, dstStride);

                if (memcmp(dst0_y, dst1_y, MAX_LINE_SIZE * NUM_LINES) ||
                    memcmp(dst0_u, dst1_u, MAX_LINE_SIZE * NUM_LINES / 2) ||
                    memcmp(dst0_v, dst1_v, MAX_LINE_SIZE * NUM_LINES / 2))
                    fail();

                bench_new(c, src, srcStride, srcSliceY,
                          srcSliceH, dst0, dstStride);
            }
            sws_freeContext(sws);
        }
    }
}

#undef NUM_LINES
#undef MAX_LINE_SIZE

/* Odd and non-multiple-of-16 widths exercise the scalar tails. */
static const int packed10_widths[] = {
    1, 2, 3, 4, 5, 7, 8, 9, 13, 15, 16, 17, 31, 33, 130, 853, 1080, 1919, 1920,
};

#define PACKED10_MAX_WIDTH  1920
#define PACKED10_SRC_STRIDE (PACKED10_MAX_WIDTH * 10 / 8)
#define PACKED10_DST_STRIDE (PACKED10_MAX_WIDTH * 2)
#define PACKED10_CHR_STRIDE PACKED10_MAX_WIDTH
/* Odd, so that 4:2:0 input has a last chroma row for a single luma row. */
#define PACKED10_LINES      5
/* The C reader loads one byte past the last group of a row. */
#define PACKED10_SRC_SIZE   (PACKED10_SRC_STRIDE * PACKED10_LINES + 16)
#define PACKED10_DST_SIZE   (PACKED10_DST_STRIDE * PACKED10_LINES)
#define PACKED10_CHR_SIZE   (PACKED10_CHR_STRIDE * PACKED10_LINES)

static void check_packed10_to_planar(void)
{
    static const struct {
        enum AVPixelFormat src, dst;
    } pairs[] = {
        { AV_PIX_FMT_NV15,        AV_PIX_FMT_YUV420P10 },
        { AV_PIX_FMT_NV15,        AV_PIX_FMT_YUV420P12 },
        { AV_PIX_FMT_NV15,        AV_PIX_FMT_YUV420P14 },
        { AV_PIX_FMT_NV15,        AV_PIX_FMT_YUV420P16 },
        { AV_PIX_FMT_NV20_PACKED, AV_PIX_FMT_YUV422P10 },
        { AV_PIX_FMT_NV20_PACKED, AV_PIX_FMT_YUV422P12 },
        { AV_PIX_FMT_NV20_PACKED, AV_PIX_FMT_YUV422P14 },
        { AV_PIX_FMT_NV20_PACKED, AV_PIX_FMT_YUV422P16 },
    };
    const int srcStride[4] = { PACKED10_SRC_STRIDE, PACKED10_SRC_STRIDE };
    const int dstStride[4] = { PACKED10_DST_STRIDE, PACKED10_CHR_STRIDE, PACKED10_CHR_STRIDE };

    declare_func(int, SwsInternal *c, const uint8_t *const src[],
                      const int srcStride[], int srcSliceY, int srcSliceH,
                      uint8_t *const dstParam[], const int dstStride[]);

    LOCAL_ALIGNED_16(uint8_t, src_y,  [PACKED10_SRC_SIZE]);
    LOCAL_ALIGNED_16(uint8_t, src_uv, [PACKED10_SRC_SIZE]);
    LOCAL_ALIGNED_16(uint8_t, dst0_y, [PACKED10_DST_SIZE]);
    LOCAL_ALIGNED_16(uint8_t, dst0_u, [PACKED10_CHR_SIZE]);
    LOCAL_ALIGNED_16(uint8_t, dst0_v, [PACKED10_CHR_SIZE]);
    LOCAL_ALIGNED_16(uint8_t, dst1_y, [PACKED10_DST_SIZE]);
    LOCAL_ALIGNED_16(uint8_t, dst1_u, [PACKED10_CHR_SIZE]);
    LOCAL_ALIGNED_16(uint8_t, dst1_v, [PACKED10_CHR_SIZE]);
    uint8_t *const dst0[4] = { dst0_y, dst0_u, dst0_v };
    uint8_t *const dst1[4] = { dst1_y, dst1_u, dst1_v };

    randomize_buffers(src_y,  PACKED10_SRC_SIZE);
    randomize_buffers(src_uv, PACKED10_SRC_SIZE);

    for (int i = 0; i < FF_ARRAY_ELEMS(pairs); i++) {
        const AVPixFmtDescriptor *src_desc = av_pix_fmt_desc_get(pairs[i].src);
        const AVPixFmtDescriptor *dst_desc = av_pix_fmt_desc_get(pairs[i].dst);
        int vsub = src_desc->log2_chroma_h;

        for (int j = 0; j < FF_ARRAY_ELEMS(packed10_widths); j++) {
            int width = packed10_widths[j];
            SwsContext *sws;
            SwsInternal *c;

            sws = sws_getContext(width, PACKED10_LINES, pairs[i].src,
                                 width, PACKED10_LINES, pairs[i].dst,
                                 0, NULL, NULL, NULL);
            if (!sws) {
                fail();
                continue;
            }
            c = sws_internal(sws);

            if (check_func(c->convert_unscaled, "%s_%s_%d",
                           src_desc->name, dst_desc->name, width)) {
                const uint8_t *src[4] = { src_y, src_uv };
                /* Second slice, starting at an even line. */
                const uint8_t *src_s[4] = { src_y  + 2 * srcStride[0],
                                            src_uv + (2 >> vsub) * srcStride[1] };

                memset(dst0_y, 0xFF, PACKED10_DST_SIZE);
                memset(dst0_u, 0xFF, PACKED10_CHR_SIZE);
                memset(dst0_v, 0xFF, PACKED10_CHR_SIZE);
                memset(dst1_y, 0xFF, PACKED10_DST_SIZE);
                memset(dst1_u, 0xFF, PACKED10_CHR_SIZE);
                memset(dst1_v, 0xFF, PACKED10_CHR_SIZE);

                call_ref(c, src, srcStride, 0, PACKED10_LINES, dst0, dstStride);
                call_new(c, src, srcStride, 0, PACKED10_LINES, dst1, dstStride);
                if (memcmp(dst0_y, dst1_y, PACKED10_DST_SIZE) ||
                    memcmp(dst0_u, dst1_u, PACKED10_CHR_SIZE) ||
                    memcmp(dst0_v, dst1_v, PACKED10_CHR_SIZE))
                    fail();

                call_ref(c, src_s, srcStride, 2, PACKED10_LINES - 2, dst0, dstStride);
                call_new(c, src_s, srcStride, 2, PACKED10_LINES - 2, dst1, dstStride);
                if (memcmp(dst0_y, dst1_y, PACKED10_DST_SIZE) ||
                    memcmp(dst0_u, dst1_u, PACKED10_CHR_SIZE) ||
                    memcmp(dst0_v, dst1_v, PACKED10_CHR_SIZE))
                    fail();

                bench_new(c, src, srcStride, 0, PACKED10_LINES, dst1, dstStride);
            }
            sws_freeContext(sws);
        }
    }
}

static void check_packed10_to_p01x(void)
{
    static const struct {
        enum AVPixelFormat src, dst;
    } pairs[] = {
        { AV_PIX_FMT_NV15,        AV_PIX_FMT_P010LE },
        { AV_PIX_FMT_NV20_PACKED, AV_PIX_FMT_P210LE },
    };
    const int srcStride[4] = { PACKED10_SRC_STRIDE, PACKED10_SRC_STRIDE };
    const int dstStride[4] = { PACKED10_DST_STRIDE, PACKED10_DST_STRIDE };

    declare_func(int, SwsInternal *c, const uint8_t *const src[],
                      const int srcStride[], int srcSliceY, int srcSliceH,
                      uint8_t *const dstParam[], const int dstStride[]);

    LOCAL_ALIGNED_16(uint8_t, src_y,  [PACKED10_SRC_SIZE]);
    LOCAL_ALIGNED_16(uint8_t, src_uv, [PACKED10_SRC_SIZE]);
    LOCAL_ALIGNED_16(uint8_t, dst0_y,  [PACKED10_DST_SIZE]);
    LOCAL_ALIGNED_16(uint8_t, dst0_uv, [PACKED10_DST_SIZE]);
    LOCAL_ALIGNED_16(uint8_t, dst1_y,  [PACKED10_DST_SIZE]);
    LOCAL_ALIGNED_16(uint8_t, dst1_uv, [PACKED10_DST_SIZE]);
    uint8_t *const dst0[4] = { dst0_y, dst0_uv };
    uint8_t *const dst1[4] = { dst1_y, dst1_uv };

    randomize_buffers(src_y,  PACKED10_SRC_SIZE);
    randomize_buffers(src_uv, PACKED10_SRC_SIZE);

    for (int i = 0; i < FF_ARRAY_ELEMS(pairs); i++) {
        const AVPixFmtDescriptor *src_desc = av_pix_fmt_desc_get(pairs[i].src);
        const AVPixFmtDescriptor *dst_desc = av_pix_fmt_desc_get(pairs[i].dst);
        int vsub = src_desc->log2_chroma_h;

        for (int j = 0; j < FF_ARRAY_ELEMS(packed10_widths); j++) {
            int width = packed10_widths[j];
            SwsContext *sws;
            SwsInternal *c;

            sws = sws_getContext(width, PACKED10_LINES, pairs[i].src,
                                 width, PACKED10_LINES, pairs[i].dst,
                                 0, NULL, NULL, NULL);
            if (!sws) {
                fail();
                continue;
            }
            c = sws_internal(sws);

            if (check_func(c->convert_unscaled, "%s_%s_%d",
                           src_desc->name, dst_desc->name, width)) {
                const uint8_t *src[4] = { src_y, src_uv };
                /* Second slice, starting at an even line. */
                const uint8_t *src_s[4] = { src_y  + 2 * srcStride[0],
                                            src_uv + (2 >> vsub) * srcStride[1] };

                memset(dst0_y,  0xFF, PACKED10_DST_SIZE);
                memset(dst0_uv, 0xFF, PACKED10_DST_SIZE);
                memset(dst1_y,  0xFF, PACKED10_DST_SIZE);
                memset(dst1_uv, 0xFF, PACKED10_DST_SIZE);

                call_ref(c, src, srcStride, 0, PACKED10_LINES, dst0, dstStride);
                call_new(c, src, srcStride, 0, PACKED10_LINES, dst1, dstStride);
                if (memcmp(dst0_y,  dst1_y,  PACKED10_DST_SIZE) ||
                    memcmp(dst0_uv, dst1_uv, PACKED10_DST_SIZE))
                    fail();

                call_ref(c, src_s, srcStride, 2, PACKED10_LINES - 2, dst0, dstStride);
                call_new(c, src_s, srcStride, 2, PACKED10_LINES - 2, dst1, dstStride);
                if (memcmp(dst0_y,  dst1_y,  PACKED10_DST_SIZE) ||
                    memcmp(dst0_uv, dst1_uv, PACKED10_DST_SIZE))
                    fail();

                bench_new(c, src, srcStride, 0, PACKED10_LINES, dst1, dstStride);
            }
            sws_freeContext(sws);
        }
    }
}

static void check_packed10_input(void)
{
    static const enum AVPixelFormat formats[] = {
        AV_PIX_FMT_NV15,
        AV_PIX_FMT_NV20_PACKED,
    };
    SwsContext *sws;
    SwsInternal *c;

    LOCAL_ALIGNED_16(uint8_t, src,    [PACKED10_SRC_SIZE]);
    LOCAL_ALIGNED_16(uint8_t, dst0_y, [PACKED10_DST_STRIDE]);
    LOCAL_ALIGNED_16(uint8_t, dst0_u, [PACKED10_DST_STRIDE]);
    LOCAL_ALIGNED_16(uint8_t, dst0_v, [PACKED10_DST_STRIDE]);
    LOCAL_ALIGNED_16(uint8_t, dst1_y, [PACKED10_DST_STRIDE]);
    LOCAL_ALIGNED_16(uint8_t, dst1_u, [PACKED10_DST_STRIDE]);
    LOCAL_ALIGNED_16(uint8_t, dst1_v, [PACKED10_DST_STRIDE]);

    sws = sws_getContext(PACKED10_MAX_WIDTH, PACKED10_LINES, AV_PIX_FMT_NV15,
                         PACKED10_MAX_WIDTH, PACKED10_LINES, AV_PIX_FMT_YUV420P10,
                         0, NULL, NULL, NULL);
    if (!sws) {
        fail();
        return;
    }
    c = sws_internal(sws);

    randomize_buffers(src, PACKED10_SRC_SIZE);

    for (int i = 0; i < FF_ARRAY_ELEMS(formats); i++) {
        const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get(formats[i]);

        sws->src_format = formats[i];
        ff_sws_init_scale(c);

        for (int j = 0; j < FF_ARRAY_ELEMS(packed10_widths); j++) {
            int width = packed10_widths[j];
            int chr_width = AV_CEIL_RSHIFT(width, 1);

            {
                declare_func(void, uint8_t *dst, const uint8_t *src,
                             const uint8_t *unused1, const uint8_t *unused2,
                             int width, uint32_t *pal, void *opq);

                if (check_func(c->lumToYV12, "%s_to_y_%d", desc->name, width)) {
                    memset(dst0_y, 0xFF, PACKED10_DST_STRIDE);
                    memset(dst1_y, 0xFF, PACKED10_DST_STRIDE);
                    call_ref(dst0_y, src, NULL, NULL, width, NULL, NULL);
                    call_new(dst1_y, src, NULL, NULL, width, NULL, NULL);
                    if (memcmp(dst0_y, dst1_y, PACKED10_DST_STRIDE))
                        fail();
                    bench_new(dst1_y, src, NULL, NULL, width, NULL, NULL);
                }
            }

            {
                declare_func(void, uint8_t *dstU, uint8_t *dstV,
                             const uint8_t *src0, const uint8_t *src1,
                             const uint8_t *src2, int width,
                             uint32_t *pal, void *opq);

                if (check_func(c->chrToYV12, "%s_to_uv_%d", desc->name, width)) {
                    memset(dst0_u, 0xFF, PACKED10_DST_STRIDE);
                    memset(dst0_v, 0xFF, PACKED10_DST_STRIDE);
                    memset(dst1_u, 0xFF, PACKED10_DST_STRIDE);
                    memset(dst1_v, 0xFF, PACKED10_DST_STRIDE);
                    call_ref(dst0_u, dst0_v, NULL, src, src, chr_width, NULL, NULL);
                    call_new(dst1_u, dst1_v, NULL, src, src, chr_width, NULL, NULL);
                    if (memcmp(dst0_u, dst1_u, PACKED10_DST_STRIDE) ||
                        memcmp(dst0_v, dst1_v, PACKED10_DST_STRIDE))
                        fail();
                    bench_new(dst1_u, dst1_v, NULL, src, src, chr_width, NULL, NULL);
                }
            }
        }
    }

    sws_freeContext(sws);
}

void checkasm_check_sw_yuv2yuv(void)
{
    check_semiplanar(AV_PIX_FMT_YUV420P);
    report("yuv420p");

    check_packed10_to_planar();
    report("packed10_to_planar");

    check_packed10_to_p01x();
    report("packed10_to_p01x");

    check_packed10_input();
    report("packed10_input");
}
