/*
 * Health-checks functions.
 *
 * Copyright 2000-2007 Willy Tarreau <w@1wt.eu>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version
 * 2 of the License, or (at your option) any later version.
 *
 */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include <common/compat.h>
#include <common/config.h>
#include <common/mini-clist.h>
#include <common/standard.h>
#include <common/time.h>

#include <types/global.h>
#include <types/polling.h>
#include <types/proxy.h>
#include <types/session.h>

#include <proto/backend.h>
#include <proto/fd.h>
#include <proto/log.h>
#include <proto/queue.h>
#include <proto/proto_http.h>
#include <proto/proxy.h>
#include <proto/server.h>
#include <proto/task.h>

#ifdef CONFIG_HAP_CTTPROXY
#include <import/ip_tproxy.h>
#endif


/* Sets server <s> down, notifies by all available means, recounts the
 * remaining servers on the proxy and transfers queued sessions whenever
 * possible to other servers.
 */
static void set_server_down(struct server *s)
{
	struct pendconn *pc, *pc_bck, *pc_end;
	struct session *sess;
	int xferred;

	s->state &= ~SRV_RUNNING;

	if (s->health == s->rise) {
		recount_servers(s->proxy);
		recalc_server_map(s->proxy);

		/* we might have sessions queued on this server and waiting for
		 * a connection. Those which are redispatchable will be queued
		 * to another server or to the proxy itself.
		 */
		xferred = 0;
		FOREACH_ITEM_SAFE(pc, pc_bck, &s->pendconns, pc_end, struct pendconn *, list) {
			sess = pc->sess;
			if ((sess->be->options & PR_O_REDISP)) {
				/* The REDISP option was specified. We will ignore
				 * cookie and force to balance or use the dispatcher.
				 */
				sess->flags &= ~(SN_DIRECT | SN_ASSIGNED | SN_ADDR_SET);
				sess->srv = NULL; /* it's left to the dispatcher to choose a server */
				http_flush_cookie_flags(&sess->txn);
				pendconn_free(pc);
				task_wakeup(sess->task);
				xferred++;
			}
		}

		sprintf(trash, "%sServer %s/%s is DOWN. %d active and %d backup servers left.%s"
			" %d sessions active, %d requeued, %d remaining in queue.\n",
			s->state & SRV_BACKUP ? "Backup " : "",
			s->proxy->id, s->id, s->proxy->srv_act, s->proxy->srv_bck,
			(s->proxy->srv_bck && !s->proxy->srv_act) ? " Running on backup." : "",
			s->cur_sess, xferred, s->nbpend);

		Warning("%s", trash);
		send_log(s->proxy, LOG_ALERT, "%s", trash);
	
		if (s->proxy->srv_bck == 0 && s->proxy->srv_act == 0) {
			Alert("%s '%s' has no server available !\n", proxy_type_str(s->proxy), s->proxy->id);
			send_log(s->proxy, LOG_EMERG, "%s %s has no server available !\n", proxy_type_str(s->proxy), s->proxy->id);
		}
		s->down_trans++;
	}
	s->health = 0; /* failure */
}


/*
 * This function is used only for server health-checks. It handles
 * the connection acknowledgement. If the proxy requires HTTP health-checks,
 * it sends the request. In other cases, it returns 1 in s->result if the
 * socket is OK, or -1 if an error occured.
 * The function itself returns 0 if it needs some polling before being called
 * again, otherwise 1.
 */
