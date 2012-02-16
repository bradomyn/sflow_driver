obj-m           := wr_rtu.o
LINUX           ?= ../../../kernel

export ARCH ?= arm
export CROSS_COMPILE ?= $(CROSS_COMPILE_ARM)

all modules:
	$(MAKE) -C $(LINUX) SUBDIRS=$(shell /bin/pwd) modules

# We might "$(MAKE) -C $(LINUX)" but "make clean" with no LINUX defined
# is sometimes useful to have
clean:
	rm -f *.mod.c *.o *.ko *.i .*cmd Module.symvers modules.order *~
	rm -rf .tmp_versions
