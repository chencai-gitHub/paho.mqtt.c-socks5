
#include <errno.h>
#include <string.h>
#include <limits.h>
#ifdef WIN32
#  include <ws2tcpip.h>
#elif __QNX__
#  include <sys/socket.h>
#  include <arpa/inet.h>
#  include <netinet/in.h>
#else
#  include <arpa/inet.h>
#endif
#if defined(__FreeBSD__) || defined(__OpenBSD__)
#  include <sys/socket.h>
#  include <netinet/in.h>
#endif

//#include <ctime>

#include "MQTTSocks5Proxy.h"
#include "memory_pahomqtt.h"
#include <assert.h>
#include <time.h>

#  define G_BYTES_SENT_INC(A)

#define SOCKS_AUTH_NONE 0x00U
#define SOCKS_AUTH_GSS 0x01U
#define SOCKS_AUTH_USERPASS 0x02U
#define SOCKS_AUTH_NO_ACCEPTABLE 0xFFU

#define SOCKS_ATYPE_IP_V4 1U /* four bytes */
#define SOCKS_ATYPE_DOMAINNAME 3U /* one byte length, followed by fqdn no null, 256 max chars */
#define SOCKS_ATYPE_IP_V6 4U /* 16 bytes */

#define SOCKS_REPLY_SUCCEEDED 0x00U
#define SOCKS_REPLY_GENERAL_FAILURE 0x01U
#define SOCKS_REPLY_CONNECTION_NOT_ALLOWED 0x02U
#define SOCKS_REPLY_NETWORK_UNREACHABLE 0x03U
#define SOCKS_REPLY_HOST_UNREACHABLE 0x04U
#define SOCKS_REPLY_CONNECTION_REFUSED 0x05U
#define SOCKS_REPLY_TTL_EXPIRED 0x06U
#define SOCKS_REPLY_COMMAND_NOT_SUPPORTED 0x07U
#define SOCKS_REPLY_ADDRESS_TYPE_NOT_SUPPORTED 0x08U


ssize_t net__read(Clients *aClient, char *buf, size_t count)
{
	assert(aClient);
	/* Call normal read/recv */
	return recv(aClient->net.socket, buf, count, 0); //第4个参数0表示剪切缓冲区数据
}

ssize_t net__write(Clients *aClient, char *buf, size_t count)
{
	assert(aClient);

	return send(aClient->net.socket, buf, count, 0);
}

void packet__cleanup(pahomqtt_socks5_packet *packet)
{
	if (!packet) return;

	/* Free data and reset values */
	packet->command = 0;
	packet->remaining_count = 0;
	packet->remaining_mult = 1;
	packet->remaining_length = 0;
	pahomqtt__free(packet->payload);
	packet->payload = NULL;
	packet->to_process = 0;
	packet->pos = 0;
}

int packet__write(Clients *aClient)
{
	ssize_t write_length;
	pahomqtt_socks5_packet *packet;
	enum pahomqtt_client_socks5_state state;

	if (!aClient) return PAHOMQTT_ERR_INVAL;
	if (aClient->net.socket == INVALID_SOCKET) 
		return PAHOMQTT_ERR_NO_CONN;

	state = pahomqtt_get_socks5_state(aClient);

	if (state == pahomqtt_cs_new) 
	{
		return PAHOMQTT_ERR_SUCCESS;
	}

	pthread_mutex_lock(&aClient->out_packet_mutex);

	//循环发送队列里的消息，发送成功一个删一个
	while (aClient->out_packet) 
	{
		packet = aClient->out_packet;

		while (packet->to_process > 0)
		{
			//发送数据到远端服务器，返回值表示发送成功的字节数
			write_length = net__write(aClient, &(packet->payload[packet->pos]), packet->to_process); //起始地址与发送长度
			if (write_length > 0) {
				G_BYTES_SENT_INC(write_length);
				packet->to_process -= (uint32_t)write_length;
				packet->pos += (uint32_t)write_length;
			}
			else {

				errno = WSAGetLastError();

				if (errno == EAGAIN || errno == COMPAT_EWOULDBLOCK || errno == WSAENOTCONN) 
				{
					pthread_mutex_unlock(&aClient->out_packet_mutex);
					return PAHOMQTT_ERR_SUCCESS;
				}
				else 
				{
					switch (errno) {
					case COMPAT_ECONNRESET:
						pthread_mutex_unlock(&aClient->out_packet_mutex);
						return PAHOMQTT_ERR_CONN_LOST; //返回调用地方，调用断连操作，告诉上层需要重连
					case COMPAT_EINTR:
						pthread_mutex_unlock(&aClient->out_packet_mutex);
						return PAHOMQTT_ERR_SUCCESS;
					default:
						pthread_mutex_unlock(&aClient->out_packet_mutex);
						return PAHOMQTT_ERR_ERRNO;
					}
				}
			}
		}

		/* Free data and reset values */
		if (aClient->out_packet) {
			aClient->out_packet = aClient->out_packet->next;
			if (!aClient->out_packet) {
				aClient->out_packet_last = NULL;
			}
		}

		packet__cleanup(packet);
		pahomqtt__free(packet);

	}
	pthread_mutex_unlock(&aClient->out_packet_mutex);
	return PAHOMQTT_ERR_SUCCESS;
}

