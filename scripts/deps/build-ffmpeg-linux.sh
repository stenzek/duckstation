#!/usr/bin/env bash

set -e

if [ "$#" -lt 1 ]; then
    echo "Syntax: $0 <output director>"
    exit 1
fi

for arg in "$@"; do
	if [ "$arg" == "-skip-download" ]; then
		echo "Not downloading sources."
		SKIP_DOWNLOAD=true
		shift
	elif [ "$arg" == "-skip-cleanup" ]; then
		echo "Not removing build directory."
		SKIP_CLEANUP=true
		shift
	elif [ "$arg" == "" ]; then
		# Eat empty args.
		shift
	fi
done

SCRIPTDIR=$(realpath $(dirname "${BASH_SOURCE[0]}"))
NPROCS="$(getconf _NPROCESSORS_ONLN)"
INSTALLDIR="$1"
if [ "${INSTALLDIR:0:1}" != "/" ]; then
	INSTALLDIR="$PWD/$INSTALLDIR"
fi

mkdir -p deps-build
cd deps-build

DEPSINSTALLDIR="$PWD/ffmpeg-deps"
echo "Installation directory is $INSTALLDIR"
echo "FFmpeg dependencies directory is $DEPSINSTALLDIR"

FFMPEG=7.0.2
LAME=3.100
LIBVPX=1.14.1
FDK_AAC=0fc0e0e0b89de3becd5f099eae725f13eeecc0d1
LIBAOM=3ab84a7710ee34db3e43b3e61c7d69ab80276a33
LIBOGG=1.3.5
LIBVORBIS=1.3.7
LIBTHEORA=1.1.1
FLAC=1.4.3
SPEEX=1.2.0
AMF=1.4.34
OPUS=1.5.2
SVT_AV1=2.2.1

# Encoder list from freedesktop SDK, which apparently came from Fedora.
# Disabled list: av1_qsv h264_qsv hevc_qsv mjpeg_qsv mpeg2_qsv vc1_qsv vp8_qsv vp9_qsv
# av1_nvenc h264_nvenc hevc_nvenc libxvid libtwolame libopenh264 libgsm libgsm_ms
# ilbc libilbc libopencore_amrnb libopenjpeg libvo_amrwbenc libjxl libcodec2 hap librav1e
FFMPEG_ENCODER_LIST=""\
"a64multi a64multi5 aac libfdk_aac ac3 adpcm_adx "\
"adpcm_argo adpcm_g722 adpcm_g726 adpcm_g726le adpcm_ima_alp adpcm_ima_amv "\
"adpcm_ima_apm adpcm_ima_qt adpcm_ima_ssi adpcm_ima_wav adpcm_ima_ws adpcm_ms "\
"adpcm_swf adpcm_yamaha alac alias_pix amv anull "\
"apng ass asv1 asv2 av1_amf "\
"av1_vaapi ayuv bitpacked bmp cinepak "\
"cljr dca dfpwm dnxhd dpx dvbsub "\
"dvdsub dvvideo exr ffv1 ffvhuff flac "\
"flashsv flashsv2 flv g723_1 gif h261 "\
"h263 h263_v4l2m2m h263p h264_amf "\
"h264_v4l2m2m h264_vaapi hdr hevc_amf "\
"hevc_v4l2m2m hevc_vaapi huffyuv jpegls "\
"jpeg2000 libaom libaom_av1 libmp3lame "\
"libopus libschroedinger libspeex libsvtav1 libtheora "\
"libvorbis libvpx_vp8 libvpx_vp9 libwebp "\
"libwebp_anim mjpeg mjpeg_vaapi mlp "\
"mp2 mp2fixed mpeg1video mpeg2video mpeg2_vaapi "\
"mpeg4 mpeg4_v4l2m2m msmpeg4v2 msmpeg4v3 msvideo1 nellymoser "\
"opus pam pbm pcm_alaw pcm_f32be pcm_f32le "\
"pcm_f64be pcm_f64le pcm_mulaw pcm_s16be pcm_s16be_planar pcm_s16le "\
"pcm_s16le_planar pcm_s24be pcm_s24le pcm_s24le_planar pcm_s32be pcm_s32le "\
"pcm_s32le_planar pcm_s8 pcm_s8_planar pcm_u16be pcm_u16le pcm_u24be "\
"pcm_u24le pcm_u32be pcm_u32le pcm_u8 pcx pgm "\
"pgmyuv phm png ppm qoi qtrle "\
"r10k r210 ra_144 rawvideo roq roq_dpcm "\
"rpza rv10 rv20 s302m sbc sgi "\
"smc snow sonic sonic_ls speedhq srt "\
"ssa subrip sunrast svq1 targa text "\
"tiff truehd tta ttml utvideo v210 "\
"v308 v408 v410 vc1_v4l2m2m vc2 "\
"vnull vorbis vp8_v4l2m2m vp8_vaapi "\
"vp9_vaapi wavpack wbmp webvtt wmav1 wmav2 "\
"wmv1 wmv2 wrapped_avframe xbm xface xsub "\
"xwd y41p yuv4 zlib zmbv"
FFMPEG_ENCODERS=""
for encoder in $FFMPEG_ENCODER_LIST; do
	if [ -z "$FFMPEG_ENCODERS" ]; then
		FFMPEG_ENCODERS="--enable-encoder=$encoder"
	else
		FFMPEG_ENCODERS="$FFMPEG_ENCODERS,$encoder"
	fi
