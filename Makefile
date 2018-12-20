# include parent_common.mk for buildsystem's defines
# It allows you to inherit an environment configuration from larger project
REPO_PARENT ?= ..
-include $(REPO_PARENT)/parent_common.mk

LINUX ?= /lib/modules/$(shell uname -r)/build

all: modules tools

modules:
	$(MAKE) -C $(LINUX) M=$(shell /bin/pwd)/drivers

modules_install:
	$(MAKE) -C $(LINUX) M=$(shell /bin/pwd)/drivers $@

coccicheck:
	$(MAKE) -C $(LINUX) M=$(shell /bin/pwd)/drivers coccicheck


.PHONY: tools

tools:
	$(MAKE) -C tools M=$(shell /bin/pwd)

# this make clean is ugly, I'm aware...
clean:
	rm -rf `find . -name \*.o -o -name \*.ko -o -name \*~ `
	rm -rf `find . -name Module.\* -o -name \*.mod.c`
	rm -rf `find . -name \*.ko.cmd -o -name \*.o.cmd`
	rm -rf `find . -name modules.order`
	rm -rf `find . -name *.a`
	rm -rf .tmp_versions modules.order
	$(MAKE) -C tools M=$(shell /bin/pwd) clean
