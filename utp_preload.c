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

typedef int (*orig_socket_func)(int domain, int type, int protocol);
typedef int (*orig_connect_func)(int sockfd, const struct sockaddr *address, socklen_t address_len);
typedef int (*orig_bind_func)(int sockfd, const struct sockaddr* addr, socklen_t addr_len);
typedef int (*orig_listen_func)(int sockfd, int backlog);
typedef int (*orig_accept_func)(int sockfd, struct sockaddr *addr, socklen_t *addrlen);
typedef int (*orig_write_func)(int fd, const void *buf, size_t count);
typedef int (*orig_read_func)(int fd, void *buf, size_t count);
typedef ssize_t (*orig_send_func)(int sockfd, const void *buf, size_t len, int flags);
typedef ssize_t (*orig_recv_func)(int sockfd, void *buf, size_t len, int flags);
typedef int (*orig_epoll_wait_func)(int epfd, struct epoll_event *events, int maxevents, int timeout);
typedef int (*orig_select_func)(int nfds, fd_set *readfds, fd_set *writefds,
        fd_set *exceptfds, struct timeval *timeout);
typedef int (*orig_close_func)(int fd);

orig_socket_func orig_socket;
orig_connect_func orig_connect;
orig_bind_func orig_bind;
orig_listen_func orig_listen;
orig_accept_func orig_accept;
orig_write_func orig_write;
orig_read_func orig_read;
orig_send_func orig_send;
orig_recv_func orig_recv;
orig_epoll_wait_func orig_epoll_wait;
orig_select_func orig_select;
orig_close_func orig_close;

#define SETSYM_OR_FAIL(funcptr, funcstr) { \
	dlerror(); \
	funcptr = dlsym(RTLD_NEXT, funcstr); \
	char* errorMessage = dlerror(); \
	if(errorMessage != NULL) { \
		info("dlsym(%s): dlerror(): %s", funcstr, errorMessage); \
		exit(EXIT_FAILURE); \
	} else if(funcptr == NULL) { \
		info("dlsym(%s): returned NULL pointer", funcstr); \
		exit(EXIT_FAILURE); \
	} \
}

typedef enum log_level {
    UTP_PRELOAD_LOG_LEVEL_DEBUG,
    UTP_PRELOAD_LOG_LEVEL_INFO,
    UTP_PRELOAD_LOG_LEVEL_MESSAGE,
    UTP_PRELOAD_LOG_LEVEL_WARNING,
    UTP_PRELOAD_LOG_LEVEL_ERROR,
} log_level;

typedef struct utp_context_data {
    int udpfd;
    int blocking;
    GQueue *incoming_utp_sockets;
} utp_context_data;

typedef struct utp_socket_data {
    int connected;
    GByteArray *readbuf;
    int writeable;
} utp_socket_data;

/* Map of TCP sockets to UTP contexts */
GHashTable *tcp_to_context = NULL;
GHashTable *tcp_to_socket = NULL;

uint64 utp_log_cb(utp_callback_arguments *a);
uint64 utp_sendto_cb(utp_callback_arguments *a);
uint64 utp_on_state_change_cb(utp_callback_arguments *a);
uint64 utp_on_error_cb(utp_callback_arguments *a);
uint64 utp_on_accept_cb(utp_callback_arguments *a);
uint64 utp_on_read_cb(utp_callback_arguments *a);

log_level LOG_LEVEL = UTP_PRELOAD_LOG_LEVEL_INFO;

#define debug(...) utplog(UTP_PRELOAD_LOG_LEVEL_DEBUG, __FUNCTION__, __VA_ARGS__);
#define info(...) utplog(UTP_PRELOAD_LOG_LEVEL_INFO, __FUNCTION__, __VA_ARGS__);
#define message(...) utplog(UTP_PRELOAD_LOG_LEVEL_MESSAGE, __FUNCTION__, __VA_ARGS__);
#define warning(...) utplog(UTP_PRELOAD_LOG_LEVEL_WARNING, __FUNCTION__, __VA_ARGS__);
#define error(...) utplog(UTP_PRELOAD_LOG_LEVEL_ERROR, __FUNCTION__, __VA_ARGS__);

