#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <string.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <poll.h>
#include <netdb.h>
#include <signal.h>
#include <time.h>
#include <dlfcn.h>
#include <sys/epoll.h>

#include <glib.h>

#include "utp.h"

typedef struct context_data {
    int udpfd;
    int blocking;
    GQueue *incoming_utp_sockets;
} context_data;

typedef struct socket_data {
    int connected;
    GByteArray *readbuf;
    int writeable;
} socket_data;

/* Map of TCP sockets to UTP contexts */
GHashTable *tcpToContext = NULL;
GHashTable *tcpToSocket = NULL;

uint64 utp_log_cb(utp_callback_arguments *a);
uint64 utp_sendto_cb(utp_callback_arguments *a);
uint64 utp_on_state_change_cb(utp_callback_arguments *a);
uint64 utp_on_error_cb(utp_callback_arguments *a);
uint64 utp_on_accept_cb(utp_callback_arguments *a);
uint64 utp_on_read_cb(utp_callback_arguments *a);

static void init() __attribute__((constructor));

void init() {
    fprintf(stderr, "DEBUG: initializing UTP preload\n");

    tcpToContext = g_hash_table_new(g_direct_hash, g_direct_equal);
    tcpToSocket = g_hash_table_new(g_direct_hash, g_direct_equal);
}

utp_context *create_utp_context(int udpfd) {
    utp_context *ctx = utp_init(2);
    utp_set_callback(ctx, UTP_LOG,				&utp_log_cb);
    utp_set_callback(ctx, UTP_SENDTO,			&utp_sendto_cb);
    utp_set_callback(ctx, UTP_ON_ERROR,			&utp_on_error_cb);
    utp_set_callback(ctx, UTP_ON_STATE_CHANGE,	&utp_on_state_change_cb);
    utp_set_callback(ctx, UTP_ON_READ,			&utp_on_read_cb);
    /*utp_set_callback(ctx, UTP_ON_FIREWALL,		&utp);*/
    utp_set_callback(ctx, UTP_ON_ACCEPT,		&utp_on_accept_cb);

    utp_context_set_option(ctx, UTP_LOG_NORMAL, 1);
    utp_context_set_option(ctx, UTP_LOG_MTU,    1);
    utp_context_set_option(ctx, UTP_LOG_DEBUG,  1);

    context_data *data = (context_data *)malloc(sizeof(*data));
    memset(data, 0, sizeof(*data));
    data->udpfd = udpfd;
    data->incoming_utp_sockets = g_queue_new();

    utp_context_set_userdata(ctx, data);

    return ctx;
}

socket_data *create_utp_socket_data(utp_socket *s) {
    socket_data *data = (socket_data *)malloc(sizeof(*data));
    memset(data, 0, sizeof(*data));
    data->readbuf = g_byte_array_new();
    data->writeable = FALSE;
    data->connected = FALSE;
    utp_set_userdata(s, data);
    return data;
}

typedef int (*orig_socket_func)(int domain, int type, int protocol);
typedef int (*orig_bind_func)(int sockfd, const struct sockaddr* addr, socklen_t addr_len);
typedef int (*orig_epoll_wait_func)(int epfd, struct epoll_event *events, int maxevents, int timeout);
typedef int (*orig_write_func)(int fd, const void *buf, size_t count);
typedef int (*orig_read_func)(int fd, void *buf, size_t count);
typedef int (*orig_connect_func)(int sockfd, const struct sockaddr *address, socklen_t address_len);
typedef ssize_t (*orig_send_func)(int sockfd, const void *buf, size_t len, int flags);
typedef ssize_t (*orig_recv_func)(int sockfd, void *buf, size_t len, int flags);
typedef int (*orig_select_func)(int nfds, fd_set *readfds, fd_set *writefds,
        fd_set *exceptfds, struct timeval *timeout);
typedef int (*orig_close_func)(int fd);

/**
 * Callback Functions
 **/
