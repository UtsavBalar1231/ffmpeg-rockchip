# ffmpeg-rockchip

FFmpeg 9.0 with Rockchip MPP and RGA hardware video, validated on RK3576.

[![Latest release](https://img.shields.io/github/v/release/UtsavBalar1231/ffmpeg-rockchip)](https://github.com/UtsavBalar1231/ffmpeg-rockchip/releases)

This is FFmpeg `release/9.0` with support for Rockchip's media hardware: the video
decoders and encoders through [MPP](https://github.com/rockchip-linux/mpp), the
2D engine through [librga](https://github.com/airockchip/librga), the IEP2
deinterlacer and VDPP post-processor, HDMI capture through V4L2, and display
output through KMS. Frames stay in DMA-BUF memory from capture or decode to
encode or display, so a hardware pipeline doesn't copy pictures through the CPU.

The Rockchip code started as [nyanmisaka's ffmpeg-rockchip](https://github.com/nyanmisaka/ffmpeg-rockchip)
series, imported with her authorship and ported to FFmpeg 9.0. Every
release is tested on an RK3576 board with the Rockchip vendor 6.1 kernel. The
code paths for other Rockchip SoCs are kept, but they are not tested here.

## Contents

- [Why use it](#why-use-it)
- [What is included](#what-is-included)
- [Compared with other builds](#compared-with-other-builds)
- [Requirements](#requirements)
- [Building](#building)
- [Usage](#usage)
- [Known limitations](#known-limitations)
- [Reporting bugs and contributing](#reporting-bugs-and-contributing)
- [License](#license)

## Why use it

Upstream FFmpeg 9.0 has `h264`, `hevc`, `vp8` and `vp9` MPP decoders and
`h264` and `hevc` encoders, and nothing else for Rockchip: no hardware frames
context and no RGA filters. On RK3576 this fork adds:

- More decoders: AV1, AVS2 and MJPEG in hardware, plus 10-bit, 4:2:2 and
  monochrome H.264 and HEVC, with 10-bit output as NV15 (converted to P010 by
  swscale when needed) and optional AFBC compression.
- Encoder controls: CBR, VBR, AVBR and CQP rate control, QP bounds,
  profiles and levels, regions of interest, slices, temporal layers, long-term
  references and intra refresh for H.264 and HEVC, and quality, quantization
  tables and restart markers for JPEG.
- An `rkmpp` hardware device, so `-hwaccel rkmpp` decodes into DRM PRIME frames
  that RGA filters, the encoders, the KMS output and Vulkan take without a
  copy.
- RGA filters for scaling, cropping, format conversion, transposition, overlay,
  boxes, mosaic and colour keying, and IEP2 and VDPP filters for deinterlacing,
  de-ringing, sharpening and luma histograms.
- HDMI capture from rkcif that sets itself up from the incoming signal and can
  hand its buffers straight to the encoder, and a KMS output device that shows
  video on a display from the command line.
- HDR metadata: mastering display and content light level, plus HDR10+ and
  HDR Vivid dynamic metadata, are attached to the decoded frames.

Measured on RK3576 with this release:

| Pipeline | Result |
|---|---|
| 4K H.264 to HEVC transcode, all on the device | 63 fps |
| 4K HEVC Main10 to H.264, converted by RGA | 68 fps |
| 1080i deinterlace, `deinterlace_rkiep2` against `bwdif` on the CPU | 436 fps against 127 fps |
| 1080p60 HDMI capture to H.264, buffers passed by DMA-BUF instead of copied | half the CPU time (4.10 s to 1.85 s per 600 frames) |
| 4K decode read back to system memory (`hwdownload`), H.264 to NV12 | 144 fps |

## What is included

| Kind | Name | RK3576 |
|---|---|---|
| Decoders | `h264_rkmpp` | 8 and 10-bit, 4:2:0, 4:2:2 and monochrome; 4:4:4 is refused |
| | `hevc_rkmpp` | Main, Main10, 4:2:2 8 and 10-bit, 4:4:4 8-bit, monochrome 8-bit |
| | `vp9_rkmpp` | profiles 0 and 2 |
| | `av1_rkmpp` | Main, 8 and 10-bit |
| | `avs2_rkmpp` | Main (8-bit); Main10 is refused |
| | `mjpeg_rkmpp` | 4:2:0, 4:2:2, 4:4:4, 4:1:1 and 4:4:0 |
| | `vp8_rkmpp`, `mpeg1_rkmpp`, `mpeg2_rkmpp`, `mpeg4_rkmpp`, `h263_rkmpp` | no hardware on RK3576; kept for other SoCs |
| Encoders | `h264_rkmpp`, `hevc_rkmpp` | YUV, packed YUV, RGB and DRM PRIME input |
| | `mjpeg_rkmpp` | |
| Filters | `scale_rkrga`, `vpp_rkrga`, `overlay_rkrga` | scale, crop, convert, transpose, overlay |
| | `drawbox_rkrga`, `pixelize_rkrga`, `colorkey_rkrga` | boxes, mosaic, colour key |
| | `deinterlace_rkiep2` | IEP2, up to 1920x1080 |
| | `vdpp_rkmpp` | VDPP de-ringing, sharpening, scaling, histogram |
| Devices | `v4l2` input | DV timings detection, DMA-BUF export (`-dmabuf 1`) |
| | `kms` output | atomic modesetting, DRM PRIME and AFBC frames shown without a copy |
| Hardware frames | `rkmpp` | maps to and from DRM, and to Vulkan |

Run `ffmpeg -h decoder=hevc_rkmpp`, `-h encoder=h264_rkmpp` or `-h
filter=scale_rkrga` for the options of each one. The filters are documented in
[doc/filters.texi](doc/filters.texi), the devices in
[doc/indevs.texi](doc/indevs.texi) and [doc/outdevs.texi](doc/outdevs.texi).

## Compared with other builds

| | Upstream FFmpeg 9.0 | nyanmisaka/ffmpeg-rockchip | This fork |
|---|---|---|---|
| FFmpeg base | 9.0 | 8.1 (no 9.0 branch) | 9.0 |
| MPP decoders | H.264, HEVC, VP8, VP9 | adds AV1, MJPEG, MPEG-1/2/4, H.263 | adds AVS2 to those |
| MPP encoders | H.264, HEVC | H.264, HEVC, MJPEG | H.264, HEVC, MJPEG |
| Encoder controls | `rc` (VBR, CBR, AVBR) | rate control, QP, profile, level, intra refresh | adds ROI, slices, temporal layers, long-term references, JPEG tables |
| Hardware frames context | none | `rkmpp` | `rkmpp`, mapping to DRM and Vulkan |
| RGA filters | none | scale, vpp, overlay | adds drawbox, pixelize, colorkey |
| IEP2 and VDPP filters | none | IEP2 inside the decoder only | `deinterlace_rkiep2`, `vdpp_rkmpp` |
| HDR dynamic metadata | none | not documented | HDR10+, HDR Vivid |
| HDMI capture and KMS output | generic `v4l2`, `kmsgrab` | generic `v4l2`, `kmsgrab` | DV timings, DMA-BUF capture, `kms` output |
| Tested SoC | none stated | RK3588 | RK3576 |

jellyfin-ffmpeg carries the same Rockchip pipeline as nyanmisaka's series,
with RK3588 as its verified target. Mainline Linux exposes some Rockchip
decoders as stateless V4L2 devices, which FFmpeg reaches through out-of-tree
`v4l2-request` patches; that route uses a mainline kernel instead of MPP.

## Requirements

> [!IMPORTANT]
> This needs the Rockchip vendor (BSP) kernel. MPP and RGA talk to
> `/dev/mpp_service` and `/dev/rga`, which mainline Linux does not have.

- An RK3576 board running the Rockchip vendor 6.1 kernel. Other Rockchip SoCs
  share the code but are not tested.
- Read and write access to `/dev/mpp_service`, `/dev/rga`, `/dev/dri/*` and
  `/dev/dma_heap/*`.
- [MPP](https://github.com/rockchip-linux/mpp): pkg-config version 1.3.9 or
  newer with `MPP_DEC_GET_THUMBNAIL_FRAME_INFO`, which means MPP from April 2024
  or later, such as the `1.1.0` tag. `deinterlace_rkiep2` and `vdpp_rkmpp` also
  need MPP to install `rockchip/iep2_api.h` and `rockchip/hwpq_vdpp_proc_api.h`.
  Upstream MPP doesn't install them yet ([rockchip-linux/mpp#1023](https://github.com/rockchip-linux/mpp/pull/1023)
  adds that), and without them the two filters are left out of the build.
- [librga](https://github.com/JeffyCN/mirrors/tree/linux-rga-multi) with
  `improcessOpt()` and a `librga.pc`. The `linux-rga-multi` source branch has
  both; the prebuilt airockchip/librga package has no `.pc` file.
- `libdrm`. OpenCL and Vulkan are optional, for GPU interop.

Debian, Ubuntu and Armbian do not package MPP or librga, so build them from
source as below.

## Building

On the board, Debian or Ubuntu:

```sh
sudo apt install build-essential git cmake meson ninja-build pkg-config libdrm-dev

# MPP
git clone -b 1.1.0 https://github.com/rockchip-linux/mpp.git
cmake -S mpp -B mpp/build -DCMAKE_INSTALL_PREFIX=/usr -DCMAKE_BUILD_TYPE=Release -DBUILD_TEST=OFF
cmake --build mpp/build -j"$(nproc)"
sudo cmake --install mpp/build

# librga
git clone -b linux-rga-multi https://github.com/JeffyCN/mirrors.git librga
meson setup librga/build librga --prefix=/usr --libdir=lib/$(gcc -dumpmachine) \
    --buildtype=release -Dlibrga_demo=false
ninja -C librga/build
sudo ninja -C librga/build install

# FFmpeg
git clone https://github.com/UtsavBalar1231/ffmpeg-rockchip.git
cd ffmpeg-rockchip
./configure --prefix=/usr/local --enable-version3 \
    --enable-libdrm --enable-rkmpp --enable-rkrga
make -j"$(nproc)"
sudo make install
```

Check the result:

```console
$ ffmpeg -hide_banner -decoders | grep rkmpp
$ ffmpeg -hide_banner -filters | grep -E 'rkrga|rkiep2|rkmpp'
```

Add `--enable-opencl` and `--enable-vulkan` for GPU interop, and `--enable-gpl`
for GPL parts such as libx264.

To cross-compile, install MPP, librga and libdrm into an arm64 sysroot, point
`PKG_CONFIG_LIBDIR` at its `pkgconfig` directories, and add
`--enable-cross-compile --arch=aarch64 --target-os=linux
--cross-prefix=aarch64-linux-gnu-` to the configure line.

## Usage

Decode to hardware frames and encode them, with no copy in between:

```sh
ffmpeg -hwaccel rkmpp -hwaccel_output_format drm_prime -i in.mp4 \
    -c:v hevc_rkmpp -b:v 8M out.mp4
```

Scale and convert with RGA on the way:

```sh
ffmpeg -hwaccel rkmpp -hwaccel_output_format drm_prime -i in_4k_10bit.mp4 \
    -vf scale_rkrga=w=1920:h=1080:format=nv12 -c:v h264_rkmpp -b:v 6M out.mp4
```

Deinterlace with the IEP2:

```sh
ffmpeg -hwaccel rkmpp -hwaccel_output_format drm_prime -i interlaced.ts \
    -vf deinterlace_rkiep2 -c:v h264_rkmpp out.mp4
```

Capture HDMI and encode it without copying the frames:

```sh
ffmpeg -f v4l2 -dmabuf 1 -i /dev/video0 -c:v h264_rkmpp -b:v 10M capture.mp4
```

Show a video on the display, with no display server running:

```sh
ffmpeg -re -hwaccel rkmpp -hwaccel_output_format drm_prime -i in.mp4 -f kms rockchip
```

Frames can also be mapped to DRM and Vulkan frames with `hwmap`. For that,
create the MPP device from a DRM device, such as the GPU's render node:
`-init_hw_device drm=dr:/dev/dri/renderD129 -init_hw_device rkmpp=rk@dr
-hwaccel rkmpp -hwaccel_device rk`.

## Known limitations

- Only RK3576 is tested. The code for other SoCs is kept from the original
  series and may or may not work.
- RK3576 has no hardware for VP8, MPEG-1/2/4 or H.263.
- HEVC temporal layers are refused on RK3576. With the layer structures this
  encoder builds, the hardware failed frames at random (error status 0x100);
  MPP's own temporal layer presets didn't fail in the same test, so the cause is
  still open.
- RGA2 (RK3576 has no RGA3) cannot read AFBC16x16, and crops 10-bit images
  only at x offsets that are multiples of 64.
- On Mali-G52, Vulkan needs a Mesa whose panvk supports it (tested with Mesa
  26.1) and `PAN_I_WANT_A_BROKEN_VULKAN_DRIVER=1`. Imported frames read back
  exactly, but Vulkan processing such as `scale_vulkan` gives wrong output.
  OpenCL works through `hwdownload` and `hwupload` only, and neither path takes
  10-bit (NV15) frames.
- Bugs found in MPP, librga and the kernel driver while testing are reported
  upstream with fixes; the release notes list them.

## Reporting bugs and contributing

Report bugs and send pull requests on [GitHub](https://github.com/UtsavBalar1231/ffmpeg-rockchip/issues).
Include the board, the kernel version, the MPP and librga versions, the full
command and the output with `-v debug`. Changes that aren't specific to
Rockchip belong in upstream FFmpeg, which takes patches on the ffmpeg-devel
mailing list.

## License

FFmpeg is LGPL 2.1 or later, with optional GPL parts; see [LICENSE.md](LICENSE.md).
MPP and librga are Apache-2.0, which is compatible with version 3 of the
(L)GPL only, so `--enable-version3` is required and a build with them is
LGPLv3, or GPLv3 with `--enable-gpl`.

The Rockchip support builds on the work of nyanmisaka and the people she
credits, [hbiyik](https://github.com/hbiyik) and
[rigaya](https://github.com/rigaya), and on Rockchip's MPP and RGA projects.
