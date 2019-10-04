/*
 *   Unreal Internet Relay Chat Daemon, src/bsd.c
 *   Copyright (C) 1990 Jarkko Oikarinen and
 *                      University of Oulu, Computing Center
 *
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; either version 1, or (at your option)
 *   any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program; if not, write to the Free Software
 *   Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

/* -- Jto -- 07 Jul 1990
 * Added jlp@hamblin.byu.edu's debugtty fix
 */

/* -- Armin -- Jun 18 1990
 * Added setdtablesize() for more socket connections
 */

/* -- Jto -- 13 May 1990
 * Added several fixes from msa:
 *   Better error messages
 *   Changes in check_access
 * Added SO_REUSEADDR fix from zessel@informatik.uni-kl.de
 */

/* 2.78 2/7/94 (C) 1988 University of Oulu, Computing Center and Jarkko Oikarinen */

#include "unrealircd.h"
#include "dns.h"

#ifndef _WIN32
#define SET_ERRNO(x) errno = x
#else
#define SET_ERRNO(x) WSASetLastError(x)
#endif /* _WIN32 */

#ifndef SOMAXCONN
# define LISTEN_SIZE	(5)
#else
# define LISTEN_SIZE	(SOMAXCONN)
#endif

extern char backupbuf[8192];
int      OpenFiles = 0;    /* GLOBAL - number of files currently open */
int readcalls = 0;

int connect_inet(ConfigItem_link *, Client *);
void completed_connection(int, int, void *);
void set_sock_opts(int, Client *, int);
void set_ipv6_opts(int);
void close_listener(ConfigItem_listen *listener);
static char readbuf[BUFSIZE];
char zlinebuf[BUFSIZE];
extern char *version;
MODVAR time_t last_allinuse = 0;

#ifdef USE_LIBCURL
extern void url_do_transfers_async(void);
#endif

/*
 * Try and find the correct name to use with getrlimit() for setting the max.
 * number of files allowed to be open by this process.
 */
#ifdef RLIMIT_FDMAX
# define RLIMIT_FD_MAX   RLIMIT_FDMAX
#else
# ifdef RLIMIT_NOFILE
#  define RLIMIT_FD_MAX RLIMIT_NOFILE
# else
#  ifdef RLIMIT_OPEN_MAX
#   define RLIMIT_FD_MAX RLIMIT_OPEN_MAX
#  else
#   undef RLIMIT_FD_MAX
#  endif
# endif
#endif

void start_of_normal_client_handshake(Client *acptr);
void proceed_normal_client_handshake(Client *acptr, struct hostent *he);

/* winlocal */
void close_connections(void)
{
	Client* cptr;

	list_for_each_entry(cptr, &lclient_list, lclient_node)
	{
		if (cptr->local->fd >= 0)
		{
			fd_close(cptr->local->fd);
			cptr->local->fd = -2;
		}
	}

	list_for_each_entry(cptr, &unknown_list, lclient_node)
	{
		if (cptr->local->fd >= 0)
		{
			fd_close(cptr->local->fd);
			cptr->local->fd = -2;
		}

		if (cptr->local->authfd >= 0)
		{
			fd_close(cptr->local->authfd);
			cptr->local->fd = -1;
		}
	}

	close_listeners();

	OpenFiles = 0;

#ifdef _WIN32
	WSACleanup();
#endif
}

/*
** Cannot use perror() within daemon. stderr is closed in
** ircd and cannot be used. And, worse yet, it might have
** been reassigned to a normal connection...
*/

/*
** report_error
**	This a replacement for perror(). Record error to log and
**	also send a copy to all *LOCAL* opers online.
**
**	text	is a *format* string for outputting error. It must
**		contain only two '%s', the first will be replaced
**		by the sockhost from the cptr, and the latter will
**		be taken from strerror(errno).
**
**	cptr	if not NULL, is the *LOCAL* client associated with
**		the error.
*/
void report_error(char *text, Client *cptr)
{
	int errtmp = ERRNO, origerr = ERRNO;
	char *host, xbuf[256];
	int  err, len = sizeof(err), n;
	
	host = (cptr) ? get_client_name(cptr, FALSE) : "";

	Debug((DEBUG_ERROR, text, host, STRERROR(errtmp)));

	/*
	 * Get the *real* error from the socket (well try to anyway..).
	 * This may only work when SO_DEBUG is enabled but its worth the
	 * gamble anyway.
	 */
#ifdef	SO_ERROR
	if (cptr && !IsMe(cptr) && cptr->local->fd >= 0)
		if (!getsockopt(cptr->local->fd, SOL_SOCKET, SO_ERROR, (void *)&err, &len))
			if (err)
				errtmp = err;
#endif
	if (origerr != errtmp) {
		/* Socket error is different than original error,
		 * some tricks are needed because of 2x strerror() (or at least
		 * according to the man page) -- Syzop.
		 */
		snprintf(xbuf, 200, "[syserr='%s'", STRERROR(origerr));
		n = strlen(xbuf);
		snprintf(xbuf+n, 256-n, ", sockerr='%s']", STRERROR(errtmp));
		sendto_snomask(SNO_JUNK, text, host, xbuf);
		ircd_log(LOG_ERROR, text, host, xbuf);
	} else {
		sendto_snomask(SNO_JUNK, text, host, STRERROR(errtmp));
		ircd_log(LOG_ERROR, text,host,STRERROR(errtmp));
	}
	return;
}

void report_baderror(char *text, Client *cptr)
{
#ifndef _WIN32
	int  errtmp = errno;	/* debug may change 'errno' */
#else
	int  errtmp = WSAGetLastError();	/* debug may change 'errno' */
#endif
	char *host;
	int  err, len = sizeof(err);

	host = (cptr) ? get_client_name(cptr, FALSE) : "";

/*	fprintf(stderr, text, host, strerror(errtmp));
	fputc('\n', stderr); */
	Debug((DEBUG_ERROR, text, host, STRERROR(errtmp)));

	/*
	 * Get the *real* error from the socket (well try to anyway..).
	 * This may only work when SO_DEBUG is enabled but its worth the
	 * gamble anyway.
	 */
#ifdef	SO_ERROR
	if (cptr && !IsMe(cptr) && cptr->local->fd >= 0)
		if (!getsockopt(cptr->local->fd, SOL_SOCKET, SO_ERROR, (void *)&err, &len))
			if (err)
				errtmp = err;
#endif
	sendto_umode(UMODE_OPER, text, host, STRERROR(errtmp));
	ircd_log(LOG_ERROR, text, host, STRERROR(errtmp));
	return;
}