int packet__queue(Clients *aClient, struct pahomqtt_socks5_packet *packet)
{
	assert(aClient);
	assert(packet);

	packet->pos = 0; //记录发送到的位置
	packet->to_process = packet->packet_length; //记录未发送字节数

	packet->next = NULL;
	pthread_mutex_lock(&aClient->out_packet_mutex);
	if (aClient->out_packet) {
		aClient->out_packet_last->next = packet;
	}
	else {
		aClient->out_packet = packet;
	}
	aClient->out_packet_last = packet;
	pthread_mutex_unlock(&aClient->out_packet_mutex);

	return packet__write(aClient);
}


int socks5__send(Clients *aClient)
{
	pahomqtt_socks5_packet *packet;
	size_t slen;
	uint8_t ulen, plen;

	struct in_addr addr_ipv4;
	struct in6_addr addr_ipv6;
	int ipv4_pton_result;
	int ipv6_pton_result;
	enum pahomqtt_client_socks5_state state;

	state = pahomqtt_get_socks5_state(aClient);

	//创建socks5
	if(state == pahomqtt_cs_socks5_new)
	{
		packet = pahomqtt__calloc(1, sizeof(struct pahomqtt_socks5_packet));
		if(!packet) return PAHOMQTT_ERR_NOMEM;

		/*
			**客户端** 请求第一步,发送认证请求
			+----+----------+----------+
			| VER|NMETHODS  | METHODS  |
			+----+----------+----------+
			| 1  |    1     | 1 - 255  |
			+----+----------+----------+
			VER 表示版本号:sock5 为 X'05'
			NMETHODS（认证数量选择，如1种2种等）
			METHODS具体的认证类型数组，NMETHODS有几种，就对应几种认证方式
			目前定义的METHOD有以下几种:
			X'00'  无需认证
			X'01'  通用安全服务应用程序(GSSAPI)
			X'02'  用户名/密码 auth (USERNAME/PASSWORD)
			X'03'- X'7F' IANA 分配(IANA ASSIGNED)
			X'80'- X'FE' 私人方法保留(RESERVED FOR PRIVATE METHODS)
			X'FF'  无可接受方法(NO ACCEPTABLE METHODS)
		*/
		if(aClient->socks5_username){
			packet->packet_length = 4;
		}else{
			packet->packet_length = 3;
		}

		packet->payload = pahomqtt__malloc(sizeof(uint8_t)*packet->packet_length);

		packet->payload[0] = 0x05;
		if(aClient->socks5_username){
			packet->payload[1] = 2;
			packet->payload[2] = SOCKS_AUTH_NONE;
			packet->payload[3] = SOCKS_AUTH_USERPASS;
		}else{
			packet->payload[1] = 1;
			packet->payload[2] = SOCKS_AUTH_NONE;
		}

		//标记开始socks5认证
		pahomqtt_set_socks5_state(aClient, pahomqtt_cs_socks5_start);

		/*
			构建准备接收服务器回馈的数据包（服务器回馈的消息存在这里）

			**服务器** 响应第一步
			服务器从客户端发来的消息中选择一种方法作为返回
			服务器从METHODS给出的方法中选出一种，发送一个METHOD（方法）选择报文：
			+----+--------+
			|VER | METHOD |
			+----+--------+
			|  1 | 　1　 　|
			+----+--------+
		*/
		aClient->in_packet.pos = 0;
		aClient->in_packet.packet_length = 2;
		aClient->in_packet.to_process = 2;
		aClient->in_packet.payload = pahomqtt__malloc(sizeof(uint8_t)*2);
		if(!aClient->in_packet.payload){
			pahomqtt__free(packet->payload);
			pahomqtt__free(packet);
			return PAHOMQTT_ERR_NOMEM;
		}

		return packet__queue(aClient, packet);
	}
	else if(state == pahomqtt_cs_socks5_auth_ok) //socks5认证成功
	{
		packet = pahomqtt__calloc(1, sizeof(struct pahomqtt_socks5_packet));
		if(!packet) return PAHOMQTT_ERR_NOMEM;

		ipv4_pton_result = inet_pton(AF_INET, aClient->serverURI, &addr_ipv4);
		ipv6_pton_result = inet_pton(AF_INET6, aClient->serverURI, &addr_ipv6);

		/*
			SOCKS5代理请求报文如下表所示:
			+----+-----+-------+------+----------+----------+
			| VER| CMD | RSV   | ATYP |  DST.ADDR|  DST.PORT|
			+----+-----+-------+------+----------+----------+
			| 1  | 1   | X'00' | 1    | variable |      2   |
			+----+-----+-------+------+----------+----------+

			各个字段含义如下:
			VER  版本号X'05'
			CMD：
				1. CONNECT X'01'
				2. BIND    X'02'
				3. UDP ASSOCIATE X'03'
			RSV  保留字段
			ATYP IP类型
				1.IPV4 X'01'
				2.DOMAINNAME X'03'
				3.IPV6 X'04'
			DST.ADDR 目标地址
				1.如果是IPv4地址，是4字节数据
				2.如果是FQDN，比如"www.nsfocus.net"，这里将是:
					0F 77 77 77 2E 6E 73 66 6F 63 75 73 2E 6E 65 74
					注意，没有结尾的NUL字符，非ASCIZ串，第一字节是长度域
				3.如果是IPv6地址，这里是16字节数据。
			DST.PORT 目标端口big-endian存储（按网络次序排列）
		*/
		if(ipv4_pton_result == 1){
			packet->packet_length = 10;
			packet->payload = pahomqtt__malloc(sizeof(uint8_t)*packet->packet_length);
			if(!packet->payload){
				pahomqtt__free(packet);
				return PAHOMQTT_ERR_NOMEM;
			}
			packet->payload[3] = SOCKS_ATYPE_IP_V4;
			memcpy(&(packet->payload[4]), (const void*)&addr_ipv4, 4);
			packet->payload[4+4] = PAHOMQTT_MSB(1883); //高字节
			packet->payload[4+4+1] = PAHOMQTT_LSB(1883); //低字节

		}else if(ipv6_pton_result == 1){
			packet->packet_length = 22;
			packet->payload = pahomqtt__malloc(sizeof(uint8_t)*packet->packet_length);
			if(!packet->payload){
				pahomqtt__free(packet);
				return PAHOMQTT_ERR_NOMEM;
			}
			packet->payload[3] = SOCKS_ATYPE_IP_V6;
			memcpy(&(packet->payload[4]), (const void*)&addr_ipv6, 16);
			packet->payload[4+16] = PAHOMQTT_MSB(1883);
			packet->payload[4+16+1] = PAHOMQTT_LSB(1883);

		}else{
			slen = strlen(aClient->serverURI);
			if(slen > UCHAR_MAX){
				pahomqtt__free(packet);
				return PAHOMQTT_ERR_NOMEM;
			}
			packet->packet_length = 7U + (uint32_t)slen; //有一个字节用于存储长度
			packet->payload = pahomqtt__malloc(sizeof(uint8_t)*packet->packet_length);
			if(!packet->payload){
				pahomqtt__free(packet);
				return PAHOMQTT_ERR_NOMEM;
			}
			packet->payload[3] = SOCKS_ATYPE_DOMAINNAME;
			packet->payload[4] = (uint8_t)slen;
			memcpy(&(packet->payload[5]), aClient->serverURI, slen);
			packet->payload[5+slen] = PAHOMQTT_MSB(1883);
			packet->payload[6+slen] = PAHOMQTT_LSB(1883);
		}
		packet->payload[0] = 0x05;
		packet->payload[1] = 0x01;
		packet->payload[2] = 0x00;

		pahomqtt_set_socks5_state(aClient, pahomqtt_cs_socks5_request);

		/*
			**sock5代理服务器响应如下:**
			OCKS Server评估来自SOCKS Client的转发请求并发送响应报文:
			+----+-----+-------+------+----------+----------+
			|VER | REP |  RSV  | ATYP | BND.ADDR | BND.PORT |
			+----+-----+-------+------+----------+----------+
			| 1  |  1  | X'00' |  1   | Variable |    2     |
			+----+-----+-------+------+----------+----------+
			VER  版本号X'05'
			REP
				1. 0x00        成功
				2. 0x01        一般性失败
				3. 0x02        规则不允许转发
				4. 0x03        网络不可达
				5. 0x04        主机不可达
				6. 0x05        连接拒绝
				7. 0x06        TTL超时
				8. 0x07        不支持请求包中的CMD
				9. 0x08        不支持请求包中的ATYP
				10. 0x09-0xFF   unassigned
			RSV         保留字段，必须为0x00
			ATYP        用于指明BND.ADDR域的类型
			BND.ADDR    CMD相关的地址信息，不要为BND所迷惑，一个字节是长度域
			BND.PORT    CMD相关的端口信息，big-endian序的2字节数据
			
			**注意**
			假设CMD为CONNECT，SOCKS Client、SOCKS Server之间通信的相关四元组是:
			SOCKSCLIENT.ADDR，SOCKSCLIENT.PORT，SOCKSSERVER.ADDR，SOCKSSERVER.PORT

			一般SOCKSSERVER.PORT是1080/TCP。
			CONNECT请求包中的DST.ADDR/DST.PORT指明转发目的地。SOCKS Server可以靠
			DST.ADDR、DST.PORT、SOCKSCLIENT.ADDR、SOCKSCLIENT.PORT进行评估，以决定建立到转发
			目的地的TCP连接还是拒绝转发。

			假设规则允许转发并且成功建立到转发目的地的TCP连接，相关四元组是:
			BND.ADDR，BND.PORT，DST.ADDR，DST.PORT
			此时SOCKS Server向SOCKS Client发送的CONNECT响应包中将指明BND.ADDR/BND.PORT。
			注意，BND.ADDR可能不同于SOCKSSERVER.ADDR，SOCKS Server所在主机可能是多目
			(multi-homed)主机。

			假设拒绝转发或未能成功建立到转发目的地的TCP连接，CONNECT响应包中REP字段将指明具体原因。

			响应包中REP非零时表示失败，SOCKS Server必须在发送响应包后不久(不超过10s)关闭与
			SOCKS Client之间的TCP连接。
			响应包中REP为零时表示成功。之后SOCKS Client直接在当前TCP连接上发送待转发数据。
		*/
		aClient->in_packet.pos = 0;
		aClient->in_packet.packet_length = 5; //先定5个，后面根据反馈结果里第五个字节里的长度进行扩充
		aClient->in_packet.to_process = 5;
		aClient->in_packet.payload = pahomqtt__malloc(sizeof(uint8_t)*5);
		if(!aClient->in_packet.payload){
			pahomqtt__free(packet->payload);
			pahomqtt__free(packet);
			return PAHOMQTT_ERR_NOMEM;
		}
		return packet__queue(aClient, packet);
	}
	else if(state == pahomqtt_cs_socks5_send_userpass)
	{
		packet = pahomqtt__calloc(1, sizeof(struct pahomqtt_socks5_packet));
		if(!packet) return PAHOMQTT_ERR_NOMEM;

		ulen = (uint8_t)strlen(aClient->socks5_username);
		plen = (uint8_t)strlen(aClient->socks5_password);

		/*
			客户端发送账号密码到代理服务器认证
			+---------+-----------------+----------+-----------------+----------+
			| VERSION | USERNAME_LENGTH | USERNAME | PASSWORD_LENGTH | PASSWORD |
			+---------+-----------------+----------+-----------------+----------+
			|    1	  |			1		|	1-255  |		1		 |	1-255	|
			+---------+-----------------+----------+-----------------+----------+

			VERSION 认证子协商版本（与SOCKS协议版本的0x05无关系,一般是0x01）
			USERNAME_LENGTH 用户名长度
			USERNAME 用户名字节数组，长度为USERNAME_LENGTH
			PASSWORD_LENGTH 密码长度
			PASSWORD 密码字节数组，长度为PASSWORD_LENGTH
		*/

		packet->packet_length = 3U + ulen + plen;
		packet->payload = pahomqtt__malloc(sizeof(uint8_t)*packet->packet_length);


		packet->payload[0] = 0x01;
		packet->payload[1] = ulen;
		memcpy(&(packet->payload[2]), aClient->socks5_username, ulen);
		packet->payload[2+ulen] = plen;
		memcpy(&(packet->payload[3+ulen]), aClient->socks5_password, plen);

		//标记socks5准备账号密码认证
		pahomqtt_set_socks5_state(aClient, pahomqtt_cs_socks5_userpass_reply);

		/*
			构建准备接收服务器回馈的数据包（服务器回馈的消息存在这里）
			+---------+--------+
			| VERSION | STATUS |
			+---------+--------+
			|	1	  |	   1   |
			+---------+--------+
			VERSION 认证子协商版本，与客户端VERSION字段一致
			STATUS 认证结果
			0x00 认证成功
			大于0x00 认证失败
		*/
		aClient->in_packet.pos = 0;
		aClient->in_packet.packet_length = 2;
		aClient->in_packet.to_process = 2;
		aClient->in_packet.payload = pahomqtt__malloc(sizeof(uint8_t)*2);
		if(!aClient->in_packet.payload){
			pahomqtt__free(packet->payload);
			pahomqtt__free(packet);
			return PAHOMQTT_ERR_NOMEM;
		}

		return packet__queue(aClient, packet);
	}
	return PAHOMQTT_ERR_SUCCESS;
}

