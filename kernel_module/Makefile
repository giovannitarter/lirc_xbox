

obj-m += lirc_xbox.o

all:
	make -C /lib/modules/$(shell uname -r)/build M=$(PWD) modules
	mkdir -p /lib/modules/$$(uname -r)/extra
	cp lirc_xbox.ko /lib/modules/$$(uname -r)/extra
	depmod -a

clean:
	make -C /lib/modules/$(shell uname -r)/build M=$(PWD) clean
