/*
 * Trivial utility that reports data from ZIO input channels
 */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <getopt.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/select.h>

#include <linux/zio-user.h>

static char git_version[] = "version: " GIT_VERSION;

unsigned char buf[1024*1024];
char *prgname;
int opt_print_attr;
int opt_print_memaddr;
int reduce = -1; /**< number of bytes to show at begin/end of the buffer */

void print_attr_set(char *name, int nattr, uint32_t mask, uint32_t *val)
{
	int all = (opt_print_attr == 2);
	int i;

	if (!(all || mask))
		return;

	if (nattr == 16)
		printf("Ctrl: %s-mask: 0x%04x\n", name, mask);
	else
		printf("Ctrl: %s-mask: 0x%04x\n", name, mask);
	for (i = 0; i < nattr; ++i) {
		if (!(all || (mask & (1 << i))))
			continue;
		printf ("Ctrl: %s-%-2i  0x%08x %9i\n",
			name, i, val[i], val[i]);
	}
}

void print_attributes(struct zio_control *ctrl)
{
	print_attr_set("device-std", 16, ctrl->attr_channel.std_mask,
		       ctrl->attr_channel.std_val);
	print_attr_set("device-ext", 32, ctrl->attr_channel.ext_mask,
		       ctrl->attr_channel.ext_val);
	print_attr_set("trigger-std", 16, ctrl->attr_trigger.std_mask,
		       ctrl->attr_trigger.std_val);
	print_attr_set("trigger-ext", 32, ctrl->attr_trigger.ext_mask,
		       ctrl->attr_trigger.ext_val);
}

void print_buffer(int start, int end)
{
	int j;

	for (j = start; j < end; j++) {
		if (!(j & 0xf) || j == start)
			printf("Data:");
		printf(" %02x", buf[j]);
		if ((j & 0xf) == 0xf || j == end - 1)
			putchar('\n');
	}
}

static void ziodump_dataeof(int cfd, int dfd, int expected_size)
{
	struct stat stbuf;

	if (!expected_size) {
		/* We got a zero-size block, so report end-of-data */
		printf("\n");
		return;
	}
	/*
	 * If ctrl == data and this is a regular file, EOF is expected
	 * (it is likely the current-ctrl in sysfs). If not a regular file
	 * is is likely a network strema, so EOF generates a warning.
	 */
	fstat(dfd, &stbuf);
	if (cfd == dfd && S_ISREG(stbuf.st_mode))
		exit(0);
	fprintf(stderr, "%s: data read: unexpected EOF\n", prgname);
}


