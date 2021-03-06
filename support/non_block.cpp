/*
--------------------------------------------------------------------------------
This library is free software; you can redistribute it and/or
modify it under the terms of the GNU Library General Public
License as published by the Free Software Foundation; either
version 2 of the License, or (at your option) any later version.
This library is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
Library General Public License for more details.
You should have received a copy of the GNU Library General Public
License along with this library; if not, write to the
Free Software Foundation, Inc., 51 Franklin St, Fifth Floor,
Boston, MA  02110-1301, USA.
--------------------------------------------------------------------------------
*/

// Copyright (c) 2014-2017 John Seamons, ZL/KF6VO

#include "types.h"
#include "config.h"
#include "kiwi.h"
#include "misc.h"
#include "str.h"
#include "web.h"
#include "spi.h"
#include "cfg.h"
#include "coroutines.h"
#include "net.h"
#include "debug.h"

#include <sys/file.h>
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <setjmp.h>
#include <ctype.h>
#include <syslog.h>
#include <stdlib.h>
#include <stdarg.h>
#include <time.h>

#ifdef HOST
	#include <wait.h>
    #include <sys/prctl.h>
#endif

typedef struct {
    #define ZEXP 4      // >= 2
    int size;
    pid_t *list;
} zombies_t;

zombies_t zombies;

void cull_zombies()
{
    for (int i=0; i < zombies.size; i++) {
        if (zombies.list[i] == 0) continue;
        int status;
		pid_t pid = waitpid(zombies.list[i], &status, WNOHANG);
		if (pid > 0) {
		    zombies.list[i] = 0;
		    //lprintf("==== cull ZOMBIE @%d pid=%d status=%d\n", i, pid, status);
		}
    }
}

int child_task(const char *pname, int poll_msec, funcP_t func, void *param)
{
    int i;
    
    // If the child is not going to be waited for because poll_msec == NO_WAIT then it will end up as
    // a zombie process unless we eventually wait for it.
    // We accomplish this by waiting for all child processes in the waitpid() below and detect the zombies.

	pid_t child_pid;
	scall("fork", (child_pid = fork()));
	
	if (child_pid == 0) {
		TaskForkChild();

        // rename process as seen by top command
        #ifdef HOST
            prctl(PR_SET_NAME, (unsigned long) pname, 0, 0, 0);
        #endif
        // rename process as seen by ps command
        int sl = strlen(main_argv[0]);
        sprintf(main_argv[0], "%-*.*s", sl, sl, pname);     // have to blank fill, and not overrun, old argv[0]
        
		func(param);	// this function should exit() with some other value if it wants
		exit(EXIT_SUCCESS);
	}
	
	// parent
	
    //lprintf("==== child_task: child_pid=%d %s pname=%s\n", child_pid, (poll_msec == 0)? "NO_WAIT":"WAIT", pname);
	if (poll_msec == 0) {
	    for (i=0; i < zombies.size; i++) {
	        if (zombies.list[i] == 0) {
	            zombies.list[i] = child_pid;
		        //lprintf("==== add ZOMBIE @%d pid=%d\n", i, child_pid);
	            break;
	        }
	    }
	    if (i == zombies.size) {
	        zombies.list = (pid_t *) realloc(zombies.list, sizeof(pid_t)*(zombies.size + ZEXP));
	        zombies.list[zombies.size] = child_pid;
		    //lprintf("==== add exp ZOMBIE @%d pid=%d\n", zombies.size, child_pid);
	        memset(&zombies.list[zombies.size+1], 0, sizeof(pid_t)*(ZEXP-1));
	        zombies.size += ZEXP;
	        //printf("### zombies.size %d\n", zombies.size);
	    }
	    return 0;   // don't wait
	}
	
	int pid = 0, status, polls = 0;
	do {
	    if (pid == 0) {
            TaskSleepMsec(poll_msec);
            polls += poll_msec;
        }
		status = 0;
		pid = waitpid(child_pid, &status, WNOHANG);
        //lprintf("==== child_task: waitpid child_pid=%d pid=%d pname=%s status=%d poll=%d polls=%d\n", child_pid, pid, pname, status, poll_msec, polls);
		//if (pid == 0) lprintf("==== child_task: child_pid=%d NOT YET\n", child_pid);
		//if (pid == child_pid) lprintf("==== child_task: child_pid=%d DONE\n", child_pid);
		//if (pid < 0) lprintf("==== child_task: child_pid=%d ERROR\n", child_pid);
		//if (pid < 0) perror("child_task: waitpid");
	} while (pid != child_pid && pid != -1);

    //lprintf("==== child_task: child_pid=%d status=0x%08x WIFEXITED=%d WEXITSTATUS=%d\n", child_pid, status, WIFEXITED(status), WEXITSTATUS(status));
    if (!WIFEXITED(status))
        printf("child_task WARNING: child returned without WIFEXITED status=0x%08x WIFEXITED=%d WEXITSTATUS=%d\n",
            status, WIFEXITED(status), WEXITSTATUS(status));

    return status;
}