static int event_srv_chk_w(int fd)
{
	__label__ out_wakeup, out_nowake, out_poll, out_error;
	struct task *t = fdtab[fd].owner;
	struct server *s = t->context;

	if (unlikely(fdtab[fd].state == FD_STERROR || (fdtab[fd].ev & FD_POLL_ERR)))
		goto out_error;

	/* here, we know that the connection is established */

	if (s->result != -1) {
		/* we don't want to mark 'UP' a server on which we detected an error earlier */
		if ((s->proxy->options & PR_O_HTTP_CHK) ||
		    (s->proxy->options & PR_O_SSL3_CHK) ||
		    (s->proxy->options & PR_O_SMTP_CHK)) {
			int ret;
			/* we want to check if this host replies to HTTP or SSLv3 requests
			 * so we'll send the request, and won't wake the checker up now.
			 */

			if (s->proxy->options & PR_O_SSL3_CHK) {
				/* SSL requires that we put Unix time in the request */
				int gmt_time = htonl(now.tv_sec);
				memcpy(s->proxy->check_req + 11, &gmt_time, 4);
			}

#ifndef MSG_NOSIGNAL
			ret = send(fd, s->proxy->check_req, s->proxy->check_len, MSG_DONTWAIT);
#else
			ret = send(fd, s->proxy->check_req, s->proxy->check_len, MSG_DONTWAIT | MSG_NOSIGNAL);
#endif
			if (ret == s->proxy->check_len) {
				EV_FD_SET(fd, DIR_RD);   /* prepare for reading reply */
				goto out_nowake;
			}
			else if (ret == 0 || errno == EAGAIN)
				goto out_poll;
			else
				goto out_error;
		}
		else {
			/* We have no data to send to check the connection, and
			 * getsockopt() will not inform us whether the connection
			 * is still pending. So we'll reuse connect() to check the
			 * state of the socket. This has the advantage of givig us
			 * the following info :
			 *  - error
			 *  - connecting (EALREADY, EINPROGRESS)
			 *  - connected (EISCONN, 0)
			 */

			struct sockaddr_in sa;

			sa = (s->check_addr.sin_addr.s_addr) ? s->check_addr : s->addr;
			sa.sin_port = htons(s->check_port);

			if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) == 0)
				errno = 0;

			if (errno == EALREADY || errno == EINPROGRESS)
				goto out_poll;

			if (errno && errno != EISCONN)
				goto out_error;

			/* good TCP connection is enough */
			s->result = 1;
			goto out_wakeup;
		}
	}
 out_wakeup:
	task_wakeup(t);
 out_nowake:
	EV_FD_CLR(fd, DIR_WR);   /* nothing more to write */
	fdtab[fd].ev &= ~FD_POLL_WR;
	return 1;
 out_poll:
	/* The connection is still pending. We'll have to poll it
	 * before attempting to go further. */
	fdtab[fd].ev &= ~FD_POLL_WR;
	return 0;
 out_error:
	s->result = -1;
	fdtab[fd].state = FD_STERROR;
	goto out_wakeup;
}


/*
 * This function is used only for server health-checks. It handles the server's
 * reply to an HTTP request or SSL HELLO. It returns 1 in s->result if the
 * server replies HTTP 2xx or 3xx (valid responses), or if it returns at least
 * 5 bytes in response to SSL HELLO. The principle is that this is enough to
 * distinguish between an SSL server and a pure TCP relay. All other cases will
 * return -1. The function returns 0 if it needs to be called again after some
 * polling, otherwise non-zero..
 */
static int event_srv_chk_r(int fd)
{
	__label__ out_wakeup;
	char reply[64];
	int len, result;
	struct task *t = fdtab[fd].owner;
	struct server *s = t->context;
	int skerr;
	socklen_t lskerr = sizeof(skerr);

	result = len = -1;

	if (unlikely(fdtab[fd].state == FD_STERROR ||
		     (fdtab[fd].ev & FD_POLL_ERR) ||
		     (getsockopt(fd, SOL_SOCKET, SO_ERROR, &skerr, &lskerr) == -1) ||
		     (skerr != 0))) {
		/* in case of TCP only, this tells us if the connection failed */
		s->result = -1;
		fdtab[fd].state = FD_STERROR;
		goto out_wakeup;
	}

#ifndef MSG_NOSIGNAL
	len = recv(fd, reply, sizeof(reply), 0);
#else
	/* Warning! Linux returns EAGAIN on SO_ERROR if data are still available
	 * but the connection was closed on the remote end. Fortunately, recv still
	 * works correctly and we don't need to do the getsockopt() on linux.
	 */
	len = recv(fd, reply, sizeof(reply), MSG_NOSIGNAL);
#endif
	if (unlikely(len < 0 && errno == EAGAIN)) {
		/* we want some polling to happen first */
		fdtab[fd].ev &= ~FD_POLL_RD;
		return 0;
	}

	if ((s->proxy->options & PR_O_HTTP_CHK) && (len >= sizeof("HTTP/1.0 000")) &&
	     (memcmp(reply, "HTTP/1.", 7) == 0) && (reply[9] == '2' || reply[9] == '3')) {
		/* HTTP/1.X 2xx or 3xx */
		result = 1;
	}
	else if ((s->proxy->options & PR_O_SSL3_CHK) && (len >= 5) &&
		 (reply[0] == 0x15 || reply[0] == 0x16)) {
		/* SSLv3 alert or handshake */
		result = 1;
	}
	else if ((s->proxy->options & PR_O_SMTP_CHK) && (len >= 3) &&
		   (reply[0] == '2')) /* 2xx (should be 250) */ {
		result = 1;
	}

	if (result == -1)
		fdtab[fd].state = FD_STERROR;

	if (s->result != -1)
		s->result = result;

 out_wakeup:
	EV_FD_CLR(fd, DIR_RD);
	task_wakeup(t);
	fdtab[fd].ev &= ~FD_POLL_RD;
	return 1;
}

/*
 * manages a server health-check. Returns
 * the time the task accepts to wait, or TIME_ETERNITY for infinity.
 */
