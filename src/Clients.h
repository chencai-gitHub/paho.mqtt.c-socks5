/*******************************************************************************
 * Copyright (c) 2009, 2020 IBM Corp.
 *
 * All rights reserved. This program and the accompanying materials
 * are made available under the terms of the Eclipse Public License v2.0
 * and Eclipse Distribution License v1.0 which accompany this distribution. 
 *
 * The Eclipse Public License is available at 
 *    https://www.eclipse.org/legal/epl-2.0/
 * and the Eclipse Distribution License is available at 
 *   http://www.eclipse.org/org/documents/edl-v10.php.
 *
 * Contributors:
 *    Ian Craggs - initial API and implementation and/or initial documentation
 *    Ian Craggs - add SSL support
 *    Ian Craggs - fix for bug 413429 - connectionLost not called
 *    Ian Craggs - change will payload to binary
 *    Ian Craggs - password to binary
 *    Ian Craggs - MQTT 5 support
 *******************************************************************************/

#if !defined(CLIENTS_H)
#define CLIENTS_H

#include <stdint.h>
#include "MQTTTime.h"

#define HAVE_STRUCT_TIMESPEC
#include "pthread.h"
#if defined(OPENSSL)
#if defined(_WIN32) || defined(_WIN64)
#include <winsock2.h>
#endif
#include <openssl/ssl.h>
#endif
#include "MQTTClient.h"
#include "LinkedList.h"
#include "MQTTClientPersistence.h"

#ifdef WIN32
#  define COMPAT_CLOSE(a) closesocket(a)
#  define COMPAT_ECONNRESET WSAECONNRESET
#  define COMPAT_EINTR WSAEINTR
#  define COMPAT_EWOULDBLOCK WSAEWOULDBLOCK
#else
#  define COMPAT_CLOSE(a) close(a)
#  define COMPAT_ECONNRESET ECONNRESET
#  define COMPAT_EINTR EINTR
#  define COMPAT_EWOULDBLOCK EWOULDBLOCK
#endif

enum pahomqtt_err_t {
	PAHOMQTT_ERR_AUTH_CONTINUE = -4,
	PAHOMQTT_ERR_NO_SUBSCRIBERS = -3,
	PAHOMQTT_ERR_SUB_EXISTS = -2,
	PAHOMQTT_ERR_CONN_PENDING = -1,
	PAHOMQTT_ERR_SUCCESS = 0,
	PAHOMQTT_ERR_NOMEM = 1,
	PAHOMQTT_ERR_PROTOCOL = 2,
	PAHOMQTT_ERR_INVAL = 3,
	PAHOMQTT_ERR_NO_CONN = 4,
	PAHOMQTT_ERR_CONN_REFUSED = 5,
	PAHOMQTT_ERR_NOT_FOUND = 6,
	PAHOMQTT_ERR_CONN_LOST = 7,
	PAHOMQTT_ERR_TLS = 8,
	PAHOMQTT_ERR_PAYLOAD_SIZE = 9,
	PAHOMQTT_ERR_NOT_SUPPORTED = 10,
	PAHOMQTT_ERR_AUTH = 11,
	PAHOMQTT_ERR_ACL_DENIED = 12,
	PAHOMQTT_ERR_UNKNOWN = 13,
	PAHOMQTT_ERR_ERRNO = 14,
	PAHOMQTT_ERR_EAI = 15,
	PAHOMQTT_ERR_PROXY = 16,
	PAHOMQTT_ERR_PLUGIN_DEFER = 17,
	PAHOMQTT_ERR_MALFORMED_UTF8 = 18,
	PAHOMQTT_ERR_KEEPALIVE = 19,
	PAHOMQTT_ERR_LOOKUP = 20,
	PAHOMQTT_ERR_MALFORMED_PACKET = 21,
	PAHOMQTT_ERR_DUPLICATE_PROPERTY = 22,
	PAHOMQTT_ERR_TLS_HANDSHAKE = 23,
	PAHOMQTT_ERR_QOS_NOT_SUPPORTED = 24,
	PAHOMQTT_ERR_OVERSIZE_PACKET = 25,
	PAHOMQTT_ERR_OCSP = 26,
	PAHOMQTT_ERR_TIMEOUT = 27,
	PAHOMQTT_ERR_RETAIN_NOT_SUPPORTED = 28,
	PAHOMQTT_ERR_TOPIC_ALIAS_INVALID = 29,
	PAHOMQTT_ERR_ADMINISTRATIVE_ACTION = 30,
	PAHOMQTT_ERR_ALREADY_EXISTS = 31,
	PAHOMQTT_ERR_NOT_FINISHED = 32,
};