done

if [ "$SKIP_DOWNLOAD" != true ]; then
	if [ ! -f "ffmpeg-$FFMPEG.tar.xz" ]; then
		curl -C - -L -O "https://ffmpeg.org/releases/ffmpeg-$FFMPEG.tar.xz"
	fi
	if [ ! -f "lame-$LAME.tar.gz" ]; then
		curl -C - -L -o "lame-$LAME.tar.gz" "https://sourceforge.net/projects/lame/files/lame/$LAME/lame-$LAME.tar.gz/download"
	fi
	if [ ! -f "libvpx-$LIBVPX.tar.gz" ]; then
		curl -C - -L -o "libvpx-$LIBVPX.tar.gz" "https://github.com/webmproject/libvpx/archive/refs/tags/v$LIBVPX.tar.gz"
	fi
	if [ ! -f "fdk-aac-stripped-$FDK_AAC.tar.gz" ]; then
		curl -C - -L -o "fdk-aac-stripped-$FDK_AAC.tar.gz" "https://gitlab.freedesktop.org/wtaymans/fdk-aac-stripped/-/archive/$FDK_AAC/fdk-aac-stripped-$FDK_AAC.tar.gz"
	fi
	if [ ! -d "aom" ]; then
		git clone https://aomedia.googlesource.com/aom
		cd aom
		git checkout "$LIBAOM"
		cd ..
	fi
	if [ ! -f "libogg-$LIBOGG.tar.gz" ]; then
		curl -C - -L -O "https://downloads.xiph.org/releases/ogg/libogg-$LIBOGG.tar.gz"
	fi
	if [ ! -f "libvorbis-$LIBVORBIS.tar.gz" ]; then
		curl -C - -L -O "https://github.com/xiph/vorbis/releases/download/v$LIBVORBIS/libvorbis-$LIBVORBIS.tar.gz"
	fi
	if [ ! -f "libtheora-$LIBTHEORA.tar.bz2" ]; then
		curl -C - -L -O "https://downloads.xiph.org/releases/theora/libtheora-$LIBTHEORA.tar.bz2"
	fi
	if [ ! -f "flac-$FLAC.tar.xz" ]; then
		curl -C - -L -O "https://downloads.xiph.org/releases/flac/flac-$FLAC.tar.xz"
	fi
	if [ ! -f "speex-$SPEEX.tar.gz" ]; then
		curl -C - -L -O "https://downloads.xiph.org/releases/speex/speex-$SPEEX.tar.gz"
	fi
	if [ ! -f "AMF-headers.tar.gz" ]; then
		curl -C - -L -O "https://github.com/GPUOpen-LibrariesAndSDKs/AMF/releases/download/v$AMF/AMF-headers.tar.gz"
	fi
	if [ ! -f "opus-$OPUS.tar.gz" ]; then
		curl -C - -L -O "https://downloads.xiph.org/releases/opus/opus-$OPUS.tar.gz"
	fi
	if [ ! -f "SVT-AV1-$SVT_AV1.tar.gz" ]; then
		curl -C - -L -O "https://gitlab.com/AOMediaCodec/SVT-AV1/-/archive/v$SVT_AV1/SVT-AV1-$SVT_AV1.tar.gz"
	fi
fi

