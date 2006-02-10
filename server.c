/*
 * server.c -- nsd(8) network input/output
 *
 * Copyright (c) 2001-2006, NLnet Labs. All rights reserved.
 *
 * See LICENSE for the license.
 *
 */

#include <config.h>

#include <sys/types.h>
#include <sys/param.h>
#include <sys/socket.h>
#include <sys/wait.h>

#include <netinet/in.h>
#include <arpa/inet.h>

#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <netdb.h>

#include "axfr.h"
#include "namedb.h"
#include "netio.h"
#include "plugins.h"


/*
 * Data for the UDP handlers.
 */
struct udp_handler_data
{
	struct nsd        *nsd;
	struct nsd_socket *socket;
	query_type        *query;
};

/*
 * Data for the TCP accept handlers.  Most data is simply passed along
 * to the TCP connection handler.
 */
struct tcp_accept_handler_data {
	struct nsd         *nsd;
	struct nsd_socket  *socket;
	size_t              tcp_accept_handler_count;
	netio_handler_type *tcp_accept_handlers;
};

/*
 * Data for the TCP connection handlers.
 *
 * The TCP handlers use non-blocking I/O.  This is necessary to avoid
 * blocking the entire server on a slow TCP connection, but does make
 * reading from and writing to the socket more complicated.
 *
 * Basically, whenever a read/write would block (indicated by the
 * EAGAIN errno variable) we remember the position we were reading
 * from/writing to and return from the TCP reading/writing event
 * handler.  When the socket becomes readable/writable again we
 * continue from the same position.
 */
struct tcp_handler_data
{
	/*
	 * The region used to allocate all TCP connection related
	 * data, including this structure.  This region is destroyed
	 * when the connection is closed.
	 */
	region_type     *region;

	/*
	 * The global nsd structure.
	 */
	struct nsd      *nsd;

	/*
	 * The current query data for this TCP connection.
	 */
	query_type      *query;

	/*
	 * These fields are used to enable the TCP accept handlers
	 * when the number of TCP connection drops below the maximum
	 * number of TCP connections.
	 */
	size_t              tcp_accept_handler_count;
	netio_handler_type *tcp_accept_handlers;
	
	/*
	 * The query_state is used to remember if we are performing an
	 * AXFR, if we're done processing, or if we should discard the
	 * query and connection.
	 */
	query_state_type query_state;

	/*
	 * The bytes_transmitted field is used to remember the number
	 * of bytes transmitted when receiving or sending a DNS
	 * packet.  The count includes the two additional bytes used
	 * to specify the packet length on a TCP connection.
	 */
	size_t           bytes_transmitted;
};

/*
 * Handle incoming queries on the UDP server sockets.
 */
static void handle_udp(netio_type *netio,
		       netio_handler_type *handler,
		       netio_event_types_type event_types);

/*
 * Handle incoming connections on the TCP sockets.  These handlers
 * usually wait for the NETIO_EVENT_READ event (indicating an incoming
 * connection) but are disabled when the number of current TCP
 * connections is equal to the maximum number of TCP connections.
 * Disabling is done by changing the handler to wait for the
 * NETIO_EVENT_NONE type.  This is done using the function
 * configure_tcp_accept_handlers.
 */
static void handle_tcp_accept(netio_type *netio,
			      netio_handler_type *handler,
			      netio_event_types_type event_types);

/*
 * Handle incoming queries on a TCP connection.  The TCP connections
 * are configured to be non-blocking and the handler may be called
 * multiple times before a complete query is received.
 */
static void handle_tcp_reading(netio_type *netio,
			       netio_handler_type *handler,
			       netio_event_types_type event_types);

/*
 * Handle outgoing responses on a TCP connection.  The TCP connections
 * are configured to be non-blocking and the handler may be called
 * multiple times before a complete response is sent.
 */
static void handle_tcp_writing(netio_type *netio,
			       netio_handler_type *handler,
			       netio_event_types_type event_types);

/*
 * Handle a command received from the parent process.
 */
static void handle_parent_command(netio_type *netio,
				  netio_handler_type *handler,
				  netio_event_types_type event_types);

/*
 * Send all children the command.
 */
static void send_children_command(struct nsd* nsd, sig_atomic_t command);

/*
 * Handle a command received from the children processes.
 */
static void handle_child_command(netio_type *netio,
				  netio_handler_type *handler,
				  netio_event_types_type event_types);

/*
 * Change the event types the HANDLERS are interested in to
 * EVENT_TYPES.
 */
static void configure_handler_event_types(size_t count,
					  netio_handler_type *handlers,
					  netio_event_types_type event_types);


static uint16_t *compressed_dname_offsets;

/*
 * Remove the specified pid from the list of child pids.  Returns 0 if
 * the pid is not in the list, 1 otherwise.  The field is set to 0.
 */
static int
delete_child_pid(struct nsd *nsd, pid_t pid)
{
	size_t i;
	for (i = 0; i < nsd->child_count; ++i) {
		if (nsd->children[i].pid == pid) {
			nsd->children[i].pid = 0;
			if(nsd->children[i].child_fd > 0) close(nsd->children[i].child_fd);
			nsd->children[i].child_fd = -1;
			if(nsd->children[i].handler) nsd->children[i].handler->fd = -1;
			return 1;
		}
	}
	return 0;
}

/*
 * Restart child servers if necessary.
 */