enum pahomqtt_client_socks5_state {
	pahomqtt_cs_new = 0,  //代理成功
	pahomqtt_cs_connected = 1,
	pahomqtt_cs_disconnecting = 2,
	pahomqtt_cs_active = 3,
	pahomqtt_cs_connect_pending = 4, //待连接
	pahomqtt_cs_connect_srv = 5,
	pahomqtt_cs_disconnect_ws = 6,
	pahomqtt_cs_disconnected = 7,
	pahomqtt_cs_socks5_new = 8, //创建socks5
	pahomqtt_cs_socks5_start = 9, //开始socks5认证
	pahomqtt_cs_socks5_request = 10, //代理请求
	pahomqtt_cs_socks5_reply = 11,
	pahomqtt_cs_socks5_auth_ok = 12, //socks5认证成功
	pahomqtt_cs_socks5_userpass_reply = 13, //用户名密码认证反回
	pahomqtt_cs_socks5_send_userpass = 14, //用户名密码认证
	pahomqtt_cs_expiring = 15,
	pahomqtt_cs_duplicate = 17, /* client that has been taken over by another with the same id */
	pahomqtt_cs_disconnect_with_will = 18,
	pahomqtt_cs_disused = 19, /* client that has been added to the disused list to be freed */
	pahomqtt_cs_authenticating = 20, /* Client has sent CONNECT but is still undergoing extended authentication */
	pahomqtt_cs_reauthenticating = 21, /* Client is undergoing reauthentication and shouldn't do anything else until complete */
	pahomqtt_cs_error = 22
};

typedef struct pahomqtt_socks5_packet {
	uint8_t *payload;
	struct pahomqtt_socks5_packet *next;
	uint32_t remaining_mult;
	uint32_t remaining_length;
	uint32_t packet_length;
	uint32_t to_process; //发送进度，记录还未发送多少字节，缺省为packet_length
	uint32_t pos;//组包或者发送时用到，发送时记录发送到什么位置
	uint16_t mid;//消息id
	uint8_t command;
	int8_t remaining_count;
}pahomqtt_socks5_packet;

/**
 * Stored publication data to minimize copying
 */
typedef struct
{
	char *topic;
	int topiclen;
	char* payload;
	int payloadlen;
	int refcount;
	uint8_t mask[4];
} Publications;

/**
 * Client publication message data
 */
typedef struct
{
	int qos;
	int retain;
	int msgid;
	int MQTTVersion;
	MQTTProperties properties;
	Publications *publish;
	START_TIME_TYPE lastTouch;		    /**> used for retry and expiry */
	char nextMessageType;	/**> PUBREC, PUBREL, PUBCOMP */
	int len;				/**> length of the whole structure+data */
} Messages;

/**
 * Client will message data
 */
typedef struct
{
	char *topic;
	int payloadlen;
	void *payload;
	int retained;
	int qos;
} willMessages;

typedef struct
{
	int socket;
	START_TIME_TYPE lastSent;
	START_TIME_TYPE lastReceived;
	START_TIME_TYPE lastPing;
#if defined(OPENSSL)
	SSL* ssl;
	SSL_CTX* ctx;
	char *https_proxy;
	char *https_proxy_auth;
#endif
	char *http_proxy;
	char *http_proxy_auth;
	int websocket; /**< socket has been upgraded to use web sockets */
	char *websocket_key;

	/* socks5 proxy*/
	char *socks5_host;
	uint16_t socks5_port;
	char *socks5_username;
	char *socks5_password;

	const MQTTClient_nameValue* httpHeaders;
} networkHandles;


