/*
 * Copyright (c) 1988, 1990, 1993
 *	The Regents of the University of California.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 * Modified Sun Mar 12 10:34:34 1995, faith@cs.unc.edu, for Linux
 */

/*
 * This program is not related to David Wall, whose Stanford Ph.D. thesis
 * is entitled "Mechanisms for Broadcast and Selective Broadcast".
 *
 * 1999-02-22 Arkadiusz Miśkiewicz <misiek@pld.ORG.PL>
 * - added Native Language Support
 *
 */

#include <sys/param.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/uio.h>

#include <ctype.h>
#include <err.h>
#include <errno.h>
#include <paths.h>
#include <ctype.h>
#include <pwd.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <utmpx.h>
#include <getopt.h>
#include <sys/types.h>
#include <grp.h>

#include "strutils.h"
#include "ttymsg.h"

#define	TERM_WIDTH	79
#define	WRITE_TIME_OUT	300		/* in seconds */

/* Function prototypes */
static char *makemsg(char *fname, char **mvec, int mvecsz,
		    size_t *mbufsize, int print_banner);

static void usage(void)
{
    fprintf (stderr, "wall: usage: wall [-g group] [-n] [-t timeout] [file | message]\n");

	exit(EXIT_FAILURE);
}

struct group_workspace {
	gid_t	requested_group;
	int	ngroups;

/* getgrouplist() on OSX takes int* not gid_t* */
#ifdef __APPLE__
	int	*groups;
#else
	gid_t	*groups;
#endif
};

static gid_t get_group_gid(const char *group)
{
	struct group *gr;
	gid_t gid;

	if ((gr = getgrnam(group)))
		return gr->gr_gid;

	gid = strtou32_or_err(group, "invalid group argument");
	if (!getgrgid(gid))
		errx(EXIT_FAILURE, "%s: unknown gid", group);

	return gid;
}

static struct group_workspace *init_group_workspace(const char *group)
{
	struct group_workspace *buf = malloc(sizeof(struct group_workspace));

	buf->requested_group = get_group_gid(group);
	buf->ngroups = sysconf(_SC_NGROUPS_MAX) + 1;  /* room for the primary gid */
	buf->groups = calloc(sizeof(*buf->groups), buf->ngroups);

	return buf;
}

static void free_group_workspace(struct group_workspace *buf)
{
	if (!buf)
		return;

	free(buf->groups);
	free(buf);
}

static int is_gr_member(const char *login, const struct group_workspace *buf)
{
	struct passwd *pw;
	int ngroups = buf->ngroups;
	int rc;

	pw = getpwnam(login);
	if (!pw)
		return 0;

	if (buf->requested_group == pw->pw_gid)
		return 1;

	rc = getgrouplist(login, pw->pw_gid, buf->groups, &ngroups);
	if (rc < 0) {
		/* buffer too small, not sure how this can happen, since
		   we used sysconf to get the size... */
		errx(EXIT_FAILURE,
			"getgrouplist found more groups than sysconf allows");
	}

	for (; ngroups >= 0; --ngroups) {
		if (buf->requested_group == (gid_t) buf->groups[ngroups])
			return 1;
	}

	return 0;
}

int main(int argc, char **argv)
{
	int ch;
	struct iovec iov;
	struct utmpx *utmpptr;
	char *p;
	char line[sizeof(utmpptr->ut_line) + 1];
	int print_banner = 1;
	struct group_workspace *group_buf = NULL;
	char *mbuf, *fname = NULL;
	size_t mbufsize;
	unsigned timeout = WRITE_TIME_OUT;
	char **mvec = NULL;
	int mvecsz = 0;

	while ((ch = getopt(argc, argv, "nt:g:")) != -1) {
		switch (ch) {
		case 'n':
			if (geteuid() == 0)
				print_banner = 0;
			else
				warnx("-n is available only for root");
			break;
		case 't':
			timeout = strtou32_or_err(optarg, "invalid timeout argument");
			if (timeout < 1)
				errx(EXIT_FAILURE, "invalid timeout argument: %s", optarg);
			break;
		case 'g':
			group_buf = init_group_workspace(optarg);
			break;

		default:
			usage();
		}
	}
	argc -= optind;
	argv += optind;

	if (argc == 1 && access(argv[0], F_OK) == 0)
		fname = argv[0];
	else if (argc >= 1) {
		mvec = argv;
		mvecsz = argc;
	}

	mbuf = makemsg(fname, mvec, mvecsz, &mbufsize, print_banner);

	iov.iov_base = mbuf;
	iov.iov_len = mbufsize;
	while((utmpptr = getutxent())) {
		if (!utmpptr->ut_user[0])
			continue;
#ifdef USER_PROCESS
		if (utmpptr->ut_type != USER_PROCESS)
			continue;
#endif
		/* Joey Hess reports that use-sessreg in /etc/X11/wdm/ produces
		 * ut_line entries like :0, and a write to /dev/:0 fails.
		 *
		 * It also seems that some login manager may produce empty ut_line.
		 */
		if (!*utmpptr->ut_line || *utmpptr->ut_line == ':')
			continue;

		if (group_buf && !is_gr_member(utmpptr->ut_user, group_buf))
			continue;

		mem2strcpy(line, utmpptr->ut_line, sizeof(utmpptr->ut_line), sizeof(line));
		if ((p = ttymsg(&iov, 1, line, timeout)) != NULL)
			warnx("%s", p);
	}
	endutxent();
	free(mbuf);
	free_group_workspace(group_buf);
	exit(EXIT_SUCCESS);
}

