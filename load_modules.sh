#!/bin/bash


modprobe rc-core
insmod output/xbox_remote_keymap.ko
insmod output/xbox_remote.ko