static int
restart_child_servers(struct nsd *nsd, region_type* region, netio_type* netio)
{
	size_t i;
	int sv[2];

	/* Fork the child processes... */
	for (i = 0; i < nsd->child_count; ++i) {
		if (nsd->children[i].pid <= 0) {
			if (nsd->children[i].child_fd > 0)
				close(nsd->children[i].child_fd);
			if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == -1) {
				log_msg(LOG_ERR, "socketpair: %s",
					strerror(errno));
				return -1;
			}
			nsd->children[i].child_fd = sv[0];
			nsd->children[i].parent_fd = sv[1];
			nsd->children[i].pid = fork();
			switch (nsd->children[i].pid) {
			default: /* SERVER MAIN */
				close(nsd->children[i].parent_fd);
				nsd->children[i].parent_fd = -1;
				if(!nsd->children[i].handler)
				{
					nsd->children[i].handler = (struct netio_handler*) region_alloc(
						region, sizeof(struct netio_handler));
					nsd->children[i].handler->fd = nsd->children[i].child_fd;
					nsd->children[i].handler->timeout = NULL;
					nsd->children[i].handler->user_data = nsd;
					nsd->children[i].handler->event_types = NETIO_EVENT_READ;
					nsd->children[i].handler->event_handler = handle_child_command;
					netio_add_handler(netio, nsd->children[i].handler);
				}
				/* restart - update fd */
				nsd->children[i].handler->fd = nsd->children[i].child_fd;
				break;
			case 0: /* CHILD */
				nsd->pid = 0;
				nsd->child_count = 0;
				nsd->server_kind = nsd->children[i].kind;
				nsd->this_child = &nsd->children[i];
				close(nsd->this_child->child_fd);
				nsd->this_child->child_fd = -1;
				server_child(nsd);
				/* NOTREACH */
				exit(0);
			case -1:
				log_msg(LOG_ERR, "fork failed: %s",
					strerror(errno));
				return -1;
			}
		}
	}
	return 0;
}

static void
initialize_dname_compression_tables(struct nsd *nsd)
{
	compressed_dname_offsets = (uint16_t *) xalloc(
		(domain_table_count(nsd->db->domains) + 1) * sizeof(uint16_t));
	memset(compressed_dname_offsets, 0,
	       (domain_table_count(nsd->db->domains) + 1) * sizeof(uint16_t));
	compressed_dname_offsets[0] = QHEADERSZ; /* The original query name */
	region_add_cleanup(nsd->db->region, free, compressed_dname_offsets);
}

/*
 * Initialize the server, create and bind the sockets.
 * Drop the priviledges and chroot if requested.
 *
 */
int
server_init(struct nsd *nsd)
{
	size_t i;
#if defined(SO_REUSEADDR) || (defined(INET6) && (defined(IPV6_V6ONLY) || defined(IPV6_USE_MIN_MTU)))
	int on = 1;
#endif

	/* UDP */

	/* Make a socket... */
	for (i = 0; i < nsd->ifs; i++) {
		if ((nsd->udp[i].s = socket(nsd->udp[i].addr->ai_family, nsd->udp[i].addr->ai_socktype, 0)) == -1) {
			log_msg(LOG_ERR, "can't create a socket: %s", strerror(errno));
			return -1;
		}

#if defined(INET6)
		if (nsd->udp[i].addr->ai_family == AF_INET6) {
# if defined(IPV6_V6ONLY)
			if (setsockopt(nsd->udp[i].s,
				       IPPROTO_IPV6, IPV6_V6ONLY,
				       &on, sizeof(on)) < 0)
			{
				log_msg(LOG_ERR, "setsockopt(..., IPV6_V6ONLY, ...) failed: %s",
					strerror(errno));
				return -1;
			}
# endif
# if defined(IPV6_USE_MIN_MTU)
			/*
			 * There is no fragmentation of IPv6 datagrams
			 * during forwarding in the network. Therefore
			 * we do not send UDP datagrams larger than
			 * the minimum IPv6 MTU of 1280 octets. The
			 * EDNS0 message length can be larger if the
			 * network stack supports IPV6_USE_MIN_MTU.
			 */
			if (setsockopt(nsd->udp[i].s,
				       IPPROTO_IPV6, IPV6_USE_MIN_MTU,
				       &on, sizeof(on)) < 0)
			{
				log_msg(LOG_ERR, "setsockopt(..., IPV6_USE_MIN_MTU, ...) failed: %s",
					strerror(errno));
				return -1;
			}
# endif
		}
#endif

		/* Bind it... */
		if (bind(nsd->udp[i].s, (struct sockaddr *) nsd->udp[i].addr->ai_addr, nsd->udp[i].addr->ai_addrlen) != 0) {
			log_msg(LOG_ERR, "can't bind the socket: %s", strerror(errno));
			return -1;
		}
	}

	/* TCP */

	/* Make a socket... */
	for (i = 0; i < nsd->ifs; i++) {
		if ((nsd->tcp[i].s = socket(nsd->tcp[i].addr->ai_family, nsd->tcp[i].addr->ai_socktype, 0)) == -1) {
			log_msg(LOG_ERR, "can't create a socket: %s", strerror(errno));
			return -1;
		}

#ifdef	SO_REUSEADDR
		if (setsockopt(nsd->tcp[i].s, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on)) < 0) {
			log_msg(LOG_ERR, "setsockopt(..., SO_REUSEADDR, ...) failed: %s", strerror(errno));
			return -1;
		}
#endif /* SO_REUSEADDR */

#if defined(INET6) && defined(IPV6_V6ONLY)
		if (nsd->tcp[i].addr->ai_family == AF_INET6 &&
		    setsockopt(nsd->tcp[i].s, IPPROTO_IPV6, IPV6_V6ONLY, &on, sizeof(on)) < 0)
		{
			log_msg(LOG_ERR, "setsockopt(..., IPV6_V6ONLY, ...) failed: %s", strerror(errno));
			return -1;
		}
#endif

		/* Bind it... */
		if (bind(nsd->tcp[i].s, (struct sockaddr *) nsd->tcp[i].addr->ai_addr, nsd->tcp[i].addr->ai_addrlen) != 0) {
			log_msg(LOG_ERR, "can't bind the socket: %s", strerror(errno));
			return -1;
		}

		/* Listen to it... */
		if (listen(nsd->tcp[i].s, TCP_BACKLOG) == -1) {
			log_msg(LOG_ERR, "can't listen: %s", strerror(errno));
			return -1;
		}
	}

