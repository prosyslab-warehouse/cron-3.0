/* Copyright 1988,1990,1993,1994 by Paul Vixie
 * All rights reserved
 *
 * Distribute freely, except: don't remove my name from the source or
 * documentation (don't take credit for my work), mark your changes (don't
 * get me blamed for your possible bugs), don't alter or remove this
 * notice.  May be sold if buildable source is provided to buyer.  No
 * warrantee of any kind, express or implied, is included with this
 * software; use at your own risk, responsibility for damages (if any) to
 * anyone resulting from the use of this software rests entirely with the
 * user.
 *
 * Send bug reports, bug fixes, enhancements, requests, flames, etc., and
 * I'll try to keep a version up to date.  I can be reached as follows:
 * Paul Vixie          <paul@vix.com>          uunet!decwrl!vixie!paul
 */

#if !defined(lint) && !defined(LINT)
static char rcsid[] = "$Id: entry.c,v 2.12 1994/01/17 03:20:37 vixie Exp $";
#endif

/* vix 26jan87 [RCS'd; rest of log is in RCS file]
 * vix 01jan87 [added line-level error recovery]
 * vix 31dec86 [added /step to the from-to range, per bob@acornrc]
 * vix 30dec86 [written]
 */


#include "cron.h"

extern int change_child_path;

typedef	enum ecode {
	e_none, e_minute, e_hour, e_dom, e_month, e_dow,
	e_cmd, e_timespec, e_username, e_cmd_len
} ecode_e;

static char	get_list __P((bitstr_t *, int, int, char *[], int, FILE *)),
		get_range __P((bitstr_t *, int, int, char *[], int, FILE *)),
		get_number __P((int *, int, char *[], int, FILE *));
static int	set_element __P((bitstr_t *, int, int, int));

static char *ecodes[] =
	{
		"no error",
		"bad minute",
		"bad hour",
		"bad day-of-month",
		"bad month",
		"bad day-of-week",
		"bad command",
		"bad time specifier",
		"bad username",
		"command too long",
	};


void
free_entry(e)
	entry	*e;
{
	free(e->cmd);
	env_free(e->envp);
	free(e);
}


/* return NULL if eof or syntax error occurs;
 * otherwise return a pointer to a new entry.
 */
