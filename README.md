# ffmpeg-rockchip

FFmpeg with Rockchip hardware video through Rockchip MPP and RGA, maintained
for RK3576.

- `*_rkmpp` decoders. RK3576 runs H.264, HEVC, VP9, AV1 and MJPEG; the
  H.263, MPEG-1/2/4 and VP8 decoders are kept for older SoCs.
- `h264_rkmpp`, `hevc_rkmpp` and `mjpeg_rkmpp` encoders.
- An RKMPP hardware-frames context over DRM PRIME, and the `scale_rkrga`,
  `vpp_rkrga`, `overlay_rkrga`, `drawbox_rkrga`, `pixelize_rkrga` and
  `colorkey_rkrga` filters.

Base: FFmpeg `release/9.0`. The Rockchip code is nyanmisaka's ffmpeg-rockchip
series, imported with her authorship, with fixes on top. Tested on RK3576
with the vendor 6.1 kernel, MPP 1.1 and librga 1.10; other Rockchip SoCs are
not tested.

Configure with `--enable-rkmpp --enable-rkrga --enable-libdrm
--enable-version3`. MPP and librga are Apache-2.0, so the result is
(L)GPLv3.

---

# FFmpeg README

FFmpeg is a collection of libraries and tools to process multimedia content
such as audio, video, subtitles and related metadata.

## Libraries

* `libavcodec` provides implementation of a wider range of codecs.
* `libavformat` implements streaming protocols, container formats and basic I/O access.
* `libavutil` includes hashers, decompressors and miscellaneous utility functions.
* `libavfilter` provides means to alter decoded audio and video through a directed graph of connected filters.
* `libavdevice` provides an abstraction to access capture and playback devices.
* `libswresample` implements audio mixing and resampling routines.
* `libswscale` implements color conversion and scaling routines.

## Tools

* [ffmpeg](https://ffmpeg.org/ffmpeg.html) is a command line toolbox to
  manipulate, convert and stream multimedia content.
* [ffplay](https://ffmpeg.org/ffplay.html) is a minimalistic multimedia player.
* [ffprobe](https://ffmpeg.org/ffprobe.html) is a simple analysis tool to inspect
  multimedia content.
* Additional small tools such as `aviocat`, `ismindex` and `qt-faststart`.

## Documentation

The offline documentation is available in the **doc/** directory.

The online documentation is available in the main [website](https://ffmpeg.org)
and in the [wiki](https://trac.ffmpeg.org).

### Examples

Coding examples are available in the **doc/examples** directory.

## License

FFmpeg codebase is mainly LGPL-licensed with optional components licensed under
GPL. Please refer to the LICENSE file for detailed information.

## Contributing

Patches should be submitted to the ffmpeg-devel mailing list using
`git format-patch` or `git send-email`. Github pull requests should be
avoided because they are not part of our review process and will be ignored.
