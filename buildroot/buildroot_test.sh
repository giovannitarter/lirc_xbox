#!/bin/bash

sudo -v

BUILDROOT_DIR="buildroot/buildroot-2018.08/"
IMAGES_DIR="$BUILDROOT_DIR/output/images"
TARGET_DIR="$BUILDROOT_DIR/output/target"
HEADERS_DIR="$BUILDROOT_DIR/output/build/linux-4.17.19"
HEADERS_ABS="$(realpath $HEADERS_DIR)"
export HEADERS

pushd lirc_xbox/kernel_module
make clean
HEADERS=$HEADERS_ABS make build
RES="$?"
popd

if [ "$RES" != "0" ];
then
    echo "error compiling"
    exit 1
fi



cp lirc_xbox/kernel_module/xbox_remote.ko $TARGET_DIR/

pushd $BUILDROOT_DIR
make
popd


sudo qemu-system-x86_64 \
    -kernel $IMAGES_DIR/bzImage \
    -initrd $IMAGES_DIR/rootfs.cpio \
    -nographic \
    -serial mon:stdio \
    -enable-kvm \
    -usb \
    -device usb-host,vendorid=0x045e,productid=0x0284 \
    -netdev user,id=user.0 -device e1000,netdev=user.0 \
    -m 512M \
    -append 'console=ttyS0'