void read_channel(int cfd, int dfd, FILE *log)
{
	int err = 0;
	struct zio_control ctrl;
	int i;

	i = read(cfd, &ctrl, sizeof(ctrl));
	switch (i) {
	case -1:
		fprintf(stderr, "%s: control read: %s\n",
			prgname, strerror(errno));
		exit(1);
	case 0:
		fprintf(stderr, "%s: control read: unexpected EOF\n",
			prgname);
		exit(1);
	default:
		fprintf(stderr, "%s: ctrl read: %i bytes (expected %zi)\n",
			prgname, i, sizeof(ctrl));
		/* continue anyways */
	case sizeof(ctrl):
		break; /* ok */
	}

	/* Fail badly if the version is not the right one */
	if (ctrl.major_version != __ZIO_MAJOR_VERSION)
		err++;
	if (__ZIO_MAJOR_VERSION == 0 && ctrl.minor_version != __ZIO_MINOR_VERSION)
		err++;
	if (err) {
		fprintf(stderr, "%s: kernel has zio %i.%i, "
			"but I'm compiled for %i.%i\n", prgname,
			ctrl.major_version, ctrl.minor_version,
			__ZIO_MAJOR_VERSION, __ZIO_MINOR_VERSION);
		exit(1);
	}
	if (ctrl.minor_version != __ZIO_MINOR_VERSION) {
		static int warned;

		if (!warned++)
			fprintf(stderr, "%s: warning: minor version mismatch\n",
				prgname);
	}

	printf("Ctrl: version %i.%i, trigger %.16s, dev %.16s-%04x, "
	       "cset %i, chan %i\n",
	       ctrl.major_version, ctrl.minor_version,
	       ctrl.triggername, ctrl.addr.devname, ctrl.addr.dev_id,
	       ctrl.addr.cset, ctrl.addr.chan);
	printf("Ctrl: alarms 0x%02x 0x%02x\n",
	       ctrl.zio_alarms, ctrl.drv_alarms);
	printf("Ctrl: seq %i, n %i, size %i, bits %i, "
	       "flags %08x (%s)\n",
	       ctrl.seq_num,
	       ctrl.nsamples,
	       ctrl.ssize,
	       ctrl.nbits,
	       ctrl.flags,
	       ctrl.flags & ZIO_CONTROL_LITTLE_ENDIAN
	       ? "little-endian" :
	       ctrl.flags & ZIO_CONTROL_BIG_ENDIAN
	       ? "big-endian" : "unknown-endian");
	printf("Ctrl: stamp %lli.%09lli (%lli)\n",
	       (long long)ctrl.tstamp.secs,
	       (long long)ctrl.tstamp.ticks,
	       (long long)ctrl.tstamp.bins);
	if (opt_print_memaddr)
		printf("Ctrl: mem_offset %08x\n", ctrl.mem_offset);
	if (opt_print_attr)
		print_attributes(&ctrl);

	/* FIXME: some control information not being printed yet */
	if (dfd < 0) {
		/* No data (i.e., we are sniffing control-only) */
		return;
	}
	if (cfd != dfd) {
		/* different files, we expect _only_ this data */
		i = read(dfd, buf, sizeof(buf));
	} else {
		/* combined mode or control-only: read the size we expect */
		if (ctrl.nsamples * ctrl.ssize > sizeof(buf)) {
			fprintf(stderr, "%s: buffer too small: "
				"please fix me and recompile\n", prgname);
			exit(1);
		}
		i = read(dfd, buf, ctrl.nsamples * ctrl.ssize);
	}
	if (i < 0) {
		fprintf(stderr, "%s: data read: %s\n",
			prgname, strerror(errno));
		return; /* next ctrl, let's see... */
	}
	if (!i) { /* EOF: handle the various cases */
		ziodump_dataeof(cfd, dfd, ctrl.nsamples * ctrl.ssize);
		return;
	}
	if (i != ctrl.nsamples * ctrl.ssize) {
		if (i == sizeof(buf)) {
			fprintf(stderr, "%s: buffer too small: "
				"please fix me and recompile\n", prgname);
			/* FIXME: empty the data channel */
		} else {
			fprintf(stderr, "%s: ctrl: read %i bytes "
				"(expected %i)\n", prgname, i,
				ctrl.nsamples * ctrl.ssize);
		}
		/* continue anyways */
	}
	fwrite(buf, 1, i, log);

	/* report data to stdout */
	if (reduce < 0) {
		print_buffer(0, i);
	} else {
		print_buffer(0, reduce);
		printf("Data: ...\n");
		print_buffer(i - reduce, i);
	}
	putchar('\n');
}

void help(char *name)
{
	fprintf(stderr, "%s: Wrong number of arguments\n"
		"Use:    \"%s [<opts>] <ctrl-file> <data-file> [...]\"\n"
		"    or  \"%s -c <control-only-or-combined-file>\"\n",
		name, name, name);
	fprintf(stderr,
		"       -a           dump attributes too\n"
		"       -A           dump all attributes\n"
		"       -c           1 control only or combined (ctrl+data)\n"
		"       -s           sniff-device (array of controls)\n"
		"       -m           print memory address (for mmap)\n"
		"       -n <number>  stop after that many blocks\n"
		"       -r <number>  shown bytes at buffer begin/end\n"
		"       -V           print version information \n");
	exit(1);
}

static void print_version(char *pname)
{
	printf("%s %s\n", pname, git_version);
}

