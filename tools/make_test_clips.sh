#!/usr/bin/env bash
# Generate the H.264 test clips used by the GES timeline evidence.
#
# Each 320x240 @ 30 fps clip burns its own source frame number N in two ways:
#   - readable text in the middle;
#   - a machine-readable strip of 16 cells, 20 px wide: cell 0 is always
#     white (alpha reference), cells 1..12 hold bits 0..11 of N, cells 13..15
#     are black. Clip A draws the strip in rows 0..31, clip B in rows
#     208..239, so both stay decodable while they are cross-faded.
# x264 defaults keep B-frames; -g 30 puts keyframes every second so seeks
# land on non-keyframes.
set -euo pipefail

out=${1:-build/ys-media}
frames=${FRAMES:-90}
mkdir -p "$out"

bits='if(eq(floor(X/20),0),255,if(between(floor(X/20),1,12),255*mod(floor(N/pow(2,floor(X/20)-1)),2),0))'

make_clip() {
  local name=$1 strip_cond=$2 bg_r=$3 bg_g=$4 bg_b=$5 label=$6
  local strip_other
  if [[ $strip_cond == top ]]; then
    strip='lt(Y,32)'; strip_other='gt(Y,207)'
  else
    strip='gt(Y,207)'; strip_other='lt(Y,32)'
  fi
  ffmpeg -hide_banner -loglevel error -y \
    -f lavfi -i "color=c=black:s=320x240:r=30:d=$((frames / 30))" \
    -vf "format=rgb24,geq=r='if(${strip},${bits},if(${strip_other},0,${bg_r}))':g='if(${strip},${bits},if(${strip_other},0,${bg_g}))':b='if(${strip},${bits},if(${strip_other},0,${bg_b}))',drawtext=fontfile=/usr/share/fonts/truetype/dejavu/DejaVuSansMono-Bold.ttf:text='${label} %{eif\\:n\\:d\\:4}':fontsize=40:fontcolor=white:x=(w-tw)/2:y=(h-th)/2,format=yuv420p" \
    -frames:v "$frames" -c:v libx264 -preset medium -crf 12 -g 30 -bf 2 \
    -movflags +faststart "$out/$name.mp4"
}

make_clip clip-a top 150 30 30 A
make_clip clip-b bottom 30 30 150 B
ls -l "$out"/clip-a.mp4 "$out"/clip-b.mp4