/* connection states */
/** no connection in progress, see connected value */
#define NOT_IN_PROGRESS  0x0
/** TCP connection in progress */
#define TCP_IN_PROGRESS  0x1
/** SSL connection in progress */
#define SSL_IN_PROGRESS  0x2
/** Websocket connection in progress */
#define WEBSOCKET_IN_PROGRESS   0x3
/** TCP completed, waiting for MQTT ACK */
#define WAIT_FOR_CONNACK 0x4
/** Proxy connection in progress */
#define PROXY_CONNECT_IN_PROGRESS 0x5
/** Socks5 Proxy connection in progress */
#define SOCKS5_PROXY_CONNECT_IN_PROGRESS 0x6
/** Disconnecting */
#define DISCONNECTING    -2

/**
 * Data related to one client
 */
typedef struct
{
	char* clientID;					      /**< the string id of the client */
	const char* username;					/**< MQTT v3.1 user name */
	int passwordlen;              /**< MQTT password length */
	const void* password;					/**< MQTT v3.1 binary password */
	unsigned int cleansession : 1;	/**< MQTT V3 clean session flag */
	unsigned int cleanstart : 1;		/**< MQTT V5 clean start flag */
	unsigned int connected : 1;		/**< whether it is currently connected */
	unsigned int good : 1; 			  /**< if we have an error on the socket we turn this off */
	unsigned int ping_outstanding : 1;
	signed int connect_state : 4;
	networkHandles net;             /**< network info for this client */
	int msgID;                      /**< the MQTT message id */
	int keepAliveInterval;          /**< the MQTT keep alive interval */
	int retryInterval;
	int maxInflightMessages;        /**< the max number of inflight outbound messages we allow */
	willMessages* will;             /**< the MQTT will message, if any */
	List* inboundMsgs;              /**< inbound in flight messages */
	List* outboundMsgs;				/**< outbound in flight messages */
	List* messageQueue;             /**< inbound complete but undelivered messages */
	unsigned int qentry_seqno;
	void* phandle;                  /**< the persistence handle */
	MQTTClient_persistence* persistence; /**< a persistence implementation */
    MQTTPersistence_beforeWrite* beforeWrite; /**< persistence write callback */
    MQTTPersistence_afterRead* afterRead; /**< persistence read callback */
    void* beforeWrite_context;      /**< context to be used with the persistence beforeWrite callbacks */
    void* afterRead_context;        /**< context to be used with the persistence afterRead callback */
	void* context;                  /**< calling context - used when calling disconnect_internal */
	int MQTTVersion;                /**< the version of MQTT being used, 3, 4 or 5 */
	int sessionExpiry;              /**< MQTT 5 session expiry */
	char* httpProxy;                /**< HTTP proxy for websockets */
	char* httpsProxy;               /**< HTTPS proxy for websockets */

	/*
	 * socks5 proxy
	*/
	char *socks5_host;
	uint16_t socks5_port;
	char *socks5_username;
	char *socks5_password;

	enum pahomqtt_client_socks5_state socks5State;

	pthread_mutex_t socks5_state_mutex;
	pthread_mutex_t out_packet_mutex;
	pahomqtt_socks5_packet in_packet;
	pahomqtt_socks5_packet *out_packet;
	pahomqtt_socks5_packet *out_packet_last;

	char* serverURI;
	int* mqttAsync_tostop;

#if defined(OPENSSL)
	MQTTClient_SSLOptions *sslopts; /**< the SSL/TLS connect options */
	SSL_SESSION* session;           /**< SSL session pointer for fast handhake */
#endif
} Clients;

int clientIDCompare(void* a, void* b);
int clientSocketCompare(void* a, void* b);

int pahomqtt_set_socks5_state(Clients *aClient, enum pahomqtt_client_socks5_state state);
enum pahomqtt_client_socks5_state pahomqtt_get_socks5_state(Clients *aClient);

/**
 * Configuration data related to all clients
 */
typedef struct
{
	const char* version;
	List* clients;
} ClientStates;

#endif