uint64 utp_log_cb(utp_callback_arguments *a) {
	fprintf(stderr, "log: %s\n", a->buf);
	return 0;
}

uint64 utp_sendto_cb(utp_callback_arguments *a) {
	struct sockaddr_in *sin = (struct sockaddr_in *) a->address;
    utp_context *ctx = a->context;
    context_data *data = utp_context_get_userdata(ctx);
    if(!data) {
        fprintf(stderr, "ERROR: could not get data for context %p\n", ctx);
        return -1;
    }

    int fd = data->udpfd;
	int ret = sendto(fd, a->buf, a->len, 0, a->address, a->address_len);

	fprintf(stderr, "sendto %d: %zd byte packet to %s:%d%s  [%d]\n", fd, a->len, inet_ntoa(sin->sin_addr), ntohs(sin->sin_port),
				(a->flags & UTP_UDP_DONTFRAG) ? "  (DF bit requested, but not yet implemented)" : "", ret);


	return 0;
}

uint64 utp_on_state_change_cb(utp_callback_arguments *a) {
	fprintf(stderr, "state %d: %s\n", a->state, utp_state_names[a->state]);
	utp_socket_stats *stats;
    utp_socket *s = a->socket;
    socket_data *sdata = utp_get_userdata(s);

	switch (a->state) {
		case UTP_STATE_CONNECT:
            sdata->connected = TRUE;
		case UTP_STATE_WRITABLE:
            sdata->writeable = TRUE;
			break;

		case UTP_STATE_EOF:
			fprintf(stderr, "Received EOF from socket; closing\n");
			utp_close(a->socket);
			break;

		case UTP_STATE_DESTROYING:
			fprintf(stderr, "UTP socket is being destroyed; exiting\n");

			stats = utp_get_stats(a->socket);
			if (stats) {
				/*fprintf(stderr, "Socket Statistics:\n");*/
				/*fprintf(stderr, "    Bytes sent:          %lu\n", stats->nbytes_xmit);*/
				/*fprintf(stderr, "    Bytes received:      %lu\n", stats->nbytes_recv);*/
				/*fprintf(stderr, "    Packets received:    %lu\n", stats->nrecv);*/
				/*fprintf(stderr, "    Packets sent:        %lu\n", stats->nxmit);*/
				/*fprintf(stderr, "    Duplicate receives:  %lu\n", stats->nduprecv);*/
				/*fprintf(stderr, "    Retransmits:         %lu\n", stats->rexmit);*/
				/*fprintf(stderr, "    Fast Retransmits:    %lu\n", stats->fastrexmit);*/
				/*fprintf(stderr, "    Best guess at MTU:   %lu\n", stats->mtu_guess);*/
			}
			else {
				fprintf(stderr, "No socket statistics available\n");
			}
			break;
	}

	return 0;
}

uint64 utp_on_error_cb(utp_callback_arguments *a) {
	fprintf(stderr, "Error: %s\n", utp_error_code_names[a->error_code]);
	/*utp_close(s);*/
	/*s = NULL;*/
	/*quit_flag = 1;*/
	/*exit_code++;*/
	return 0;
}


uint64 utp_on_accept_cb(utp_callback_arguments *a) {
    utp_context *ctx = a->context;
    utp_socket *s = a->socket;

    context_data *data = utp_context_get_userdata(ctx);
    if(!data) {
        fprintf(stderr, "ERROR: context %p had no data\n", ctx);
        return -1;
    }

    int udpfd = data->udpfd;
    if(!udpfd) {
        fprintf(stderr, "ERROR: no UDP socket for context %p\n", ctx);
        return 0;
    }

    g_queue_push_tail(data->incoming_utp_sockets, s);

    socket_data *sdata = create_utp_socket_data(s);
    sdata->connected = TRUE;

    fprintf(stderr, "added UTP socket %p on UDP fd %d\n", s, udpfd);

	return 0;
}

