/*
 * Simple program that cats one zio device to stdout
 */
#define _GNU_SOURCE /* for mremap */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/select.h>
#include <sys/time.h>
#include <sys/mman.h>

#include <linux/zio-user.h>

static char git_version[] = "version: " GIT_VERSION;

#define VERBOSE 0

static void print_version(char *pname)
{
	printf("%s %s\n", pname, git_version);
}

int main(int argc, char **argv)
{
	int cfd; /* control file descriptor */
	int dfd; /* data file descriptor */
	int i, j, size, off;
	char *s, *dataname, *ctrlname;
	int pagesize = getpagesize();
	unsigned long nblocks, datadone = 0;
	struct zio_control ctrl;
	void *buffer;
	int buffersize = 0;
	void *map,*ptr;
	struct timeval tv1, tv2;

	if ((argc == 2) && (!strcmp(argv[1], "-V"))) {
		print_version(argv[0]);
		exit(0);
	}
	if (argc != 3) {
		fprintf(stderr, "%s: Wrong number of arguments\n"
			"Use: \"%s <data-file> <nblocks>\"\n",
			argv[0], argv[0]);
		fprintf(stderr, "Or -V for version details\n");
		exit(1);
	}
	nblocks = atoi(argv[2]);

	/* receive data name, build ctrl name */
	dataname = argv[1];
	ctrlname = strdup(dataname);
	s = strstr(ctrlname, "data");
	if (!s || strlen(s) != 4) {
		fprintf(stderr, "%s: \"%s\" doesn't look like "
			"a ZIO data device\n", argv[0], argv[1]);
		exit(1);
	}
	strcpy(s, "ctrl");

	/* open both descriptors */
	dfd = open(dataname, O_RDONLY);
	if (dfd < 0) {
		fprintf(stderr, "%s: %s: %s\n", argv[0], dataname,
			strerror(errno));
		exit(1);
	}
	cfd = open(ctrlname, O_RDONLY);
	if (cfd < 0) {
		fprintf(stderr, "%s: %s: %s\n", argv[0], dataname,
			strerror(errno));
		exit(1);
	}

	/* Try to mmap the data file */
	buffersize = pagesize;
	map = mmap(0, buffersize, PROT_READ, MAP_PRIVATE, dfd, 0);
	if (map != MAP_FAILED) {
		buffer = map;
	} else {
		fprintf(stderr, "%s: %s: no mmap available\n", argv[0],
			dataname);
		map = NULL;
		buffer = malloc(buffersize);
		if (!buffer) {
			fprintf(stderr, "%s: %s\n", argv[0], strerror(errno));
			exit(1);
		}
	}

	/* Ok, setup is done, now loop to read data */
	gettimeofday(&tv1, NULL);
	for (j = 0; j < nblocks; j++) {

		i = read(cfd, &ctrl, sizeof(ctrl));
		if (i != sizeof(ctrl))
			goto ctrl_read_error;
		if (!j) {
			if (ctrl.major_version != __ZIO_MAJOR_VERSION
			    || ctrl.minor_version != __ZIO_MINOR_VERSION) {
				fprintf(stderr, "%s: unexpected ZIO version\n",
					argv[0]);
				exit(1);
			}
		}
		size = ctrl.ssize * ctrl.nsamples;
		off = ctrl.mem_offset;
		if (VERBOSE)
			fprintf(stderr, "block %i: offset %i size %i\n",
				j, off, size);

		if (map) {
			/* increase map size if needed */
			while (off + size > buffersize) {
				if (VERBOSE)
					fprintf(stderr, "doubling map size "
						"from %i to %i\n", buffersize,
						buffersize * 2);
				map = mremap(map, buffersize, buffersize * 2,
					     MREMAP_MAYMOVE);
				buffersize *= 2;
				buffer = map;
				if (map == MAP_FAILED) {
					fprintf(stderr, "mremap failed\n");
					exit(1);
				}
			}
			ptr = map + off;
		} else {
			/* increase malloc size if needed */
			while (size > buffersize) {
				if (VERBOSE)
					fprintf(stderr, "doubling alloc size "
						"from %i to %i\n", buffersize,
						buffersize * 2);
				buffersize *= 2;
				buffer = realloc(buffer, buffersize);
				if (!buffer) {
					fprintf(stderr, "realloc failed\n");
					exit(1);
				}
			}
			i = read(dfd, buffer, size);
			if (i != size)
				goto data_read_error;
			ptr = buffer;
		}

		/* Ok, we read. Now write to stdout */
		i = write(STDOUT_FILENO, ptr, size);
		if (i != size)
			goto write_error;
		datadone += size;
	}
	gettimeofday(&tv2, NULL);
	i = (tv2.tv_sec - tv1.tv_sec) * 1000 * 1000
		+ tv2.tv_usec - tv1.tv_usec;
	fprintf(stderr, "%s: trasferred %li blocks, %li bytes, %i.%06i secs\n",
		argv[0], nblocks, datadone, i/1000/1000, i % (1000 * 1000));
	exit(0);

ctrl_read_error:
	switch(i) {
	case -1:
		fprintf(stderr, "%s: control read: %s\n",
                        argv[0], strerror(errno));
		break;
	case 0:
		fprintf(stderr, "%s: control read: unexpected EOF\n",
                        argv[0]);
		break;
	default:
		fprintf(stderr, "%s: control read: expected %zi, got %i\n",
			argv[0], sizeof(ctrl), i);
		break;
	}
	exit(1);

data_read_error:
	switch(i) {
	case -1:
		fprintf(stderr, "%s: data read: %s\n",
                        argv[0], strerror(errno));
		break;
	case 0:
		fprintf(stderr, "%s: data read: unexpected EOF\n",
                        argv[0]);
		break;
	default:
		fprintf(stderr, "%s: data read: expected %i, got %i\n",
			argv[0], size, i);
		break;
	}
	exit(1);

write_error:
	switch(i) {
	case -1:
		fprintf(stderr, "%s: write: %s\n",
                        argv[0], strerror(errno));
		break;
	default:
		fprintf(stderr, "%s: write: pushed %i, wrote only %i\n",
			argv[0], size, i);
		break;
	}
	exit(1);
}
