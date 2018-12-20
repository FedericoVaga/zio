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
#include <time.h>
#include <sys/types.h>
#include <sys/stat.h>

#include <linux/zio-user.h>

static char git_version[] = "version: " GIT_VERSION;

#define FNAME "/dev/zdtc-0000-0-0-ctrl"

void help(char *name)
{
	fprintf(stderr, "%s: Wrong number of arguments\n"
		"Use:    \"%s [<opts>]\n", name, name);
	fprintf(stderr,
		"       -f <file>    default: %s\n"
		"       -t <time>    default: \"1.5\" (see code for details)\n"
		"       -p <period>  default: \"0\"\n"
		"       -n <number>  default: infinite\n"
		"       -v           increase verbosity\n"
		"       -V           print version information \n", FNAME);
	exit(1);
}

static void print_version(char *pname)
{
	printf("%s %s\n", pname, git_version);
}

/* Boring parsing separated to a separate function (same code as elsewhere) */
static int parse_ts(char *s, struct timespec *ts)
{
	int i, n;
	unsigned long nano;
	char c;

	/*
 	 * Hairy: if we scan "%ld%lf", the 0.009999 will become 9998 micro.
	 * Thus, scan as integer and string, so we can count leading zeros
	 */

	nano = 0;
	ts->tv_sec = 0;
	ts->tv_nsec = 0;

	if ( (i = sscanf(s, "%ld.%ld%c", &ts->tv_sec, &nano, &c)) == 1)
		return 0; /* seconds only */
	if (i == 3)
		return -1; /* trailing crap */
	if (i == 0)
		if (sscanf(s, ".%ld%c", &nano, &c) != 1)
			return -1; /* leading or trailing crap */

	s = strchr(s, '.') + 1;
	n = strlen(s);
	if (n > 9)
		return -1; /* too many decimals */
	while (n < 9) {
		nano *= 10;
		n++;
	}
	ts->tv_nsec = nano;
	return 0;
}


int main(int argc, char **argv)
{
	struct zio_control ctrl = {0,};
	char *fname = FNAME;
	int verbose = 0;
	char *t = NULL, *p = NULL;
	int i, fd, n = -1;
	struct timespec ts = {1, 5};
	struct timespec period = {0, 0};

	/* -f <filename> -t <[+][secs].frac> -p <.frac> -v */
	while ((i = getopt (argc, argv, "f:t:p:n:vV")) != -1) {
		switch(i) {
		case 'f':
			fname = optarg;
			break;
		case 't':
			t = optarg;
			break;
		case 'p':
			p = optarg;
			break;
		case 'n':
			n = atoi(optarg);
			break;
		case 'v':
			verbose++;
			break;
		case 'V':
			print_version(argv[0]);
			exit(0);
		default:
			help(argv[0]);
		}
	}
	if (!n)
		help(argv[0]);

	if (t) {
		char *t2 = t;

		if (t[0] == '+')
			t2++;
		if (parse_ts(t2, &ts) < 0) {
			fprintf(stderr, "%s: can't parse time \"%s\"\n",
				argv[0], t);
			exit(1);
		}
		if (t[0] == '+')
			ts.tv_sec += time(NULL);
	}
	if (p) {
		if (parse_ts(p, &period) < 0) {
			fprintf(stderr, "%s: can't parse period \"%s\"\n",
				argv[0], p);
			exit(1);
		}
	}

	fd = open(fname, O_WRONLY);
	if (fd < 0) {
		fprintf(stderr, "%s: %s: %s\n", argv[0], fname,
			strerror(errno));
		exit(1);
	}

	ctrl.major_version = __ZIO_MAJOR_VERSION;
	ctrl.minor_version = __ZIO_MINOR_VERSION;
	if (verbose) {
		printf("  time: %9li.%09li\n", ts.tv_sec, ts.tv_nsec);
		printf("period: %9li.%09li\n", period.tv_sec, period.tv_nsec);
	}
	while (n != 0) {
		ctrl.tstamp.secs = ts.tv_sec;
		ctrl.tstamp.ticks = ts.tv_nsec;
		if (verbose)
			printf("%9li.%09li", ts.tv_sec, ts.tv_nsec);
		i = write(fd, &ctrl, sizeof(ctrl));
		if (i != sizeof(ctrl)) {
			fprintf(stderr, "%s: %s: write error (%i bytes, %s)\n",
				argv[0], fname, i, strerror(errno));
			exit(1);
		}
		if (verbose)
			printf("\n");
		ts.tv_nsec += period.tv_nsec;
		if (ts.tv_nsec >= 1000 * 1000 * 1000) {
			ts.tv_nsec -= 1000 * 1000 * 1000;
			ts.tv_sec++;
		}
		ts.tv_sec += period.tv_sec;
		if (n > 0)
			n--;
	}
	exit(0);
}
