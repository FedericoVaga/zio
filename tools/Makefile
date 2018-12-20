# SPDX-License-Identifier: GPL-2.0

M ?= $(shell /bin/pwd)/../

all: zio_all

clean: zio_clean

zio_all:
	$(MAKE) -C zio M=$(M) all

zio_clean:
	$(MAKE) -C zio M=$(M) clean
