#!/usr/bin/env bash

set -euxo pipefail

PROJECT="$1"

packages=(
	'build-essential'
	'curl'
	'gcc-14-i686-linux-gnu'
	'qemu-system-x86'
)

apt-get update -qq
# Install LD_PRELOAD utility to disable fsync() and speed up dpkg batches
apt-get install -qqy eatmydata
# Install project dependencies via OS package manager
eatmydata apt-get install -qqy "${packages[@]}" > /dev/null

# Kludge toolchain paths: linux-gnu -> elf
ln -s /usr/bin/i686-linux-gnu-gcc-14 /usr/bin/i686-elf-gcc
for tool in ld objcopy ar ranlib ; do
	ln -s /usr/bin/i686-linux-gnu-$tool /usr/bin/i686-elf-$tool
done

# Workaround distro package's lack of nogui support
if [ "$PROJECT" == threads ] ; then
	curl -Lo bochs.tgz https://github.com/bochs-emu/Bochs/archive/refs/tags/REL_3_0_FINAL.tar.gz
	tar zxf bochs.tgz
	cd Bochs-*/bochs

	./configure --prefix=/usr --with-nogui
	sed -i 's/bx_print_header();//g' main.cc
	make -j "$(nproc)"

	make install
fi