/*uint64 callback_on_firewall(utp_callback_arguments *a) {*/
	/*fprintf(stderr, "Firewall allowing inbound connection\n");*/
	/*return 0;*/
/*}*/

uint64 utp_on_read_cb(utp_callback_arguments *a) {
    utp_socket *s = a->socket;
    socket_data *data = utp_get_userdata(s);

    data->readbuf = g_byte_array_append(data->readbuf, a->buf, a->len);
    fprintf(stderr, "read in %lu bytes\n", a->len);

	utp_read_drained(s);
	return 0;
}


/**
 * Socket functions we're intercepting
 **/

int read_utp_context(utp_context *ctx) {
    int nfds;
    ssize_t len;
    char buf[4096];
    struct sockaddr_in src_addr;
    socklen_t saddrlen = sizeof(src_addr);

    context_data *data = utp_context_get_userdata(ctx);
    if(!data) {
        fprintf(stderr, "ERROR: could not get data to read on for context %p\n", ctx);
        return -1;
    }
    int udpfd = data->udpfd;

    struct pollfd p[1];
    p[0].fd = udpfd;
    p[0].events = POLLIN;
    nfds = poll(p, 1, 0);

    if(!nfds) {
        return 0;
    }

    if(p[0].revents & POLLIN) {
        while(1) {
            len = recvfrom(udpfd, buf, sizeof(buf), MSG_DONTWAIT, (struct sockaddr *)&src_addr, &saddrlen);
            if(len < 0) {
                if(errno == EAGAIN || errno == EWOULDBLOCK) {
                    utp_issue_deferred_acks(ctx);
                    break;
                } else {
                    fprintf(stderr, "ERROR: recv on %d: %s\n", udpfd, strerror(errno));
                    return -1;
                }
            }
            if(!utp_process_udp(ctx, buf, len, (struct sockaddr *)&src_addr, saddrlen)) {
                fprintf(stderr, "DEBUG: UDP packet not handled by UTP. Ignoring\n");
            }
        }
    }

    return 0;
}

int epoll_wait(int epfd, struct epoll_event *events, int maxevents, int timeout) {
    int i, nfds;

    /* run original epoll_wait and ignore any TCP sockets we have */
    orig_epoll_wait_func orig_epoll_wait = (orig_epoll_wait_func)dlsym(RTLD_NEXT, "epoll_wait");
    struct epoll_event *all_events = (struct epoll_event *)malloc(maxevents * sizeof(struct epoll_event));
    int ret = orig_epoll_wait(epfd, all_events, maxevents, 0);

    /*fprintf(stderr, "DEBUG: epoll_wait %d returned %d nfds\n", epfd, ret);*/

    nfds = 0;
    for(i = 0; i < ret; i++) {
        int fd = all_events[i].data.fd;
        int ev = all_events[i].events;

        utp_context *ctx = g_hash_table_lookup(tcpToContext, GINT_TO_POINTER(fd));
        utp_socket *s = g_hash_table_lookup(tcpToSocket, GINT_TO_POINTER(fd));

        if(ctx) {
            read_utp_context(ctx);

            context_data *data = utp_context_get_userdata(ctx);
            if(!data) {
                fprintf(stderr, "ERROR: context %p has no data\n", ctx);
                return -1;
            }

            if(g_queue_get_length(data->incoming_utp_sockets) > 0) {
                events[nfds].data.fd = fd;
                events[nfds].events = EPOLLIN;
                nfds++;
            }

            utp_check_timeouts(ctx);
        }

        if(s) {
            socket_data *data = utp_get_userdata(s);
            if(!data) {
                fprintf(stderr, "ERROR: socket %p has no data\n", s);
                return -1;
            }

            ev = 0;
            if(data->readbuf->len > 0) {
                ev |= EPOLLIN;
            }
            if(data->writeable) {
                ev |= EPOLLOUT;
            }
            if(ev) {
                events[nfds].data.fd = fd;
                events[nfds].events = ev;
                nfds++;
            }

        }

        if(!ctx && !s) {
            memcpy(&events[nfds], &all_events[i], sizeof(struct epoll_event));
            nfds++;
        }
    }

    return nfds;
}

