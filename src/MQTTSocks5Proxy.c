
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
	return recv(aClient->net.socket, buf, count, 0); //��4������0��ʾ���л���������
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

	//ѭ�����Ͷ��������Ϣ�����ͳɹ�һ��ɾһ��
	while (aClient->out_packet) 
	{
		packet = aClient->out_packet;

		while (packet->to_process > 0)
		{
			//�������ݵ�Զ�˷�����������ֵ��ʾ���ͳɹ����ֽ���
			write_length = net__write(aClient, &(packet->payload[packet->pos]), packet->to_process); //��ʼ��ַ�뷢�ͳ���
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
						return PAHOMQTT_ERR_CONN_LOST; //���ص��õط������ö��������������ϲ���Ҫ����
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

	packet->pos = 0; //��¼���͵���λ��
	packet->to_process = packet->packet_length; //��¼δ�����ֽ���

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

	//����socks5
	if(state == pahomqtt_cs_socks5_new)
	{
		packet = pahomqtt__calloc(1, sizeof(struct pahomqtt_socks5_packet));
		if(!packet) return PAHOMQTT_ERR_NOMEM;

		/*
			**�ͻ���** �����һ��,������֤����
			+----+----------+----------+
			| VER|NMETHODS  | METHODS  |
			+----+----------+----------+
			| 1  |    1     | 1 - 255  |
			+----+----------+----------+
			VER ��ʾ�汾��:sock5 Ϊ X'05'
			NMETHODS����֤����ѡ����1��2�ֵȣ�
			METHODS�������֤�������飬NMETHODS�м��֣��Ͷ�Ӧ������֤��ʽ
			Ŀǰ�����METHOD�����¼���:
			X'00'  ������֤
			X'01'  ͨ�ð�ȫ����Ӧ�ó���(GSSAPI)
			X'02'  �û���/���� auth (USERNAME/PASSWORD)
			X'03'- X'7F' IANA ����(IANA ASSIGNED)
			X'80'- X'FE' ˽�˷�������(RESERVED FOR PRIVATE METHODS)
			X'FF'  �޿ɽ��ܷ���(NO ACCEPTABLE METHODS)
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

		//��ǿ�ʼsocks5��֤
		pahomqtt_set_socks5_state(aClient, pahomqtt_cs_socks5_start);

		/*
			����׼�����շ��������������ݰ�����������������Ϣ�������

			**������** ��Ӧ��һ��
			�������ӿͻ��˷�������Ϣ��ѡ��һ�ַ�����Ϊ����
			��������METHODS�����ķ�����ѡ��һ�֣�����һ��METHOD��������ѡ���ģ�
			+----+--------+
			|VER | METHOD |
			+----+--------+
			|  1 | ��1�� ��|
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
	else if(state == pahomqtt_cs_socks5_auth_ok) //socks5��֤�ɹ�
	{
		packet = pahomqtt__calloc(1, sizeof(struct pahomqtt_socks5_packet));
		if(!packet) return PAHOMQTT_ERR_NOMEM;

		ipv4_pton_result = inet_pton(AF_INET, aClient->serverURI, &addr_ipv4);
		ipv6_pton_result = inet_pton(AF_INET6, aClient->serverURI, &addr_ipv6);

		/*
			SOCKS5�������������±���ʾ:
			+----+-----+-------+------+----------+----------+
			| VER| CMD | RSV   | ATYP |  DST.ADDR|  DST.PORT|
			+----+-----+-------+------+----------+----------+
			| 1  | 1   | X'00' | 1    | variable |      2   |
			+----+-----+-------+------+----------+----------+

			�����ֶκ�������:
			VER  �汾��X'05'
			CMD��
				1. CONNECT X'01'
				2. BIND    X'02'
				3. UDP ASSOCIATE X'03'
			RSV  �����ֶ�
			ATYP IP����
				1.IPV4 X'01'
				2.DOMAINNAME X'03'
				3.IPV6 X'04'
			DST.ADDR Ŀ���ַ
				1.�����IPv4��ַ����4�ֽ�����
				2.�����FQDN������"www.nsfocus.net"�����ｫ��:
					0F 77 77 77 2E 6E 73 66 6F 63 75 73 2E 6E 65 74
					ע�⣬û�н�β��NUL�ַ�����ASCIZ������һ�ֽ��ǳ�����
				3.�����IPv6��ַ��������16�ֽ����ݡ�
			DST.PORT Ŀ��˿�big-endian�洢��������������У�
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
			packet->payload[4+4] = PAHOMQTT_MSB(1883); //���ֽ�
			packet->payload[4+4+1] = PAHOMQTT_LSB(1883); //���ֽ�

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
			packet->packet_length = 7U + (uint32_t)slen; //��һ���ֽ����ڴ洢����
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
			**sock5�����������Ӧ����:**
			OCKS Server��������SOCKS Client��ת�����󲢷�����Ӧ����:
			+----+-----+-------+------+----------+----------+
			|VER | REP |  RSV  | ATYP | BND.ADDR | BND.PORT |
			+----+-----+-------+------+----------+----------+
			| 1  |  1  | X'00' |  1   | Variable |    2     |
			+----+-----+-------+------+----------+----------+
			VER  �汾��X'05'
			REP
				1. 0x00        �ɹ�
				2. 0x01        һ����ʧ��
				3. 0x02        ��������ת��
				4. 0x03        ���粻�ɴ�
				5. 0x04        �������ɴ�
				6. 0x05        ���Ӿܾ�
				7. 0x06        TTL��ʱ
				8. 0x07        ��֧��������е�CMD
				9. 0x08        ��֧��������е�ATYP
				10. 0x09-0xFF   unassigned
			RSV         �����ֶΣ�����Ϊ0x00
			ATYP        ����ָ��BND.ADDR�������
			BND.ADDR    CMD��صĵ�ַ��Ϣ����ҪΪBND���Ի�һ���ֽ��ǳ�����
			BND.PORT    CMD��صĶ˿���Ϣ��big-endian���2�ֽ�����
			
			**ע��**
			����CMDΪCONNECT��SOCKS Client��SOCKS Server֮��ͨ�ŵ������Ԫ����:
			SOCKSCLIENT.ADDR��SOCKSCLIENT.PORT��SOCKSSERVER.ADDR��SOCKSSERVER.PORT

			һ��SOCKSSERVER.PORT��1080/TCP��
			CONNECT������е�DST.ADDR/DST.PORTָ��ת��Ŀ�ĵء�SOCKS Server���Կ�
			DST.ADDR��DST.PORT��SOCKSCLIENT.ADDR��SOCKSCLIENT.PORT�����������Ծ���������ת��
			Ŀ�ĵص�TCP���ӻ��Ǿܾ�ת����

			�����������ת�����ҳɹ�������ת��Ŀ�ĵص�TCP���ӣ������Ԫ����:
			BND.ADDR��BND.PORT��DST.ADDR��DST.PORT
			��ʱSOCKS Server��SOCKS Client���͵�CONNECT��Ӧ���н�ָ��BND.ADDR/BND.PORT��
			ע�⣬BND.ADDR���ܲ�ͬ��SOCKSSERVER.ADDR��SOCKS Server�������������Ƕ�Ŀ
			(multi-homed)������

			����ܾ�ת����δ�ܳɹ�������ת��Ŀ�ĵص�TCP���ӣ�CONNECT��Ӧ����REP�ֶν�ָ������ԭ��

			��Ӧ����REP����ʱ��ʾʧ�ܣ�SOCKS Server�����ڷ�����Ӧ���󲻾�(������10s)�ر���
			SOCKS Client֮���TCP���ӡ�
			��Ӧ����REPΪ��ʱ��ʾ�ɹ���֮��SOCKS Clientֱ���ڵ�ǰTCP�����Ϸ��ʹ�ת�����ݡ�
		*/
		aClient->in_packet.pos = 0;
		aClient->in_packet.packet_length = 5; //�ȶ�5����������ݷ�������������ֽ���ĳ��Ƚ�������
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
			�ͻ��˷����˺����뵽�����������֤
			+---------+-----------------+----------+-----------------+----------+
			| VERSION | USERNAME_LENGTH | USERNAME | PASSWORD_LENGTH | PASSWORD |
			+---------+-----------------+----------+-----------------+----------+
			|    1	  |			1		|	1-255  |		1		 |	1-255	|
			+---------+-----------------+----------+-----------------+----------+

			VERSION ��֤��Э�̰汾����SOCKSЭ��汾��0x05�޹�ϵ,һ����0x01��
			USERNAME_LENGTH �û�������
			USERNAME �û����ֽ����飬����ΪUSERNAME_LENGTH
			PASSWORD_LENGTH ���볤��
			PASSWORD �����ֽ����飬����ΪPASSWORD_LENGTH
		*/

		packet->packet_length = 3U + ulen + plen;
		packet->payload = pahomqtt__malloc(sizeof(uint8_t)*packet->packet_length);


		packet->payload[0] = 0x01;
		packet->payload[1] = ulen;
		memcpy(&(packet->payload[2]), aClient->socks5_username, ulen);
		packet->payload[2+ulen] = plen;
		memcpy(&(packet->payload[3+ulen]), aClient->socks5_password, plen);

		//���socks5׼���˺�������֤
		pahomqtt_set_socks5_state(aClient, pahomqtt_cs_socks5_userpass_reply);

		/*
			����׼�����շ��������������ݰ�����������������Ϣ�������
			+---------+--------+
			| VERSION | STATUS |
			+---------+--------+
			|	1	  |	   1   |
			+---------+--------+
			VERSION ��֤��Э�̰汾����ͻ���VERSION�ֶ�һ��
			STATUS ��֤���
			0x00 ��֤�ɹ�
			����0x00 ��֤ʧ��
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
	if(state == pahomqtt_cs_socks5_start) //��֤����
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
				aClient->in_packet.pos += (uint32_t)len; //���������ȡ��ֹλ��
				aClient->in_packet.to_process -= (uint32_t)len; //��ʣ�೤û��
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
				if(aClient->in_packet.payload[4] > 0) //��ʾ���һ���ֽ������ݣ������ݱ�ʾ���ص�ADDR�ĳ���
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
			return PAHOMQTT_ERR_NOT_FINISHED; //������Ϣû�ж�ȡ�꣬����socks5__read���̼߳�����ȡʣ������
		}

		/* Entire packet is now read. */
		if(aClient->in_packet.payload[0] != 5){
			packet__cleanup(&aClient->in_packet);
			return PAHOMQTT_ERR_PROXY;
		}
		if(aClient->in_packet.payload[1] == 0) //��ʾ�ɹ�
		{
			/* Auth passed */
			packet__cleanup(&aClient->in_packet);
			pahomqtt_set_socks5_state(aClient, pahomqtt_cs_new); //����ɹ�
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
		else//��ʾʧ��
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
	fd_set readFdset; //�����ļ�����������
	FD_ZERO(&readFdset); //����ļ�����������
	FD_SET(socket, &readFdset); //���һ���ļ���������������

	int fdcount = 0;
	const clock_t begin_time = clock();

	while (state != pahomqtt_cs_new && !(*(aClient->mqttAsync_tostop)))
	{
		long interval = clock() - begin_time;
		if (interval > 3000) //����3��
		{
			pahomqtt_set_socks5_state(aClient, pahomqtt_cs_error);
			rc = -1;
			break;
		}

		FD_ZERO(&readFdset); //����ļ�����������
		FD_SET(socket, &readFdset); //���һ���ļ���������������

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
