NAME = simrupt
obj-m := sum3.o
sum3-objs += game.o
sum3-objs += mt19937-64.o
sum3-objs += zobrist.o
sum3-objs += negamax.o
sum3-objs += $(NAME).o

KDIR ?= /lib/modules/`uname -r`/build
PWD := $(shell pwd)

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