#ifdef HAVE_CHROOT
	/* Chroot */
	if (nsd->chrootdir) {
		int l = strlen(nsd->chrootdir);

		nsd->dbfile += l;
		nsd->pidfile += l;

		if (chroot(nsd->chrootdir)) {
			log_msg(LOG_ERR, "unable to chroot: %s", strerror(errno));
			return -1;
		}
	}
#endif

	/* Drop the permissions */
	if (setgid(nsd->gid) != 0 || setuid(nsd->uid) !=0) {
		log_msg(LOG_ERR, "unable to drop user priviledges: %s", strerror(errno));
		return -1;
	}

	/* Open the database... */
	if ((nsd->db = namedb_open(nsd->dbfile)) == NULL) {
		return -1;
	}

	initialize_dname_compression_tables(nsd);
	
#ifdef	BIND8_STATS
	/* Initialize times... */
	time(&nsd->st.boot);
	alarm(nsd->st.period);
#endif /* BIND8_STATS */

	return 0;
}

/*
 * Fork the required number of servers.
 */
static int
server_start_children(struct nsd *nsd, region_type* region, netio_type* netio)
{
	size_t i;

	/* Start all child servers initially.  */
	for (i = 0; i < nsd->child_count; ++i) {
		nsd->children[i].pid = 0;
	}

	return restart_child_servers(nsd, region, netio);
}

static void
close_all_sockets(struct nsd_socket sockets[], size_t n)
{
	size_t i;

	/* Close all the sockets... */
	for (i = 0; i < n; ++i) {
		if (sockets[i].s != -1) {
			close(sockets[i].s);
			sockets[i].s = -1;
		}
	}
}

/*
 * Close the sockets, shutdown the server and exit.
 * Does not return.
 *
 */
static void
server_shutdown(struct nsd *nsd)
{
	size_t i;

	close_all_sockets(nsd->udp, nsd->ifs);
	close_all_sockets(nsd->tcp, nsd->ifs);
	/* CHILD: close command channel to parent */
	if(nsd->this_child && nsd->this_child->parent_fd > 0)
	{
		close(nsd->this_child->parent_fd);
		nsd->this_child->parent_fd = -1;
	}
	/* SERVER: close command channels to children */
	if(!nsd->this_child)
	{
		for(i=0; i<nsd->child_count; ++i)
			if(nsd->children[i].child_fd > 0)
			{
				close(nsd->children[i].child_fd);
				nsd->children[i].child_fd = -1;
			}
	}

	exit(0);
}

/*
 * The main server simply waits for signals and child processes to
 * terminate.  Child processes are restarted as necessary.
 */