/** Accept an incoming client. */
static void listener_accept(int listener_fd, int revents, void *data)
{
	ConfigItem_listen *listener = data;
	int cli_fd;

	if ((cli_fd = fd_accept(listener->fd)) < 0)
	{
		if ((ERRNO != P_EWOULDBLOCK) && (ERRNO != P_ECONNABORTED))
		{
			/* Trouble! accept() returns a strange error.
			 * Previously in such a case we would just log/broadcast the error and return,
			 * causing this message to be triggered at a rate of XYZ per second (100% CPU).
			 * Now we close & re-start the listener.
			 * Of course the underlying cause of this issue should be investigated, as this
			 * is very much a workaround.
			 */
			report_baderror("Cannot accept connections %s:%s", NULL);
			sendto_realops("[BUG] Restarting listener on %s:%d due to fatal errors (see previous message)", listener->ip, listener->port);
			close_listener(listener);
			start_listeners();
		}
		return;
	}

	ircstats.is_ac++;

	set_sock_opts(cli_fd, NULL, listener->ipv6);

	if ((++OpenFiles >= maxclients) || (cli_fd >= maxclients))
	{
		ircstats.is_ref++;
		if (last_allinuse < TStime() - 15)
		{
			sendto_ops_and_log("All connections in use. ([@%s/%u])", listener->ip, listener->port);
			last_allinuse = TStime();
		}

		(void)send(cli_fd, "ERROR :All connections in use\r\n", 31, 0);

		fd_close(cli_fd);
		--OpenFiles;
		return;
	}

	/* add_connection() may fail. we just don't care. */
	(void)add_connection(listener, cli_fd);
}

/*
 * inetport
 *
 * Create a socket, bind it to the 'ip' and 'port' and listen to it.
 * Returns the fd of the socket created or -1 on error.
 */
int inetport(ConfigItem_listen *listener, char *ip, int port, int ipv6)
{
	if (BadPtr(ip))
		ip = "*";
	
	if (*ip == '*')
	{
		if (ipv6)
			ip = "::";
		else
			ip = "0.0.0.0";
	}

	/* At first, open a new socket */
	if (listener->fd >= 0)
		abort(); /* Socket already exists but we are asked to create and listen on one. Bad! */
	
	if (port == 0)
		abort(); /* Impossible as well, right? */

	listener->fd = fd_socket(ipv6 ? AF_INET6 : AF_INET, SOCK_STREAM, 0, "Listener socket");
	if (listener->fd < 0)
	{
		report_baderror("Cannot open stream socket() %s:%s", NULL);
		return -1;
	}

	if (++OpenFiles >= maxclients)
	{
		sendto_ops_and_log("No more connections allowed (%s)", listener->ip);
		fd_close(listener->fd);
		listener->fd = -1;
		--OpenFiles;
		return -1;
	}

	set_sock_opts(listener->fd, NULL, ipv6);

	if (!unreal_bind(listener->fd, ip, port, ipv6))
	{
		ircsnprintf(backupbuf, sizeof(backupbuf), "Error binding stream socket to IP %s port %i",
			ip, port);
		strlcat(backupbuf, " - %s:%s", sizeof backupbuf);
		report_baderror(backupbuf, NULL);
		fd_close(listener->fd);
		listener->fd = -1;
		--OpenFiles;
		return -1;
	}

	if (listen(listener->fd, LISTEN_SIZE) < 0)
	{
		report_error("listen failed for %s:%s", NULL);
		fd_close(listener->fd);
		listener->fd = -1;
		--OpenFiles;
		return -1;
	}

#ifdef TCP_DEFER_ACCEPT
	if (listener->options & LISTENER_DEFER_ACCEPT)
	{
		int yes = 1;

		(void)setsockopt(listener->fd, IPPROTO_TCP, TCP_DEFER_ACCEPT, &yes, sizeof(int));
	}
#endif

#ifdef SO_ACCEPTFILTER
	if (listener->options & LISTENER_DEFER_ACCEPT)
	{
		struct accept_filter_arg afa;

		memset(&afa, '\0', sizeof afa);
		strlcpy(afa.af_name, "dataready", sizeof afa.af_name);
		(void)setsockopt(listener->fd, SOL_SOCKET, SO_ACCEPTFILTER, &afa, sizeof afa);
	}
#endif

	/* ircd_log(LOG_ERROR, "FD #%d: Listener on %s:%d", listener->fd, ipname, port); */

	fd_setselect(listener->fd, FD_SELECT_READ, listener_accept, listener);

	return 0;
}

int add_listener(ConfigItem_listen *conf)
{
	if (inetport(conf, conf->ip, conf->port, conf->ipv6))
	{
		/* This error is already handled upstream:
		 * ircd_log(LOG_ERROR, "inetport failed for %s:%u", conf->ip, conf->port);
		 */
		conf->fd = -2;
	}
	if (conf->fd >= 0)
	{
		conf->options |= LISTENER_BOUND;
		return 1;
	}
	else
	{
		conf->fd = -1;
		return -1;
	}
}

/*
 * close_listeners
 *
 * Close the listener. Note that this won't *free* the listen block, it
 * just makes it so no new clients are accepted (and marks the listener
 * as "not bound").
 */
void close_listener(ConfigItem_listen *listener)
{
	if (listener->fd >= 0)
	{
		ircd_log(LOG_ERROR, "IRCd no longer listening on %s:%d (%s)%s",
			listener->ip, listener->port,
			listener->ipv6 ? "IPv6" : "IPv4",
			listener->options & LISTENER_TLS ? " (SSL/TLS)" : "");
		fd_close(listener->fd);
		--OpenFiles;
	}

	listener->options &= ~LISTENER_BOUND;
	listener->fd = -1;
	if (listener->ssl_ctx)
	{
		SSL_CTX_free(listener->ssl_ctx);
		listener->ssl_ctx = NULL;
	}
}

void close_listeners(void)
{
	ConfigItem_listen *aconf, *aconf_next;

	/* close all 'extra' listening ports we have */
	for (aconf = conf_listen; aconf != NULL; aconf = aconf_next)
	{
		aconf_next = aconf->next;
		if (aconf->flag.temporary)
			close_listener(aconf);
	}
}

