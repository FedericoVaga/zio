include $(src)/../../../scripts/Makefile.include

# add versions of supermodule
ifdef CONFIG_SUPER_REPO
ifdef CONFIG_SUPER_REPO_VERSION
SUBMODULE_VERSIONS += MODULE_INFO(version_$(CONFIG_SUPER_REPO),\"$(CONFIG_SUPER_REPO_VERSION)\");
endif
endif

ccflags-y += -DADDITIONAL_VERSIONS="$(SUBMODULE_VERSIONS)"

ccflags-y += -I$(src)/../../../include/ -DGIT_VERSION=\"$(GIT_VERSION)\"
ccflags-$(CONFIG_ZIO_DEBUG) += -DDEBUG

# zio-buf-kmalloc.o is now part of zio-core
obj-m = zio-buf-vmalloc.o
