#!/bin/bash


#HEADERS_DIR="/home/pupillo/uarchlinux/linux_kernel/linux-4.18.8"

if [ -z "$HEADERS" ];
then
    HEADERS="/lib/modules/$(uname -r)/build/"
fi

export HEADERS

pushd xbox_remote
make clean
#make build 
make
RES="$?"
popd

if [ "$RES" != "0" ];
then
    echo "error compiling"
    exit 1
fi

pushd xbox_remote_keymap
make clean
#make build
make
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