cat > SHASUMS <<EOF
5393759308f6d7bc9eb1ed8013c954e03aadb85f0ed6e96f969a5df447b0f79c  AMF-headers.tar.gz
7322744f239a0d8460fde84e92cca77f2fe9d7e25a213789659df9e86b696b42  fdk-aac-stripped-$FDK_AAC.tar.gz
8646515b638a3ad303e23af6a3587734447cb8fc0a0c064ecdb8e95c4fd8b389  ffmpeg-$FFMPEG.tar.xz
6c58e69cd22348f441b861092b825e591d0b822e106de6eb0ee4d05d27205b70  flac-$FLAC.tar.xz
ddfe36cab873794038ae2c1210557ad34857a4b6bdc515785d1da9e175b1da1e  lame-$LAME.tar.gz
0eb4b4b9420a0f51db142ba3f9c64b333f826532dc0f48c6410ae51f4799b664  libogg-$LIBOGG.tar.gz
b6ae1ee2fa3d42ac489287d3ec34c5885730b1296f0801ae577a35193d3affbc  libtheora-$LIBTHEORA.tar.bz2
0e982409a9c3fc82ee06e08205b1355e5c6aa4c36bca58146ef399621b0ce5ab  libvorbis-$LIBVORBIS.tar.gz
b6ae1ee2fa3d42ac489287d3ec34c5885730b1296f0801ae577a35193d3affbc  libtheora-$LIBTHEORA.tar.bz2
901747254d80a7937c933d03bd7c5d41e8e6c883e0665fadcb172542167c7977  libvpx-$LIBVPX.tar.gz
65c1d2f78b9f2fb20082c38cbe47c951ad5839345876e46941612ee87f9a7ce1  opus-$OPUS.tar.gz
eaae8af0ac742dc7d542c9439ac72f1f385ce838392dc849cae4536af9210094  speex-$SPEEX.tar.gz
66ba0c0c33329e776e363432cf9bdf22e78f10e3771c3e36a8af5bbef13f3356  SVT-AV1-$SVT_AV1.tar.gz
EOF

shasum -a 256 --check SHASUMS

export PKG_CONFIG_PATH="$INSTALLDIR/lib/pkgconfig:$INSTALLDIR/lib64/pkgconfig:$DEPSINSTALLDIR/lib/pkgconfig:$DEPSINSTALLDIR/lib64/pkgconfig:$PKG_CONFIG_PATH"

echo "Building LAME"
rm -fr "lame-$LAME"
tar xf "lame-$LAME.tar.gz"
cd "lame-$LAME"
mkdir build
cd build
../configure --prefix="$DEPSINSTALLDIR" --disable-shared --enable-static --with-pic --disable-frontend
make -j "$NPROCS"
make install
cd ../..

echo "Building libvpx..."
rm -fr "libvpx-$LIBVPX"
tar xf "libvpx-$LIBVPX.tar.gz"
cd "libvpx-$LIBVPX"
mkdir build-ds
cd build-ds
../configure --prefix="$DEPSINSTALLDIR" --disable-shared --enable-static --enable-pic --disable-examples --disable-tools --disable-docs --enable-vp8 --enable-vp9
make -j "$NPROCS"
make install
cd ../..

echo "Building fdk-aac..."
rm -fr "fdk-aac-stripped-$FDK_AAC"
tar xf "fdk-aac-stripped-$FDK_AAC.tar.gz"
cd "fdk-aac-stripped-$FDK_AAC"
./autogen.sh
mkdir build
cd build
../configure --prefix="$DEPSINSTALLDIR" --enable-static --disable-shared --with-pic
make -j "$NPROCS"
make install
cd ../..

echo "Building libaom..."
cd aom
rm -fr build-ds
cmake -B build-ds -G Ninja -DCMAKE_INSTALL_PREFIX="$DEPSINSTALLDIR" -DCMAKE_PREFIX_PATH="$DEPSINSTALLDIR" -DCMAKE_BUILD_TYPE=Release -DCMAKE_POSITION_INDEPENDENT_CODE=ON -DBUILD_SHARED_LIBS=OFF -DENABLE_DOCS=OFF -DENABLE_EXAMPLES=OFF -DENABLE_TESTDATA=OFF -DENABLE_TESTS=OFF -DENABLE_TOOLS=OFF
cmake --build build-ds --parallel
cmake --install build-ds
cd ..

echo "Building libogg..."
rm -fr "libogg-$LIBOGG"
tar xf "libogg-$LIBOGG.tar.gz"
cd "libogg-$LIBOGG"
mkdir build-ds
cd build-ds
../configure --prefix="$DEPSINSTALLDIR" --enable-static --disable-shared --with-pic
make -j "$NPROCS"
make install
cd ../..

echo "Building libvorbis..."
rm -fr "libvorbis-$LIBVORBIS"
tar xf "libvorbis-$LIBVORBIS.tar.gz"
cd "libvorbis-$LIBVORBIS"
mkdir build-ds
cd build-ds
../configure --prefix="$DEPSINSTALLDIR" --enable-static --disable-shared --with-pic
make -j "$NPROCS"
make install
cd ../..