int maxclients = 1024 - CLIENTS_RESERVE;

void check_user_limit(void)
{
#ifdef RLIMIT_FD_MAX
	struct rlimit limit;
	long m;

	if (!getrlimit(RLIMIT_FD_MAX, &limit))
	{
		if (limit.rlim_max < MAXCONNECTIONS)
			m = limit.rlim_max;
		else
			m = MAXCONNECTIONS;

		/* Adjust soft limit (if necessary, which is often the case) */
		if (m != limit.rlim_cur)
		{
			limit.rlim_cur = limit.rlim_max = m;
			if (setrlimit(RLIMIT_FD_MAX, &limit) == -1)
			{
				/* HACK: if it's mac os X then don't error... */
#ifndef OSXTIGER
				fprintf(stderr, "error setting maximum number of open files to %ld\n",
					(long)limit.rlim_cur);
				exit(-1);
#endif // OSXTIGER
			}
		}
		/* This can only happen if it is due to resource limits (./Config already rejects <100) */
		if (m < 100)
		{
			fprintf(stderr, "\nERROR: Your OS has a limit placed on this account.\n"
			                "This machine only allows UnrealIRCd to handle a maximum of %ld open connections/files, which is VERY LOW.\n"
			                "Please check with your system administrator to bump this limit.\n"
			                "The recommended ulimit -n setting is at least 1024 and "
			                "preferably 4096.\n"
			                "Note that this error is often seen on small web shells that are not meant for running IRC servers.\n",
			                m);
			exit(-1);
		}
		maxclients = m - CLIENTS_RESERVE;
	}
#endif // RLIMIT_FD_MAX

#ifndef _WIN32
#ifdef BACKEND_SELECT
	if (MAXCONNECTIONS > FD_SETSIZE)
	{
		fprintf(stderr, "MAXCONNECTIONS (%d) is higher than FD_SETSIZE (%d)\n", MAXCONNECTIONS, FD_SETSIZE);
		fprintf(stderr, "You should not see this error on Linux or FreeBSD\n");
		fprintf(stderr, "You might need to recompile the IRCd and answer a lower value to the MAXCONNECTIONS question in ./Config\n");
		exit(-1);
	}
#endif
#endif
#ifdef _WIN32
	maxclients = MAXCONNECTIONS - CLIENTS_RESERVE;
#endif
}

void init_sys(void)
{
#ifndef _WIN32
	/* Create new session / set process group */
	(void)setsid();
#endif

	init_resolver(1);
	return;
}

/** Replace a file descriptor (*NIX only).
 * @param oldfd: the old FD to close and re-use
 * @param name: descriptive string of the old fd, eg: "stdin".
 * @param mode: an open() mode, such as O_WRONLY.
 */
void replacefd(int oldfd, char *name, int mode)
{
#ifndef _WIN32
	int newfd = open("/dev/null", mode);
	if (newfd < 0)
	{
		fprintf(stderr, "Warning: could not open /dev/null\n");
		return;
	}
	if (oldfd < 0)
	{
		fprintf(stderr, "Warning: could not replace %s (invalid fd)\n", name);
		return;
	}
	if (dup2(newfd, oldfd) < 0)
	{
		fprintf(stderr, "Warning: could not replace %s (dup2 error)\n", name);
		return;
	}
#endif
}

/* Mass close standard file descriptors.
 * We used to really just close them here (or in init_sys() actually),
 * making the fd's available for other purposes such as internet sockets.
 * For safety we now dup2() them to /dev/null. This in case someone
 * accidentally does a fprintf(stderr,..) somewhere in the code or some
 * library outputs error messages to stderr (such as libc with heap
 * errors). We don't want any IRC client to receive such a thing!
 */
void close_std_descriptors(void)
{
#if !defined(_WIN32) && !defined(NOCLOSEFD)
	replacefd(fileno(stdin), "stdin", O_RDONLY);
	replacefd(fileno(stdout), "stdout", O_WRONLY);
	replacefd(fileno(stderr), "stderr", O_WRONLY);
#endif
}

void write_pidfile(void)
{
#ifdef IRCD_PIDFILE
	int fd;
	char buff[20];
	if ((fd = open(conf_files->pid_file, O_CREAT | O_WRONLY, 0600)) < 0)
	{
		ircd_log(LOG_ERROR, "Error writing to pid file %s: %s", conf_files->pid_file, strerror(ERRNO));
		return;
	}
	ircsnprintf(buff, sizeof(buff), "%5d\n", (int)getpid());
	if (write(fd, buff, strlen(buff)) < 0)
		ircd_log(LOG_ERROR, "Error writing to pid file %s: %s", conf_files->pid_file, strerror(ERRNO));
	if (close(fd) < 0)
		ircd_log(LOG_ERROR, "Error writing to pid file %s: %s", conf_files->pid_file, strerror(ERRNO));
#endif
}

/** Reject an insecure (outgoing) server link that isn't SSL/TLS.
 * This function is void and not int because it can be called from other void functions
 */
void reject_insecure_server(Client *cptr)
{
	sendto_umode(UMODE_OPER, "Could not link with server %s with SSL/TLS enabled. "
	                         "Please check logs on the other side of the link. "
	                         "If you insist with insecure linking then you can set link::options::outgoing::insecure "
	                         "(NOT recommended!).",
	                         cptr->name);
	dead_link(cptr, "Rejected link without SSL/TLS");
}

void start_server_handshake(Client *cptr)
{
	ConfigItem_link *aconf = cptr->serv ? cptr->serv->conf : NULL;

	if (!aconf)
	{
		/* Should be impossible. */
		sendto_ops_and_log("Lost configuration for %s in start_server_handshake()", get_client_name(cptr, FALSE));
		return;
	}

	RunHook(HOOKTYPE_SERVER_HANDSHAKE_OUT, cptr);

	sendto_one(cptr, NULL, "PASS :%s", (aconf->auth->type == AUTHTYPE_PLAINTEXT) ? aconf->auth->data : "*");

	send_protoctl_servers(cptr, 0);
	send_proto(cptr, aconf);
	if (NEW_LINKING_PROTOCOL)
	{
		/* Sending SERVER message moved to cmd_protoctl, so it's send after the first PROTOCTL
		 * we receive from the remote server. Of course, this assumes that the remote server
		 * to which we are connecting will at least send one PROTOCTL... but since it's an
		 * outgoing connect, we can safely assume it's a remote UnrealIRCd server (or some
		 * other advanced server..). -- Syzop
		 */

		/* Use this nasty hack, to make 3.2.9<->pre-3.2.9 linking work */
		sendto_one(cptr, NULL, "__PANGPANG__");
	} else {
		send_server_message(cptr);
	}
}