void
server_main(struct nsd *nsd)
{
        region_type *server_region = region_create(xalloc, free);
	netio_type *netio = netio_create(server_region);
	struct timespec timeout_spec;
	int fd;
	int status;
	pid_t child_pid;
	pid_t reload_pid = -1;
	pid_t old_pid;
	sig_atomic_t mode;
	
	assert(nsd->server_kind == NSD_SERVER_MAIN);

	if (server_start_children(nsd, server_region, netio) != 0) {
		send_children_command(nsd, NSD_QUIT);
		kill(nsd->pid, SIGTERM);
		exit(1);
	}
	assert(nsd->this_child == 0);

	while ((mode = nsd->mode) != NSD_SHUTDOWN) {
		switch (mode) {
		case NSD_RUN:
			/* timeout to collect processes. In case no sigchild happens. */
			timeout_spec.tv_sec = 60; 
			timeout_spec.tv_nsec = 0;
			timespec_add(&timeout_spec, netio_current_time(netio));

			/* listen on ports, timeout for collecting terminated children */
			if(netio_dispatch(netio, &timeout_spec, 0) == -1) {
				if (errno != EINTR) {
					log_msg(LOG_ERR, "netio_dispatch failed: %s", strerror(errno));
				}
			}

			/* see if any child processes terminated */
			while((child_pid = waitpid(0, &status, WNOHANG)) != -1 && child_pid != 0) {
				int is_child = delete_child_pid(nsd, child_pid);
				if (is_child) {
					log_msg(LOG_WARNING,
					       "server %d died unexpectedly with status %d, restarting",
					       (int) child_pid, status);
					restart_child_servers(nsd, server_region, netio);
				} else if (child_pid == reload_pid) {
					log_msg(LOG_WARNING,
					       "Reload process %d failed with status %d, continuing with old database",
					       (int) child_pid, status);
					reload_pid = -1;
				} else {
					log_msg(LOG_WARNING,
					       "Unknown child %d terminated with status %d",
					       (int) child_pid, status);
				}
			}
			if (child_pid == -1) {
				if (errno == EINTR) {
					continue;
				}
				log_msg(LOG_WARNING, "wait failed: %s", strerror(errno));
			}
			break;
		case NSD_RELOAD:
			nsd->mode = NSD_RUN;

			if (reload_pid != -1) {
				log_msg(LOG_WARNING, "Reload already in progress (pid = %d)",
				       (int) reload_pid);
				break;
			}

			log_msg(LOG_WARNING, "signal received, reloading...");

			reload_pid = fork();
			switch (reload_pid) {
			case -1:
				log_msg(LOG_ERR, "fork failed: %s", strerror(errno));
				break;
			case 0:
				/* CHILD */

				namedb_close(nsd->db);
				if ((nsd->db = namedb_open(nsd->dbfile)) == NULL) {
					log_msg(LOG_ERR, "unable to reload the database: %s", strerror(errno));
					exit(1);
				}

				initialize_dname_compression_tables(nsd);
	
#ifdef PLUGINS
				if (plugin_database_reloaded() != NSD_PLUGIN_CONTINUE) {
					log_msg(LOG_ERR, "plugin reload failed");
					exit(1);
				}
#endif /* PLUGINS */

				old_pid = nsd->pid;
				nsd->pid = getpid();
				reload_pid = -1;

#ifdef BIND8_STATS
				/* Restart dumping stats if required.  */
				time(&nsd->st.boot);
				alarm(nsd->st.period);
#endif

				if (server_start_children(nsd, server_region, netio) != 0) {
					send_children_command(nsd, NSD_QUIT);
					kill(nsd->pid, SIGTERM);
					exit(1);
				}

				/* Send SIGINT to terminate the parent quietly... */
				if (kill(old_pid, SIGINT) != 0) {
					log_msg(LOG_ERR, "cannot kill %d: %s",
						(int) old_pid, strerror(errno));
					exit(1);
				}

				/* Overwrite pid... */
				if (writepid(nsd) == -1) {
					log_msg(LOG_ERR, "cannot overwrite the pidfile %s: %s", nsd->pidfile, strerror(errno));
				}

				break;
			default:
				/* PARENT */
				break;
			}
			break;
		case NSD_QUIT: 
			/* silent shutdown during reload */
			send_children_command(nsd, NSD_QUIT);
			region_destroy(server_region);
			server_shutdown(nsd);
			/* ENOTREACH */
			break;
		case NSD_SHUTDOWN:
			send_children_command(nsd, NSD_QUIT);
			log_msg(LOG_WARNING, "signal received, shutting down...");
			break;
		case NSD_REAP_CHILDREN:
			/* continue; wait for child in run loop */
			nsd->mode = NSD_RUN;
			break;
		case NSD_STATS:
#ifdef BIND8_STATS
			{
				/* restart timer if =0, or not running */
				int old_timeout = alarm(nsd->st.period);
				if(old_timeout > 0 && old_timeout <= nsd->st.period)
					alarm(old_timeout);
			}
			send_children_command(nsd, NSD_STATS);
#endif
			nsd->mode = NSD_RUN;
			break;
		default:
			log_msg(LOG_WARNING, "NSD main server mode invalid: %d", nsd->mode);
			nsd->mode = NSD_RUN;
			break;
		}
	}

#ifdef PLUGINS
	plugin_finalize_all();
#endif /* PLUGINS */
	
	/* Truncate the pid file.  */
	if ((fd = open(nsd->pidfile, O_WRONLY | O_TRUNC, 0644)) == -1) {
		log_msg(LOG_ERR, "can not truncate the pid file %s: %s", nsd->pidfile, strerror(errno));
	}
	close(fd);

	/* Unlink it if possible... */
	unlink(nsd->pidfile);

	region_destroy(server_region);
	server_shutdown(nsd);
}

static query_state_type
process_query(struct nsd *nsd, struct query *query)
{
#ifdef PLUGINS
	query_state_type rc;
	nsd_plugin_callback_args_type callback_args;
	nsd_plugin_callback_result_type callback_result;
	
	callback_args.query = query;
	callback_args.data = NULL;
	callback_args.result_code = NSD_RC_OK;

	callback_result = query_received_callbacks(&callback_args, NULL);
	if (callback_result != NSD_PLUGIN_CONTINUE) {
		return handle_callback_result(callback_result, &callback_args);
	}

	rc = query_process(query, nsd);
	if (rc == QUERY_PROCESSED) {
		callback_args.data = NULL;
		callback_args.result_code = NSD_RC_OK;

		callback_result = query_processed_callbacks(
			&callback_args,
			query->domain ? query->domain->plugin_data : NULL);
		if (callback_result != NSD_PLUGIN_CONTINUE) {
			return handle_callback_result(callback_result, &callback_args);
		}
	}
	return rc;
#else /* !PLUGINS */
	return query_process(query, nsd);
#endif /* !PLUGINS */
}


/*
 * Serve DNS requests.
 */