entry *
load_entry(file, error_func, pw, envp)
	FILE		*file;
	void		(*error_func)();
	struct passwd	*pw;
	char		**envp;
{
	/* this function reads one crontab entry -- the next -- from a file.
	 * it skips any leading blank lines, ignores comments, and returns
	 * EOF if for any reason the entry can't be read and parsed.
	 *
	 * the entry is also parsed here.
	 *
	 * syntax:
	 *   user crontab:
	 *	minutes hours doms months dows cmd\n
	 *   system crontab (/etc/crontab):
	 *	minutes hours doms months dows USERNAME cmd\n
	 */

	ecode_e	ecode = e_none;
	entry	*e;
	int	ch;
	char	cmd[MAX_COMMAND];
	char	envstr[MAX_ENVSTR];
	char	**tenvp;

	Debug(DPARS, ("load_entry()...about to eat comments\n"))

	skip_comments(file);

	ch = get_char(file);
	if (ch == EOF)
		return NULL;

	/* ch is now the first useful character of a useful line.
	 * it may be an @special or it may be the first character
	 * of a list of minutes.
	 */

	e = (entry *) calloc(sizeof(entry), sizeof(char));
	if (e == NULL) {
		log_it("CRON", getpid(), "OOM", "Out of memory parsing crontab");
		return NULL;
	}

	if (ch == '@') {
		/* all of these should be flagged and load-limited; i.e.,
		 * instead of @hourly meaning "0 * * * *" it should mean
		 * "close to the front of every hour but not 'til the
		 * system load is low".  Problems are: how do you know
		 * what "low" means? (save me from /etc/cron.conf!) and:
		 * how to guarantee low variance (how low is low?), which
		 * means how to we run roughly every hour -- seems like
		 * we need to keep a history or let the first hour set
		 * the schedule, which means we aren't load-limited
		 * anymore.  too much for my overloaded brain. (vix, jan90)
		 * HINT
		 */
		ch = get_string(cmd, MAX_COMMAND, file, " \t\n");
		if (!strcmp("reboot", cmd)) {
			e->flags |= WHEN_REBOOT;
		} else if (!strcmp("yearly", cmd) || !strcmp("annually", cmd)){
			bit_set(e->minute, 0);
			bit_set(e->hour, 0);
			bit_set(e->dom, 0);
			bit_set(e->month, 0);
			bit_nset(e->dow, 0, (LAST_DOW-FIRST_DOW+1));
			e->flags |= DOW_STAR;
		} else if (!strcmp("monthly", cmd)) {
			bit_set(e->minute, 0);
			bit_set(e->hour, 0);
			bit_set(e->dom, 0);
			bit_nset(e->month, 0, (LAST_MONTH-FIRST_MONTH+1));
			bit_nset(e->dow, 0, (LAST_DOW-FIRST_DOW+1));
			e->flags |= DOW_STAR;
		} else if (!strcmp("weekly", cmd)) {
			bit_set(e->minute, 0);
			bit_set(e->hour, 0);
			bit_nset(e->dom, 0, (LAST_DOM-FIRST_DOM+1));
			e->flags |= DOM_STAR;
			bit_nset(e->month, 0, (LAST_MONTH-FIRST_MONTH+1));
			bit_nset(e->dow, 0,0);
		} else if (!strcmp("daily", cmd) || !strcmp("midnight", cmd)) {
			bit_set(e->minute, 0);
			bit_set(e->hour, 0);
			bit_nset(e->dom, 0, (LAST_DOM-FIRST_DOM+1));
			bit_nset(e->month, 0, (LAST_MONTH-FIRST_MONTH+1));
			bit_nset(e->dow, 0, (LAST_DOW-FIRST_DOW+1));
		} else if (!strcmp("hourly", cmd)) {
			bit_set(e->minute, 0);
			bit_nset(e->hour, 0, (LAST_HOUR-FIRST_HOUR+1));
			bit_nset(e->dom, 0, (LAST_DOM-FIRST_DOM+1));
			bit_nset(e->month, 0, (LAST_MONTH-FIRST_MONTH+1));
			bit_nset(e->dow, 0, (LAST_DOW-FIRST_DOW+1));
			e->flags |= HR_STAR;
		} else {
			ecode = e_timespec;
			goto eof;
		}
	} else {
		Debug(DPARS, ("load_entry()...about to parse numerics\n"))

		if (ch == '*')
			e->flags |= MIN_STAR;
		ch = get_list(e->minute, FIRST_MINUTE, LAST_MINUTE,
			      PPC_NULL, ch, file);
		if (ch == EOF) {
			ecode = e_minute;
			goto eof;
		}

		/* hours
		 */

		if (ch == '*')
			e->flags |= HR_STAR;
		ch = get_list(e->hour, FIRST_HOUR, LAST_HOUR,
			      PPC_NULL, ch, file);
		if (ch == EOF) {
			ecode = e_hour;
			goto eof;
		}

		/* DOM (days of month)
		 */

		if (ch == '*')
			e->flags |= DOM_STAR;
		ch = get_list(e->dom, FIRST_DOM, LAST_DOM,
			      PPC_NULL, ch, file);
		if (ch == EOF) {
			ecode = e_dom;
			goto eof;
		}

		/* month
		 */

		ch = get_list(e->month, FIRST_MONTH, LAST_MONTH,
			      MonthNames, ch, file);
		if (ch == EOF) {
			ecode = e_month;
			goto eof;
		}

		/* DOW (days of week)
		 */

		if (ch == '*')
			e->flags |= DOW_STAR;
		ch = get_list(e->dow, FIRST_DOW, LAST_DOW,
			      DowNames, ch, file);
		if (ch == EOF) {
			ecode = e_dow;
			goto eof;
		}
	}

	/* make sundays equivilent */
	if (bit_test(e->dow, 0) || bit_test(e->dow, 7)) {
		bit_set(e->dow, 0);
		bit_set(e->dow, 7);
	}

	/* If we used one of the @commands, we may be pointing at
	   blanks, and if we don't skip over them, we'll miss the user/command */
	Skip_Blanks(ch, file);
	/* ch is the first character of a command, or a username */
	unget_char(ch, file);

	if (!pw) {
		char		*username = cmd;	/* temp buffer */

		Debug(DPARS, ("load_entry()...about to parse username\n"))
		ch = get_string(username, MAX_COMMAND, file, " \t");

		Debug(DPARS, ("load_entry()...got %s\n",username))
		if (ch == EOF) {
			ecode = e_cmd;
			goto eof;
		}

		pw = getpwnam(username);
		if (pw == NULL) {
			ecode = e_username;
			goto eof;
		}
		Debug(DPARS, ("load_entry()...uid %d, gid %d\n",e->uid,e->gid))
	} else if (ch == '*') {
		ecode = e_cmd;
		goto eof;
	}

	e->uid = pw->pw_uid;
	e->gid = pw->pw_gid;

	/* copy and fix up environment.  some variables are just defaults and
	 * others are overrides.
	 */
	if ((e->envp = env_copy(envp)) == NULL) {
		ecode = e_none;
		goto eof;
	}
	if (!env_get("SHELL", e->envp)) {
		snprintf(envstr, MAX_ENVSTR, "SHELL=%s", _PATH_BSHELL);
		if ((tenvp = env_set(e->envp, envstr))) {
			e->envp = tenvp;
		} else {
			ecode = e_none;
			goto eof;
		}
	}
	if (!env_get("HOME", e->envp)) {
		snprintf(envstr, MAX_ENVSTR, "HOME=%s", pw->pw_dir);
		if ((tenvp = env_set(e->envp, envstr))) {
			e->envp = tenvp;
		} else {
			ecode = e_none;
			goto eof;
		}
	}
	if (!env_get("PATH", e->envp) && change_child_path) {
		snprintf(envstr, MAX_ENVSTR, "PATH=%s", _PATH_DEFPATH);
		if ((tenvp = env_set(e->envp, envstr))) {
			e->envp = tenvp;
		} else {
			ecode = e_none;
			goto eof;
		}
	}
	snprintf(envstr, MAX_ENVSTR, "%s=%s", "LOGNAME", pw->pw_name);
	if ((tenvp = env_set(e->envp, envstr))) {
		e->envp = tenvp;
	} else {
		ecode = e_none;
		goto eof;
	}
#if defined(BSD)
	snprintf(envstr, MAX_ENVSTR, "%s=%s", "USER", pw->pw_name);
	if ((tenvp = env_set(e->envp, envstr))) {
		e->envp = tenvp;
	} else {
		ecode = e_none;
		goto eof;
	}
#endif

	Debug(DPARS, ("load_entry()...about to parse command\n"))

	/* Everything up to the next \n or EOF is part of the command...
	 * too bad we don't know in advance how long it will be, since we
	 * need to malloc a string for it... so, we limit it to MAX_COMMAND.
	 *
	 * To err on the side of caution, if the command string length is
	 * equal to MAX_COMMAND, we will assume that the command has been
	 * truncated and generate an error.
	 *
	 * XXX - should use realloc().
	 */
	ch = get_string(cmd, MAX_COMMAND, file, "\n");
	if (strnlen(cmd, MAX_COMMAND) == MAX_COMMAND - 1) {
		ecode = e_cmd_len;
		goto eof;
	}

	/* a file without a \n before the EOF is rude, so we'll complain...

	   CK 2010-04-14: this code will never be reached. All calls to
	   load_entry are proceeded by calls to load_env, which aborts on EOF, and
	   where load_env fails, the code bails out.
	 */
	if (ch == EOF) {
		ecode = e_cmd;
		goto eof;
	}

	/* got the command in the 'cmd' string; save it in *e.
	 */
	if ((e->cmd = strdup(cmd)) == NULL) {
		ecode = e_none;
		goto eof;
	}

	Debug(DPARS, ("load_entry()...returning successfully\n"))

	/* success, fini, return pointer to the entry we just created...
	 */
	return e;

 eof:
	if (e->envp)
		env_free(e->envp);
	if (e->cmd)
		free(e->cmd);
	free(e);
	if (ecode != e_none && error_func)
		(*error_func)(ecodes[(int)ecode]);
	while (ch != EOF && ch != '\n')
		ch = get_char(file);
	return NULL;
}