int select(int nfds, fd_set *readfds, fd_set *writefds, fd_set *exceptfds, struct timeval *timeout) {
    orig_select_func orig_select = (orig_select_func)dlsym(RTLD_NEXT, "select");

    struct pollfd *p = (struct pollfd *)malloc(sizeof(struct pollfd) * nfds);
    int pidx = 0;

    GHashTable *fdmap = g_hash_table_new(g_direct_hash, g_direct_equal);

    int i;
    for(i = 0; i < nfds; i++) {
        context_data *data = NULL;
        utp_socket *s = g_hash_table_lookup(tcpToSocket, GINT_TO_POINTER(i));
        if(s) {
            utp_context *ctx = utp_get_context(s);
            data = utp_context_get_userdata(ctx);
        }

        /*if(i < 3) {*/
            /*if(readfds) FD_CLR(i, readfds);*/
            /*if(writefds) FD_CLR(i, writefds);*/
            /*continue;*/
        /*}*/

        int ev = 0;
        if(readfds && FD_ISSET(i, readfds)) {
            fprintf(stderr, "select wants to know if %d able to read\n", i);
            FD_CLR(i, readfds);
            ev |= POLLIN;
        }
        if(writefds && FD_ISSET(i, writefds)) {
            fprintf(stderr, "select wants to know if %d able to write\n", i);
            FD_CLR(i, writefds);
            ev |= POLLOUT;
        }

        if(ev) {
            p[pidx].fd = i;
            if(data) {
                p[pidx].fd = data->udpfd;
            }
            g_hash_table_insert(fdmap, GINT_TO_POINTER(p[pidx].fd), GINT_TO_POINTER(i));
            p[pidx].events = ev;
            fprintf(stderr, "select polling on %d with events %d\n", p[pidx].fd, p[pidx].events);
            pidx++;
        }
    }

    while(!poll(p, pidx, 0)) {}

    int ret = 0;
    for(i = 0; i < pidx; i++) {
        int fd = p[i].fd;
        int sockfd = GPOINTER_TO_INT(g_hash_table_lookup(fdmap, GINT_TO_POINTER(fd)));

        int inc = 0;

        fprintf(stderr, "select fd %d sockfd %d has revents %d\n", fd, sockfd, p[i].revents);

        if(readfds && (p[i].revents & (POLLIN|POLLHUP))) {
            FD_SET(sockfd, readfds);
            inc = 1;
        }
        if(writefds && (p[i].revents & POLLOUT)) {
            FD_SET(sockfd, writefds);
            inc = 1;
        }
        ret += inc;
    }

    for(i = 0; i < nfds; i++) {
        if(readfds && FD_ISSET(i, readfds)) {
            fprintf(stderr, "select has %d able to read\n", i);
        }
        if(writefds && FD_ISSET(i, writefds)) {
            fprintf(stderr, "select has %d able to write\n", i);
        }
    }

    free(p);
    g_hash_table_destroy(fdmap);

    return ret;

    /*int i;*/
    /*for(i = 0; i < nfds; i++) {*/
        /*if(readfds && FD_ISSET(i, readfds)) {*/
            /*fprintf(stderr, "select wants to know if %d able to read\n", i);*/
        /*}*/
        /*if(writefds && FD_ISSET(i, writefds)) {*/
            /*fprintf(stderr, "select wants to know if %d able to write\n", i);*/
        /*}*/
    /*}*/

    /*int ret = orig_select(nfds, readfds, writefds, exceptfds, timeout);*/

    /*fprintf(stderr, "select %d returned %d\n", nfds, ret);*/

    /*GHashTableIter iter;*/
    /*gpointer key, value;*/

    /*g_hash_table_iter_init(&iter, tcpToSocket);*/
    /*while(g_hash_table_iter_next(&iter, &key, &value)) {*/
        /*int sockfd = GPOINTER_TO_INT(key);*/
        /*utp_socket *s = value;*/
        /*socket_data *data = utp_get_userdata(s);*/
        /*utp_context *ctx = utp_get_context(s);*/
        /*context_data *ctxdata = utp_context_get_userdata(ctx);*/

        /*if(readfds && FD_ISSET(sockfd, readfds)) {*/
            /*if(!data->connected) {*/
                /*FD_CLR(sockfd, readfds);*/
            /*}*/
        /*}*/

        /*fprintf(stderr, "select (tcp) fd %d read %d write %d  (readbuflen %d)\n", sockfd,*/
                /*(readfds ? FD_ISSET(sockfd, readfds) : -1), */
                /*(writefds ? FD_ISSET(sockfd, writefds) : -1), data->readbuf->len);*/
        /*fprintf(stderr, "select (udp) fd %d read %d write %d  (readbuflen %d)\n", ctxdata->udpfd,*/
                /*(readfds ? FD_ISSET(ctxdata->udpfd, readfds) : -1), */
                /*(writefds ? FD_ISSET(ctxdata->udpfd, writefds) : -1), data->readbuf->len);*/

        /*[>if(sockfd == 3 && !(FD_ISSET(sockfd, writefds))) {<]*/
            /*[>FD_CLR(sockfd, readfds);<]*/
        /*[>}<]*/

        /*[>if(readfds && FD_ISSET(sockfd, readfds)) {<]*/
            /*[>if(data->readbuf->len == 0) {<]*/
                /*[>FD_CLR(sockfd, readfds);<]*/
            /*[>}<]*/
        /*[>}<]*/

        /*[>if(writefds && FD_ISSET(sockfd, writefds)) {<]*/
            /*[>if(!data->writeable) {<]*/
                /*[>FD_CLR(sockfd, writefds);<]*/
            /*[>}<]*/
        /*[>}<]*/
    /*}*/

    /*for(i = 0; i < nfds; i++) {*/
        /*if(readfds && FD_ISSET(i, readfds)) {*/
            /*fprintf(stderr, "select has %d able to read\n", i);*/
        /*}*/
        /*if(writefds && FD_ISSET(i, writefds)) {*/
            /*fprintf(stderr, "select has %d able to write\n", i);*/
        /*}*/
    /*}*/

    return ret;
}

