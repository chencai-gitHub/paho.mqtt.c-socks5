
#include "MQTTSocks5Net.h"
#include "MQTTSocks5Proxy.h"

int net__socket_nonblock(SOCKET *sock)
{

	unsigned long opt = 1;
	if (ioctlsocket(*sock, FIONBIO, &opt)) { //设置为非阻塞模式
		COMPAT_CLOSE(*sock);
		*sock = INVALID_SOCKET;
		return PAHOMQTT_ERR_ERRNO;
	}
	return PAHOMQTT_ERR_SUCCESS;
}

int net__try_connect_tcp(const char *host, uint16_t port, SOCKET *sock, const char *bind_address, bool blocking)
{
	struct addrinfo hints;
	struct addrinfo *ainfo, *rp;
	struct addrinfo *ainfo_bind, *rp_bind;
	int s;
	int rc = PAHOMQTT_ERR_SUCCESS;

	ainfo_bind = NULL;

	*sock = INVALID_SOCKET;
	memset(&hints, 0, sizeof(struct addrinfo));
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;

	s = getaddrinfo(host, NULL, &hints, &ainfo);
	if (s) {
		errno = s;
		return PAHOMQTT_ERR_EAI;
	}

	if (bind_address) {
		s = getaddrinfo(bind_address, NULL, &hints, &ainfo_bind);
		if (s) {
			freeaddrinfo(ainfo);
			errno = s;
			return PAHOMQTT_ERR_EAI;
		}
	}

	for (rp = ainfo; rp != NULL; rp = rp->ai_next) {
		*sock = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
		if (*sock == INVALID_SOCKET) continue;

		if (rp->ai_family == AF_INET) {
			((struct sockaddr_in *)rp->ai_addr)->sin_port = htons(port);
		}
		else if (rp->ai_family == AF_INET6) {
			((struct sockaddr_in6 *)rp->ai_addr)->sin6_port = htons(port);
		}
		else {
			COMPAT_CLOSE(*sock);
			*sock = INVALID_SOCKET;
			continue;
		}

		if (bind_address) {
			for (rp_bind = ainfo_bind; rp_bind != NULL; rp_bind = rp_bind->ai_next) {
				if (bind(*sock, rp_bind->ai_addr, rp_bind->ai_addrlen) == 0) {
					break;
				}
			}
			if (!rp_bind) {
				COMPAT_CLOSE(*sock);
				*sock = INVALID_SOCKET;
				continue;
			}
		}

		if (!blocking) {
			/* Set non-blocking */
			if (net__socket_nonblock(sock)) {
				continue;
			}
		}

		rc = connect(*sock, rp->ai_addr, rp->ai_addrlen);
		if (rc == 0)
		{
		}
		
#ifdef WIN32
		errno = WSAGetLastError();
#endif
		if (rc == 0 || errno == EINPROGRESS || errno == COMPAT_EWOULDBLOCK) {
			if (rc < 0 && (errno == EINPROGRESS || errno == COMPAT_EWOULDBLOCK)) {
				rc = PAHOMQTT_ERR_CONN_PENDING;
			}

			if (blocking) {
				//return; //test
				/* Set non-blocking */
				if (net__socket_nonblock(sock)) {
					continue;
				}
			}
			break;
		}

		COMPAT_CLOSE(*sock);
		*sock = INVALID_SOCKET;
	}
	freeaddrinfo(ainfo);
	if (bind_address) {
		freeaddrinfo(ainfo_bind);
	}
	if (!rp) {
		return PAHOMQTT_ERR_ERRNO;
	}
	return rc;
}

int net__try_connect(const char *host, uint16_t port, SOCKET *sock, const char *bind_address, bool blocking)
{
	if (port == 0) 
	{
		return PAHOMQTT_ERR_NOT_SUPPORTED;
	}
	else {
		return net__try_connect_tcp(host, port, sock, bind_address, blocking);
	}
}

int net__socket_connect(Clients *aClient, const char *host, uint16_t port, const char* bind_address, bool blocking)
{
	SOCKET sock = INVALID_SOCKET;
	int rc;
	if (!host) return PAHOMQTT_ERR_INVAL;

	rc = net__try_connect(host, port, &sock, NULL, blocking);
	if (rc > 0) return rc;

	aClient->net.socket = sock;

	//if (mosq->tcp_nodelay) //应该使用true
	{
		int flag = 1;
		if (setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, (const void*)&flag, sizeof(int)) != 0) 
		{
			//log__printf(mosq, MOSQ_LOG_WARNING, "Warning: Unable to set TCP_NODELAY.");
		}
	}
	return rc;
}

int  socks5_reconnect(Clients *aClient, bool blocking)
{
	int rc = -1;
	if (aClient->socks5_host) {
		rc = net__socket_connect(aClient, aClient->socks5_host, aClient->socks5_port, NULL, blocking);
	}

	if (rc > 0)
	{
		pahomqtt_set_socks5_state(aClient, pahomqtt_cs_connect_pending);
		return rc;
	}

	//if (aClient->socks5_host) 
	//{
	//	pahomqtt_set_socks5_state(aClient, pahomqtt_cs_socks5_new);

	//	Thread_start(Socks5_ReadThread, (void*)aClient);

	//	rc = socks5__send(aClient);
	//	if (rc != PAHOMQTT_ERR_SUCCESS)
	//	{
	//		
	//	}
	//	return rc;
	//}
	return rc;
}

int  socks5_connect(Clients *aClient, const char *host, int port, int keepalive)
{
	return  socks5_reconnect(aClient, true);
}


