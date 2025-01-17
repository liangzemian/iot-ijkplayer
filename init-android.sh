#! /usr/bin/env bash
#
# Copyright (C) 2013-2015 Bilibili
# Copyright (C) 2013-2015 Zhang Rui <bbcallen@gmail.com>
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#

# IJK_FFMPEG_UPSTREAM=git://git.videolan.org/ffmpeg.git
# IJK_FFMPEG_UPSTREAM=https://github.com/liangzemian/FFmpeg-webrtc.git
# IJK_FFMPEG_FORK=https://github.com/liangzemian/FFmpeg-webrtc.git
# IJK_FFMPEG_COMMIT=ijkplayer
# IJK_FFMPEG_LOCAL_REPO=extra/ffmpeg
# IJK_FFMPEG_UPSTREAM=https://github.com/tonychanchen/FFmpeg.git
# IJK_FFMPEG_FORK=https://github.com/tonychanchen/FFmpeg.git
# IJK_FFMPEG_COMMIT=ff4.0--ijk0.8.8--20210205--003
# IJK_FFMPEG_LOCAL_REPO=extra/ffmpeg
IJK_FFMPEG_UPSTREAM=https://github.com/liangzemian/ff6.1.1.git
IJK_FFMPEG_FORK=https://github.com/liangzemian/ff6.1.1.git
IJK_FFMPEG_COMMIT=ijkplayer
IJK_FFMPEG_LOCAL_REPO=extra/ffmpeg

set -e
TOOLS=tools

git --version

echo "== pull ffmpeg base =="
sh $TOOLS/pull-repo-base.sh $IJK_FFMPEG_UPSTREAM $IJK_FFMPEG_LOCAL_REPO

function pull_fork()
{
    echo "== pull ffmpeg fork $1 =="
    sh $TOOLS/pull-repo-ref.sh $IJK_FFMPEG_FORK android/contrib/ffmpeg-$1 ${IJK_FFMPEG_LOCAL_REPO}
    cd android/contrib/ffmpeg-$1
    # git checkout ${IJK_FFMPEG_COMMIT} -B ijkplayer
    # git checkout -b ${IJK_FFMPEG_COMMIT} b8767b1
    # git checkout -b ${IJK_FFMPEG_COMMIT} origin/ijkplayer
    git checkout -b ${IJK_FFMPEG_COMMIT} origin/ff6.1.1--ijk0.8.8
    # echo "== Configure FFmpeg =="
    # ./configure --enable-muxer=whip --enable-openssl --enable-version3 --enable-libx264 --enable-libopus
    # ./configure --enable-muxer=whip --enable-openssl --enable-version3 --enable-libx264 --enable-gpl --enable-libopus --enable-nonfree
    # ./configure --enable-muxer=whip --enable-openssl --enable-version3 --enable-libx264 --enable-gpl --enable-libopus
    # ./configure --extra-cflags=-I/path/libLebConnection/include --extra-ldflags=-L/path/libLebConnection/lib/ --extra-libs=-lLebConnection_so -lc++_shared
    cd -
}

pull_fork "armv5"
pull_fork "armv7a"
pull_fork "arm64"
pull_fork "x86"
pull_fork "x86_64"

./init-config.sh
./init-android-libyuv.sh
./init-android-soundtouch.sh
