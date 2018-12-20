M ?= $(shell /bin/pwd)/../../

include $(M)/scripts/Makefile.include

# build user-space tools for zio

CFLAGS = -I$(M)/include/ -Wall $(ZIO_VERSION) $(EXTRACFLAGS)
CFLAGS += -DGIT_VERSION=\"$(GIT_VERSION)\"

CC ?= $(CROSS_COMPILE)gcc

progs := zio-dump
progs += zio-cat-file
progs += test-dtc

# The following is ugly, please forgive me by now
all: $(progs)

clean:
	rm -f $(progs) *~ *.o

%: %.c
	$(CC) $(CFLAGS) $^ -o $@