static char
get_list(bits, low, high, names, ch, file)
	bitstr_t	*bits;		/* one bit per flag, default=FALSE */
	int		low, high;	/* bounds, impl. offset for bitstr */
	char		*names[];	/* NULL or *[] of names for these elements */
	int		ch;		/* current character being processed */
	FILE		*file;		/* file being read */
{
	register int	done;

	/* we know that we point to a non-blank character here;
	 * must do a Skip_Blanks before we exit, so that the
	 * next call (or the code that picks up the cmd) can
	 * assume the same thing.
	 */

	Debug(DPARS|DEXT, ("get_list()...entered\n"))

	/* list = range {"," range}
	 */
	
	/* clear the bit string, since the default is 'off'.
	 */
	bit_nclear(bits, 0, (high-low+1));

	/* process all ranges
	 */
	done = FALSE;
	while (!done) {
		ch = get_range(bits, low, high, names, ch, file);
		if (ch == ',')
			ch = get_char(file);
		else
			done = TRUE;
	}

	/* exiting.  skip to some blanks, then skip over the blanks.
	 */
	Skip_Nonblanks(ch, file)
	Skip_Blanks(ch, file)

	Debug(DPARS|DEXT, ("get_list()...exiting w/ %02x\n", ch))

	return ch;
}


