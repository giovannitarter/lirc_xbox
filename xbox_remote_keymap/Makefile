
DRIVER_NAME=xbox_remote_keymap
HEADERS ?= /lib/modules/$(shell uname -r)/build

obj-m += $(DRIVER_NAME).o


all: build install


xbox_remote_keymap.ko: xbox_remote_keymap.c
	make -C $(HEADERS)  M=$(PWD) modules


build: $(DRIVER_NAME).ko


install:
	mkdir -p /lib/modules/$$(uname -r)/extra
	cp $(DRIVER_NAME).ko /lib/modules/$$(uname -r)/extra
	depmod -a


clean:
	make -C $(HEADERS) M=$(PWD) clean
