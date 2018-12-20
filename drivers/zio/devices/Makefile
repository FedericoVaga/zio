# add versions of supermodule
ifdef CONFIG_SUPER_REPO
ifdef CONFIG_SUPER_REPO_VERSION
SUBMODULE_VERSIONS += MODULE_INFO(version_$(CONFIG_SUPER_REPO),\"$(CONFIG_SUPER_REPO_VERSION)\");
endif
endif

ccflags-y += -DADDITIONAL_VERSIONS="$(SUBMODULE_VERSIONS)"

ccflags-y += -I$(src)/../../../include/ -DGIT_VERSION=\"$(GIT_VERSION)\"
ccflags-y += $(ZIO_VERSION)
ccflags-$(CONFIG_ZIO_DEBUG) += -DDEBUG

obj-m := zio-zero.o
obj-m += zio-loop.o
obj-m += zio-irq-tdc.o
obj-m += zio-fake-dtc.o
obj-m += zio-mini.o
obj-m += zio-gpio.o

ifdef CONFIG_USB
obj-m += zio-vmk8055.o
endif

ifdef CONFIG_SPI
obj-m += zio-ad788x.o
endif