void consider_ident_lookup(Client *cptr)
{
	char buf[BUFSIZE];

	/* If ident checking is disabled or it's an outgoing connect, then no ident check */
	if ((IDENT_CHECK == 0) || (cptr->serv && IsHandshake(cptr)))
	{
		ClearIdentLookupSent(cptr);
		ClearIdentLookup(cptr);
		if (!IsDNSLookup(cptr))
			finish_auth(cptr);
		return;
	}
	RunHook(HOOKTYPE_IDENT_LOOKUP, cptr);

	return;
}


/*
** completed_connection
**	Complete non-blocking connect()-sequence. Check access and
**	terminate connection, if trouble detected.
**
**	Return	TRUE, if successfully completed
**		FALSE, if failed and ClientExit
*/
void completed_connection(int fd, int revents, void *data)
{
	Client *cptr = data;
	ConfigItem_link *aconf = cptr->serv ? cptr->serv->conf : NULL;

	if (IsHandshake(cptr))
	{
		/* Due to delayed ircd_SSL_connect call */
		start_server_handshake(cptr);
		fd_setselect(fd, FD_SELECT_READ, read_packet, cptr);
		return;
	}

	SetHandshake(cptr);

	if (!aconf)
	{
		sendto_ops_and_log("Lost configuration for %s", get_client_name(cptr, FALSE));
		return;
	}

	if (!cptr->local->ssl && !(aconf->outgoing.options & CONNECT_INSECURE))
	{
		sendto_one(cptr, NULL, "STARTTLS");
	} else
	{
		start_server_handshake(cptr);
	}

	if (!IsDeadSocket(cptr))
		consider_ident_lookup(cptr);

	fd_setselect(fd, FD_SELECT_READ, read_packet, cptr);
}

/*
** close_connection
**	Close the physical connection. This function must make
**	MyConnect(cptr) == FALSE, and set cptr->direction == NULL.
*/
void close_connection(Client *cptr)
{
	if (IsServer(cptr))
	{
		ircstats.is_sv++;
		ircstats.is_sbs += cptr->local->sendB;
		ircstats.is_sbr += cptr->local->receiveB;
		ircstats.is_sks += cptr->local->sendK;
		ircstats.is_skr += cptr->local->receiveK;
		ircstats.is_sti += TStime() - cptr->local->firsttime;
		if (ircstats.is_sbs > 1023)
		{
			ircstats.is_sks += (ircstats.is_sbs >> 10);
			ircstats.is_sbs &= 0x3ff;
		}
		if (ircstats.is_sbr > 1023)
		{
			ircstats.is_skr += (ircstats.is_sbr >> 10);
			ircstats.is_sbr &= 0x3ff;
		}
	}
	else if (IsUser(cptr))
	{
		ircstats.is_cl++;
		ircstats.is_cbs += cptr->local->sendB;
		ircstats.is_cbr += cptr->local->receiveB;
		ircstats.is_cks += cptr->local->sendK;
		ircstats.is_ckr += cptr->local->receiveK;
		ircstats.is_cti += TStime() - cptr->local->firsttime;
		if (ircstats.is_cbs > 1023)
		{
			ircstats.is_cks += (ircstats.is_cbs >> 10);
			ircstats.is_cbs &= 0x3ff;
		}
		if (ircstats.is_cbr > 1023)
		{
			ircstats.is_ckr += (ircstats.is_cbr >> 10);
			ircstats.is_cbr &= 0x3ff;
		}
	}
	else
		ircstats.is_ni++;

	/*
	 * remove outstanding DNS queries.
	 */
	unrealdns_delreq_bycptr(cptr);

	if (cptr->local->authfd >= 0)
	{
		fd_close(cptr->local->authfd);
		cptr->local->authfd = -1;
		--OpenFiles;
	}

	if (cptr->local->fd >= 0)
	{
		send_queued(cptr);
		if (IsTLS(cptr) && cptr->local->ssl) {
			SSL_set_shutdown(cptr->local->ssl, SSL_RECEIVED_SHUTDOWN);
			SSL_smart_shutdown(cptr->local->ssl);
			SSL_free(cptr->local->ssl);
			cptr->local->ssl = NULL;
		}
		fd_close(cptr->local->fd);
		cptr->local->fd = -2;
		--OpenFiles;
		DBufClear(&cptr->local->sendQ);
		DBufClear(&cptr->local->recvQ);

	}

	cptr->direction = NULL;	/* ...this should catch them! >:) --msa */

	return;
}

void set_ipv6_opts(int fd)
{
#if defined(IPV6_V6ONLY)
	int opt = 1;
	(void)setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, (void *)&opt, sizeof(opt));
#endif
}

/** This sets the *OS* socket buffers.
 * Note that setting these high is not always a good idea.
 * For example for regular users we keep the receive buffer tight
 * so we detect a high receive queue (Excess Flood) properly.
 * See include/fdlist.h for more information
 */
void set_socket_buffers(int fd, int rcvbuf, int sndbuf)
{
	int opt;

	opt = rcvbuf;
	setsockopt(fd, SOL_SOCKET, SO_RCVBUF, (void *)&opt, sizeof(opt));

	opt = sndbuf;
	setsockopt(fd, SOL_SOCKET, SO_SNDBUF, (void *)&opt, sizeof(opt));
}

/*
** set_sock_opts
*/
void set_sock_opts(int fd, Client *cptr, int ipv6)
{
	int opt;

	if (ipv6)
		set_ipv6_opts(fd);
#ifdef SO_REUSEADDR
	opt = 1;
	if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (void *)&opt, sizeof(opt)) < 0)
			report_error("setsockopt(SO_REUSEADDR) %s:%s", cptr);
