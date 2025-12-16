#ifdef _WIN32
#define _WINSOCK_DEPRECATED_NO_WARNINGS
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "Ws2_32.lib")
static int ws_inited = 0;
static void ensure_wsa() { if (!ws_inited) { WSADATA d; WSAStartup(MAKEWORD(2,2), &d); ws_inited = 1; } }
#define CLOSESOCK closesocket
#else
#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netdb.h>
#include <unistd.h>
#define SOCKET int
#define INVALID_SOCKET (-1)
#define CLOSESOCK close
static void ensure_wsa() {}
#endif

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>
#include "../include/net.h"

static int net_verbose = 0;

static void net_logf(const char *tag, const char *fmt, ...) {
    if (!net_verbose) return;
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "[NET][%s] ", tag);
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
}

void net_set_verbose(int enabled) {
    net_verbose = enabled;
}

static int resolve_ipv4(const char *host, uint16_t port, struct sockaddr_in *out) {
    memset(out, 0, sizeof(*out));
    out->sin_family = AF_INET;
    out->sin_port = htons(port);
    if (!host || strcmp(host, "0.0.0.0") == 0 || strcmp(host, "*") == 0) {
        out->sin_addr.s_addr = htonl(INADDR_ANY);
        return 0;
    }
    if (strcmp(host, "localhost") == 0) host = "127.0.0.1";
#ifdef _WIN32
    if (InetPton(AF_INET, host, &out->sin_addr) == 1) return 0;
#else
    if (inet_pton(AF_INET, host, &out->sin_addr) == 1) return 0;
#endif
    struct addrinfo hints; memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;
    char port_str[16];
    snprintf(port_str, sizeof(port_str), "%u", (unsigned)port);
    struct addrinfo *res = NULL;
    if (getaddrinfo(host, port_str, &hints, &res) != 0) {
        return -1;
    }
    struct sockaddr_in *addr_in = (struct sockaddr_in*)res->ai_addr;
    out->sin_addr = addr_in->sin_addr;
    freeaddrinfo(res);
    return 0;
}

int net_listen_addr(const char *host, uint16_t port) {
    ensure_wsa();
    SOCKET fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd == INVALID_SOCKET) return -1;
    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (const char*)&opt, sizeof(opt));
    struct sockaddr_in addr;
    if (resolve_ipv4(host, port, &addr) != 0) {
        CLOSESOCK(fd);
        return -1;
    }
    if (bind(fd, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
        CLOSESOCK(fd);
        return -1;
    }
    if (listen(fd, 64) != 0) {
        CLOSESOCK(fd);
        return -1;
    }
    char ipbuf[64];
    inet_ntop(AF_INET, &addr.sin_addr, ipbuf, sizeof(ipbuf));
    net_logf("LISTEN", "%s:%u (fd=%d)", ipbuf, (unsigned)port, (int)fd);
    return (int)fd;
}

int net_listen(uint16_t port) {
    return net_listen_addr(NULL, port);
}

int net_accept(int server_fd, char *peer_ip_buf, int ip_buf_len, uint16_t *peer_port) {
    struct sockaddr_in peer; socklen_t len = sizeof(peer);
    SOCKET c = accept(server_fd, (struct sockaddr*)&peer, &len);
    if (c == INVALID_SOCKET) return -1;
    if (peer_ip_buf && ip_buf_len > 0) {
        peer_ip_buf[0] = '\0';
        const char *p = inet_ntoa(peer.sin_addr);
        if (p && strlen(p) > 0) {
            strncpy(peer_ip_buf, p, ip_buf_len-1);
            peer_ip_buf[ip_buf_len-1] = '\0';
        } else {
            strncpy(peer_ip_buf, "127.0.0.1", ip_buf_len-1);
            peer_ip_buf[ip_buf_len-1] = '\0';
        }
    }
    if (peer_port) *peer_port = ntohs(peer.sin_port);
    if (net_verbose) {
        char iptmp[64] = "";
        if (peer_ip_buf && peer_ip_buf[0]) {
            strncpy(iptmp, peer_ip_buf, sizeof(iptmp)-1);
        } else {
            inet_ntop(AF_INET, &peer.sin_addr, iptmp, sizeof(iptmp));
        }
        net_logf("ACCEPT", "fd=%d peer=%s:%u", (int)c, iptmp, (unsigned)(peer_port ? *peer_port : 0));
    }
    return (int)c;
}

