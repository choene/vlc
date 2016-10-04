#
# Debian Jessie Desktop (MATE) Dockerfile for compiling VNC
#
# https://github.com/choene/vlc-docker
#

# Pull base image.
FROM mikelh/debian-desktop

MAINTAINER Christian Hoene <christian.hoene@symonics>

RUN apt-get update && \
	DEBIAN_FRONTEND=noninteractive apt-get install -y \
	git libtool build-essential pkg-config autoconf

RUN DEBIAN_FRONTEND=noninteractive apt-get -y upgrade

# From apt-rdepends --build-depends --follow=DEPENDS vlc
RUN DEBIAN_FRONTEND=noninteractive apt-get install -y \
	autopoint debhelper dh-autoreconf dh-buildinfo gettext liba52-0.7.4-dev \
	libaa1-dev libasound2-dev libass-dev libavahi-client-dev libavc1394-dev \
	libbluray-dev libcaca-dev libcddb2-dev \
	libcdio-dev libchromaprint-dev libcrystalhd-dev libdbus-1-dev \
	libdc1394-22-dev libdca-dev libdirectfb-dev libdvbpsi-dev \
	libdvdnav-dev libdvdread-dev libegl1-mesa-dev libfaad-dev libflac-dev \
	libfluidsynth-dev libfreerdp-dev libfreetype6-dev libfribidi-dev \
	libgl1-mesa-dev libgles1-mesa-dev libgles2-mesa-dev libgnutls28-dev \
	libgtk2.0-dev libidn11-dev libiso9660-dev libjack-dev libkate-dev \
	liblircclient-dev liblivemedia-dev liblua5.2-dev libmad0-dev libmatroska-dev \
	libmodplug-dev libmpcdec-dev libmpeg2-4-dev libmtp-dev libncursesw5-dev \
	libnotify-dev libogg-dev libomxil-bellagio-dev libopus-dev libpng-dev \
	libpostproc-dev libpulse-dev libqt4-dev libraw1394-dev libresid-builder-dev \
	librsvg2-dev libsamplerate0-dev libschroedinger-dev libsdl-image1.2-dev \
	libsdl1.2-dev libshine-dev libshout3-dev libsidplay2-dev libsmbclient-dev \
	libspeex-dev libspeexdsp-dev libssh2-1-dev libswscale-dev libtag1-dev \
	libtheora-dev libtwolame-dev libudev-dev libupnp-dev libv4l-dev libva-dev \
	libvcdinfo-dev libvdpau-dev libvncserver-dev libvorbis-dev libx11-dev \
	libx264-dev libxcb-composite0-dev libxcb-keysyms1-dev libxcb-randr0-dev \
	libxcb-shm0-dev libxcb-xv0-dev libxcb1-dev libxext-dev \
	libxml2-dev libxpm-dev libzvbi-dev lua5.2 pkg-config zlib1g-dev

RUN DEBIAN_FRONTEND=noninteractive apt-get install -y \
	protobuf-compiler

RUN DEBIAN_FRONTEND=noninteractive apt-get install -y \
	libnetcdf-dev

RUN DEBIAN_FRONTEND=noninteractive apt-get install -y \
	subversion yasm cvs cmake ragel

RUN DEBIAN_FRONTEND=noninteractive apt-get install -y \
	libhidapi-dev
# 
# in vlc, you can run:
#% cd contrib
#% mkdir native
#% cd native
#% ../bootstrap
#% make

# ./configure --enable-run-as-root --disable-archive