struct buffer {
	size_t	sz;
	size_t	used;
	char	*data;
};

static void buf_enlarge(struct buffer *bs, size_t len)
{
	if (bs->sz == 0 || len > bs->sz - bs->used) {
		bs->sz += len < 128 ? 128 : len;
		bs->data = realloc(bs->data, bs->sz);
	}
}

static void buf_puts(struct buffer *bs, const char *s)
{
	size_t len = strlen(s);

	buf_enlarge(bs, len + 1);
	memcpy(bs->data + bs->used, s, len + 1);
	bs->used += len;
}

static void buf_printf(struct buffer *bs, const char *fmt, ...)
{
	int rc;
	va_list ap;
	size_t limit;

	buf_enlarge(bs, 0);	/* default size */
	limit = bs->sz - bs->used;

	va_start(ap, fmt);
	rc = vsnprintf(bs->data + bs->used, limit, fmt, ap);
	va_end(ap);

	if (rc >= 0 && (size_t) rc >= limit) {	/* not enough, enlarge */
		buf_enlarge(bs, (size_t)rc + 1);
		limit = bs->sz - bs->used;
		va_start(ap, fmt);
		rc = vsnprintf(bs->data  + bs->used, limit, fmt, ap);
		va_end(ap);
	}

	if (rc > 0)
		bs->used += rc;
}

static void buf_putc_careful(struct buffer *bs, int c)
{
	if (isprint(c) || c == '\a' || c == '\t' || c == '\r' || c == '\n') {
		buf_enlarge(bs, 1);
		bs->data[bs->used++] = c;
	} else if (!isascii(c))
		buf_printf(bs, "\\%3o", (unsigned char)c);
	else {
		char tmp[] = { '^', c ^ 0x40, '\0' };
		buf_puts(bs, tmp);
	}
}

static char *makemsg(char *fname, char **mvec, int mvecsz,
		     size_t *mbufsize, int print_banner)
{
	struct buffer _bs = {.used = 0}, *bs = &_bs;
	register int ch, cnt;
	char *p, *lbuf;
	long line_max;

	line_max = sysconf(_SC_LINE_MAX);
	if (line_max <= 0)
		line_max = 512;

	lbuf = malloc(line_max);

	if (print_banner) {
		char hostname[HOST_NAME_MAX+1];
		char *whom, *where, *date;
		struct passwd *pw;
		time_t now;
        
        if (gethostname(hostname, sizeof(hostname))) err(1, "gethostname");

		if (!(whom = getlogin()) || !*whom)
			whom = (pw = getpwuid(getuid())) ? pw->pw_name : "???";
		if (!whom) {
			whom = "someone";
			warn("cannot get passwd uid");
		}
		where = ttyname(STDOUT_FILENO);
		if (!where) {
			where = "somewhere";
		} else if (strncmp(where, "/dev/", 5) == 0)
			where += 5;

		time(&now);
		date = strdup(ctime(&now));
		date[strlen(date) - 1] = '\0';

		/*
		 * all this stuff is to blank out a square for the message;
		 * we wrap message lines at column 79, not 80, because some
		 * terminals wrap after 79, some do not, and we can't tell.
		 * Which means that we may leave a non-blank character
		 * in column 80, but that can't be helped.
		 */
		/* snprintf is not always available, but the sprintf's here
		   will not overflow as long as %d takes at most 100 chars */
		buf_printf(bs, "\r%*s\r\n", TERM_WIDTH, " ");

		snprintf(lbuf, line_max,
				"Broadcast message from %s@%s (%s (%s):",
				whom, hostname, where, date);
		buf_printf(bs, "%-*.*s\007\007\r\n", TERM_WIDTH, TERM_WIDTH, lbuf);
		free(date);
	}
	buf_printf(bs, "%*s\r\n", TERM_WIDTH, " ");

	 if (mvec) {
		/*
		 * Read message from argv[]
		 */
		int i;

		for (i = 0; i < mvecsz; i++) {
			buf_puts(bs, mvec[i]);
			if (i < mvecsz - 1)
				buf_puts(bs, " ");
		}
		buf_puts(bs, "\r\n");
	} else {
		/*
		 * read message from <file>
		 */
		if (fname) {
			/*
			 * When we are not root, but suid or sgid, refuse to read files
			 * (e.g. device files) that the user may not have access to.
			 * After all, our invoker can easily do "wall < file"
			 * instead of "wall file".
			 */
			uid_t uid = getuid();
			if (uid && (uid != geteuid() || getgid() != getegid()))
				errx(EXIT_FAILURE, "will not read %s - use stdin.",
				     fname);

			if (!freopen(fname, "r", stdin))
				err(EXIT_FAILURE, "cannot open %s", fname);

		}

		/*
		 * Read message from stdin.
		 */
		while (fgets(lbuf, line_max, stdin)) {
			for (cnt = 0, p = lbuf; (ch = *p) != '\0'; ++p, ++cnt) {
				if (cnt == TERM_WIDTH || ch == '\n') {
					for (; cnt < TERM_WIDTH; ++cnt)
						buf_puts(bs, " ");
					buf_puts(bs, "\r\n");
					cnt = 0;
				}
				if (ch == '\t')
					cnt += (7 - (cnt % 8));
				if (ch != '\n')
					buf_putc_careful(bs, ch);
			}
		}
	}
	buf_printf(bs, "%*s\r\n", TERM_WIDTH, " ");

	free(lbuf);

	bs->data[bs->used] = '\0';	/* be paranoid */
	*mbufsize = bs->used;
	return bs->data;
}