void
server_child(struct nsd *nsd)
{
	size_t i;
	region_type *server_region = region_create(xalloc, free);
	netio_type *netio = netio_create(server_region);
	netio_handler_type *tcp_accept_handlers;
	sig_atomic_t mode;
	
	assert(nsd->server_kind != NSD_SERVER_MAIN);
	
	if (!(nsd->server_kind & NSD_SERVER_TCP)) {
		close_all_sockets(nsd->tcp, nsd->ifs);
	}
	if (!(nsd->server_kind & NSD_SERVER_UDP)) {
		close_all_sockets(nsd->udp, nsd->ifs);
	}
	
	if (nsd->this_child && nsd->this_child->parent_fd != -1) {
		netio_handler_type *handler;

		handler = (netio_handler_type *) region_alloc(
			server_region, sizeof(netio_handler_type));
		handler->fd = nsd->this_child->parent_fd;
		handler->timeout = NULL;
		handler->user_data = nsd;
		handler->event_types = NETIO_EVENT_READ;
		handler->event_handler = handle_parent_command;
		netio_add_handler(netio, handler);
	}

	if (nsd->server_kind & NSD_SERVER_UDP) {
		for (i = 0; i < nsd->ifs; ++i) {
			struct udp_handler_data *data;
			netio_handler_type *handler;

			data = (struct udp_handler_data *) region_alloc(
				server_region,
				sizeof(struct udp_handler_data));
			data->query = query_create(
				server_region, compressed_dname_offsets);
			data->nsd = nsd;
			data->socket = &nsd->udp[i];

			handler = (netio_handler_type *) region_alloc(
				server_region, sizeof(netio_handler_type));
			handler->fd = nsd->udp[i].s;
			handler->timeout = NULL;
			handler->user_data = data;
			handler->event_types = NETIO_EVENT_READ;
			handler->event_handler = handle_udp;
			netio_add_handler(netio, handler);
		}
	}

	/*
	 * Keep track of all the TCP accept handlers so we can enable
	 * and disable them based on the current number of active TCP
	 * connections.
	 */
	tcp_accept_handlers = (netio_handler_type *) region_alloc(
		server_region, nsd->ifs * sizeof(netio_handler_type));
	if (nsd->server_kind & NSD_SERVER_TCP) {
		for (i = 0; i < nsd->ifs; ++i) {
			struct tcp_accept_handler_data *data;
			netio_handler_type *handler;
			
			data = (struct tcp_accept_handler_data *) region_alloc(
				server_region,
				sizeof(struct tcp_accept_handler_data));
			data->nsd = nsd;
			data->socket = &nsd->tcp[i];
			data->tcp_accept_handler_count = nsd->ifs;
			data->tcp_accept_handlers = tcp_accept_handlers;
			
			handler = &tcp_accept_handlers[i];
			handler->fd = nsd->tcp[i].s;
			handler->timeout = NULL;
			handler->user_data = data;
			handler->event_types = NETIO_EVENT_READ;
			handler->event_handler = handle_tcp_accept;
			netio_add_handler(netio, handler);
		}
	}
	
	/* The main loop... */	
	while ((mode = nsd->mode) != NSD_QUIT) {

		/* Do we need to do the statistics... */
		if (mode == NSD_STATS) {
#ifdef BIND8_STATS
			/* Dump the statistics */
			bind8_stats(nsd);
#else /* BIND8_STATS */
			log_msg(LOG_NOTICE, "Statistics support not enabled at compile time.");
#endif /* BIND8_STATS */

			nsd->mode = NSD_RUN;
		}
		else if (mode == NSD_REAP_CHILDREN) {
			/* got signal, notify parent. parent reaps terminated children. */
			if (nsd->this_child->parent_fd > 0) {
				sig_atomic_t parent_notify = NSD_REAP_CHILDREN;
				if (write(nsd->this_child->parent_fd,
				    &parent_notify, 
				    sizeof(parent_notify)) == -1)
				{
					log_msg(LOG_ERR, "problems sending command from %d to parent: %s",
					(int) nsd->this_child->pid,
					strerror(errno));
				}
			} else while (waitpid(0, NULL, WNOHANG) > 0) /* no parent, so reap 'em */;
			nsd->mode = NSD_RUN;
		}

		/* Wait for a query... */
		if (netio_dispatch(netio, NULL, NULL) == -1) {
			if (errno != EINTR) {
				log_msg(LOG_ERR, "netio_dispatch failed: %s", strerror(errno));
				break;
			}
		}
	}

#ifdef	BIND8_STATS
	bind8_stats(nsd);
#endif /* BIND8_STATS */

	region_destroy(server_region);
	
	server_shutdown(nsd);
}


static void
handle_udp(netio_type *ATTR_UNUSED(netio),
	   netio_handler_type *handler,
	   netio_event_types_type event_types)
{
	struct udp_handler_data *data
		= (struct udp_handler_data *) handler->user_data;
	int received, sent;
	struct query *q = data->query;
	
	if (!(event_types & NETIO_EVENT_READ)) {
		return;
	}
	
	/* Account... */
	if (data->socket->addr->ai_family == AF_INET) {
		STATUP(data->nsd, qudp);
	} else if (data->socket->addr->ai_family == AF_INET6) {
		STATUP(data->nsd, qudp6);
	}

	/* Initialize the query... */
	query_reset(q, UDP_MAX_MESSAGE_LEN, 0);

	received = recvfrom(handler->fd,
			    buffer_begin(q->packet),
			    buffer_remaining(q->packet),
			    0,
			    (struct sockaddr *)&q->addr,
			    &q->addrlen);
	if (received == -1) {
		if (errno != EAGAIN && errno != EINTR) {
			log_msg(LOG_ERR, "recvfrom failed: %s", strerror(errno));
			STATUP(data->nsd, rxerr);
		}
	} else {
		buffer_skip(q->packet, received);
		buffer_flip(q->packet);

		/* Process and answer the query... */
		if (process_query(data->nsd, q) != QUERY_DISCARDED) {
			if (RCODE(q->packet) == RCODE_OK && !AA(q->packet))
				STATUP(data->nsd, nona);

			/* Add EDNS0 and TSIG info if necessary.  */
			query_add_optional(q, data->nsd);

			buffer_flip(q->packet);

			/* check for dst port 0 */
#ifdef INET6
			if (((struct sockaddr_storage *) &q->addr)->ss_family == AF_INET6) {
				if (((struct sockaddr_in6 *) &q->addr)->sin6_port == 0) {
					goto drop;
				}
			} else {
				if (((struct sockaddr_in *) &q->addr)->sin_port == 0) {
					goto drop;
				}
			}
#else
			if (((struct sockaddr_in *) &q->addr)->sin_port == 0) {
				goto drop;
			}
#endif /* INET6 */
			
			sent = sendto(handler->fd,
				      buffer_begin(q->packet),
				      buffer_remaining(q->packet),
				      0,
				      (struct sockaddr *) &q->addr,
				      q->addrlen);
			if (sent == -1) {
				log_msg(LOG_ERR, "sendto failed: %s", strerror(errno));
				STATUP(data->nsd, txerr);
			} else if ((size_t) sent != buffer_remaining(q->packet)) {
				log_msg(LOG_ERR, "sent %d in place of %d bytes", sent, (int) buffer_remaining(q->packet));
			} else {
#ifdef BIND8_STATS
				/* Account the rcode & TC... */
				STATUP2(data->nsd, rcode, RCODE(q->packet));
				if (TC(q->packet))
					STATUP(data->nsd, truncated);
#endif /* BIND8_STATS */
			}
		} else {
drop:
			STATUP(data->nsd, dropped);
		}
	}
}


