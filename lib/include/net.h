#ifndef NET_H
#define NET_H

#include <stdint.h>

void net_set_verbose(int enabled);

int net_listen(uint16_t port);
int net_listen_addr(const char *host, uint16_t port);
int net_accept(int server_fd, char *peer_ip_buf, int ip_buf_len, uint16_t *peer_port);

int net_connect(const char *ip, uint16_t port);
int net_connect_ex(const char *remote_host, uint16_t remote_port, const char *local_host, uint16_t local_port);

int net_get_local_addr(int fd, char *ip_buf, int ip_buf_len, uint16_t *port_out);

int net_send_line(int fd, const char *line);
int net_recv_line(int fd, char *buf, int buflen);
void net_close(int fd);

#endif

