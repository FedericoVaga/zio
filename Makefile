# include parent_common.mk for buildsystem's defines
# It allows you to inherit an environment configuration from larger project
REPO_PARENT ?= ..
-include $(REPO_PARENT)/parent_common.mk

LINUX ?= /lib/modules/$(shell uname -r)/build
SRC ?= /usr/src
SRC_TARGET := $(SRC)/$(shell git describe --tags --abbrev=0)

all: modules tools

modules:
	$(MAKE) -C $(LINUX) M=$(shell /bin/pwd)/drivers

modules_install:
	$(MAKE) -C $(LINUX) M=$(shell /bin/pwd)/drivers $@

coccicheck:
	$(MAKE) -C $(LINUX) M=$(shell /bin/pwd)/drivers coccicheck


.PHONY: tools dkms_pkg

tools:
	$(MAKE) -C tools M=$(shell /bin/pwd)

install_dkms_src:
	mkdir -p $(SRC_TARGET)
	cp -a dkms.conf Makefile drivers include scripts $(SRC_TARGET)
# dkms add --sourcetree=`pwd` -m zio/1.2 --dkmstree=`pwd`/test --installtree=`pwd`/test --verbose
# dkms build --sourcetree=`pwd` -m zio/1.2 --dkmstree=`pwd`/test --installtree=`pwd`/test --verbose

# this make clean is ugly, I'm aware...
clean:
	rm -rf `find . -name \*.o -o -name \*.ko -o -name \*~ `
	rm -rf `find . -name Module.\* -o -name \*.mod.c`
	rm -rf `find . -name \*.ko.cmd -o -name \*.o.cmd`
	rm -rf `find . -name modules.order`
	rm -rf `find . -name *.a`
	rm -rf .tmp_versions modules.order
	$(MAKE) -C tools M=$(shell /bin/pwd) clean
