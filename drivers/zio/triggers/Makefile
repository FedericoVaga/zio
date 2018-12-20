# add versions of supermodule
ifdef CONFIG_SUPER_REPO
ifdef CONFIG_SUPER_REPO_VERSION
SUBMODULE_VERSIONS += MODULE_INFO(version_$(CONFIG_SUPER_REPO),\"$(CONFIG_SUPER_REPO_VERSION)\");
endif
endif

ccflags-y += -DADDITIONAL_VERSIONS="$(SUBMODULE_VERSIONS)"
ccflags-y += -I$(src)/../../../include/ -DGIT_VERSION=\"$(GIT_VERSION)\"
ccflags-$(CONFIG_ZIO_DEBUG) += -DDEBUG

# zio-trig-user.o is now part of zio-core
obj-m = zio-trig-timer.o
obj-m += zio-trig-irq.o
obj-$(CONFIG_HIGH_RES_TIMERS) += zio-trig-hrt.o
