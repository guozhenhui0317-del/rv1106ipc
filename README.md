# RV1106 SC3336 IPC

This is a standalone source package. It keeps the useful RKIPC demo pieces (INI parser, ISP and RockIVA wrapper) and removes
the unrelated display, web server, storage, JPEG and legacy streaming modules.

The bundled `common` directory is the complete local subset required by this program. Moving only
the whole `gzh_ipc` directory to Linux does not require the original RKIPC repository. Rockchip SDK,
FFmpeg 4.x and EasyLogger remain external dependencies and are supplied through CMake paths.

## Pipeline

```text
SC3336 -> ISP -> VI channel 0 -> bind -> VENC 0 (1080p H.264/H.265 selectable)
                                         -> FFmpeg RTSP main (G711A audio)
                                         -> FFmpeg RTMP main (G711A audio, H.264 only)

             -> VI channel 1 -> bind -> VENC 1 (704x576 H.264)
                    |                    + RGN boxes -> FFmpeg RTSP/RTMP AI video
                    +-> RockIVA official model

Mic -> AI 0 -> bind -> AENC 0 G711A -> main RTSP and RTMP only
```

There are exactly two VI channels. `video.2` in the INI is only a compatibility geometry alias
for the reused official RockIVA wrapper; no VI channel 2 is created.

Each FFmpeg output waits for the first video IDR containing SPS/PPS (and VPS for H.265), then uses
that VENC timestamp as its common origin. Later video and AENC timestamps are rescaled from the
same microsecond timeline, which preserves A/V synchronization without synthesizing timestamps.

## Required external files

Supply target builds of:

- The RV1106 Rockchip SDK headers and libraries: MPI/Rockit, MPP, RKAIQ, RockIVA, RKNN runtime and
  `rksysutils`. The include tree must also contain `rk_gpio.h` and `rk_pwm.h` used by the official
  ISP source.
- FFmpeg 4.x shared or static libraries built with networking, the RTSP and FLV muxers, and the
  RTSP/RTP/TCP/UDP/RTMP protocols.
- EasyLogger source from <https://github.com/armink/EasyLogger>.
- The SC3336 IQ files, official RockIVA model files and the official VQE JSON file.

The CMake file documents every linked library and checks the important external headers during
configuration. Override `ROCKCHIP_INCLUDE_DIR`, `ROCKCHIP_SYSUTILS_INCLUDE_DIR` and
`ROCKCHIP_LIB_DIR` if the SDK is not staged below `ROCKCHIP_SDK_ROOT/usr`. Do not copy target
libraries into this source tree; keeping them outside prevents accidentally linking host libraries.

## Cross-compile

After copying the complete `gzh_ipc` directory to Linux, enter that directory and run:

```sh
cmake -S . -B build \
  -DCMAKE_TOOLCHAIN_FILE=/path/to/rv1106-toolchain.cmake \
  -DCMAKE_BUILD_TYPE=Release \
  -DROCKCHIP_SDK_ROOT=/path/to/rv1106/staging \
  -DFFMPEG_ROOT=/path/to/ffmpeg-4-target \
  -DEASYLOGGER_ROOT=/path/to/EasyLogger

cmake --build build -j
```

If necessary, add explicit overrides:

```sh
-DROCKCHIP_INCLUDE_DIR=/path/to/sdk/include \
-DROCKCHIP_SYSUTILS_INCLUDE_DIR=/path/to/sdk/sysutils/include \
-DROCKCHIP_LIB_DIR=/path/to/sdk/lib
```

## PC and board setup

1. Run MediaMTX on the wired PC with its default RTSP port `8554` and RTMP port `1935`.
2. Edit all four URLs in `rv1106_sc3336.ini`, replacing `192.168.1.100` with the PC address.
3. Copy the executable and INI to the board, plus any shared libraries not already in the image.
4. Ensure the model directory, IQ directory and VQE JSON paths match the INI.
5. Start:

```sh
chmod +x /oem/usr/bin/gzh_ipc
/oem/usr/bin/gzh_ipc \
  -c /oem/usr/share/rv1106_sc3336.ini \
  -a /etc/iqfiles
```

The log is written to both stderr and `/root/rkipc.log`. Stop with `SIGINT` or `SIGTERM` so every
MPI binding, thread, channel, publisher, RockIVA handle and ISP context is released in order.

Open these in VLC:

```text
rtsp://192.168.1.100:8554/main_rtsp  (main video + G711A audio)
rtsp://192.168.1.100:8554/ai_rtsp    (boxed AI video, no audio)
rtmp://192.168.1.100:1935/main_rtmp  (main video + G711A audio)
rtmp://192.168.1.100:1935/ai_rtmp    (boxed AI video, no audio)
```

The RTSP and RTMP publishers intentionally use different MediaMTX paths. A path accepts one active
publisher, so assigning both protocols to `/main` (or both AI publishers to `/ai`) would make the
second connection replace or reject the first.

## Codec rule

With FFmpeg 4.x, H.265 can be published through RTSP but not through the traditional FLV muxer
used by RTMP. If either video stream selects H.265 while `stream:enable_rtmp=1`, startup fails with
a clear configuration error. Disable RTMP or select H.264. This is intentional; silent creation of
an incompatible stream would be harder to diagnose on the board.

## First board checks

```sh
tail -f /root/rkipc.log
ffprobe -show_streams rtsp://PC_IP:8554/main_rtsp
ffprobe -show_streams rtsp://PC_IP:8554/ai_rtsp
```

Confirm that the main stream reports one video and one `pcm_alaw` audio track, the AI stream only
one video track, boxes appear when a person enters the configured region, and audio drift remains
bounded during a 30-minute VLC playback.