#endif
#if defined(SO_USELOOPBACK) && !defined(_WIN32)
	opt = 1;
	if (setsockopt(fd, SOL_SOCKET, SO_USELOOPBACK, (void *)&opt, sizeof(opt)) < 0)
		report_error("setsockopt(SO_USELOOPBACK) %s:%s", cptr);
#endif
	set_socket_buffers(fd, USER_SOCKET_RECEIVE_BUFFER, USER_SOCKET_SEND_BUFFER);
	/* Set to non blocking: */
#if !defined(_WIN32)
	if ((opt = fcntl(fd, F_GETFL, 0)) == -1)
	{
		if (cptr)
		{
			report_error("fcntl(fd, F_GETFL) failed for %s:%s", cptr);
		}
	}
	else if (fcntl(fd, F_SETFL, opt | O_NONBLOCK) == -1)
	{
		if (cptr)
		{
			report_error("fcntl(fd, F_SETL, nonb) failed for %s:%s", cptr);
		}
	}
#else
	opt = 1;
	if (ioctlsocket(fd, FIONBIO, &opt) < 0)
	{
		if (cptr)
		{
			report_error("ioctlsocket(fd,FIONBIO) failed for %s:%s", cptr);
		}
	}
#endif
}


int  get_sockerr(Client *cptr)
{
#ifndef _WIN32
	int  errtmp = errno, err = 0, len = sizeof(err);
#else
	int  errtmp = WSAGetLastError(), err = 0, len = sizeof(err);
#endif
#ifdef	SO_ERROR
	if (cptr->local->fd >= 0)
		if (!getsockopt(cptr->local->fd, SOL_SOCKET, SO_ERROR, (void *)&err, &len))
			if (err)
				errtmp = err;
#endif
	return errtmp;
}

/** Returns 1 if using a loopback IP (127.0.0.1) or
 * using a local IP number on the same machine (effectively the same;
 * no network traffic travels outside this machine).
 */
int is_loopback_ip(char *ip)
{
    ConfigItem_listen *e;

	if (!strcmp(ip, "127.0.0.1") || !strcmp(ip, "0:0:0:0:0:0:0:1") || !strcmp(ip, "0:0:0:0:0:ffff:127.0.0.1"))
		return 1;

    for (e = conf_listen; e; e = e->next)
    {
        if ((e->options & LISTENER_BOUND) && !strcmp(ip, e->ip))
            return 1;
    }
	return 0;
}

char *getpeerip(Client *acptr, int fd, int *port)
{
	static char ret[HOSTLEN+1];

	if (IsIPV6(acptr))
	{
		struct sockaddr_in6 addr;
		int len = sizeof(addr);

		if (getpeername(fd, (struct sockaddr *)&addr, &len) < 0)
			return NULL;
		*port = ntohs(addr.sin6_port);
		return inetntop(AF_INET6, &addr.sin6_addr.s6_addr, ret, sizeof(ret));
	} else
	{
		struct sockaddr_in addr;
		int len = sizeof(addr);

		if (getpeername(fd, (struct sockaddr *)&addr, &len) < 0)
			return NULL;
		*port = ntohs(addr.sin_port);
		return inetntop(AF_INET, &addr.sin_addr.s_addr, ret, sizeof(ret));
	}
}

/** This checks set::max-unknown-connections-per-ip,
 * which is an important safety feature.
 */
static int check_too_many_unknown_connections(Client *acptr)
{
	int cnt = 1;
	Client *c;

	if (!find_tkl_exception(TKL_CONNECT_FLOOD, acptr))
	{
		list_for_each_entry(c, &unknown_list, lclient_node)
		{
			if (!strcmp(acptr->ip,GetIP(c)))
			{
				cnt++;
				if (cnt > iConf.max_unknown_connections_per_ip)
					return 1;
			}
		}
	}

	return 0;
}

/*
 * Creates a client which has just connected to us on the given fd.
 * The sockhost field is initialized with the ip# of the host.
 * The client is added to the linked list of clients but isnt added to any
 * hash tables yuet since it doesnt have a name.
 */
Client *add_connection(ConfigItem_listen *listener, int fd)
{
	Client *acptr;
	char *ip;
	int port = 0;
	
	acptr = make_client(NULL, &me);

	/* If listener is IPv6 then mark client (acptr) as IPv6 */
	if (listener->ipv6)
		SetIPV6(acptr);

	ip = getpeerip(acptr, fd, &port);
	
	if (!ip)
	{
		/* On Linux 2.4 and FreeBSD the socket may just have been disconnected
		 * so it's not a serious error and can happen quite frequently -- Syzop
		 */
		if (ERRNO != P_ENOTCONN)
		{
			report_error("Failed to accept new client %s :%s", acptr);
		}
refuse_client:
			ircstats.is_ref++;
			acptr->local->fd = -2;
			free_client(acptr);
			fd_close(fd);
			--OpenFiles;
			return NULL;
	}

	/* Fill in sockhost & ip ASAP */
	set_sockhost(acptr, ip);
	safe_strdup(acptr->ip, ip);
	acptr->local->port = port;
	acptr->local->fd = fd;

	/* Tag loopback connections */
	if (is_loopback_ip(acptr->ip))
	{
		ircstats.is_loc++;
		SetLocalhost(acptr);
	}

	/* Check set::max-unknown-connections-per-ip */
	if (check_too_many_unknown_connections(acptr))
	{
		ircsnprintf(zlinebuf, sizeof(zlinebuf),
		            "ERROR :Closing Link: [%s] (Too many unknown connections from your IP)\r\n",
		            acptr->ip);
		(void)send(fd, zlinebuf, strlen(zlinebuf), 0);
		goto refuse_client;
	}

	/* Check (G)Z-Lines and set::anti-flood::connect-flood */
	if (check_banned(acptr, NO_EXIT_CLIENT) < 0)
		goto refuse_client;

	acptr->local->listener = listener;
	if (acptr->local->listener != NULL)
		acptr->local->listener->clients++;
	add_client_to_list(acptr);

	irccounts.unknown++;
	acptr->status = CLIENT_STATUS_UNKNOWN;

	list_add(&acptr->lclient_node, &unknown_list);

	if ((listener->options & LISTENER_TLS) && ctx_server)
	{
		SSL_CTX *ctx = listener->ssl_ctx ? listener->ssl_ctx : ctx_server;

		if (ctx)
		{
			SetTLSAcceptHandshake(acptr);
			Debug((DEBUG_DEBUG, "Starting TLS accept handshake for %s", acptr->local->sockhost));
			if ((acptr->local->ssl = SSL_new(ctx)) == NULL)
			{
				goto refuse_client;
			}
			SetTLS(acptr);
			SSL_set_fd(acptr->local->ssl, fd);
			SSL_set_nonblocking(acptr->local->ssl);
			SSL_set_ex_data(acptr->local->ssl, ssl_client_index, acptr);
			if (!ircd_SSL_accept(acptr, fd))
			{
				Debug((DEBUG_DEBUG, "Failed TLS accept handshake in instance 1: %s", acptr->local->sockhost));
				SSL_set_shutdown(acptr->local->ssl, SSL_RECEIVED_SHUTDOWN);
				SSL_smart_shutdown(acptr->local->ssl);
				SSL_free(acptr->local->ssl);
				goto refuse_client;
			}
		}
	}
	else
		start_of_normal_client_handshake(acptr);
	return acptr;
}