static void
cleanup_tcp_handler(netio_type *netio, netio_handler_type *handler)
{
	struct tcp_handler_data *data
		= (struct tcp_handler_data *) handler->user_data;
	netio_remove_handler(netio, handler);
	close(handler->fd);

	/*
	 * Enable the TCP accept handlers when the current number of
	 * TCP connections is about to drop below the maximum number
	 * of TCP connections.
	 */
	if (data->nsd->current_tcp_count == data->nsd->maximum_tcp_count) {
		configure_handler_event_types(data->tcp_accept_handler_count,
					      data->tcp_accept_handlers,
					      NETIO_EVENT_READ);
	}
	--data->nsd->current_tcp_count;
	assert(data->nsd->current_tcp_count >= 0);

	region_destroy(data->region);
}

static void
handle_tcp_reading(netio_type *netio,
		   netio_handler_type *handler,
		   netio_event_types_type event_types)
{
	struct tcp_handler_data *data
		= (struct tcp_handler_data *) handler->user_data;
	ssize_t received;

	if (event_types & NETIO_EVENT_TIMEOUT) {
		/* Connection timed out.  */
		cleanup_tcp_handler(netio, handler);
		return;
	}

	assert(event_types & NETIO_EVENT_READ);

	if (data->bytes_transmitted == 0) {
		query_reset(data->query, TCP_MAX_MESSAGE_LEN, 1);
	}

	/*
	 * Check if we received the leading packet length bytes yet.
	 */
	if (data->bytes_transmitted < sizeof(uint16_t)) {
		received = read(handler->fd,
				(char *) &data->query->tcplen
				+ data->bytes_transmitted,
				sizeof(uint16_t) - data->bytes_transmitted);
		if (received == -1) {
			if (errno == EAGAIN || errno == EINTR) {
				/*
				 * Read would block, wait until more
				 * data is available.
				 */
				return;
			} else {
				log_msg(LOG_ERR, "failed reading from tcp: %s", strerror(errno));
				cleanup_tcp_handler(netio, handler);
				return;
			}
		} else if (received == 0) {
			/* EOF */
			cleanup_tcp_handler(netio, handler);
			return;
		}

		data->bytes_transmitted += received;
		if (data->bytes_transmitted < sizeof(uint16_t)) {
			/*
			 * Not done with the tcplen yet, wait for more
			 * data to become available.
			 */
			return;
		}

		assert(data->bytes_transmitted == sizeof(uint16_t));

		data->query->tcplen = ntohs(data->query->tcplen);
		
		/*
		 * Minimum query size is:
		 *
		 *     Size of the header (12)
		 *   + Root domain name   (1)
		 *   + Query class        (2)
		 *   + Query type         (2)
		 */
		if (data->query->tcplen < QHEADERSZ + 1 + sizeof(uint16_t) + sizeof(uint16_t)) {
			log_msg(LOG_WARNING, "dropping bogus tcp connection");
			cleanup_tcp_handler(netio, handler);
			return;
		}

		if (data->query->tcplen > data->query->maxlen) {
			log_msg(LOG_ERR, "insufficient tcp buffer, dropping connection");
			cleanup_tcp_handler(netio, handler);
			return;
		}

		buffer_set_limit(data->query->packet, data->query->tcplen);
	}

	assert(buffer_remaining(data->query->packet) > 0);

	/* Read the (remaining) query data.  */
	received = read(handler->fd,
			buffer_current(data->query->packet),
			buffer_remaining(data->query->packet));
	if (received == -1) {
		if (errno == EAGAIN || errno == EINTR) {
			/*
			 * Read would block, wait until more data is
			 * available.
			 */
			return;
		} else {
			log_msg(LOG_ERR, "failed reading from tcp: %s", strerror(errno));
			cleanup_tcp_handler(netio, handler);
			return;
		}
	} else if (received == 0) {
		/* EOF */
		cleanup_tcp_handler(netio, handler);
		return;
	}

	data->bytes_transmitted += received;
	buffer_skip(data->query->packet, received);
	if (buffer_remaining(data->query->packet) > 0) {
		/*
		 * Message not yet complete, wait for more data to
		 * become available.
		 */
		return;
	}

	assert(buffer_position(data->query->packet) == data->query->tcplen);

	/* Account... */
	if (data->query->addr.ss_family == AF_INET) {
		STATUP(data->nsd, ctcp);
	} else if (data->query->addr.ss_family == AF_INET6) {
		STATUP(data->nsd, ctcp6);
	}

	/* We have a complete query, process it.  */

	buffer_flip(data->query->packet);
	data->query_state = process_query(data->nsd, data->query);
	if (data->query_state == QUERY_DISCARDED) {
		/* Drop the packet and the entire connection... */
		STATUP(data->nsd, dropped);
		cleanup_tcp_handler(netio, handler);
		return;
	}

	if (RCODE(data->query->packet) == RCODE_OK
	    && !AA(data->query->packet))
	{
		STATUP(data->nsd, nona);
	}
		
	query_add_optional(data->query, data->nsd);

	/* Switch to the tcp write handler.  */
	buffer_flip(data->query->packet);
	data->query->tcplen = buffer_remaining(data->query->packet);
	data->bytes_transmitted = 0;
	
	handler->timeout->tv_sec = TCP_TIMEOUT;
	handler->timeout->tv_nsec = 0L;
	timespec_add(handler->timeout, netio_current_time(netio));
	
	handler->event_types = NETIO_EVENT_WRITE | NETIO_EVENT_TIMEOUT;
	handler->event_handler = handle_tcp_writing;
}