echo "Building libtheora..."
rm -fr "libtheora-$LIBTHEORA"
tar xf "libtheora-$LIBTHEORA.tar.bz2"
cd "libtheora-$LIBTHEORA"
mkdir build-ds
cd build-ds
../configure --prefix="$DEPSINSTALLDIR" --enable-static --disable-shared --with-pic --disable-examples --disable-sdltest --disable-vorbistest --disable-oggtest
make -j "$NPROCS"
make install
cd ../..

echo "Building speex..."
rm -fr "speex-$SPEEX"
tar xf "speex-$SPEEX.tar.gz"
cd "speex-$SPEEX"
mkdir build-ds
cd build-ds
../configure --prefix="$DEPSINSTALLDIR" --enable-static --disable-shared --with-pic
make -j "$NPROCS"
make install
cd ../..

echo "Building flac..."
rm -fr "flac-$FLAC"
tar xf "flac-$FLAC.tar.xz"
cd "flac-$FLAC"
mkdir build-ds
cd build-ds
../configure --prefix="$DEPSINSTALLDIR" --enable-static --disable-shared --with-pic --disable-examples --disable-programs
make -j "$NPROCS"
make install
cd ../..

echo "Installing AMF..."
rm -fr "AMF"
tar xf "AMF-headers.tar.gz"
cd "AMF"
mkdir -p "$DEPSINSTALLDIR/include/AMF"
cp -a core components "$DEPSINSTALLDIR/include/AMF"
cd ..

echo "Building libopus..."
rm -fr "opus-$OPUS"
tar xf "opus-$OPUS.tar.gz"
cd "opus-$OPUS"
mkdir build
cd build
../configure --prefix="$DEPSINSTALLDIR" --enable-static --disable-shared --with-pic --disable-doc
make -j "$NPROCS"
make install
cd ../..

echo "Building SVT-AV1..."
rm -fr SVT-AV1-v$SVT_AV1-*
tar xf "SVT-AV1-$SVT_AV1.tar.gz"
cd SVT-AV1-v$SVT_AV1-*
cmake -B build-ds -G Ninja -DCMAKE_INSTALL_PREFIX="$DEPSINSTALLDIR" -DCMAKE_PREFIX_PATH="$DEPSINSTALLDIR" -DCMAKE_BUILD_TYPE=Release -DCMAKE_POSITION_INDEPENDENT_CODE=ON -DBUILD_SHARED_LIBS=OFF -DBUILD_TESTING=OFF
cmake --build build-ds --parallel
cmake --install build-ds
cd ..

echo "Building ffmpeg..."
rm -fr "ffmpeg-$FFMPEG"
tar xf "ffmpeg-$FFMPEG.tar.xz"
cd "ffmpeg-$FFMPEG"
mkdir build
cd build
../configure --prefix="$INSTALLDIR" --disable-static --enable-shared \
    --pkg-config-flags="--static" \
    --extra-cflags="-I$DEPSINSTALLDIR/include" \
    --extra-ldflags="-L$DEPSINSTALLDIR/lib" --extra-ldflags="-L$DEPSINSTALLDIR/lib64" \
    --extra-ldsoflags="-Wl,-rpath,XORIGIN" \
    --disable-all --disable-autodetect --enable-libmp3lame --enable-libvpx --enable-zlib --enable-libwebp \
    --enable-libfdk-aac --enable-libaom --enable-libvorbis --enable-libtheora --enable-libspeex \
    --enable-v4l2-m2m --enable-vaapi --enable-amf --enable-libopus --enable-libsvtav1 \
    --enable-avcodec --enable-avformat --enable-avutil --enable-swresample --enable-swscale \
    --enable-muxer=avi,matroska,mov,mp3,mp4,wav \
    --enable-protocol=file \
    $FFMPEG_ENCODERS

make -j "$NPROCS"
make install

# Fix up rpath to point to current directory.
find "$INSTALLDIR" -name 'libavcodec.so' -exec patchelf --set-rpath '$ORIGIN' {} \;
find "$INSTALLDIR" -name 'libavformat.so' -exec patchelf --set-rpath '$ORIGIN' {} \;
find "$INSTALLDIR" -name 'libavutil.so' -exec patchelf --set-rpath '$ORIGIN' {} \;
find "$INSTALLDIR" -name 'libswresample.so' -exec patchelf --set-rpath '$ORIGIN' {} \;
find "$INSTALLDIR" -name 'libswscale.so' -exec patchelf --set-rpath '$ORIGIN' {} \;

cd ../..

if [ "$SKIP_CLEANUP" != true ]; then
	echo "Cleaning up..."
	cd ..
	rm -fr deps-build
fi