int socks5__read(Clients *aClient)
{
	ssize_t len;
	uint8_t *payload;
	uint8_t i;
	enum mosquitto_client_state state;

	state = pahomqtt_get_socks5_state(aClient);
	if(state == pahomqtt_cs_socks5_start) //认证返回
	{
		while(aClient->in_packet.to_process > 0)
		{
			len = net__read(aClient, &(aClient->in_packet.payload[aClient->in_packet.pos]), aClient->in_packet.to_process);
			if(len > 0){
				aClient->in_packet.pos += (uint32_t)len;
				aClient->in_packet.to_process -= (uint32_t)len;
			}else{
#ifdef WIN32
				errno = WSAGetLastError();
#endif
				if(errno == EAGAIN || errno == COMPAT_EWOULDBLOCK){
					return PAHOMQTT_ERR_SUCCESS;
				}else{
					packet__cleanup(&aClient->in_packet);
					switch(errno){
						case 0:
							return PAHOMQTT_ERR_PROXY;
						case COMPAT_ECONNRESET:
							return PAHOMQTT_ERR_CONN_LOST;
						default:
							return PAHOMQTT_ERR_ERRNO;
					}
				}
			}
		}
		if(aClient->in_packet.payload[0] != 5){
			packet__cleanup(&aClient->in_packet);
			return PAHOMQTT_ERR_PROXY;
		}
		switch(aClient->in_packet.payload[1]){
			case SOCKS_AUTH_NONE:
				packet__cleanup(&aClient->in_packet);
				pahomqtt_set_socks5_state(aClient, pahomqtt_cs_socks5_auth_ok);
				return socks5__send(aClient);
			case SOCKS_AUTH_USERPASS:
				packet__cleanup(&aClient->in_packet);
				pahomqtt_set_socks5_state(aClient, pahomqtt_cs_socks5_send_userpass);
				return socks5__send(aClient);
			default:
				packet__cleanup(&aClient->in_packet);
				return PAHOMQTT_ERR_AUTH;
		}
	}
	else if(state == pahomqtt_cs_socks5_userpass_reply)
	{
		while(aClient->in_packet.to_process > 0){
			len = net__read(aClient, &(aClient->in_packet.payload[aClient->in_packet.pos]), aClient->in_packet.to_process);
			if(len > 0){
				aClient->in_packet.pos += (uint32_t)len;
				aClient->in_packet.to_process -= (uint32_t)len;
			}else{
#ifdef WIN32
				errno = WSAGetLastError();
#endif
				if(errno == EAGAIN || errno == COMPAT_EWOULDBLOCK){
					return PAHOMQTT_ERR_SUCCESS;
				}else{
					packet__cleanup(&aClient->in_packet);
					switch(errno){
						case 0:
							return PAHOMQTT_ERR_PROXY;
						case COMPAT_ECONNRESET:
							return PAHOMQTT_ERR_CONN_LOST;
						default:
							return PAHOMQTT_ERR_ERRNO;
					}
				}
			}
		}
		if(aClient->in_packet.payload[0] != 1){
			packet__cleanup(&aClient->in_packet);
			return PAHOMQTT_ERR_PROXY;
		}
		if(aClient->in_packet.payload[1] == 0){
			packet__cleanup(&aClient->in_packet);
			pahomqtt_set_socks5_state(aClient, pahomqtt_cs_socks5_auth_ok);
			return socks5__send(aClient);
		}else{
			i = aClient->in_packet.payload[1];
			packet__cleanup(&aClient->in_packet);
			switch(i){
				case SOCKS_REPLY_CONNECTION_NOT_ALLOWED:
					return PAHOMQTT_ERR_AUTH;

				case SOCKS_REPLY_NETWORK_UNREACHABLE:
				case SOCKS_REPLY_HOST_UNREACHABLE:
				case SOCKS_REPLY_CONNECTION_REFUSED:
					return PAHOMQTT_ERR_NO_CONN;

				case SOCKS_REPLY_GENERAL_FAILURE:
				case SOCKS_REPLY_TTL_EXPIRED:
				case SOCKS_REPLY_COMMAND_NOT_SUPPORTED:
				case SOCKS_REPLY_ADDRESS_TYPE_NOT_SUPPORTED:
					return PAHOMQTT_ERR_PROXY;

				default:
					return PAHOMQTT_ERR_INVAL;
			}
			return PAHOMQTT_ERR_PROXY;
		}
	}else if(state == pahomqtt_cs_socks5_request){
		while(aClient->in_packet.to_process > 0){
			len = net__read(aClient, &(aClient->in_packet.payload[aClient->in_packet.pos]), aClient->in_packet.to_process);
			if(len > 0){
				aClient->in_packet.pos += (uint32_t)len; //缓冲区里读取终止位置
				aClient->in_packet.to_process -= (uint32_t)len; //还剩多长没读
			}else{
#ifdef WIN32
				errno = WSAGetLastError();
#endif
				if(errno == EAGAIN || errno == COMPAT_EWOULDBLOCK){
					return PAHOMQTT_ERR_SUCCESS;
				}else{
					packet__cleanup(&aClient->in_packet);
					switch(errno){
						case 0:
							return PAHOMQTT_ERR_PROXY;
						case COMPAT_ECONNRESET:
							return PAHOMQTT_ERR_CONN_LOST;
						default:
							return PAHOMQTT_ERR_ERRNO;
					}
				}
			}
		}

		if(aClient->in_packet.packet_length == 5){
			/* First part of the packet has been received, we now know what else to expect. */
			if(aClient->in_packet.payload[3] == SOCKS_ATYPE_IP_V4){
				aClient->in_packet.to_process += 4+2-1; /* 4 bytes IPv4, 2 bytes port, -1 byte because we've already read the first byte */
				aClient->in_packet.packet_length += 4+2-1;
			}else if(aClient->in_packet.payload[3] == SOCKS_ATYPE_IP_V6){
				aClient->in_packet.to_process += 16+2-1; /* 16 bytes IPv6, 2 bytes port, -1 byte because we've already read the first byte */
				aClient->in_packet.packet_length += 16+2-1;
			}else if(aClient->in_packet.payload[3] == SOCKS_ATYPE_DOMAINNAME){
				if(aClient->in_packet.payload[4] > 0) //表示最后一个字节有数据，该数据表示返回的ADDR的长度
				{
					aClient->in_packet.to_process += aClient->in_packet.payload[4];
					aClient->in_packet.packet_length += aClient->in_packet.payload[4];
				}
			}else{
				packet__cleanup(&aClient->in_packet);
				return PAHOMQTT_ERR_PROTOCOL;
			}
			payload = pahomqtt__realloc(aClient->in_packet.payload, aClient->in_packet.packet_length);
			if(payload){
				aClient->in_packet.payload = payload;
			}else{
				packet__cleanup(&aClient->in_packet);
				return PAHOMQTT_ERR_NOMEM;
			}
			return PAHOMQTT_ERR_NOT_FINISHED; //该条信息没有读取完，触发socks5__read的线程继续读取剩余内容
		}

		/* Entire packet is now read. */
		if(aClient->in_packet.payload[0] != 5){
			packet__cleanup(&aClient->in_packet);
			return PAHOMQTT_ERR_PROXY;
		}
		if(aClient->in_packet.payload[1] == 0) //表示成功
		{
			/* Auth passed */
			packet__cleanup(&aClient->in_packet);
			pahomqtt_set_socks5_state(aClient, pahomqtt_cs_new); //代理成功
			if(aClient->socks5_host)
			{
				//int rc = net__socket_connect_step3(aClient, aClient->socks5_host);
				//if(rc) 
					//return rc;
				return 0;
			}
			return 0;
			//return send__connect(aClient, aClient->keepalive, aClient->clean_start, NULL);
		}
		else//表示失败
		{
			i = aClient->in_packet.payload[1];
			packet__cleanup(&aClient->in_packet);
			pahomqtt_set_socks5_state(aClient, pahomqtt_cs_socks5_new);
			switch(i){
				case SOCKS_REPLY_CONNECTION_NOT_ALLOWED:
					return PAHOMQTT_ERR_AUTH;

				case SOCKS_REPLY_NETWORK_UNREACHABLE:
				case SOCKS_REPLY_HOST_UNREACHABLE:
				case SOCKS_REPLY_CONNECTION_REFUSED:
					return PAHOMQTT_ERR_NO_CONN;

				case SOCKS_REPLY_GENERAL_FAILURE:
				case SOCKS_REPLY_TTL_EXPIRED:
				case SOCKS_REPLY_COMMAND_NOT_SUPPORTED:
				case SOCKS_REPLY_ADDRESS_TYPE_NOT_SUPPORTED:
					return PAHOMQTT_ERR_PROXY;

				default:
					return PAHOMQTT_ERR_INVAL;
			}
		}
	}
	else if(state == pahomqtt_cs_new)
	{
		return 0;
	}
	else
	{
		return -1;
	}
	return PAHOMQTT_ERR_SUCCESS;
}