static void
handle_tcp_writing(netio_type *netio,
		   netio_handler_type *handler,
		   netio_event_types_type event_types)
{
	struct tcp_handler_data *data
		= (struct tcp_handler_data *) handler->user_data;
	ssize_t sent;
	struct query *q = data->query;

	if (event_types & NETIO_EVENT_TIMEOUT) {
		/* Connection timed out.  */
		cleanup_tcp_handler(netio, handler);
		return;
	}

	assert(event_types & NETIO_EVENT_WRITE);

	if (data->bytes_transmitted < sizeof(q->tcplen)) {
		/* Writing the response packet length.  */
		uint16_t n_tcplen = htons(q->tcplen);
		sent = write(handler->fd,
			     (const char *) &n_tcplen + data->bytes_transmitted,
			     sizeof(n_tcplen) - data->bytes_transmitted);
		if (sent == -1) {
			if (errno == EAGAIN || errno == EINTR) {
				/*
				 * Write would block, wait until
				 * socket becomes writable again.
				 */
				return;
			} else {
				log_msg(LOG_ERR, "failed writing to tcp: %s", strerror(errno));
				cleanup_tcp_handler(netio, handler);
				return;
			}
		}

		data->bytes_transmitted += sent;
		if (data->bytes_transmitted < sizeof(q->tcplen)) {
			/*
			 * Writing not complete, wait until socket
			 * becomes writable again.
			 */
			return;
		}

		assert(data->bytes_transmitted == sizeof(q->tcplen));
	}

	assert(data->bytes_transmitted < q->tcplen + sizeof(q->tcplen));

	sent = write(handler->fd,
		     buffer_current(q->packet),
		     buffer_remaining(q->packet));
	if (sent == -1) {
		if (errno == EAGAIN || errno == EINTR) {
			/*
			 * Write would block, wait until
			 * socket becomes writable again.
			 */
			return;
		} else {
			log_msg(LOG_ERR, "failed writing to tcp: %s", strerror(errno));
			cleanup_tcp_handler(netio, handler);
			return;
		}
	}

	buffer_skip(q->packet, sent);
	data->bytes_transmitted += sent;
	if (data->bytes_transmitted < q->tcplen + sizeof(q->tcplen)) {
		/*
		 * Still more data to write when socket becomes
		 * writable again.
		 */
		return;
	}

	assert(data->bytes_transmitted == q->tcplen + sizeof(q->tcplen));

	if (data->query_state == QUERY_IN_AXFR) {
		/* Continue processing AXFR and writing back results.  */
		buffer_clear(q->packet);
		data->query_state = query_axfr(data->nsd, q);
		if (data->query_state != QUERY_PROCESSED) {
			query_add_optional(data->query, data->nsd);
			
			/* Reset data. */
			buffer_flip(q->packet);
			q->tcplen = buffer_remaining(q->packet);
			data->bytes_transmitted = 0;
			/* Reset timeout.  */
			handler->timeout->tv_sec = TCP_TIMEOUT;
			handler->timeout->tv_nsec = 0;
			timespec_add(handler->timeout, netio_current_time(netio));

			/*
			 * Write data if/when the socket is writable
			 * again.
			 */
			return;
		}
	}

	/*
	 * Done sending, wait for the next request to arrive on the
	 * TCP socket by installing the TCP read handler.
	 */
	data->bytes_transmitted = 0;
	
	handler->timeout->tv_sec = TCP_TIMEOUT;
	handler->timeout->tv_nsec = 0;
	timespec_add(handler->timeout, netio_current_time(netio));

	handler->event_types = NETIO_EVENT_READ | NETIO_EVENT_TIMEOUT;
	handler->event_handler = handle_tcp_reading;
}


/*
 * Handle an incoming TCP connection.  The connection is accepted and
 * a new TCP reader event handler is added to NETIO.  The TCP handler
 * is responsible for cleanup when the connection is closed.
 */
