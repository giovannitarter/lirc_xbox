#!/bin/bash


#HEADERS_DIR="/home/pupillo/uarchlinux/linux_kernel/linux-4.18.8"

HEADERS_DIR="/lib/modules/4.15.0-36-generic/build/"
export HEADERS

pushd kernel_module
make clean
make build HEADERS=$HEADERS_DIR
RES="$?"
popd

if [ "$RES" != "0" ];
then
    echo "error compiling"
    exit 1
fi

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

rm -rf output
mkdir -p output
find . -name "*.ko" -exec cp {} output \;