void process_chk(struct task *t, struct timeval *next)
{
	__label__ new_chk, out;
	struct server *s = t->context;
	struct sockaddr_in sa;
	int fd;

	//fprintf(stderr, "process_chk: task=%p\n", t);

 new_chk:
	fd = s->curfd;
	if (fd < 0) {   /* no check currently running */
		//fprintf(stderr, "process_chk: 2\n");
		if (!tv_isle(&t->expire, &now)) { /* not good time yet */
			task_queue(t);	/* restore t to its place in the task list */
			*next = t->expire;
			goto out;
		}

		/* we don't send any health-checks when the proxy is stopped or when
		 * the server should not be checked.
		 */
		if (!(s->state & SRV_CHECKED) || s->proxy->state == PR_STSTOPPED) {
			while (tv_isle(&t->expire, &now))
				tv_ms_add(&t->expire, &t->expire, s->inter);
			task_queue(t);	/* restore t to its place in the task list */
			*next = t->expire;
			goto out;
		}

		/* we'll initiate a new check */
		s->result = 0; /* no result yet */
		if ((fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP)) != -1) {
			if ((fd < global.maxsock) &&
			    (fcntl(fd, F_SETFL, O_NONBLOCK) != -1) &&
			    (setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, (char *) &one, sizeof(one)) != -1)) {
				//fprintf(stderr, "process_chk: 3\n");

				
				if (s->check_addr.sin_addr.s_addr)
					/* we'll connect to the check addr specified on the server */
					sa = s->check_addr;
				else
					/* we'll connect to the addr on the server */
					sa = s->addr;

				/* we'll connect to the check port on the server */
				sa.sin_port = htons(s->check_port);

				/* allow specific binding :
				 * - server-specific at first
				 * - proxy-specific next
				 */
				if (s->state & SRV_BIND_SRC) {
					setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (char *) &one, sizeof(one));
					if (bind(fd, (struct sockaddr *)&s->source_addr, sizeof(s->source_addr)) == -1) {
						Alert("Cannot bind to source address before connect() for server %s/%s. Aborting.\n",
						      s->proxy->id, s->id);
						s->result = -1;
					}
#ifdef CONFIG_HAP_CTTPROXY
					if ((s->state & SRV_TPROXY_MASK) == SRV_TPROXY_ADDR) {
						struct in_tproxy itp1, itp2;
						memset(&itp1, 0, sizeof(itp1));
						
						itp1.op = TPROXY_ASSIGN;
						itp1.v.addr.faddr = s->tproxy_addr.sin_addr;
						itp1.v.addr.fport = s->tproxy_addr.sin_port;

						/* set connect flag on socket */
						itp2.op = TPROXY_FLAGS;
						itp2.v.flags = ITP_CONNECT | ITP_ONCE;

						if (setsockopt(fd, SOL_IP, IP_TPROXY, &itp1, sizeof(itp1)) == -1 ||
						    setsockopt(fd, SOL_IP, IP_TPROXY, &itp2, sizeof(itp2)) == -1) {
							Alert("Cannot bind to tproxy source address before connect() for server %s/%s. Aborting.\n",
							      s->proxy->id, s->id);
							s->result = -1;
						}
					}
#endif
				}
				else if (s->proxy->options & PR_O_BIND_SRC) {
					setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (char *) &one, sizeof(one));
					if (bind(fd, (struct sockaddr *)&s->proxy->source_addr, sizeof(s->proxy->source_addr)) == -1) {
						Alert("Cannot bind to source address before connect() for %s '%s'. Aborting.\n",
						      proxy_type_str(s->proxy), s->proxy->id);
						s->result = -1;
					}
#ifdef CONFIG_HAP_CTTPROXY
					if ((s->proxy->options & PR_O_TPXY_MASK) == PR_O_TPXY_ADDR) {
						struct in_tproxy itp1, itp2;
						memset(&itp1, 0, sizeof(itp1));
						
						itp1.op = TPROXY_ASSIGN;
						itp1.v.addr.faddr = s->tproxy_addr.sin_addr;
						itp1.v.addr.fport = s->tproxy_addr.sin_port;
						
						/* set connect flag on socket */
						itp2.op = TPROXY_FLAGS;
						itp2.v.flags = ITP_CONNECT | ITP_ONCE;
						
						if (setsockopt(fd, SOL_IP, IP_TPROXY, &itp1, sizeof(itp1)) == -1 ||
						    setsockopt(fd, SOL_IP, IP_TPROXY, &itp2, sizeof(itp2)) == -1) {
							Alert("Cannot bind to tproxy source address before connect() for %s '%s'. Aborting.\n",
							      proxy_type_str(s->proxy), s->proxy->id);
							s->result = -1;
						}
					}
#endif
				}

				if (!s->result) {
					if ((connect(fd, (struct sockaddr *)&sa, sizeof(sa)) != -1) || (errno == EINPROGRESS)) {
						/* OK, connection in progress or established */
			
						//fprintf(stderr, "process_chk: 4\n");
			
						s->curfd = fd; /* that's how we know a test is in progress ;-) */
						fd_insert(fd);
						fdtab[fd].owner = t;
						fdtab[fd].cb[DIR_RD].f = &event_srv_chk_r;
						fdtab[fd].cb[DIR_RD].b = NULL;
						fdtab[fd].cb[DIR_WR].f = &event_srv_chk_w;
						fdtab[fd].cb[DIR_WR].b = NULL;
						fdtab[fd].state = FD_STCONN; /* connection in progress */
						fdtab[fd].ev = 0;
						EV_FD_SET(fd, DIR_WR);  /* for connect status */
#ifdef DEBUG_FULL
						assert (!EV_FD_ISSET(fd, DIR_RD));
#endif
						/* FIXME: we allow up to <inter> for a connection to establish, but we should use another parameter */
						tv_ms_add(&t->expire, &now, s->inter);
						task_queue(t);	/* restore t to its place in the task list */
						*next = t->expire;
					}
					else if (errno != EALREADY && errno != EISCONN && errno != EAGAIN) {
						s->result = -1;    /* a real error */
					}
				}
			}
			close(fd); /* socket creation error */
		}

		if (!s->result) { /* nothing done */
			//fprintf(stderr, "process_chk: 6\n");
			while (tv_isle(&t->expire, &now))
				tv_ms_add(&t->expire, &t->expire, s->inter);
			goto new_chk; /* may be we should initialize a new check */
		}

		/* here, we have seen a failure */
		if (s->health > s->rise) {
			s->health--; /* still good */
			s->failed_checks++;
		}
		else
			set_server_down(s);

		//fprintf(stderr, "process_chk: 7\n");
		/* FIXME: we allow up to <inter> for a connection to establish, but we should use another parameter */
		while (tv_isle(&t->expire, &now))
			tv_ms_add(&t->expire, &t->expire, s->inter);
		goto new_chk;
	}
	else {
		//fprintf(stderr, "process_chk: 8\n");
		/* there was a test running */
		if (s->result > 0) { /* good server detected */
			//fprintf(stderr, "process_chk: 9\n");
			s->health++; /* was bad, stays for a while */
			if (s->health >= s->rise) {
				s->state |= SRV_RUNNING;

				if (s->health == s->rise) {
					int xferred;

					recount_servers(s->proxy);
					recalc_server_map(s->proxy);

					/* check if we can handle some connections queued at the proxy. We
					 * will take as many as we can handle.
					 */
					for (xferred = 0; !s->maxconn || xferred < srv_dynamic_maxconn(s); xferred++) {
						struct session *sess;
						struct pendconn *p;

						p = pendconn_from_px(s->proxy);
						if (!p)
							break;
						p->sess->srv = s;
						sess = p->sess;
						pendconn_free(p);
						task_wakeup(sess->task);
					}

					sprintf(trash,
						"%sServer %s/%s is UP. %d active and %d backup servers online.%s"
						" %d sessions requeued, %d total in queue.\n",
						s->state & SRV_BACKUP ? "Backup " : "",
						s->proxy->id, s->id, s->proxy->srv_act, s->proxy->srv_bck,
						(s->proxy->srv_bck && !s->proxy->srv_act) ? " Running on backup." : "",
						xferred, s->nbpend);

					Warning("%s", trash);
					send_log(s->proxy, LOG_NOTICE, "%s", trash);
				}

				s->health = s->rise + s->fall - 1; /* OK now */
			}
			s->curfd = -1; /* no check running anymore */
			fd_delete(fd);
			while (tv_isle(&t->expire, &now))
				tv_ms_add(&t->expire, &t->expire, s->inter);
			goto new_chk;
		}
		else if (s->result < 0 || tv_isle(&t->expire, &now)) {
			//fprintf(stderr, "process_chk: 10\n");
			/* failure or timeout detected */
			if (s->health > s->rise) {
				s->health--; /* still good */
				s->failed_checks++;
			}
			else
				set_server_down(s);
			s->curfd = -1;
			fd_delete(fd);
			while (tv_isle(&t->expire, &now))
				tv_ms_add(&t->expire, &t->expire, s->inter);
			goto new_chk;
		}
		/* if result is 0 and there's no timeout, we have to wait again */
	}
	//fprintf(stderr, "process_chk: 11\n");
	s->result = 0;
	task_queue(t);	/* restore t to its place in the task list */
	*next = t->expire;
 out:
	return;
}


/*
 * Local variables:
 *  c-indent-level: 8
 *  c-basic-offset: 8
 * End:
 */