thread_return_type WINAPI Socks5_ReadThread(void* n)
{
	if (n == NULL)
		return -2;
	Clients *aClient = (Clients*)n;
	if (aClient == NULL || aClient->net.socket <= 0)
		return -2;

	int rc = 0;

	enum pahomqtt_client_socks5_state state;
	state = pahomqtt_get_socks5_state(aClient);

	SOCKET socket = aClient->net.socket;

	int maxfd = aClient->net.socket;
	fd_set readFdset; //创建文件描述符集合
	FD_ZERO(&readFdset); //清空文件描述符集合
	FD_SET(socket, &readFdset); //添加一个文件描述符到集合里

	int fdcount = 0;
	const clock_t begin_time = clock();

	while (state != pahomqtt_cs_new && !(*(aClient->mqttAsync_tostop)))
	{
		long interval = clock() - begin_time;
		if (interval > 3000) //大于3秒
		{
			pahomqtt_set_socks5_state(aClient, pahomqtt_cs_error);
			rc = -1;
			break;
		}

		FD_ZERO(&readFdset); //清空文件描述符集合
		FD_SET(socket, &readFdset); //添加一个文件描述符到集合里

		fdcount = select(0, &readFdset, NULL, NULL, NULL);
		if (fdcount > 0)
		{
			if (FD_ISSET(socket, &readFdset))
			{
				AAA:
				rc = socks5__read(aClient);
				state = pahomqtt_get_socks5_state(aClient);
				if (state == pahomqtt_cs_new)
				{
					break;
				}
				if (rc == PAHOMQTT_ERR_NOT_FINISHED)
				{
					goto AAA;
				}
			}
		}
		
		Sleep(50);
		state = pahomqtt_get_socks5_state(aClient);
	}

	if (aClient->out_packet)
		free(aClient->out_packet);
	aClient->out_packet = NULL;

	if (aClient->out_packet_last)
		free(aClient->out_packet_last);
	aClient->out_packet_last = NULL;

	ExitThread(0);
	return rc;
}