int main(int argc, char **argv)
{
	FILE *f;
	char *rest;
	char *outfname;
	int *cfd; /* control file descriptors */
	int *dfd; /* data file descriptors */
	fd_set control_set, ready_set;
	int c, i, j, maxfd, ndev;
	int combined = 0, sniff = 0;
	unsigned long nblocks = -1; /* forever by default */

	prgname = argv[0];

	while ((c = getopt (argc, argv, "aAcsmn:r:V")) != -1) {
		switch(c) {
		case 'a':
			opt_print_attr = 1;
			break;
		case 'A':
			opt_print_attr = 2;
			break;
		case 'c':
			combined = 1;
			break;
		case 's':
			combined = 1; /* sniff is a special combined case */
			sniff = 1;
			break;
		case 'm':
			opt_print_memaddr = 1;
			break;
		case 'n':
			nblocks = strtoul(optarg, &rest, 0);
			if (rest && *rest) {
				fprintf(stderr, "%s: not a number \"%s\"\n",
				       argv[0], optarg);
				help(prgname);
			}
			break;
		case 'r':
			reduce = strtoul(optarg, &rest, 0);
			if (rest && *rest) {
				fprintf(stderr, "%s: not a number \"%s\"\n",
					argv[0], optarg);
				help(prgname);
			}
			break;
		case 'V':
			print_version(argv[0]);
			exit(0);
		default:
			help(prgname);
		}
	}
	argv += optind - 1;
	argc -= optind - 1;

	if (combined && argc != 2)
		help(prgname);
	if (!combined) {
		if (argc < 3 || (argc & 1) != 1)
			help(prgname);
	}

	cfd = malloc(argc / 2 * sizeof(*cfd));
	dfd = malloc(argc / 2 * sizeof(*dfd));
	if (!cfd || !dfd) {
		fprintf(stderr, "%s: malloc: %s\n", prgname, strerror(errno));
		exit(1);
	}

	/* Open all pairs, and build the fd_set for later select() */
	FD_ZERO(&control_set);
	for (i = 1, j = 0; !combined && i < argc; i += 2, j++) {
		cfd[j] = open(argv[i], O_RDONLY);
		dfd[j] = open(argv[i + 1], O_RDONLY);
		if (cfd[j] < 0) {
			fprintf(stderr, "%s: %s: %s\n", prgname, argv[i],
				strerror(errno));
			exit(1);
		}
		if (dfd[j] < 0) {
			fprintf(stderr, "%s: %s: %s\n", prgname, argv[i + 1],
				strerror(errno));
			exit(1);
		}
		/* ctrl file is used in select, data file is non-blocking */
		FD_SET(cfd[j], &control_set);
		fcntl(dfd[j], F_SETFL, fcntl(dfd[j], F_GETFL) | O_NONBLOCK);
		maxfd = dfd[j];
	}
	ndev = j;

	/* always log data we read to some filename */
	outfname = getenv("ZIO_DUMP_TO");
	if (!outfname)
		outfname = "/dev/null";
	f = fopen(outfname, "w");
	if (!f) {
		fprintf(stderr, "%s: %s: %s\n", prgname, outfname,
			strerror(errno));
		exit(1);
	}
	/* ensure proper strace information and proper output to pipes */
	setlinebuf(stdout);
	setbuf(f, NULL);

	if (!combined) {
		/* Read control and then data. Forever or nblocks if > 0 */
		while (nblocks) {
			if (nblocks > 0)
				nblocks--;
			ready_set = control_set;
			i = select(maxfd + 1, &ready_set, NULL, NULL, NULL);
			if (i < 0 && errno == EINTR)
				continue;
			if (i < 0) {
				fprintf(stderr, "%s: select(): %s\n", prgname,
					strerror(errno));
				exit(1);
			}
			for (j = 0; j < ndev; j++)
				if (FD_ISSET(cfd[j], &ready_set))
					read_channel(cfd[j], dfd[j], f);
		}
		exit(0);
	}
	/*
	 * So, we are reading one combined file. Just open it and
	 * read it forever or nblocks if > 0; read_channel() is blocking.
	 */
	cfd[0] = open(argv[1], O_RDONLY);
	if (cfd[0] < 0) {
		fprintf(stderr, "%s: %s: %s\n", prgname, argv[1],
			strerror(errno));
		exit(1);
	}
	if (sniff)
		dfd[0] = -1;
	else
		dfd[0] = cfd[0];
	while (nblocks) {
		read_channel(cfd[0], dfd[0], f);
		if (nblocks > 0)
			nblocks--;
	}
	return 0;
}