static void
handle_tcp_accept(netio_type *netio,
		  netio_handler_type *handler,
		  netio_event_types_type event_types)
{
	struct tcp_accept_handler_data *data
		= (struct tcp_accept_handler_data *) handler->user_data;
	int s;
	struct tcp_handler_data *tcp_data;
	region_type *tcp_region;
	netio_handler_type *tcp_handler;
#ifdef INET6
	struct sockaddr_storage addr;
#else
	struct sockaddr_in addr;
#endif
	socklen_t addrlen;
	
	if (!(event_types & NETIO_EVENT_READ)) {
		return;
	}

	if (data->nsd->current_tcp_count >= data->nsd->maximum_tcp_count) {
		return;
	}
	
	/* Accept it... */
	addrlen = sizeof(addr);
	s = accept(handler->fd, (struct sockaddr *) &addr, &addrlen);
	if (s == -1) {
		if (errno != EINTR) {
			log_msg(LOG_ERR, "accept failed: %s", strerror(errno));
		}
		return;
	}

	if (fcntl(s, F_SETFL, O_NONBLOCK) == -1) {
		log_msg(LOG_ERR, "fcntl failed: %s", strerror(errno));
		close(s);
		return;
	}
	
	/*
	 * This region is deallocated when the TCP connection is
	 * closed by the TCP handler.
	 */
	tcp_region = region_create(xalloc, free);
	tcp_data = (struct tcp_handler_data *) region_alloc(
		tcp_region, sizeof(struct tcp_handler_data));
	tcp_data->region = tcp_region;
	tcp_data->query = query_create(tcp_region, compressed_dname_offsets);
	tcp_data->nsd = data->nsd;
	
	tcp_data->tcp_accept_handler_count = data->tcp_accept_handler_count;
	tcp_data->tcp_accept_handlers = data->tcp_accept_handlers;

	tcp_data->query_state = QUERY_PROCESSED;
	tcp_data->bytes_transmitted = 0;
	memcpy(&tcp_data->query->addr, &addr, addrlen);
	tcp_data->query->addrlen = addrlen;
	
	tcp_handler = (netio_handler_type *) region_alloc(
		tcp_region, sizeof(netio_handler_type));
	tcp_handler->fd = s;
	tcp_handler->timeout = (struct timespec *) region_alloc(
		tcp_region, sizeof(struct timespec));
	tcp_handler->timeout->tv_sec = TCP_TIMEOUT;
	tcp_handler->timeout->tv_nsec = 0L;
	timespec_add(tcp_handler->timeout, netio_current_time(netio));

	tcp_handler->user_data = tcp_data;
	tcp_handler->event_types = NETIO_EVENT_READ | NETIO_EVENT_TIMEOUT;
	tcp_handler->event_handler = handle_tcp_reading;

	netio_add_handler(netio, tcp_handler);

	/*
	 * Keep track of the total number of TCP handlers installed so
	 * we can stop accepting connections when the maximum number
	 * of simultaneous TCP connections is reached.
	 */
	++data->nsd->current_tcp_count;
	if (data->nsd->current_tcp_count == data->nsd->maximum_tcp_count) {
		configure_handler_event_types(data->tcp_accept_handler_count,
					      data->tcp_accept_handlers,
					      NETIO_EVENT_NONE);
	}
}

static void
handle_parent_command(netio_type *ATTR_UNUSED(netio),
		      netio_handler_type *handler,
		      netio_event_types_type event_types)
{
	sig_atomic_t mode;
	int len;
	nsd_type *nsd = (nsd_type *) handler->user_data;
	if (!(event_types & NETIO_EVENT_READ)) {
		return;
	}

	if ((len = read(handler->fd, &mode, sizeof(mode))) == -1) {
		log_msg(LOG_ERR, "handle_parent_command: read: %s",
			strerror(errno));
		return;
	}
	if (len == 0)
	{
		/* parent closed the connection. Quit */
		nsd->mode = NSD_QUIT;
		return;
	}

	switch (mode) {
	case NSD_STATS: 
	case NSD_QUIT:
		nsd->mode = mode;
		break;
	default:
		log_msg(LOG_ERR, "handle_parent_command: bad mode %d",
			(int) mode);
		break;
	}
}

static void 
send_children_command(struct nsd* nsd, sig_atomic_t command)
{
	size_t i;
	assert(nsd->server_kind == NSD_SERVER_MAIN && nsd->this_child == 0);
	for (i = 0; i < nsd->child_count; ++i) {
		if (nsd->children[i].pid > 0 && nsd->children[i].child_fd > 0) {
			if (write(nsd->children[i].child_fd,
				&command,
				sizeof(command)) == -1)
			{
				log_msg(LOG_ERR, "problems sending command %d to server %d: %s",
					(int) command,
					(int) nsd->children[i].pid,
					strerror(errno));
			}
		}
	}
}

static void
handle_child_command(netio_type *ATTR_UNUSED(netio),
		      netio_handler_type *handler,
		      netio_event_types_type event_types)
{
	sig_atomic_t mode;
	int len;
	nsd_type *nsd = (nsd_type *) handler->user_data;
	if (!(event_types & NETIO_EVENT_READ)) {
		return;
	}

	if ((len = read(handler->fd, &mode, sizeof(mode))) == -1) {
		log_msg(LOG_ERR, "handle_child_command: read: %s",
			strerror(errno));
		return;
	}
	if (len == 0)
	{
		size_t i;
		if(handler->fd > 0) close(handler->fd);
		for(i=0; i<nsd->child_count; ++i)
			if(nsd->children[i].child_fd == handler->fd) {
				nsd->children[i].child_fd = -1;
				log_msg(LOG_ERR, "server %d closed cmd channel",
					(int) nsd->children[i].pid);
			}
		handler->fd = -1;
		return;
	}

	switch (mode) {
	case NSD_STATS: 
	case NSD_QUIT:
	case NSD_REAP_CHILDREN:
		nsd->mode = mode;
		break;
	default:
		log_msg(LOG_ERR, "handle_child_command: bad mode %d",
			(int) mode);
		break;
	}
}

static void
configure_handler_event_types(size_t count,
			      netio_handler_type *handlers,
			      netio_event_types_type event_types)
{
	size_t i;

	assert(handlers);
	
	for (i = 0; i < count; ++i) {
		handlers[i].event_types = event_types;
	}
}
