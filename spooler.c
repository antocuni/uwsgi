#ifdef UWSGI_SPOOLER
#include "uwsgi.h"

extern struct uwsgi_server uwsgi;

static void spooler_readdir(char *);
#ifdef __linux__
static void spooler_scandir(char *);
#endif
void spooler_manage_task(char *, char *);

// fake function to allow waking the spooler
void spooler_wakeup() {}

pid_t spooler_start() {
	
	int i;

	pid_t pid = uwsgi_fork("uWSGI spooler");
	if (pid < 0) {
		uwsgi_error("fork()");
		exit(1);
	}
	else if (pid == 0) {
		// USR1 will be used to wake up the spooler
		signal(SIGUSR1, spooler_wakeup);
		uwsgi.mywid = -1;
		uwsgi.mypid = getpid();
		// avoid race conditions !!!
		uwsgi.shared->spooler_pid = uwsgi.mypid;

		uwsgi_fixup_fds(0, 0);
		uwsgi_close_all_sockets();

		for (i = 0; i < 0xFF; i++) {
                	if (uwsgi.p[i]->post_fork) {
                        	uwsgi.p[i]->post_fork();
                	}
        	}

		uwsgi.signal_socket = uwsgi.shared->spooler_signal_pipe[1];

		for (i = 0; i < 0xFF; i++) {
                	if (uwsgi.p[i]->spooler_init) {
                        	uwsgi.p[i]->spooler_init();
                	}
        	}

        	for (i = 0; i < uwsgi.gp_cnt; i++) {
                	if (uwsgi.gp[i]->spooler_init) {
                        	uwsgi.gp[i]->spooler_init();
                	}
        	}

		spooler();
	}
	else if (pid > 0) {
		uwsgi_log("spawned the uWSGI spooler on dir %s with pid %d\n", uwsgi.spool_dir, pid);
	}

	return pid;
}

void destroy_spool(char *dir, char *file) {

	if (chdir(dir)) {
		uwsgi_error("chdir()");
                uwsgi_log("something horrible happened to the spooler. Better to kill it.\n");
		exit(1);
	}

	if (unlink(file)) {
        	uwsgi_error("unlink()");
                uwsgi_log("something horrible happened to the spooler. Better to kill it.\n");
                exit(1);
	}

}