void utplog(log_level level, const char *funcname, const char *fmt, ...) {
    if(level < LOG_LEVEL) {
        return;
    }

    fflush(stdout);
    switch(level) {
        case UTP_PRELOAD_LOG_LEVEL_DEBUG:   fprintf(stderr, "[DEBUG]   "); break;
        case UTP_PRELOAD_LOG_LEVEL_INFO:    fprintf(stderr, "[INFO]    "); break;
        case UTP_PRELOAD_LOG_LEVEL_MESSAGE: fprintf(stderr, "[MESSAGE] "); break;
        case UTP_PRELOAD_LOG_LEVEL_WARNING: fprintf(stderr, "[WARNING] "); break;
        case UTP_PRELOAD_LOG_LEVEL_ERROR:   fprintf(stderr, "[ERROR]   "); break;
    }

    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
    fflush(stderr);

    if(level == UTP_PRELOAD_LOG_LEVEL_ERROR) {
        exit(1);
    }
}

static void init() __attribute__((constructor));

void init() {
    /* get original function calls */
    SETSYM_OR_FAIL(orig_socket, "socket");
    SETSYM_OR_FAIL(orig_connect, "connect");
    SETSYM_OR_FAIL(orig_bind, "bind");
    SETSYM_OR_FAIL(orig_listen, "listen");
    SETSYM_OR_FAIL(orig_accept, "accept");
    SETSYM_OR_FAIL(orig_write, "write");
    SETSYM_OR_FAIL(orig_read, "read");
    SETSYM_OR_FAIL(orig_send, "send");
    SETSYM_OR_FAIL(orig_recv, "recv");
    SETSYM_OR_FAIL(orig_epoll_wait, "epoll_wait");
    SETSYM_OR_FAIL(orig_select, "select");
    SETSYM_OR_FAIL(orig_close, "close");

    tcp_to_context = g_hash_table_new(g_direct_hash, g_direct_equal);
    tcp_to_socket = g_hash_table_new(g_direct_hash, g_direct_equal);

    debug("successfully initialized UTP preload library");
}

utp_context *create_utp_context(int udpfd) {
    utp_context *ctx = utp_init(2);
    utp_set_callback(ctx, UTP_LOG,				&utp_log_cb);
    utp_set_callback(ctx, UTP_SENDTO,			&utp_sendto_cb);
    utp_set_callback(ctx, UTP_ON_ERROR,			&utp_on_error_cb);
    utp_set_callback(ctx, UTP_ON_STATE_CHANGE,	&utp_on_state_change_cb);
    utp_set_callback(ctx, UTP_ON_READ,			&utp_on_read_cb);
    // TODO firewall callback
    /*utp_set_callback(ctx, UTP_ON_FIREWALL,		&utp);*/
    utp_set_callback(ctx, UTP_ON_ACCEPT,		&utp_on_accept_cb);

    utp_context_set_option(ctx, UTP_LOG_NORMAL, 1);
    utp_context_set_option(ctx, UTP_LOG_MTU,    1);
    utp_context_set_option(ctx, UTP_LOG_DEBUG,  1);

    utp_context_data *data = (utp_context_data *)malloc(sizeof(*data));
    memset(data, 0, sizeof(*data));
    data->udpfd = udpfd;
    data->incoming_utp_sockets = g_queue_new();

    utp_context_set_userdata(ctx, data);

    return ctx;
}

utp_socket_data *create_utp_utp_socket_data(utp_socket *s) {
    utp_socket_data *data = (utp_socket_data *)malloc(sizeof(*data));
    memset(data, 0, sizeof(*data));
    data->readbuf = g_byte_array_new();
    data->writeable = FALSE;
    data->connected = FALSE;
    utp_set_userdata(s, data);
    return data;
}

/**
 * Callback Functions
 **/
uint64 utp_log_cb(utp_callback_arguments *a) {
    info("log: %s", a->buf);
	return 0;
}

