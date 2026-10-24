#!/bin/sh
# Generates the test clips of topic mediacodec (tests/media, committed: our
# own synthetic content, a few KB each) with ffmpeg. Only needed to change
# them; the build copies tests/media into /system/media/arcwasm/media.
#
#   Video: 320x240, 30 fps, 1 s (30 frames); frame N is flat grey with luma
#   40 + 5 * N (video range) and neutral chroma, so a test can check every
#   decoded frame and its order; key frames every 10 frames, B-frames where
#   the codec has them. Audio: a 440 Hz sine, 1 s.
#
#   avc.mp4  H.264 High        hevc.mp4  HEVC Main        vp8.webm  VP8
#   vp9.webm VP9 profile 0     aac.m4a   AAC-LC 44.1 kHz stereo
#   adts.aac AAC-LC ADTS 48 kHz mono   opus.webm Opus 48 kHz stereo
#   mp3.mp3  MP3 44.1 kHz mono         pcm.wav   PCM 16-bit 22.05 kHz mono
#   av.mp4   H.264 ramp + AAC sine (MediaPlayer, A/V)
#   red.mp4  H.264, 3 frames of RGB(200, 40, 40) (Surface output colours)
#   demo.mp4 H.264 test pattern (testsrc2) + a 330 Hz AAC tone, 640x360, 6 s
#            (the SurfaceView demo page)
set -e
cd "$(dirname "$0")/.."
OUT=tests/media
mkdir -p "$OUT"
ff() { ffmpeg -hide_banner -loglevel error -y "$@"; }
RAMP="nullsrc=s=320x240:r=30:d=1,format=yuv420p,geq=lum='40+5*N':cb=128:cr=128"
SINE="sine=frequency=440:sample_rate=44100:duration=1"

ff -f lavfi -i "$RAMP" -c:v libx264 -profile:v high -preset medium -g 10 -bf 2 \
   -x264-params "keyint=10:min-keyint=10:scenecut=0" -pix_fmt yuv420p -movflags +faststart "$OUT/avc.mp4"
ff -f lavfi -i "$RAMP" -c:v libx265 -tag:v hvc1 -x265-params "keyint=10:min-keyint=10:bframes=2:log-level=error" \
   -pix_fmt yuv420p -movflags +faststart "$OUT/hevc.mp4"
ff -f lavfi -i "$RAMP" -c:v libvpx -g 10 -b:v 200k "$OUT/vp8.webm"
ff -f lavfi -i "$RAMP" -c:v libvpx-vp9 -g 10 -b:v 200k -row-mt 1 "$OUT/vp9.webm"
ff -f lavfi -i "$SINE" -ac 2 -c:a aac -b:a 96k -movflags +faststart "$OUT/aac.m4a"
ff -f lavfi -i "sine=frequency=440:sample_rate=48000:duration=1" -ac 1 -c:a aac -b:a 64k -f adts "$OUT/adts.aac"
ff -f lavfi -i "sine=frequency=440:sample_rate=48000:duration=1" -ac 2 -c:a libopus -b:a 64k "$OUT/opus.webm"
ff -f lavfi -i "$SINE" -ac 1 -c:a libmp3lame -b:a 64k "$OUT/mp3.mp3"
ff -f lavfi -i "sine=frequency=440:sample_rate=22050:duration=1" -ac 1 -c:a pcm_s16le "$OUT/pcm.wav"
ff -f lavfi -i "$RAMP" -f lavfi -i "$SINE" -c:v libx264 -profile:v high -g 10 -bf 2 \
   -x264-params "keyint=10:min-keyint=10:scenecut=0" -pix_fmt yuv420p -c:a aac -b:a 64k -ac 2 \
   -movflags +faststart -shortest "$OUT/av.mp4"
# (BT.601 signalled in the VUI: unsignalled, both browsers take it for BT.709.)
ff -f lavfi -i "color=c=0xC82828:s=320x240:r=30:d=0.067,format=yuv420p" -c:v libx264 -profile:v high \
   -pix_fmt yuv420p -colorspace smpte170m -color_primaries smpte170m -color_trc smpte170m \
   -color_range tv -movflags +faststart "$OUT/red.mp4"
ff -f lavfi -i "testsrc2=s=640x360:r=30:d=6" -f lavfi -i "sine=frequency=330:sample_rate=44100:duration=6,volume=0.3" \
   -c:v libx264 -profile:v main -g 30 -b:v 600k -pix_fmt yuv420p -c:a aac -b:a 64k -ac 2 \
   -movflags +faststart -shortest "$OUT/demo.mp4"
ls -l "$OUT"
