#!/bin/bash


HEADERS_DIR="/home/pupillo/uarchlinux/linux_kernel/linux-4.18.8"

export HEADERS

pushd kernel_module
make clean
make build HEADERS=$HEADERS_DIR
RES="$?"
popd

pushd xbox_remote_keymap
make clean
make build HEADERS=$HEADERS_DIR
RES="$?"
popd

if [ "$RES" != "0" ];
then
    echo "error compiling"
    exit 1
fi














