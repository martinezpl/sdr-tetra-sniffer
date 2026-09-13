#!/bin/sh
# Build ./tetra-sniff from a fresh clone. Extra arguments go to the CMake
# configure step, for example: ./build.sh -DCMAKE_BUILD_TYPE=Debug
set -eu
cd "$(dirname "$0")"

DEMOD=sdrpp-tetra-demodulator
CODEC=$DEMOD/src/decoder/codec/c-code
# fix_64bit.patch adds this typedef, and the build needs it on a 64-bit host.
# The file itself comes from the ZIP, so only the typedef shows that the
# patches ran. download_and_patch.sh exits 0 even when every patch fails.
PATCH_MARK="typedef int16_t Word16"

# -DSDRPP_CORE_ROOT points the build at an SDR++ checkout that you already
# have. The submodule is then not needed.
need_sdrpp=1
for arg in "$@"; do
	case $arg in -DSDRPP_CORE_ROOT=*) need_sdrpp=0 ;; esac
done

if [ ! -f $DEMOD/src/decoder/src/tetra_common.h ]; then
	echo "==> submodule $DEMOD"
	git submodule sync -- $DEMOD
	git submodule update --init --recursive -- $DEMOD
fi
if [ $need_sdrpp -eq 1 ] && [ ! -f third_party/sdrpp/core/src/dsp/channel/rx_vfo.h ]; then
	echo "==> submodule third_party/sdrpp"
	git submodule sync -- third_party/sdrpp
	git submodule update --init -- third_party/sdrpp
fi

# ETSI does not allow redistribution of the speech codec, so no repository
# holds it. This downloads it from ETSI and patches it, one time.
if ! grep -qs "$PATCH_MARK" $CODEC/source.h; then
	echo "==> ETSI speech codec"
	# download_and_patch.sh deletes the codec directory itself before it
	# unpacks, so a half applied codec of an earlier run cannot survive.
	(cd $DEMOD/src/decoder/etsi_codec-patches && ./download_and_patch.sh)
	if ! grep -qs "$PATCH_MARK" $CODEC/source.h; then
		echo "the ETSI codec download or patch step failed." >&2
		echo "It needs curl, unzip and patch on the PATH." >&2
		exit 1
	fi
fi

echo "==> cmake"
cmake -S src -B src/build "$@"
cmake --build src/build -j"$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)"

echo
echo "built ./tetra-sniff"
echo "  ./tetra-sniff help     every option and its default"
echo "  ./tetra-sniff sweep    find the carriers of your network"