static int dns_special_flag = 0; /* This is for an "interesting" race condition  very ugly. */

void	start_of_normal_client_handshake(Client *acptr)
{
struct hostent *he;

	acptr->status = CLIENT_STATUS_UNKNOWN; /* reset, to be sure (TLS handshake has ended) */

	RunHook(HOOKTYPE_HANDSHAKE, acptr);

	if (!DONT_RESOLVE)
	{
		if (SHOWCONNECTINFO && !acptr->serv && !IsServersOnlyListener(acptr->local->listener))
			sendto_one(acptr, NULL, "%s", REPORT_DO_DNS);
		dns_special_flag = 1;
		he = unrealdns_doclient(acptr);
		dns_special_flag = 0;

		if (acptr->local->hostp)
			goto doauth; /* Race condition detected, DNS has been done, continue with auth */

		if (!he)
		{
			/* Resolving in progress */
			SetDNSLookup(acptr);
		} else {
			/* Host was in our cache */
			acptr->local->hostp = he;
			if (SHOWCONNECTINFO && !acptr->serv && !IsServersOnlyListener(acptr->local->listener))
				sendto_one(acptr, NULL, "%s", REPORT_FIN_DNSC);
		}
	}

doauth:
	consider_ident_lookup(acptr);
	fd_setselect(acptr->local->fd, FD_SELECT_READ, read_packet, acptr);
}

void proceed_normal_client_handshake(Client *acptr, struct hostent *he)
{
	ClearDNSLookup(acptr);
	acptr->local->hostp = he;
	if (SHOWCONNECTINFO && !acptr->serv && !IsServersOnlyListener(acptr->local->listener))
		sendto_one(acptr, NULL, "%s", acptr->local->hostp ? REPORT_FIN_DNS : REPORT_FAIL_DNS);

	if (!dns_special_flag && !IsIdentLookup(acptr))
		finish_auth(acptr);
}

/*
** read_packet
**
** Read a 'packet' of data from a connection and process it.  Read in 8k
** chunks to give a better performance rating (for server connections).
** Do some tricky stuff for client connections to make sure they don't do
** any flooding >:-) -avalon
** If 'doread' is set to 0 then we don't actually read (no recv()),
** however we still check if we need to dequeue anything from the recvQ.
** This is necessary, since we may have put something on the recvQ due
** to fake lag. -- Syzop
** With new I/O code, things work differently.  Surprise!
** read_one_packet() reads packets in and dumps them as quickly as
** possible into the client's DBuf.  Then we parse data out of the DBuf,
** after we're done reading crap.
**    -- nenolod
*/
static void parse_client_queued(Client *cptr)
{
	int dolen = 0;
	time_t now = TStime();
	char buf[READBUFSIZE];

	if (IsDNSLookup(cptr))
		return; /* we delay processing of data until the host is resolved */

	if (IsIdentLookup(cptr))
		return; /* we delay processing of data until identd has replied */

	if (!IsUser(cptr) && !IsServer(cptr) && (iConf.handshake_delay > 0) &&
	    (TStime() - cptr->local->firsttime < iConf.handshake_delay))
	{
		return; /* we delay processing of data until set::handshake-delay is reached */
	}

	while (DBufLength(&cptr->local->recvQ) &&
	    ((cptr->status < CLIENT_STATUS_UNKNOWN) || (cptr->local->since - now < 10)))
	{
		dolen = dbuf_getmsg(&cptr->local->recvQ, buf);

		if (dolen == 0)
			return;

		dopacket(cptr, buf, dolen);
		
		if (IsDead(cptr))
			return;
	}
}

/** Put a packet in the client receive queue and process the data (if
 * the 'fake lag' rules permit doing so).
 * @param cptr        The client
 * @param readbuf     The read buffer
 * @param length      The length of the data
 * @param killsafely  If 1 then we may call exit_client() if the client
 *                    is flooding. If 0 then we use dead_link().
 * @returns 1 in normal circumstances, 0 if client was killed.
 * @notes If killsafely is 1 and the return value is 0 then
 *        you may not touch 'cptr' after calling this function
 *        since the client (cptr) has been freed.
 *        If this is a problem, then set killsafely to 0 when calling.
 */
int process_packet(Client *cptr, char *readbuf, int length, int killsafely)
{
	dbuf_put(&cptr->local->recvQ, readbuf, length);

	/* parse some of what we have (inducing fakelag, etc) */
	parse_client_queued(cptr);

	/* We may be killed now, so check for it.. */
	if (IsDead(cptr))
		return 0;

	/* flood from unknown connection */
	if (IsUnknown(cptr) && (DBufLength(&cptr->local->recvQ) > UNKNOWN_FLOOD_AMOUNT*1024))
	{
		sendto_snomask(SNO_FLOOD, "Flood from unknown connection %s detected",
			cptr->local->sockhost);
		if (!killsafely)
			ban_flooder(cptr);
		else
			dead_link(cptr, "Flood from unknown connection");
		return 0;
	}

	/* excess flood check */
	if (IsUser(cptr) && DBufLength(&cptr->local->recvQ) > get_recvq(cptr))
	{
		sendto_snomask(SNO_FLOOD,
			"*** Flood -- %s!%s@%s (%d) exceeds %d recvQ",
			cptr->name[0] ? cptr->name : "*",
			cptr->user ? cptr->user->username : "*",
			cptr->user ? cptr->user->realhost : "*",
			DBufLength(&cptr->local->recvQ), get_recvq(cptr));
		if (!killsafely)
			exit_client(cptr, NULL, "Excess Flood");
		else
			dead_link(cptr, "Excess Flood");
		return 0;
	}

	return 1;
}