uint64 utp_sendto_cb(utp_callback_arguments *a) {
    utp_context *ctx = a->context;
    utp_context_data *data = utp_context_get_userdata(ctx);
    if(!data) {
        error("could not get data for context %p", ctx);
        return -1;
    }

    int fd = data->udpfd;

	struct sockaddr_in *sin = (struct sockaddr_in *) a->address;
	info("sendto %d: %zd byte packet to %s:%d%s", fd, a->len, inet_ntoa(sin->sin_addr), ntohs(sin->sin_port),
				(a->flags & UTP_UDP_DONTFRAG) ? "  (DF bit requested, but not yet implemented)" : "");

	sendto(fd, a->buf, a->len, 0, a->address, a->address_len);

	return 0;
}

uint64 utp_on_state_change_cb(utp_callback_arguments *a) {
	debug("state %d: %s", a->state, utp_state_names[a->state]);

	utp_socket_stats *stats;
    utp_socket *s = a->socket;
    utp_socket_data *sdata = utp_get_userdata(s);

	switch (a->state) {
		case UTP_STATE_CONNECT:
            sdata->connected = TRUE;
		case UTP_STATE_WRITABLE:
            sdata->writeable = TRUE;
			break;

		case UTP_STATE_EOF:
			info("received EOF from socket; closing");
			utp_close(a->socket);
			break;

		case UTP_STATE_DESTROYING:
			info("UTP socket is being destroyed; exiting");

			stats = utp_get_stats(a->socket);
			if (stats) {
				/*info("Socket Statistics:");*/
				/*info("    Bytes sent:          %lu", stats->nbytes_xmit);*/
				/*info("    Bytes received:      %lu", stats->nbytes_recv);*/
				/*info("    Packets received:    %lu", stats->nrecv);*/
				/*info("    Packets sent:        %lu", stats->nxmit);*/
				/*info("    Duplicate receives:  %lu", stats->nduprecv);*/
				/*info("    Retransmits:         %lu", stats->rexmit);*/
				/*info("    Fast Retransmits:    %lu", stats->fastrexmit);*/
				/*info("    Best guess at MTU:   %lu", stats->mtu_guess);*/
			}
			else {
				info("No socket statistics available");
			}
			break;
	}

	return 0;
}

uint64 utp_on_error_cb(utp_callback_arguments *a) {
    utp_socket *s = a->socket;
	warning("%s", utp_error_code_names[a->error_code]);
    utp_close(s);
	return 0;
}


uint64 utp_on_accept_cb(utp_callback_arguments *a) {
    utp_context *ctx = a->context;
    utp_socket *s = a->socket;

    utp_context_data *data = utp_context_get_userdata(ctx);
    if(!data) {
        error("context %p had no data", ctx);
    }

    int udpfd = data->udpfd;
    if(!udpfd) {
        error("no UDP socket for context %p", ctx);
    }

    g_queue_push_tail(data->incoming_utp_sockets, s);

    utp_socket_data *sdata = create_utp_utp_socket_data(s);
    sdata->connected = TRUE;

    info("added UTP socket %p on UDP fd %d", s, udpfd);

	return 0;
}

uint64 utp_on_read_cb(utp_callback_arguments *a) {
    utp_socket *s = a->socket;
    utp_socket_data *data = utp_get_userdata(s);

    data->readbuf = g_byte_array_append(data->readbuf, a->buf, a->len);
    info("read in %lu bytes", a->len);

	utp_read_drained(s);
	return 0;
}


/**
 * Socket functions we're intercepting
 **/