static char
get_range(bits, low, high, names, ch, file)
	bitstr_t	*bits;		/* one bit per flag, default=FALSE */
	int		low, high;	/* bounds, impl. offset for bitstr */
	char		*names[];	/* NULL or names of elements */
	int		ch;		/* current character being processed */
	FILE		*file;		/* file being read */
{
	/* range = number | number "-" number [ "/" number ]
	 */

	register int	i;
	auto int	num1, num2, num3;

	Debug(DPARS|DEXT, ("get_range()...entering, exit won't show\n"))

	if (ch == '*') {
		/* '*' means "first-last" but can still be modified by /step
		 */
		num1 = low;
		num2 = high;
		ch = get_char(file);
		if (ch == EOF)
			return EOF;
	} else {
		if (EOF == (ch = get_number(&num1, low, names, ch, file)))
			return EOF;

		if (ch != '-') {
			/* not a range, it's a single number.
			 */

			/* Unsupported syntax: Step specified without range,
			   eg:   1/20 * * * * /bin/echo "this fails"
			 */
			if (ch == '/')
				return EOF;

			if (EOF == set_element(bits, low, high, num1))
				return EOF;
			return ch;
		} else {
			/* eat the dash
			 */
			ch = get_char(file);
			if (ch == EOF)
				return EOF;

			/* get the number following the dash
			 */
			ch = get_number(&num2, low, names, ch, file);
			if (ch == EOF)
				return EOF;
		}
	}

	/* check for step size
	 */
	if (ch == '/') {
		/* eat the slash
		 */
		ch = get_char(file);
		if (ch == EOF)
			return EOF;

		/* get the step size -- note: we don't pass the
		 * names here, because the number is not an
		 * element id, it's a step size.  'low' is
		 * sent as a 0 since there is no offset either.
		 */
		ch = get_number(&num3, 0, PPC_NULL, ch, file);
		if (ch == EOF || num3 <= 0)
			return EOF;
	} else {
		/* no step.  default==1.
		 */
		num3 = 1;
	}

	/* Explicitly check for sane values. Certain combinations of ranges and
	 * steps which should return EOF don't get picked up by the code below,
	 * eg:
	 *      5-64/30 * * * *         touch /dev/null
	 *
	 * Code adapted from set_elements() where this error was probably intended
	 * to be catched.
	 */
	if (num1 < low || num1 > high || num2 < low || num2 > high)
		return EOF;

	/* range. set all elements from num1 to num2, stepping
	 * by num3.  (the step is a downward-compatible extension
	 * proposed conceptually by bob@acornrc, syntactically
	 * designed then implmented by paul vixie).
	 */
	for (i = num1;  i <= num2;  i += num3)
		if (EOF == set_element(bits, low, high, i))
			return EOF;

	return ch;
}


static char
get_number(numptr, low, names, ch, file)
	int	*numptr;	/* where does the result go? */
	int	low;		/* offset applied to result if symbolic enum used */
	char	*names[];	/* symbolic names, if any, for enums */
	int	ch;		/* current character */
	FILE	*file;		/* source */
{
	char	temp[MAX_TEMPSTR], *pc;
	int	len, i, all_digits;

	/* collect alphanumerics into our fixed-size temp array
	 */
	pc = temp;
	len = 0;
	all_digits = TRUE;
	while (isalnum(ch)) {
		if (++len >= MAX_TEMPSTR)
			return EOF;

		*pc++ = ch;

		if (!isdigit(ch))
			all_digits = FALSE;

		ch = get_char(file);
	}
	*pc = '\0';

	if (len == 0) {
		return EOF;
	}

	/* try to find the name in the name list
	 */
	if (names) {
		for (i = 0;  names[i] != NULL;  i++) {
			Debug(DPARS|DEXT,
				("get_num, compare(%s,%s)\n", names[i], temp))
			if (!strcasecmp(names[i], temp)) {
				*numptr = i+low;
				return ch;
			}
		}
	}

	/* no name list specified, or there is one and our string isn't
	 * in it.  either way: if it's all digits, use its magnitude.
	 * otherwise, it's an error.
	 */
	if (all_digits) {
		*numptr = atoi(temp);
		return ch;
	}

	return EOF;
}


static int
set_element(bits, low, high, number)
	bitstr_t	*bits; 		/* one bit per flag, default=FALSE */
	int		low;
	int		high;
	int		number;
{
	Debug(DPARS|DEXT, ("set_element(?,%d,%d,%d)\n", low, high, number))

	if (number < low || number > high)
		return EOF;

	bit_set(bits, (number-low));
	return OK;
}