int socket(int domain, int type, int protocol) {
    orig_socket_func orig_socket = (orig_socket_func)dlsym(RTLD_NEXT, "socket");
    if(!(type & SOCK_STREAM)) {
        return orig_socket(domain, type, protocol);
    }

    int udp_type = SOCK_DGRAM;
    if(type & SOCK_NONBLOCK) {
        udp_type |= SOCK_NONBLOCK;
    }

    int sockfd = orig_socket(domain, type, protocol);
    int udpfd = orig_socket(AF_INET, udp_type, IPPROTO_UDP);

    utp_context *ctx = create_utp_context(udpfd);
    context_data *data = utp_context_get_userdata(ctx);
    data->blocking = !(type & SOCK_NONBLOCK);

    fprintf(stderr, "socket %d is %sblocking\n", sockfd, (data->blocking ? "" : "non-"));

    g_hash_table_insert(tcpToContext, GINT_TO_POINTER(sockfd), ctx);

    int on = 1;
    if(setsockopt(udpfd, SOL_IP, IP_RECVERR, &on, sizeof(on)) != 0) {
        fprintf(stderr, "ERROR! Could not set sockopt on %d\n", udpfd);
    }



    return sockfd;
}

int bind(int sockfd, const struct sockaddr *addr, socklen_t addrlen) {
    struct sockaddr_in *sin = (struct sockaddr_in *)addr;
    fprintf(stderr, "binding socket %d to address %s:%d\n", sockfd, inet_ntoa(sin->sin_addr), ntohs(sin->sin_port));

    utp_context *ctx = g_hash_table_lookup(tcpToContext, GINT_TO_POINTER(sockfd));
    if(!ctx) {
        fprintf(stderr, "ERROR: could not find UTP context for TCP socket %d\n", sockfd);
        return -1;
    }

    context_data *data = utp_context_get_userdata(ctx);
    if(!data) {
        fprintf(stderr, "ERROR: context %p for TCP socket %d has no data\n", ctx, sockfd);
        return -1;
    }

    int udpfd = data->udpfd;

    int on = 1;
    if(setsockopt(udpfd, SOL_IP, IP_RECVERR, &on, sizeof(on)) != 0) {
        fprintf(stderr, "ERROR: Could not set sockopt on %d\n", udpfd);
        return -1;
    }

    orig_bind_func orig_bind = (orig_bind_func)dlsym(RTLD_NEXT, "bind");
    if(orig_bind(udpfd, addr, addrlen) != 0) {
        fprintf(stderr, "ERROR! could not bind!\n");
        return -1;
    }

    return 0;
}

