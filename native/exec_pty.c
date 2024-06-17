/*******************************************************************************
 * Copyright (c) 2004, 2010 QNX Software Systems and others.
 * All rights reserved. This program and the accompanying materials
 * are made available under the terms of the Eclipse Public License v1.0
 * which accompanies this distribution, and is available at
 * http://www.eclipse.org/legal/epl-v10.html
 *
 * Contributors:
 *     QNX Software Systems - initial API and implementation
 *     Wind River Systems, Inc.  
 *     Mikhail Zabaluev (Nokia) - bug 82744
 *     Mikhail Sennikovsky - bug 145737
 *******************************************************************************/
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <stdbool.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <sys/syscall.h>
#include <dirent.h>
#include <ctype.h>

#include "exec_pty.h"

int test();

/* from pfind.c */
extern char *pfind(const char *name, char * const envp[]);

/* from openpty.c */

extern int ptys_open(int fdm, const char *pts_name, bool acquire);

extern void set_noecho(int fd);


void restore_signal(int signum) {
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_handler = SIG_DFL;
    sigemptyset(&action.sa_mask);
    action.sa_flags = 0;
    if (sigaction(signum, &action, NULL) != 0) {
        fprintf(stderr, "%s(%d): cannot set SIG_DFL for signal %d: %s\n", __FUNCTION__, __LINE__, signum, strerror(errno));
    }
}

void restore_signals() {
    restore_signal(SIGPIPE);
    restore_signal(SIGINT);
    restore_signal(SIGQUIT);
}

static int sys_close_range_wrapper(unsigned int from_fd_inclusive) {
    // Use fast `close_range` (https://man7.org/linux/man-pages/man2/close_range.2.html) if available.
    // Cannot call `close_range` from libc, as it may be unavailable in older libc.
# if defined(__linux__) && defined(SYS_close_range) && defined(CLOSE_RANGE_UNSHARE)
    return syscall(SYS_close_range, from_fd_inclusive, ~0U, CLOSE_RANGE_UNSHARE);
# else
    errno = ENOSYS;
    return -1;
# endif
}

static int close_all_fds_using_parsing(unsigned int from_fd_inclusive) {
    // If `opendir` is implemented using a file descriptor, we may close it accidentally.
    // Let's close a few lowest file descriptors, in hope that `opendir` will use it.
    int lowest_fds_to_close = 2;
    for (int i = 0; i < lowest_fds_to_close; i++) {
        close(from_fd_inclusive + i);
    }

#if defined(_ALLBSD_SOURCE)
#define FD_DIR "/dev/fd"
#else
#define FD_DIR "/proc/self/fd"
#endif

    DIR *dirp = opendir(FD_DIR);
    if (dirp == NULL) return -1;

    struct dirent *direntp;

    while ((direntp = readdir(dirp)) != NULL) {
        int fd;
        if (isdigit(direntp->d_name[0])) {
            fd = strtol(direntp->d_name, NULL, 10);
            if (fd >= from_fd_inclusive + lowest_fds_to_close && fd != dirfd(dirp)) {
                close(fd);
            }
        }
    }

    closedir(dirp);

    return 0;
}

static void close_all_fds_fallback(unsigned int from_fd_inclusive) {
    int fdlimit = sysconf(_SC_OPEN_MAX);
    if (fdlimit == -1) fdlimit = 65535; // arbitrary default, just in case
    for (int fd = from_fd_inclusive; fd < fdlimit; fd++) {
        close(fd);
    }
}

static void close_all_fds() {
    unsigned int from_fd = STDERR_FILENO + 1;
    if (sys_close_range_wrapper(from_fd) == 0) return;
    if (close_all_fds_using_parsing(from_fd) == 0) return;
    close_all_fds_fallback(from_fd);
}

pid_t exec_pty(const char *path, char *const argv[], char *const envp[], const char *dirpath,
		       const char *pts_name, int fdm, const char *err_pts_name, int err_fdm, int console)
{
	pid_t childpid;
	char *full_path;

	/*
	 * We use pfind() to check that the program exists and is an executable.
	 * If not pass the error up.  Also execve() wants a full path.
	 */ 
	full_path = pfind(path, envp);
	if (full_path == NULL) {
		fprintf(stderr, "Unable to find full path for \"%s\"\n", (path) ? path : "");
		return -1;
	}

	childpid = fork();

	if (childpid < 0) {
		fprintf(stderr, "%s(%d): returning due to error: %s\n", __FUNCTION__, __LINE__, strerror(errno));
		free(full_path);
		return -1;
	} else if (childpid == 0) { /* child */

		chdir(dirpath);

		int fds;
		int err_fds = -1;

		if (!console && setsid() < 0) {
			perror("setsid()");
			return -1;
		}

		fds = ptys_open(fdm, pts_name, true);
		if (fds < 0) {
			fprintf(stderr, "%s(%d): returning due to error: %s\n", __FUNCTION__, __LINE__, strerror(errno));
			return -1;
		}

		if (console && err_fdm >= 0) {
			err_fds = ptys_open(err_fdm, err_pts_name, false);
			if (err_fds < 0) {
				fprintf(stderr, "%s(%d): returning due to error: %s\n", __FUNCTION__, __LINE__, strerror(errno));
				return -1;
			}
		}

		/* close masters, no need in the child */
		close(fdm);
		if (console && err_fdm >= 0) close(err_fdm);

		if (console) {
			set_noecho(fds);
			if (setpgid(getpid(), getpid()) < 0) {
				perror("setpgid()");
				return -1;
			}
		}

		/* redirections */
		dup2(fds, STDIN_FILENO);   /* dup stdin */
		dup2(fds, STDOUT_FILENO);  /* dup stdout */
		dup2(console && err_fds >= 0 ? err_fds : fds, STDERR_FILENO);  /* dup stderr */

		close(fds);  /* done with fds. */
		if (console && err_fds >= 0) close(err_fds);

		/* Close all the fd's in the child */
        close_all_fds();

		restore_signals();

		execve(full_path, argv, envp);

		_exit(127);

	} else if (childpid != 0) { /* parent */
		if (console) {
			set_noecho(fdm);
		}

		free(full_path);
		return childpid;
	}

	free(full_path);
	return -1;                  /*NOT REACHED */
}

int wait_for_child_process_exit(pid_t child_pid) {
    int status;
    while (waitpid(child_pid, &status, 0) < 0) {
        switch (errno) {
            case ECHILD:
                return 0;
            case EINTR:
                break;
            default:
                return -1;
        }
    }
    if (WIFEXITED(status)) {
        // The process exited normally; get its exit code.
        return WEXITSTATUS(status);
    }
    if (WIFSIGNALED(status)) {
        // The child exited because of a signal. Return 128 + signal number as all Unix shells do.
        // https://tldp.org/LDP/abs/html/exitcodes.html
        return 128 + WTERMSIG(status);
    }
    return status; // Unknown exit code; pass it as is.
}

int errno_non_zero() {
  int last_error = errno;
  return last_error > 0 ? last_error : -1; // ensure non-zero value
}

int get_window_size(int fd, struct winsize *size) {
    // TIOCGWINSZ have no portable number
    if (ioctl(fd, TIOCGWINSZ, size) < 0) {
        return errno_non_zero();
    }
    return 0;
}

int set_window_size(int fd, const struct winsize *size) {
    // TIOCSWINSZ have no portable number
    if (ioctl(fd, TIOCSWINSZ, size) < 0) {
        return errno_non_zero();
    }
    return 0;
}

int is_valid_fd(int fd) {
    return fcntl(fd, F_GETFD) != -1 || errno != EBADF;
}