#define NON_BLOCKING_POLL_MSEC 50

// child task that calls a function for the entire command input read
static void _non_blocking_cmd_forall(void *param)
{
	int n, func_rv = 0;
	nbcmd_args_t *args = (nbcmd_args_t *) param;

	#define NCHUNK 256
	char chunk[NCHUNK + SPACE_FOR_NULL];
	args->kstr = NULL;

    //printf("_non_blocking_cmd_forall: %s\n", args->cmd);
	FILE *pf = popen(args->cmd, "r");
	if (pf == NULL) exit(EXIT_FAILURE);
	int pfd = fileno(pf);
	if (pfd <= 0) exit(EXIT_FAILURE);
	fcntl(pfd, F_SETFL, O_NONBLOCK);

	do {
		TaskSleepMsec(NON_BLOCKING_POLL_MSEC);
		n = read(pfd, chunk, NCHUNK);
		if (n > 0) {
		    chunk[n] = '\0';
			args->kstr = kstr_cat(args->kstr, chunk);
		}
	} while (n > 0 || (n == -1 && errno == EAGAIN));
	// end-of-input when n == 0 or error

    //printf("_non_blocking_cmd_forall: call func %p\n", args->kstr);
    func_rv = args->func((void *) args);

	kstr_free(args->kstr);
	pclose(pf);

    //printf("_non_blocking_cmd_forall: EXIT func_rv %d\n", func_rv);
	exit(func_rv);
	#undef NCHUNK
}

// child task that calls a function for every chunk of non-blocking command input read
/*
static void _non_blocking_cmd_foreach(void *param)
{
	int n, func_rv = 0;
	nbcmd_args_t *args = (nbcmd_args_t *) param;

	#define NCHUNK 256
	char chunk[NCHUNK + SPACE_FOR_NULL];
	args->bp = NULL;

	FILE *pf = popen(args->cmd, "r");
	if (pf == NULL) exit(EXIT_FAILURE);
	int pfd = fileno(pf);
	if (pfd <= 0) exit(EXIT_FAILURE);
	fcntl(pfd, F_SETFL, O_NONBLOCK);

	do {
		TaskSleepMsec(NON_BLOCKING_POLL_MSEC);
		n = read(pfd, chunk, NCHUNK);
		if (n > 0) {
		    chunk[n] = '\0';
			args->kstr = kstr_cat(args->bp, chunk);
			func_rv = args->func((void *) args);
		}
	} while (n > 0 || (n == -1 && errno == EAGAIN));

	free(args->bp);
	pclose(pf);

	exit(func_rv);
	#undef NCHUNK
}
*/

static void _non_blocking_cmd_system(void *param)
{
	char *cmd = (char *) param;

    //printf("_non_blocking_cmd_system: %s\n", cmd);
    int rv = system(cmd);
	exit(rv);
}

// like non_blocking_cmd() below, but run in a child process because pclose() can block
// for long periods of time under certain conditions
int non_blocking_cmd_func_child(const char *pname, const char *cmd, funcPR_t func, int param, int poll_msec)
{
	nbcmd_args_t *args = (nbcmd_args_t *) malloc(sizeof(nbcmd_args_t));
	args->cmd = cmd;
	args->func = func;
	args->func_param = param;
	int status = child_task(pname, poll_msec, _non_blocking_cmd_forall, (void *) args);
	free(args);
    //printf("non_blocking_cmd_child %d\n", status);
	return status;
}

