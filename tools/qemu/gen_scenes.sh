#!/bin/bash
# gen_scenes.sh <outdir> : synthetic YUV4MPEG2 scene clips for the QEMU image-sensor "scene=" option
# (ingenic-0.7.14). Pure ffmpeg lavfi output, no camera pictures. 320x180, 15 fps, 90 s (the model loops
# the file by the guest's frame number and scales it to the sensor size).
# Luma = mean(t) + 60*(x/w - 0.5) (a horizontal gradient so the picture is not one flat level); chroma is
# 128 unless the clip is the tint one. Timelines (seconds of guest frames):
#   grey       mean 110 static
#   ramp_up    mean 60 until 10 s, linear to 190 at 60 s, then 190
#   ramp_down  mean 190 until 10 s, linear to 60 at 60 s, then 60
#   step       mean 50 until 30 s, then 200 (dark -> bright)
#   tint       mean 110; Cb/Cr 103/153 (warm, red) until 20 s, linear to 153/103 (cool, blue) at 70 s
set -e
O=${1:?outdir}; mkdir -p "$O"
W=320; H=180; FPS=15; D=90
gen() { # name lumaexpr cbexpr crexpr
  ffmpeg -v error -y -f lavfi -i "color=c=gray:s=${W}x${H}:r=$FPS:d=$D,format=yuv420p,geq=lum='clip($2+60*(X/W-0.5),16,235)':cb='$3':cr='$4'" \
    -pix_fmt yuv420p -f yuv4mpegpipe "$O/$1.y4m"
}
gen grey      "110" 128 128
gen ramp_up   "if(lt(T,10),60,if(lt(T,60),60+130*(T-10)/50,190))" 128 128
gen ramp_down "if(lt(T,10),190,if(lt(T,60),190-130*(T-10)/50,60))" 128 128
gen step      "if(lt(T,30),50,200)" 128 128
gen tint      "110" "if(lt(T,20),103,if(lt(T,70),103+50*(T-20)/50,153))" "if(lt(T,20),153,if(lt(T,70),153-50*(T-20)/50,103))"
ls -l "$O"/*.y4m