int listen(int sockfd, int backlog) {
    fprintf(stderr, "listen on %d\n", sockfd);
    return 0;
}

int accept(int sockfd, struct sockaddr *addr, socklen_t *addrlen) {
    fprintf(stderr, "accept on %d\n", sockfd);
    utp_context *ctx = g_hash_table_lookup(tcpToContext, GINT_TO_POINTER(sockfd));
    if(!ctx) {
        fprintf(stderr, "ERROR: no context to accept on TCP socket %d\n", sockfd);
        return -1;
    }

    context_data *data = utp_context_get_userdata(ctx);
    if(!data) {
        fprintf(stderr, "ERROR: no data on context %p for TCP socket %d\n", ctx, sockfd);
        return -1;
    }

    if(data->blocking) {
        fprintf(stderr, "blocking socket, waiting until we have a socket\n");
        while(!g_queue_get_length(data->incoming_utp_sockets)) {
            read_utp_context(ctx);
        }
    }

    utp_socket *s = g_queue_pop_head(data->incoming_utp_sockets);
    if(!s) {
        errno = EWOULDBLOCK;
        return -1;
    }
    if(addr) {
       utp_getpeername(s, addr, addrlen);
    }

    orig_socket_func orig_socket = (orig_socket_func)dlsym(RTLD_NEXT, "socket");
    gint newsockfd = orig_socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);

    g_hash_table_insert(tcpToSocket, GINT_TO_POINTER(newsockfd), s);

    fprintf(stderr, "DEBUG: tcp socket %d child %d has udp socket %d and utp socket %p\n", sockfd, newsockfd, data->udpfd, s);

    return newsockfd;
}

int connect(int sockfd, const struct sockaddr *address, socklen_t address_len) {
    orig_connect_func orig_connect = (orig_connect_func)dlsym(RTLD_NEXT, "connect");
    orig_bind_func orig_bind = (orig_bind_func)dlsym(RTLD_NEXT, "bind");

    utp_context *ctx = g_hash_table_lookup(tcpToContext, GINT_TO_POINTER(sockfd));
    if(!ctx) {
        return orig_connect(sockfd, address, address_len);
    }

    context_data *data = utp_context_get_userdata(ctx);
    if(!data) {
        fprintf(stderr, "ERROR: no data for context %p on TCP socket %d\n", ctx, sockfd);
        return -1;
    }

    struct addrinfo hints, *res;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_protocol = IPPROTO_UDP;

    if(getaddrinfo("0.0.0.0", NULL, &hints, &res) != 0) {
        fprintf(stderr, "ERROR: getaddrinfo: %s\n", strerror(errno));
        return -1;
    }

    if(orig_bind(data->udpfd, res->ai_addr, res->ai_addrlen) != 0) {
        fprintf(stderr, "ERROR: bind: %s\n", strerror(errno));
        return -1;
    }

    utp_socket *s = utp_create_socket(ctx);
    create_utp_socket_data(s);
    utp_connect(s, address, address_len);

    g_hash_table_insert(tcpToSocket, GINT_TO_POINTER(sockfd), s);

    return 0;
}