void read_packet(int fd, int revents, void *data)
{
	Client *cptr = data;
	int length = 0;
	time_t now = TStime();
	Hook *h;
	int processdata;

	/* Don't read from dead sockets */
	if (IsDeadSocket(cptr))
	{
		fd_setselect(fd, FD_SELECT_READ, NULL, cptr);
		return;
	}

	SET_ERRNO(0);

	fd_setselect(fd, FD_SELECT_READ, read_packet, cptr);
	/* Restore handling of writes towards send_queued_cb(), since
	 * it may be overwritten in an earlier call to read_packet(),
	 * to handle (SSL) writes by read_packet(), see below under
	 * SSL_ERROR_WANT_WRITE.
	 */
	fd_setselect(fd, FD_SELECT_WRITE, send_queued_cb, cptr);

	while (1)
	{
		if (IsTLS(cptr) && cptr->local->ssl != NULL)
		{
			length = SSL_read(cptr->local->ssl, readbuf, sizeof(readbuf));

			if (length < 0)
			{
				int err = SSL_get_error(cptr->local->ssl, length);

				switch (err)
				{
				case SSL_ERROR_WANT_WRITE:
					fd_setselect(fd, FD_SELECT_READ, NULL, cptr);
					fd_setselect(fd, FD_SELECT_WRITE, read_packet, cptr);
					length = -1;
					SET_ERRNO(P_EWOULDBLOCK);
					break;
				case SSL_ERROR_WANT_READ:
					fd_setselect(fd, FD_SELECT_READ, read_packet, cptr);
					length = -1;
					SET_ERRNO(P_EWOULDBLOCK);
					break;
				case SSL_ERROR_SYSCALL:
					break;
				case SSL_ERROR_SSL:
					if (ERRNO == P_EAGAIN)
						break;
				default:
					/*length = 0;
					SET_ERRNO(0);
					^^ why this? we should error. -- todo: is errno correct?
					*/
					break;
				}
			}
		}
		else
			length = recv(cptr->local->fd, readbuf, sizeof(readbuf), 0);

		if (length <= 0)
		{
			if (length < 0 && ((ERRNO == P_EWOULDBLOCK) || (ERRNO == P_EAGAIN) || (ERRNO == P_EINTR)))
				return;

			if (IsServer(cptr) || cptr->serv) /* server or outgoing connection */
			{
				sendto_umode_global(UMODE_OPER, "Lost connection to %s: Read error",
					get_client_name(cptr, FALSE));
				ircd_log(LOG_ERROR, "Lost connection to %s: Read error",
					get_client_name(cptr, FALSE));
			}

			exit_client(cptr, NULL, "Read error");
			return;
		}

		cptr->local->lasttime = now;
		if (cptr->local->lasttime > cptr->local->since)
			cptr->local->since = cptr->local->lasttime;
		/* FIXME: Is this correct? I have my doubts. */
		ClearPingSent(cptr);

		ClearPingWarning(cptr);

		processdata = 1;
		for (h = Hooks[HOOKTYPE_RAWPACKET_IN]; h; h = h->next)
		{
			processdata = (*(h->func.intfunc))(cptr, readbuf, &length);
			if (processdata < 0)
				return;
		}

		if (processdata && !process_packet(cptr, readbuf, length, 0))
			return;

		/* bail on short read! */
		if (length < sizeof(readbuf))
			return;
	}
}

/* Process input from clients that may have been deliberately delayed due to fake lag */
void process_clients(void)
{
	Client *cptr, *cptr2;
        
	/* Problem:
	 * When processing a client, that current client may exit due to eg QUIT.
	 * Similarly, current->next may be killed due to /KILL.
	 * When a client is killed, in the past we were not allowed to touch it anymore
	 * so that was a bit problematic. Now we can touch current->next, but it may
	 * have been removed from the lclient_list or unknown_list.
	 * In other words, current->next->next may be NULL even though there are more
	 * clients on the list.
	 * This is why the whole thing is wrapped in an additional do { } while() loop
	 * to make sure we re-run the list if we ended prematurely.
	 * We could use some kind of 'tagging' to mark already processed clients.
	 * However, parse_client_queued() already takes care not to read (fake) lagged
	 * clients, and we don't actually read/recv anything in the meantime, so clients
	 * in the beginning of the list won't benefit, they won't get higher prio.
	 * Another alternative is not to run the loop again, but that WOULD be
	 * unfair to clients later in the list which wouldn't be processed then
	 * under a heavy (kill) load scenario.
	 * I think the chosen solution is best, though it remains silly. -- Syzop
	 */

	do {
		list_for_each_entry_safe(cptr, cptr2, &lclient_list, lclient_node)
		{
			if ((cptr->local->fd >= 0) && DBufLength(&cptr->local->recvQ) && !IsDead(cptr))
				parse_client_queued(cptr);
		}
	} while(&cptr->lclient_node != &lclient_list);

	do {
		list_for_each_entry_safe(cptr, cptr2, &unknown_list, lclient_node)
		{
			if ((cptr->local->fd >= 0) && DBufLength(&cptr->local->recvQ) && !IsDead(cptr))
				parse_client_queued(cptr);
		}
	} while(&cptr->lclient_node != &unknown_list);
}

/* When auth is finished, go back and parse all prior input. */
void finish_auth(Client *acptr)
{
}

/** Returns 4 if 'str' is a valid IPv4 address
 * and 6 if 'str' is a valid IPv6 IP address.
 * Zero (0) is returned in any other case (eg: hostname).
 */
int is_valid_ip(char *str)
{
	char scratch[64];
	
	if (inet_pton(AF_INET, str, scratch) == 1)
		return 4; /* IPv4 */
	
	if (inet_pton(AF_INET6, str, scratch) == 1)
		return 6; /* IPv6 */
	
	return 0; /* not an IP address */
}

/*
 * connect_server
 */