//#ifndef WITH_BROKER
//int net__socketpair(mosq_sock_t *pairR, mosq_sock_t *pairW)
//{
//#ifdef WIN32
//	int family[2] = { AF_INET, AF_INET6 };
//	int i;
//	struct sockaddr_storage ss;
//	struct sockaddr_in *sa = (struct sockaddr_in *)&ss;
//	struct sockaddr_in6 *sa6 = (struct sockaddr_in6 *)&ss;
//	socklen_t ss_len;
//	mosq_sock_t spR, spW;
//
//	mosq_sock_t listensock;
//
//	*pairR = INVALID_SOCKET;
//	*pairW = INVALID_SOCKET;
//
//	for (i = 0; i < 2; i++) {
//		memset(&ss, 0, sizeof(ss));
//		if (family[i] == AF_INET) {
//			sa->sin_family = family[i];
//			sa->sin_addr.s_addr = htonl(INADDR_LOOPBACK); //127.0.0.1
//			sa->sin_port = 0;
//			ss_len = sizeof(struct sockaddr_in);
//		}
//		else if (family[i] == AF_INET6) {
//			sa6->sin6_family = family[i];
//			sa6->sin6_addr = in6addr_loopback;
//			sa6->sin6_port = 0;
//			ss_len = sizeof(struct sockaddr_in6);
//		}
//		else {
//			return PAHOMQTT_ERR_INVAL;
//		}
//
//		//创建本地服务端socket
//		listensock = socket(family[i], SOCK_STREAM, IPPROTO_TCP);
//		if (listensock == -1) {
//			continue;
//		}
//
//		//绑定IP端口号
//		if (bind(listensock, (struct sockaddr *)&ss, ss_len) == -1) {
//			COMPAT_CLOSE(listensock);
//			continue;
//		}
//
//		/*
//		一般服务端会有两个套接字，一个负责监听本地端口，一个负责与客户端通讯
//		监听本地127.0.0.1:0,该socket是被动socket只能进行数据接收，且同一时间只能有一个客户端连接
//		*/
//		if (listen(listensock, 1) == -1) {
//			COMPAT_CLOSE(listensock);
//			continue;
//		}
//		memset(&ss, 0, sizeof(ss));
//		ss_len = sizeof(ss);
//		if (getsockname(listensock, (struct sockaddr *)&ss, &ss_len) < 0) {
//			COMPAT_CLOSE(listensock);
//			continue;
//		}
//
//		if (family[i] == AF_INET) {
//			sa->sin_family = family[i];
//			sa->sin_addr.s_addr = htonl(INADDR_LOOPBACK);
//			ss_len = sizeof(struct sockaddr_in);
//		}
//		else if (family[i] == AF_INET6) {
//			sa6->sin6_family = family[i];
//			sa6->sin6_addr = in6addr_loopback;
//			ss_len = sizeof(struct sockaddr_in6);
//		}
//
//		//创建本地客户端socket
//		spR = socket(family[i], SOCK_STREAM, IPPROTO_TCP);
//		if (spR == -1) {
//			COMPAT_CLOSE(listensock);
//			continue;
//		}
//		//将spR设置为非阻塞模式，在读写操作时，有无数据都会立即返回，防止读无数据时等待，读写操作需要根据返回值进行后续逻辑处理
//		if (net__socket_nonblock(&spR)) {
//			COMPAT_CLOSE(listensock);
//			continue;
//		}
//
//		//本地客服端连接本地服务端，负责主动读取信息
//		if (connect(spR, (struct sockaddr *)&ss, ss_len) < 0) {
//#ifdef WIN32
//			errno = WSAGetLastError();
//#endif
//			if (errno != EINPROGRESS && errno != COMPAT_EWOULDBLOCK) {
//				COMPAT_CLOSE(spR);
//				COMPAT_CLOSE(listensock);
//				continue;
//			}
//		}
//		/*
//		本地服务端的主动socket(阻塞等待客户端连接，成功后三次握手结束，可以通讯了)
//		负责与本地客户端通讯，第二个参数为NULL标识不记录客户端地址
//		负责主动传输数据给客户端socket spR
//		*/
//		spW = accept(listensock, NULL, 0);
//		if (spW == -1) {
//#ifdef WIN32
//			errno = WSAGetLastError();
//#endif
//			if (errno != EINPROGRESS && errno != COMPAT_EWOULDBLOCK) {
//				COMPAT_CLOSE(spR);
//				COMPAT_CLOSE(listensock);
//				continue;
//			}
//		}
//
//		//将spW设置为非阻塞模式，读写操作需要根据返回值进行后续逻辑处理
//		if (net__socket_nonblock(&spW)) {
//			COMPAT_CLOSE(spR);
//			COMPAT_CLOSE(listensock);
//			continue;
//		}
//		COMPAT_CLOSE(listensock);
//
//		*pairR = spR;
//		*pairW = spW;
//		return PAHOMQTT_ERR_SUCCESS;
//	}
//	return PAHOMQTT_ERR_UNKNOWN;
//#else
//	int sv[2];
//
//	*pairR = INVALID_SOCKET;
//	*pairW = INVALID_SOCKET;
//
//	if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == -1) {
//		return PAHOMQTT_ERR_ERRNO;
//	}
//	if (net__socket_nonblock(&sv[0])) {
//		COMPAT_CLOSE(sv[1]);
//		return PAHOMQTT_ERR_ERRNO;
//	}
//	if (net__socket_nonblock(&sv[1])) {
//		COMPAT_CLOSE(sv[0]);
//		return PAHOMQTT_ERR_ERRNO;
//	}
//	*pairR = sv[0];
//	*pairW = sv[1];
//	return PAHOMQTT_ERR_SUCCESS;
//#endif
//}
//#endif