int read_utp_context(utp_context *ctx) {
    ssize_t len;
    unsigned char buf[4096];
    struct sockaddr_in addr;
    socklen_t addrlen = sizeof(addr);

    utp_context_data *data = utp_context_get_userdata(ctx);
    int udpfd = data->udpfd;

    debug("reading in on UDP socket %d", udpfd);

    while(1) {
        len = recvfrom(udpfd, buf, sizeof(buf), MSG_DONTWAIT, (struct sockaddr *)&addr, &addrlen);
        if(len < 0) {
            if(errno == EAGAIN || errno == EWOULDBLOCK) {
                utp_issue_deferred_acks(ctx);
                break;
            } else {
                error("recv on %d: %s", udpfd, strerror(errno));
                return -1;
            }
        }
        if(!utp_process_udp(ctx, buf, len, (struct sockaddr *)&addr, addrlen)) {
            warning("UDP packet not handled by UTP. Ignoring");
        }
    }

    return 0;
}

int epoll_wait(int epfd, struct epoll_event *events, int maxevents, int timeout) {
    int nfds = 0;
    int i, ret;

    /* XXX maybe wait on timeout first, then epoll_wait with timeout=0? */

    /* first get all events that aren't related to a TCP socket we have */
    struct epoll_event *tmpevents = (struct epoll_event *)malloc(maxevents * sizeof(struct epoll_event));
    ret = orig_epoll_wait(epfd, tmpevents, maxevents, timeout);
    for(i = 0; i < ret; i++) {
        utp_context *ctx = g_hash_table_lookup(tcp_to_context, GINT_TO_POINTER(tmpevents[i].data.fd));
        if(ctx) {
            read_utp_context(ctx);
        } else {
            memcpy(&events[nfds], &tmpevents[i], sizeof(struct epoll_event));
            nfds++;
        }
    }

    GHashTableIter iter;
    gpointer key, value;

    /* go through contexts and check for any incoming connections */
    g_hash_table_iter_init(&iter, tcp_to_context);
    while(g_hash_table_iter_next(&iter, &key, &value)) {
        int sockfd = GPOINTER_TO_INT(key);
        utp_context *ctx = value;
        utp_context_data *data = utp_context_get_userdata(ctx);

        if(g_queue_get_length(data->incoming_utp_sockets) > 0) {
            events[nfds].data.fd = sockfd;
            events[nfds].events = EPOLLIN;
            nfds++;
        }
    }

    /* go through UTP sockets and check if writeable and if there's data to read */
    g_hash_table_iter_init(&iter, tcp_to_socket);
    while(g_hash_table_iter_next(&iter, &key, &value)) {
        int sockfd = GPOINTER_TO_INT(key);
        utp_socket *s = value;
        utp_socket_data *data = utp_get_userdata(s);

        int ev = 0;
        if(data->readbuf->len > 0) {
            ev |= EPOLLIN;
        }
        if(data->writeable) {
            ev |= EPOLLOUT;
        }

        if(ev) {
            events[nfds].data.fd = sockfd;
            events[nfds].events = ev;
            nfds++;
        }
    }

    return nfds;
}

int select(int nfds, fd_set *readfds, fd_set *writefds, fd_set *exceptfds, struct timeval *timeout) {
    int i, ret;

    fd_set origreadfds;
    fd_set origwritefds;

    if(readfds) memcpy(&origreadfds, readfds, sizeof(fd_set));
    if(writefds) memcpy(&origwritefds, writefds, sizeof(fd_set));

    /* XXX maybe wait on timeout first, then epoll_wait with timeout=0? */

    /* run select to get what non-TCP descriptors can be read or written om */
    ret = 0;
    orig_select(nfds, readfds, writefds, exceptfds, timeout);
    for(i = 0; i < nfds; i++) {
        utp_context *ctx = g_hash_table_lookup(tcp_to_context, GINT_TO_POINTER(i));
        if(ctx) {
            if(readfds) FD_CLR(i, readfds);
            if(writefds) FD_CLR(i, writefds);
            if(exceptfds) FD_CLR(i, exceptfds);

            read_utp_context(ctx);
        } else {
            if((readfds && FD_ISSET(i, readfds)) || 
               (writefds && FD_ISSET(i, writefds)) || 
               (exceptfds && FD_ISSET(i, exceptfds)))
            {
                ret++;
            }
        }
    }

    GHashTableIter iter;
    gpointer key, value;

    /* go through contexts and check for any incoming connections */
    g_hash_table_iter_init(&iter, tcp_to_context);
    while(g_hash_table_iter_next(&iter, &key, &value)) {
        int sockfd = GPOINTER_TO_INT(key);
        utp_context *ctx = value;
        utp_context_data *data = utp_context_get_userdata(ctx);

        if(readfds && FD_ISSET(sockfd, &origreadfds) && g_queue_get_length(data->incoming_utp_sockets) > 0) {
            FD_SET(sockfd, readfds);
            ret++;
        }
    }

    /* go through UTP sockets and check if writeable and if there's data to read */
    g_hash_table_iter_init(&iter, tcp_to_socket);
    while(g_hash_table_iter_next(&iter, &key, &value)) {
        int sockfd = GPOINTER_TO_INT(key);
        utp_socket *s = value;
        utp_socket_data *data = utp_get_userdata(s);

        if(readfds && FD_ISSET(sockfd, &origreadfds) && data->readbuf->len > 0) {
            FD_SET(sockfd, readfds);
            ret++;
        }
        if(writefds && FD_ISSET(sockfd, &origwritefds) && data->writeable) {
            FD_SET(sockfd, writefds);
            ret++;
        }
    }

    return ret;
}

