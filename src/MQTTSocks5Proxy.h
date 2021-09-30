
#ifndef SOCKS_PAHO_MQTT_H
#define SOCKS_PAHO_MQTT_H
#include "Clients.h"
#include "Thread.h"

#define PAHOMQTT_MSB(A) (uint8_t)((A & 0xFF00) >> 8)
#define PAHOMQTT_LSB(A) (uint8_t)(A & 0x00FF)
typedef SSIZE_T ssize_t;

ssize_t net__read(Clients *aClient, char *buf, size_t count);
ssize_t net__write(Clients *aClient, char *buf, size_t count);
void packet__cleanup(pahomqtt_socks5_packet *packet);

int packet__write(Clients *aClient);
int packet__queue(Clients *aClient, pahomqtt_socks5_packet *packet);

int socks5__send(Clients *aClient);
int socks5__read(Clients *aClient);

thread_return_type WINAPI Socks5_ReadThread(void* n);
#endif
