
#ifndef SOCKS_PAHO_MQTT_NET_H
#define SOCKS_PAHO_MQTT_NET_H
#include "Clients.h"

#ifndef WIN32
#include <netdb.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/tcp.h>
#include <ifaddrs.h>
#else
#include <winsock2.h>
#include <ws2tcpip.h>
#endif

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>


#ifdef WITH_WRAP
#include <tcpd.h>
#endif

#ifdef HAVE_NETINET_IN_H
#  include <netinet/in.h>
#endif

#ifdef WITH_UNIX_SOCKETS
#  include "sys/stat.h"
#  include "sys/un.h"
#endif

#ifdef __QNX__
#include <net/netbyte.h>
#endif

int socks5_connect(Clients *aClient, const char *host, int port, int keepalive);
int socks5_reconnect(Clients *aClient, bool blocking);
int net__socket_connect(Clients *aClient, const char *host, uint16_t port, const char* bind_address, bool blocking);
int net__try_connect(const char *host, uint16_t port, SOCKET *sock, const char *bind_address, bool blocking);
int net__try_connect_tcp(const char *host, uint16_t port, SOCKET *sock, const char *bind_address, bool blocking);
int net__socket_nonblock(SOCKET *sock);
//int net__socket_connect_step3(Clients *aClient, const char *host);
//int net__socketpair(mosq_sock_t *pairR, mosq_sock_t *pairW);
#endif
