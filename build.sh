#!/bin/sh
# Build ./tetra-sniff from a fresh clone. Extra arguments go to the CMake
# configure step, for example: ./build.sh -DCMAKE_BUILD_TYPE=Debug
#
# The script installs the host packages and the Soapy device plugins this
# OS ships, the same way a Dockerfile would. SKIP_DEPS=1 skips the host
# packages. SOAPY_SKIP_MODULES=1 skips the plugins.
set -eu
cd "$(dirname "$0")"

DEMOD=sdrpp-tetra-demodulator
CODEC=$DEMOD/src/decoder/codec/c-code
# fix_64bit.patch adds this typedef, and the build needs it on a 64-bit host.
# The file itself comes from the ZIP, so only the typedef shows that the
# patches ran. download_and_patch.sh exits 0 even when every patch fails.
PATCH_MARK="typedef int16_t Word16"

pkg_have() {
	case $(uname -s) in
	Darwin) brew list --formula --versions "$1" >/dev/null 2>&1 ;;
	Linux) dpkg-query -W -f='${Status}\n' "$1" 2>/dev/null | grep -q 'install ok installed' ;;
	*) return 1 ;;
	esac
}

apt_updated=0
pkg_install() {
	case $(uname -s) in
	Darwin)
		brew install "$@"
		;;
	Linux)
		if [ "$apt_updated" -eq 0 ]; then
			if [ "$(id -u)" -eq 0 ]; then
				apt-get update -qq
			elif command -v sudo >/dev/null 2>&1; then
				sudo apt-get update -qq
			fi
			apt_updated=1
		fi
		if [ "$(id -u)" -eq 0 ]; then
			apt-get install -y "$@"
		elif command -v sudo >/dev/null 2>&1; then
			sudo apt-get install -y "$@"
		else
			echo "install as root: apt-get install $*" >&2
			return 1
		fi
		;;
	*)
		return 1
		;;
	esac
}

# One line per package. These are what cmake and the codec step need.
host_packages() {
	case $(uname -s) in
	Darwin)
		# coreutils gives md5sum for the ETSI codec script.
		printf '%s\n' cmake volk soapysdr coreutils
		;;
	Linux)
		printf '%s\n' build-essential cmake git curl unzip patch \
			libvolk-dev libsoapysdr-dev
		;;
	esac
}

# Homebrew puts gnubin off PATH. The codec script calls md5sum, not md5.
if [ "$(uname -s)" = Darwin ]; then
	for g in /opt/homebrew/opt/coreutils/libexec/gnubin \
		 /usr/local/opt/coreutils/libexec/gnubin; do
		if [ -x "$g/md5sum" ]; then
			PATH="$g:$PATH"
			export PATH
			break
		fi
	done
fi

if [ "${SKIP_DEPS:-}" = 1 ]; then
	echo "==> packages (skipped)"
else
	echo "==> packages"
	case $(uname -s) in
	Darwin)
		if ! command -v brew >/dev/null 2>&1; then
			echo "Homebrew is not on the PATH. Install it from https://brew.sh" >&2
			exit 1
		fi
		if ! command -v cc >/dev/null 2>&1; then
			echo "install the Xcode command line tools: xcode-select --install" >&2
			exit 1
		fi
		;;
	Linux)
		if ! command -v apt-cache >/dev/null 2>&1; then
			echo "this script installs packages with apt. Install cmake, volk and SoapySDR yourself." >&2
			exit 1
		fi
		;;
	*)
		echo "no package list for $(uname -s). Install cmake, volk and SoapySDR yourself." >&2
		exit 1
		;;
	esac
	need=
	have=
	while IFS= read -r pkg; do
		[ -n "$pkg" ] || continue
		if pkg_have "$pkg"; then
			have="$have $pkg"
		else
			need="$need $pkg"
		fi
	done <<EOF
$(host_packages)
EOF
	[ -n "$have" ] && echo "already installed:${have}"
	if [ -n "$need" ]; then
		echo "installing:${need}"
		pkg_install $need
	else
		echo "nothing new to install"
	fi
	if [ "$(uname -s)" = Darwin ]; then
		for g in /opt/homebrew/opt/coreutils/libexec/gnubin \
			 /usr/local/opt/coreutils/libexec/gnubin; do
			if [ -x "$g/md5sum" ]; then
				PATH="$g:$PATH"
				export PATH
				break
			fi
		done
	fi
fi

# SoapySDR finds a stick only after that stick's factory plugin is on disk.
# The plugins are OS packages, not something this tree compiles, so the set
# that can be included is whatever this host ships. SOAPY_SKIP_MODULES=1
# leaves the host as it is (an IQ-pipe build, or a machine that already
# has the modules).
if [ "${SOAPY_SKIP_MODULES:-}" = 1 ]; then
	echo "==> SoapySDR modules (skipped)"
else
	echo "==> SoapySDR modules"
	soapy_skip() {
		case $1 in
		soapysdr|soapyremote|soapysdr-module-all|soapysdr-module-remote|\
		soapysdr-module-audio|soapysdr-module-osmosdr) return 0 ;;
		esac
		return 1
	}
	soapy_list() {
		case $(uname -s) in
		Darwin)
			# limesuite is where Homebrew puts the Lime factory.
			{
				brew search --formula soapy 2>/dev/null || true
				echo limesuite
			} | awk '/^soapy/ || $0 == "limesuite" { print }' | sort -u
			;;
		Linux)
			apt-cache search --names-only '^soapysdr-module-' 2>/dev/null | awk '{ print $1 }'
			;;
		esac
	}
	avail=
	need=
	have=
	while IFS= read -r pkg; do
		[ -n "$pkg" ] || continue
		soapy_skip "$pkg" && continue
		avail="$avail $pkg"
		if pkg_have "$pkg"; then
			have="$have $pkg"
		else
			need="$need $pkg"
		fi
	done <<EOF
$(soapy_list)
EOF
	if [ -z "$avail" ]; then
		echo "none listed for this OS"
	else
		echo "available:${avail}"
		[ -n "$have" ] && echo "already installed:${have}"
		if [ -n "$need" ]; then
			echo "installing:${need}"
			if ! pkg_install $need; then
				echo "Soapy module install failed; enumerate() will only see what is already installed." >&2
			fi
		else
			echo "nothing new to install"
		fi
	fi
	if command -v SoapySDRUtil >/dev/null 2>&1; then
		SoapySDRUtil --info 2>/dev/null | awk '/^Available factories/ { print }'
	fi
fi

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
	git submodule update --init --recursive -- third_party/sdrpp
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