int net_get_local_addr(int fd, char *ip_buf, int ip_buf_len, uint16_t *port_out) {
    struct sockaddr_in addr; socklen_t len = sizeof(addr);
    if (getsockname(fd, (struct sockaddr*)&addr, &len) != 0) {
        return -1;
    }
    if (ip_buf && ip_buf_len > 0) {
        const char *p = inet_ntop(AF_INET, &addr.sin_addr, ip_buf, ip_buf_len);
        if (!p) {
            strncpy(ip_buf, "0.0.0.0", ip_buf_len-1);
            ip_buf[ip_buf_len-1] = '\0';
        }
    }
    if (port_out) *port_out = ntohs(addr.sin_port);
    return 0;
}

int net_connect_ex(const char *remote_host, uint16_t remote_port, const char *local_host, uint16_t local_port) {
    ensure_wsa();
    char port_str[16];
    snprintf(port_str, sizeof(port_str), "%u", (unsigned)remote_port);
    struct addrinfo hints; memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    struct addrinfo *res = NULL;
    if (getaddrinfo(remote_host, port_str, &hints, &res) != 0) {
        return -1;
    }

    SOCKET fd = INVALID_SOCKET;
    for (struct addrinfo *p = res; p; p = p->ai_next) {
        fd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (fd == INVALID_SOCKET) continue;
        if ((local_host && local_host[0]) || local_port != 0) {
            struct sockaddr_in local_addr;
            if (resolve_ipv4(local_host, local_port, &local_addr) != 0) {
                CLOSESOCK(fd);
                fd = INVALID_SOCKET;
                break;
            }
            if (bind(fd, (struct sockaddr*)&local_addr, sizeof(local_addr)) != 0) {
                CLOSESOCK(fd);
                fd = INVALID_SOCKET;
                continue;
            }
        }
        if (connect(fd, p->ai_addr, (int)p->ai_addrlen) == 0) {
            char addrbuf[64] = "";
            if (p->ai_family == AF_INET) {
                struct sockaddr_in *ipv4 = (struct sockaddr_in*)p->ai_addr;
                inet_ntop(AF_INET, &ipv4->sin_addr, addrbuf, sizeof(addrbuf));
            } else if (p->ai_family == AF_INET6) {
                struct sockaddr_in6 *ipv6 = (struct sockaddr_in6*)p->ai_addr;
                inet_ntop(AF_INET6, &ipv6->sin6_addr, addrbuf, sizeof(addrbuf));
            }
            net_logf("CONNECT", "%s:%u (fd=%d)", addrbuf, (unsigned)remote_port, (int)fd);
            break;
        }
        CLOSESOCK(fd);
        fd = INVALID_SOCKET;
    }
    freeaddrinfo(res);
    return (int)fd;
}

int net_connect(const char *ip, uint16_t port) {
    return net_connect_ex(ip, port, NULL, 0);
}

int net_send_line(int fd, const char *line) {
    if (net_verbose) {
        net_logf("SEND", "fd=%d %s", fd, line);
    }
    size_t n = strlen(line);
    size_t sent = 0;
    while (sent < n) {
#ifdef _WIN32
        int rc = send(fd, line + sent, (int)(n - sent), 0);
#else
        int rc = (int)send(fd, line + sent, n - sent, 0);
#endif
        if (rc <= 0) return -1;
        sent += rc;
    }
    const char nl = '\n';
    if (send(fd, &nl, 1, 0) != 1) return -1;
    return 0;
}

int net_recv_line(int fd, char *buf, int buflen) {
    int pos = 0;
    while (pos < buflen - 1) {
        char c;
#ifdef _WIN32
        int rc = recv(fd, &c, 1, 0);
#else
        int rc = (int)recv(fd, &c, 1, 0);
#endif
        if (rc <= 0) return -1;
        if (c == '\n') break;
        buf[pos++] = c;
    }
    buf[pos] = '\0';
    if (net_verbose) {
        net_logf("RECV", "fd=%d %s", fd, buf);
    }
    return pos;
}

void net_close(int fd) {
    if (net_verbose) {
        net_logf("CLOSE", "fd=%d", fd);
    }
    CLOSESOCK(fd);
}