ssize_t send(int sockfd, const void *buf, size_t len, int flags) {
    orig_send_func orig_send = (orig_send_func)dlsym(RTLD_NEXT, "send");

    utp_socket *s = (utp_socket *)g_hash_table_lookup(tcpToSocket, GINT_TO_POINTER(sockfd));
    if(!s) {
        return orig_send(sockfd, buf, len, flags);
    }

    utp_context *ctx = utp_get_context(s);
    context_data *ctxdata = utp_context_get_userdata(ctx);
    socket_data *sdata = utp_get_userdata(s);

    if(ctxdata->blocking) {
        while(!sdata->connected) {
            read_utp_context(ctx);
        }
    }

    fprintf(stderr, "send %zd bytes on socket %d\n", len, sockfd);

    size_t sent = utp_write(s, (void*)buf, len);

    if(sent == 0) {
        errno = EWOULDBLOCK;
        return -1;
    }

    return sent;
}

ssize_t recv(int sockfd, void *buf, size_t len, int flags) {
    orig_recv_func orig_recv = (orig_recv_func)dlsym(RTLD_NEXT, "recv");

    utp_socket *s = (utp_socket *)g_hash_table_lookup(tcpToSocket, GINT_TO_POINTER(sockfd));
    if(!s) {
        return orig_recv(sockfd, buf, len, flags);
    }

    utp_context *ctx = utp_get_context(s);
    context_data *ctxdata = utp_context_get_userdata(ctx);
    socket_data *sdata = utp_get_userdata(s);

    if(ctxdata->blocking) {
        while(sdata->readbuf->len < len) {
            read_utp_context(ctx);
        }
    }

    fprintf(stderr, "recv %zd bytes on socket %d\n", len, sockfd);

    if(sdata->readbuf->len == 0) {
        errno = EWOULDBLOCK;
        return -1;
    }

    int length = MIN(sdata->readbuf->len, len);
    if(!length) {
        fprintf(stderr, "ERROR! No data in recv buf for socket %d\n", sockfd);
        return 0;
    }

    memcpy(buf, sdata->readbuf->data, length);
    sdata->readbuf = g_byte_array_remove_range(sdata->readbuf, 0, length);

    return length;
}

ssize_t write(int fd, const void *buf, size_t count) {
    fprintf(stderr, "writing %zd bytes to fd %d\n", count, fd);

    if(g_hash_table_lookup(tcpToSocket, GINT_TO_POINTER(fd))) {
        return send(fd, buf, count, 0);
    }

    orig_write_func orig_write = (orig_write_func)dlsym(RTLD_NEXT, "write");
    return orig_write(fd, buf, count);
}

ssize_t read(int fd, void *buf, size_t count) {
    int ret = 0;

    fprintf(stderr, "reading %zd bytes from fd %d\n", count, fd);

    if(g_hash_table_lookup(tcpToSocket, GINT_TO_POINTER(fd))) {
        ret = recv(fd, buf, count, 0);
    } else {
        orig_read_func orig_read = (orig_read_func)dlsym(RTLD_NEXT, "read");
        ret = orig_read(fd, buf, count);
    }

    return ret;
}

int close(int fd) {
    fprintf(stderr, "DEBUG: closing %d\n", fd);
    orig_close_func orig_close = (orig_close_func)dlsym(RTLD_NEXT, "close");

    utp_socket *s = g_hash_table_lookup(tcpToSocket, GINT_TO_POINTER(fd));

    if(s) {
        utp_close(s);
        utp_context *ctx = utp_get_context(s);
           read_utp_context(ctx);
    }

    return orig_close(fd);
}
