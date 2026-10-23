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

#include "libavutil/aarch64/asm.S"

// NV15 and NV20_PACKED store 10-bit samples LSB first without padding, four
// samples in five bytes. Sample n of a 5-byte group starts at bit 2 * n of
// byte n, so every sample lies in one little-endian byte pair.

const packed10_tbl, align=4
        // byte pairs holding samples 0-7 and 8-15 of a 20-byte block
        .byte            0,  1,  1,  2,  2,  3,  3,  4,  5,  6,  6,  7,  7,  8,  8,  9
        .byte           10, 11, 11, 12, 12, 13, 13, 14, 15, 16, 16, 17, 17, 18, 18, 19
        // right shift of each sample within its byte pair
        .short           0, -2, -4, -6,  0, -2, -4, -6
endconst

.macro packed10_load_tbl
        movrel          x9,  packed10_tbl
        ld1             {v4.16b, v5.16b, v6.16b}, [x9]
.endm

// Unpack 16 samples from the 20 bytes at \src into v2 (samples 0-7) and
// v3 (samples 8-15), shifted left by v7. Loads exactly 20 bytes, so the
// last block of a row never reads past the row.
.macro packed10_unpack16 src
        ldr             q0,  [\src], #16
        ldr             s1,  [\src], #4
        tbl             v2.16b, {v0.16b, v1.16b}, v4.16b
        tbl             v3.16b, {v0.16b, v1.16b}, v5.16b
        ushl            v2.8h, v2.8h, v6.8h
        ushl            v3.8h, v3.8h, v6.8h
        bic             v2.8h, #0xfc, lsl #8
        bic             v3.8h, #0xfc, lsl #8
        ushl            v2.8h, v2.8h, v7.8h
        ushl            v3.8h, v3.8h, v7.8h
.endm

// Unpack the sample at bit offset \bit of \src, shift it left by \shift and
// store it to \dst. Reads only the two bytes that hold the sample.
.macro packed10_unpack1 dst, src, bit, shift
        lsr             w10, \bit, #3
        and             w11, \bit, #7
        ldrh            w12, [\src, w10, uxtw]
        lsr             w12, w12, w11
        and             w12, w12, #0x3ff
        lsl             w12, w12, \shift
        strh            w12, [\dst], #2
.endm

// void ff_packed10_to_y_neon(uint16_t *dst, const uint8_t *src,
//                            int width, int shift)
function ff_packed10_to_y_neon, export=1
        packed10_load_tbl
        dup             v7.8h, w3
        subs            w2,  w2,  #16
        b.lt            3f
2:
        packed10_unpack16 x1
        subs            w2,  w2,  #16
        stp             q2,  q3,  [x0], #32
        b.ge            2b
3:
        adds            w2,  w2,  #16
        b.le            5f
        mov             w9,  #0                 // bit offset in the last group
4:
        packed10_unpack1 x0, x1, w9, w3
        add             w9,  w9,  #10
        subs            w2,  w2,  #1
        b.gt            4b
5:
        ret
endfunc

// void ff_packed10_to_uv_neon(uint16_t *dstU, uint16_t *dstV,
//                             const uint8_t *src, int width, int shift)
// width is the number of U/V pairs.
function ff_packed10_to_uv_neon, export=1
        packed10_load_tbl
        dup             v7.8h, w4
        subs            w3,  w3,  #8
        b.lt            3f
2:
        packed10_unpack16 x2
        subs            w3,  w3,  #8
        uzp1            v0.8h, v2.8h, v3.8h     // U
        uzp2            v1.8h, v2.8h, v3.8h     // V
        str             q0,  [x0], #16
        str             q1,  [x1], #16
        b.ge            2b
3:
        adds            w3,  w3,  #8
        b.le            5f
        mov             w9,  #0                 // bit offset in the last group
4:
        packed10_unpack1 x0, x2, w9, w4
        add             w9,  w9,  #10
        packed10_unpack1 x1, x2, w9, w4
        add             w9,  w9,  #10
        subs            w3,  w3,  #1
        b.gt            4b
5:
        ret
endfunc