int spool_request(char *filename, int rn, int core_id, char *buffer, int size, char *priority, time_t at, char *body, size_t body_len) {

	struct timeval tv;
	int fd;
	struct uwsgi_header uh;

	uwsgi_lock(uwsgi.spooler_lock);

	gettimeofday(&tv, NULL);

	if (priority) {
		if (snprintf(filename, 1024, "%s/%s", uwsgi.spool_dir, priority) <= 0) {
			uwsgi_unlock(uwsgi.spooler_lock);
			return 0;
		}
		// no need to check for errors...
		(void) mkdir(filename, 0777);

		if (snprintf(filename, 1024, "%s/%s/uwsgi_spoolfile_on_%s_%d_%d_%d_%llu_%llu", uwsgi.spool_dir, priority, uwsgi.hostname, (int) getpid(), rn, core_id, (unsigned long long) tv.tv_sec, (unsigned long long) tv.tv_usec) <= 0) {
			uwsgi_unlock(uwsgi.spooler_lock);
			return 0;
		}
	}
	else {
		if (snprintf(filename, 1024, "%s/uwsgi_spoolfile_on_%s_%d_%d_%d_%llu_%llu", uwsgi.spool_dir, uwsgi.hostname, (int) getpid(), rn, core_id, (unsigned long long) tv.tv_sec, (unsigned long long) tv.tv_usec) <= 0) {
			uwsgi_unlock(uwsgi.spooler_lock);
			return 0;
		}
	}

	fd = open(filename, O_CREAT | O_EXCL | O_WRONLY, S_IRUSR | S_IWUSR);
	if (fd < 0) {
		uwsgi_error_open(filename);
		uwsgi_unlock(uwsgi.spooler_lock);
		return 0;
	}

#ifdef __sun__
	if (lockf(fd, F_LOCK, 0)) {
		uwsgi_error("lockf()");
#else
	if (flock(fd, LOCK_EX)) {
		uwsgi_error("flock()");
#endif
		close(fd);
		uwsgi_unlock(uwsgi.spooler_lock);
		return 0;
	}

	uh.modifier1 = 17;
	uh.modifier2 = 0;
	uh.pktsize = (uint16_t) size;
#ifdef __BIG_ENDIAN__
	uh.pktsize = uwsgi_swap16(uh.pktsize);
#endif

	if (write(fd, &uh, 4) != 4) {
		goto clear;
	}

	if (write(fd, buffer, size) != size) {
		goto clear;
	}

	if (body && body_len > 0) {
		if ((size_t)write(fd, body, body_len) != body_len) {
			goto clear;	
		}
	}

	if (at > 0) {
		struct timeval tv[2];
		tv[0].tv_sec = at;
		tv[0].tv_usec = 0;
		tv[1].tv_sec = at;
		tv[1].tv_usec = 0;
#ifdef __sun__
		if (futimesat(fd, NULL, tv)) {
#else
		if (futimes(fd, tv)) {
#endif
			uwsgi_error("futimes()");
		}
	}

	close(fd);

	uwsgi_log("written %d bytes to spool file %s\n", size + body_len + 4, filename);
	
	uwsgi_unlock(uwsgi.spooler_lock);

/*	wake up the spooler ... (HACKY) */
	if (uwsgi.shared->spooler_pid > 0 ) {
		(void) kill(uwsgi.shared->spooler_pid, SIGUSR1);
	}

	return 1;


      clear:
	uwsgi_unlock(uwsgi.spooler_lock);
	uwsgi_error("write()");
	if (unlink(filename)) {
		uwsgi_error("unlink()");
	}
	close(fd);
	return 0;
}



void spooler() {

	// prevent process blindly reading stdin to make mess
	int nullfd;

	// asked by Marco Beri
#ifdef __HAIKU__
#ifdef UWSGI_DEBUG
	uwsgi_log("lowering spooler priority to %d\n", B_LOW_PRIORITY);
#endif
	set_thread_priority(find_thread(NULL), B_LOW_PRIORITY);
#else
#ifdef UWSGI_DEBUG
	uwsgi_log("lowering spooler priority to %d\n", PRIO_MAX);
#endif
	setpriority(PRIO_PROCESS, getpid(), PRIO_MAX);
#endif

	nullfd = open("/dev/null", O_RDONLY);
	if (nullfd < 0) {
		uwsgi_error_open("/dev/null");
		exit(1);
	}

	if (nullfd != 0) {
		dup2(nullfd, 0);
		close(nullfd);
	}

	int spooler_event_queue = event_queue_init();
	int interesting_fd = -1;

	if (uwsgi.master_process) {
		event_queue_add_fd_read(spooler_event_queue, uwsgi.shared->spooler_signal_pipe[1]);
	}

	for (;;) {


		if (chdir(uwsgi.spool_dir)) {
			uwsgi_error("chdir()");
			exit(1);
		}

		if (uwsgi.spooler_ordered) {
#ifdef __linux__
			spooler_scandir(uwsgi.spool_dir);
#else
			spooler_readdir(uwsgi.spool_dir);
#endif
		}
		else {
			spooler_readdir(uwsgi.spool_dir);
		}

		if (event_queue_wait(spooler_event_queue, uwsgi.shared->spooler_frequency, &interesting_fd) > 0) {
			if (uwsgi.master_process) {
				if (interesting_fd == uwsgi.shared->spooler_signal_pipe[1]) {
					uint8_t uwsgi_signal;
					if (read(interesting_fd, &uwsgi_signal, 1) <= 0) {
                                                	uwsgi_log_verbose("uWSGI spooler screams: UAAAAAAH my master died, i will follow him...\n");
                                               		end_me(0); 
                                	}
                                	else {
#ifdef UWSGI_DEBUG
                                        	uwsgi_log_verbose("master sent signal %d to the spooler\n", uwsgi_signal);
#endif
                                        	if (uwsgi_signal_handler(uwsgi_signal)) {
                                                	uwsgi_log_verbose("error managing signal %d on the spooler\n", uwsgi_signal);
                                        	}
                                	}
				}
			}
		}

	}
}

#ifdef __linux__
static void spooler_scandir(char *dir) {

	struct dirent **tasklist;
	int n;

	n = scandir(dir, &tasklist, 0, versionsort);
	if (n < 0) {
		uwsgi_error("scandir()");
		return;
	}
	
	while(n--) {
		spooler_manage_task(dir, tasklist[n]->d_name);
		free(tasklist[n]);	
	}

	free(tasklist);
}
#endif


static void spooler_readdir(char *dir) {

	DIR *sdir;
	struct dirent *dp;

	sdir = opendir(dir);
	if (sdir) {
		while ((dp = readdir(sdir)) != NULL) {
			spooler_manage_task(dir, dp->d_name);
		}
		closedir(sdir);
	}
	else {
		uwsgi_error("opendir()");
	}
}

void spooler_manage_task(char *dir, char *task) {

	int i, ret;

	char spool_buf[0xffff];
	struct uwsgi_header uh;
	char *body = NULL;
	size_t body_len = 0;

	int spool_fd;

	if (!strncmp("uwsgi_spoolfile_on_", task, 19) || (uwsgi.spooler_ordered && is_a_number(task))) {
		struct stat sf_lstat;
		if (lstat(task, &sf_lstat)) {
			return;
		}

		// a spool request for the future
		if (sf_lstat.st_mtime > time(NULL)) {
			return;
		}

#ifdef __linux__
		if (S_ISDIR(sf_lstat.st_mode) && uwsgi.spooler_ordered) {
			if (chdir(task)) {
				uwsgi_error("chdir()");
				return;
			}
			char *prio_path = realpath(".", NULL);
			spooler_scandir(prio_path);
			free(prio_path);
			if (chdir(dir)) {
				uwsgi_error("chdir()");
			}
			return;
		}
#endif
		if (!S_ISREG(sf_lstat.st_mode)) {
			return;
		}
		if (!access(task, R_OK | W_OK)) {
			uwsgi_log("managing spool request %s ...\n", task);

#ifdef __sun__
			// lockf needs write permission
			spool_fd = open(task, O_RDWR);
#else
			spool_fd = open(task, O_RDONLY);
#endif
			if (spool_fd < 0) {
				uwsgi_error_open(task);
				return;
			}

#ifdef __sun__
			if (lockf(spool_fd, F_LOCK, 0)) {
				uwsgi_error("lockf()");
#else
			if (flock(spool_fd, LOCK_EX)) {
				uwsgi_error("flock()");
#endif
				close(spool_fd);
				return;
			}

			if (read(spool_fd, &uh, 4) != 4) {
				uwsgi_error("read()");
				close(spool_fd);
				return;
			}

#ifdef __BIG_ENDIAN__
			uh.pktsize = uwsgi_swap16(uh.pktsize);
#endif

			if (read(spool_fd, spool_buf, uh.pktsize) != uh.pktsize) {
				uwsgi_error("read()");
				destroy_spool(dir, task);	
				close(spool_fd);
				return;
			}			
					
			// body available ?
			if (sf_lstat.st_size > (uh.pktsize+4)) {
				body_len = sf_lstat.st_size - (uh.pktsize+4);
				body = uwsgi_malloc(body_len);
				if ((size_t)read(spool_fd, body, body_len) != body_len) {
					uwsgi_error("read()");
					destroy_spool(dir, task);
					close(spool_fd);
					free(body);
					return;
				}
			}

			close(spool_fd);

			if (uwsgi.spooler_chdir) {
				if (chdir(uwsgi.spooler_chdir)) {
					uwsgi_error("chdir()");
				}
			}

			int callable_found = 0;
			for(i=0;i<0xff;i++) {
				if (uwsgi.p[i]->spooler) {
					time_t now = time(NULL);
					if(uwsgi.shared->options[UWSGI_OPTION_SPOOLER_HARAKIRI] > 0) {
                        			set_spooler_harakiri(uwsgi.shared->options[UWSGI_OPTION_SPOOLER_HARAKIRI]);
                			}
					ret = uwsgi.p[i]->spooler(task, spool_buf, uh.pktsize, body, body_len);
					if(uwsgi.shared->options[UWSGI_OPTION_SPOOLER_HARAKIRI] > 0) {
                        			set_spooler_harakiri(0);
                			}
					if (body) {
						free(body);
					}
					if (ret == 0) continue;
					callable_found = 1;
					if (ret == -2) {
						uwsgi_log("done with task/spool %s after %d seconds\n", task, time(NULL)-now);
						destroy_spool(dir, task);	
					}
					// re-spool it
					break;	
				}
			}
			if (!callable_found) {
				uwsgi_log("unable to find the spooler function, have you loaded it into the spooler process ?\n");
			}

		}
	}
}


int uwsgi_request_spooler(struct wsgi_request *wsgi_req) {

	int i;
	char spool_filename[1024];

	if (uwsgi.spool_dir == NULL) {
		uwsgi_log("the spooler is inactive !!!...skip\n");
		uwsgi_send_empty_pkt(wsgi_req->poll.fd, NULL, 255, 0);
		return -1;
	}

	i = spool_request(spool_filename, uwsgi.workers[0].requests + 1, wsgi_req->async_id, wsgi_req->buffer, wsgi_req->uh.pktsize, NULL, 0, NULL, 0);
	wsgi_req->uh.modifier1 = 255;
	wsgi_req->uh.pktsize = 0;
	if (i > 0) {
		wsgi_req->uh.modifier2 = 1;
		if (write(wsgi_req->poll.fd, wsgi_req, 4) != 4) {
			uwsgi_log("disconnected client, remove spool file.\n");
			/* client disconnect, remove spool file */
			if (unlink(spool_filename)) {
				uwsgi_error("unlink()");
				uwsgi_log("something horrible happened !!! check your spooler ASAP !!!\n");
				exit(1);
			}
		}
		return 0;
	}
	else {
		/* announce a failed spool request */
		wsgi_req->uh.modifier2 = 0;
		i = write(wsgi_req->poll.fd, wsgi_req, 4);
		if (i != 4) {
			uwsgi_error("write()");
		}
	}

	return -1;
}

#else
#warning "*** Spooler support is disabled ***"
#endif