int non_blocking_cmd_system_child(const char *pname, const char *cmd, int poll_msec)
{
	int status = child_task(pname, poll_msec, _non_blocking_cmd_system, (void *) cmd);
    //printf("non_blocking_cmd_child %d\n", status);
	return status;
}

// the popen read can block, so do non-blocking i/o with an interspersed TaskSleep()
// NB: assumes the reply is a string (kstr_t *), not binary with embedded NULLs
kstr_t *non_blocking_cmd(const char *cmd, int *status)
{
	int n, stat;
	#define NBUF 256
	char buf[NBUF + SPACE_FOR_NULL];
	
	NextTask("non_blocking_cmd");
    evNT(EC_EVENT, EV_NEXTTASK, -1, "non_blocking_cmd", evprintf("popen %s...", cmd));
	FILE *pf = popen(cmd, "r");
	if (pf == NULL) return 0;
	int pfd = fileno(pf);
	if (pfd <= 0) return 0;
	fcntl(pfd, F_SETFL, O_NONBLOCK);
    evNT(EC_EVENT, EV_NEXTTASK, -1, "non_blocking_cmd", evprintf("...popen"));
	char *reply = NULL;

	do {
		TaskSleepMsec(NON_BLOCKING_POLL_MSEC);
		n = read(pfd, buf, NBUF);
        evNT(EC_EVENT, EV_NEXTTASK, -1, "non_blocking_cmd", evprintf("after read()"));
		if (n > 0) {
		    buf[n] = '\0';
		    reply = kstr_cat(reply, buf);
		}
	} while (n > 0 || (n == -1 && errno == EAGAIN));
	// end-of-input when n == 0 or error

	// assuming we're always expecting a string
    evNT(EC_EVENT, EV_NEXTTASK, -1, "non_blocking_cmd", evprintf("pclose..."));
	stat = pclose(pf);
    evNT(EC_EVENT, EV_NEXTTASK, -1, "non_blocking_cmd", evprintf("...pclose"));
	if (status != NULL)
		*status = stat;
	return reply;
	#undef NBUF
}

// non_blocking_cmd() broken down into separately callable routines in case you have to
// do something with each chunk of data read
int non_blocking_cmd_popen(non_blocking_cmd_t *p)
{
	NextTask("non_blocking_cmd_popen");
	p->pf = popen(p->cmd, "r");
	if (p->pf == NULL) return 0;
	p->pfd = fileno(p->pf);
	if (p->pfd <= 0) return 0;
	fcntl(p->pfd, F_SETFL, O_NONBLOCK);
	p->open = true;

	return 1;
}

int non_blocking_cmd_read(non_blocking_cmd_t *p, char *reply, int reply_size)
{
	int n;
	assert(p->open);

	do {
		TaskSleepMsec(NON_BLOCKING_POLL_MSEC);
		n = read(p->pfd, reply, reply_size - SPACE_FOR_NULL);
		if (n > 0) reply[n] = 0;	// assuming we're always expecting a string
	} while (n == -1 && errno == EAGAIN);

	return n;
}

int non_blocking_cmd_write(non_blocking_cmd_t *p, char *sbuf)
{
	int n;
	int slen = strlen(sbuf);
	assert(p->open);

	do {
		TaskSleepMsec(NON_BLOCKING_POLL_MSEC);
		n = write(p->pfd, sbuf, slen);
		if (n > 0) {
		    sbuf += n;
		    slen -= n;
		}
	} while (slen > 0 || (n == -1 && errno == EAGAIN));

	return n;
}

int non_blocking_cmd_pclose(non_blocking_cmd_t *p)
{
	assert(p->open);
	return pclose(p->pf);
}

kstr_t *read_file_string_reply(const char *filename)
{
    int n, fd;
	scall("read_file open", (fd = open(filename, O_RDONLY)));
	char *reply = NULL;
	#define NBUF 256
	char buf[NBUF + SPACE_FOR_NULL];
	do {
	    n = read(fd, buf, NBUF);
		if (n > 0) {
		    buf[n] = '\0';
		    reply = kstr_cat(reply, buf);
		}
	} while (n > 0);
	close(fd);
	return reply;
	#undef NBUF
}