int  connect_server(ConfigItem_link *aconf, Client *by, struct hostent *hp)
{
	Client *cptr;

#ifdef DEBUGMODE
	sendto_realops("connect_server() called with aconf %p, refcount: %d, TEMP: %s",
		aconf, aconf->refcount, aconf->flag.temporary ? "YES" : "NO");
#endif

	if (!aconf->outgoing.hostname)
		return -1; /* This is an incoming-only link block. Caller shouldn't call us. */
		
	if (!hp)
	{
		/* Remove "cache" */
		safe_free(aconf->connect_ip);
	}
	/*
	 * If we dont know the IP# for this host and itis a hostname and
	 * not a ip# string, then try and find the appropriate host record.
	 */
	if (!aconf->connect_ip)
	{
		if (is_valid_ip(aconf->outgoing.hostname))
		{
			/* link::outgoing::hostname is an IP address. No need to resolve host. */
			safe_strdup(aconf->connect_ip, aconf->outgoing.hostname);
		} else
		{
			/* It's a hostname, let the resolver look it up. */
			int ipv4_explicit_bind = 0;

			if (aconf->outgoing.bind_ip && (is_valid_ip(aconf->outgoing.bind_ip) == 4))
				ipv4_explicit_bind = 1;
			
			/* We need this 'aconf->refcount++' or else there's a race condition between
			 * starting resolving the host and the result of the resolver (we could
			 * REHASH in that timeframe) leading to an invalid (freed!) 'aconf'.
			 * -- Syzop, bug #0003689.
			 */
			aconf->refcount++;
			unrealdns_gethostbyname_link(aconf->outgoing.hostname, aconf, ipv4_explicit_bind);
			return -2;
		}
	}
	cptr = make_client(NULL, &me);
	cptr->local->hostp = hp;
	/*
	 * Copy these in so we have something for error detection.
	 */
	strlcpy(cptr->name, aconf->servername, sizeof(cptr->name));
	strlcpy(cptr->local->sockhost, aconf->outgoing.hostname, HOSTLEN + 1);

	if (!connect_inet(aconf, cptr))
	{
		int errtmp = ERRNO;
		report_error("Connect to host %s failed: %s", cptr);
		if (by && IsUser(by) && !MyUser(by))
			sendnotice(by, "*** Connect to host %s failed.", cptr->name);
		fd_close(cptr->local->fd);
		--OpenFiles;
		cptr->local->fd = -2;
		free_client(cptr);
		SET_ERRNO(errtmp);
		if (ERRNO == P_EINTR)
			SET_ERRNO(P_ETIMEDOUT);
		return -1;
	}
	/* The socket has been connected or connect is in progress. */
	(void)make_server(cptr);
	cptr->serv->conf = aconf;
	cptr->serv->conf->refcount++;
#ifdef DEBUGMODE
	sendto_realops("connect_server() CONTINUED (%s:%d), aconf %p, refcount: %d, TEMP: %s",
		__FILE__, __LINE__, aconf, aconf->refcount, aconf->flag.temporary ? "YES" : "NO");
#endif
	Debug((DEBUG_ERROR, "reference count for %s (%s) is now %d",
		cptr->name, cptr->serv->conf->servername, cptr->serv->conf->refcount));
	if (by && IsUser(by))
		strlcpy(cptr->serv->by, by->name, sizeof(cptr->serv->by));
	else
		strlcpy(cptr->serv->by, "AutoConn.", sizeof cptr->serv->by);
	cptr->serv->up = me.name;
	SetConnecting(cptr);
	SetOutgoing(cptr);
	irccounts.unknown++;
	list_add(&cptr->lclient_node, &unknown_list);
	set_sockhost(cptr, aconf->outgoing.hostname);
	add_client_to_list(cptr);

	if (aconf->outgoing.options & CONNECT_TLS)
	{
		SetTLSConnectHandshake(cptr);
		fd_setselect(cptr->local->fd, FD_SELECT_WRITE, ircd_SSL_client_handshake, cptr);
	}
	else
		fd_setselect(cptr->local->fd, FD_SELECT_WRITE, completed_connection, cptr);

	return 0;
}

int connect_inet(ConfigItem_link *aconf, Client *cptr)
{
	char *bindip;
	char buf[BUFSIZE];

	if (!aconf->connect_ip)
		return 0; /* handled upstream or shouldn't happen */
	
	if (strchr(aconf->connect_ip, ':'))
		SetIPV6(cptr);
	
	safe_strdup(cptr->ip, aconf->connect_ip);
	
	snprintf(buf, sizeof buf, "Outgoing connection: %s", get_client_name(cptr, TRUE));
	cptr->local->fd = fd_socket(IsIPV6(cptr) ? AF_INET6 : AF_INET, SOCK_STREAM, 0, buf);
	if (cptr->local->fd < 0)
	{
		if (ERRNO == P_EMFILE)
		{
		  sendto_realops("opening stream socket to server %s: No more sockets",
					 get_client_name(cptr, TRUE));
		  return 0;
		}
		report_baderror("opening stream socket to server %s:%s", cptr);
		return 0;
	}
	if (++OpenFiles >= maxclients)
	{
		sendto_ops_and_log("No more connections allowed (%s)", cptr->name);
		return 0;
	}

	set_sockhost(cptr, aconf->outgoing.hostname);

	if (!aconf->outgoing.bind_ip && iConf.link_bindip)
		bindip = iConf.link_bindip;
	else
		bindip = aconf->outgoing.bind_ip;

	if (bindip && strcmp("*", bindip))
	{
		if (!unreal_bind(cptr->local->fd, bindip, 0, IsIPV6(cptr)))
		{
			report_baderror("Error binding to local port for %s:%s -- "
			                "Your link::outgoing::bind-ip is probably incorrect.", cptr);
			return 0;
		}
	}

	set_sock_opts(cptr->local->fd, cptr, IsIPV6(cptr));

	return unreal_connect(cptr->local->fd, cptr->ip, aconf->outgoing.port, IsIPV6(cptr));
}

/** Checks if the system is IPv6 capable.
 * IPv6 is always available at compile time (libs, headers), but the OS may
 * not have IPv6 enabled (or ipv6 kernel module not loaded). So we better check..
 */
int ipv6_capable(void)
{
	int s = socket(AF_INET6, SOCK_STREAM, 0);
	if (s < 0)
		return 0; /* NO ipv6 */
	
	CLOSE_SOCK(s);
	return 1; /* YES */
}