int socket(int domain, int type, int protocol) {
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
    utp_context_data *data = utp_context_get_userdata(ctx);
    data->blocking = !(type & SOCK_NONBLOCK);

    info("socket %d is %sblocking", sockfd, (data->blocking ? "" : "non-"));

    g_hash_table_insert(tcp_to_context, GINT_TO_POINTER(sockfd), ctx);

    int on = 1;
    if(setsockopt(udpfd, SOL_IP, IP_RECVERR, &on, sizeof(on)) != 0) {
        error("Could not set sockopt on %d", udpfd);
    }



    return sockfd;
}

int bind(int sockfd, const struct sockaddr *addr, socklen_t addrlen) {
    struct sockaddr_in *sin = (struct sockaddr_in *)addr;
    info("binding socket %d to address %s:%d", sockfd, inet_ntoa(sin->sin_addr), ntohs(sin->sin_port));

    utp_context *ctx = g_hash_table_lookup(tcp_to_context, GINT_TO_POINTER(sockfd));
    if(!ctx) {
        info("could not find UTP context for TCP socket %d", sockfd);
        return orig_bind(sockfd, addr, addrlen);
    }

    utp_context_data *data = utp_context_get_userdata(ctx);
    int udpfd = data->udpfd;

    int on = 1;
    if(setsockopt(udpfd, SOL_IP, IP_RECVERR, &on, sizeof(on)) != 0) {
        error("Could not set sockopt on %d", udpfd);
        return -1;
    }

    if(orig_bind(udpfd, addr, addrlen) != 0) {
        warning("could not bind!");
        return -1;
    }

    return 0;
}

int listen(int sockfd, int backlog) {
    info("listen on %d", sockfd);

    if(g_hash_table_lookup(tcp_to_context, GINT_TO_POINTER(sockfd))) {
        return 0;
    }

    return orig_listen(sockfd, backlog);
}

int accept(int sockfd, struct sockaddr *addr, socklen_t *addrlen) {
    info("accept on %d", sockfd);

    utp_context *ctx = g_hash_table_lookup(tcp_to_context, GINT_TO_POINTER(sockfd));
    if(!ctx) {
        info("no context to accept on TCP socket %d", sockfd);
        return orig_accept(sockfd, addr, addrlen);
    }

    utp_context_data *data = utp_context_get_userdata(ctx);
    if(data->blocking) {
        info("blocking socket, waiting until we have a socket");
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

    gint newsockfd = orig_socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);

    g_hash_table_insert(tcp_to_socket, GINT_TO_POINTER(newsockfd), s);

    debug("tcp socket %d child %d has udp socket %d and utp socket %p", sockfd, newsockfd, data->udpfd, s);

    return newsockfd;
}

