include $(src)/../../../scripts/Makefile.include

# include parent_common.mk for buildsystem's defines
# It allows you to inherit an environment configuration from larger project
REPO_PARENT ?= ..
-include $(REPO_PARENT)/parent_common.mk

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

zio-y := core.o chardev.o sysfs.o misc.o
zio-y += bus.o objects.o helpers.o dma.o
zio-y += ../buffers/zio-buf-kmalloc.o ../triggers/zio-trig-user.o

obj-m := zio.o