int connect(int sockfd, const struct sockaddr *address, socklen_t address_len) {

    utp_context *ctx = g_hash_table_lookup(tcp_to_context, GINT_TO_POINTER(sockfd));
    if(!ctx) {
        return orig_connect(sockfd, address, address_len);
    }

    utp_context_data *data = utp_context_get_userdata(ctx);
    if(!data) {
        error("no data for context %p on TCP socket %d", ctx, sockfd);
        return -1;
    }

    struct addrinfo hints, *res;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_protocol = IPPROTO_UDP;

    if(getaddrinfo("0.0.0.0", NULL, &hints, &res) != 0) {
        error("getaddrinfo: %s", strerror(errno));
        return -1;
    }

    if(orig_bind(data->udpfd, res->ai_addr, res->ai_addrlen) != 0) {
        error("bind: %s", strerror(errno));
        return -1;
    }

    utp_socket *s = utp_create_socket(ctx);
    create_utp_utp_socket_data(s);
    utp_connect(s, address, address_len);

    g_hash_table_insert(tcp_to_socket, GINT_TO_POINTER(sockfd), s);

    return 0;
}

ssize_t send(int sockfd, const void *buf, size_t len, int flags) {
    utp_socket *s = g_hash_table_lookup(tcp_to_socket, GINT_TO_POINTER(sockfd));
    if(!s) {
        return orig_send(sockfd, buf, len, flags);
    }

    utp_context *ctx = utp_get_context(s);
    utp_context_data *ctxdata = utp_context_get_userdata(ctx);
    utp_socket_data *sdata = utp_get_userdata(s);

    if(ctxdata->blocking) {
        while(!sdata->connected) {
            read_utp_context(ctx);
        }
    }

    debug("send %zd bytes on socket %d", len, sockfd);

    size_t sent = utp_write(s, (void*)buf, len);

    if(sent == 0) {
        sdata->writeable = FALSE;
        errno = EWOULDBLOCK;
        return -1;
    }

    return sent;
}

ssize_t recv(int sockfd, void *buf, size_t len, int flags) {
    utp_socket *s = (utp_socket *)g_hash_table_lookup(tcp_to_socket, GINT_TO_POINTER(sockfd));
    if(!s) {
        return orig_recv(sockfd, buf, len, flags);
    }

    utp_context *ctx = utp_get_context(s);
    utp_context_data *ctxdata = utp_context_get_userdata(ctx);
    utp_socket_data *sdata = utp_get_userdata(s);

    if(ctxdata->blocking) {
        while(sdata->readbuf->len < len) {
            read_utp_context(ctx);
        }
    }

    debug("recv %zd bytes on socket %d", len, sockfd);

    if(sdata->readbuf->len == 0) {
        errno = EWOULDBLOCK;
        return -1;
    }

    int length = MIN(sdata->readbuf->len, len);
    if(!length) {
        error("No data in recv buf for socket %d", sockfd);
        return 0;
    }

    memcpy(buf, sdata->readbuf->data, length);
    sdata->readbuf = g_byte_array_remove_range(sdata->readbuf, 0, length);

    return length;
}

ssize_t write(int fd, const void *buf, size_t count) {
    debug("writing %zd bytes to fd %d", count, fd);

    if(g_hash_table_lookup(tcp_to_socket, GINT_TO_POINTER(fd))) {
        return send(fd, buf, count, 0);
    }

    return orig_write(fd, buf, count);
}

ssize_t read(int fd, void *buf, size_t count) {
    int ret = 0;

    debug("reading %zd bytes from fd %d", count, fd);

    if(g_hash_table_lookup(tcp_to_socket, GINT_TO_POINTER(fd))) {
        ret = recv(fd, buf, count, 0);
    } else {
        ret = orig_read(fd, buf, count);
    }

    return ret;
}

int close(int fd) {
    debug("closing %d", fd);

    utp_socket *s = g_hash_table_lookup(tcp_to_socket, GINT_TO_POINTER(fd));

    if(s) {
        utp_close(s);
        utp_context *ctx = utp_get_context(s);
        read_utp_context(ctx);
    }

    return orig_close(fd);
}
