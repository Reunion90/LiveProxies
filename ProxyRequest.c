#include "ProxyRequest.h"
#include "ProxyLists.h"
#include "Global.h"
#include "Base64.h"
#include "Logger.h"
#include "Config.h"
#include "ProxyRemove.h"
#include <event2/bufferevent.h>
#include <event2/event.h>
#include <event2/buffer.h>
#include <event2/util.h>
#include <event2/bufferevent_ssl.h>
#include <assert.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <time.h>
#include <event2/dns.h>

static void RequestFree(evutil_socket_t fd, short what, UNCHECKED_PROXY *UProxy)
{
	Log(LOG_LEVEL_DEBUG, "RequestFree BuffEvent free %p", UProxy->assocBufferEvent);

	bufferevent_free(UProxy->assocBufferEvent);

	if (UProxy->timeout != NULL) {
		event_del(UProxy->timeout);
		event_free(UProxy->timeout);
		UProxy->timeout = NULL;
	} else
		assert(false);

	if (UProxy->udpRead != NULL) {
		event_del(UProxy->udpRead);
		event_free(UProxy->udpRead);
		UProxy->udpRead = NULL;
	}

	char *ip = IPv6MapToString(UProxy->ip); {
		Log(LOG_LEVEL_DEBUG, "RequestFree -> %s", ip);
	} free(ip);

	InterlockedDecrement(&CurrentlyChecking, 1);

	pthread_mutex_lock(&LockUncheckedProxies); {
	} pthread_mutex_unlock(&LockUncheckedProxies);

	struct timespec tm;
	tm.tv_sec = 1;

	pthread_mutex_lock(&(UProxy->processing)); // locks only on EVWrite called timeout

	if (UProxy->associatedProxy == NULL) {
		if (!UProxy->checkSuccess)
			UProxy->retries++;
		if (UProxy->retries >= AcceptableSequentialFails || UProxy->checkSuccess) {
			char *ip = IPv6MapToString(UProxy->ip); {
				Log(LOG_LEVEL_DEBUG, "RequestFree: Removing proxy %s...", ip);
			} free(ip);
			UProxyRemove(UProxy);
		} else {
			pthread_mutex_unlock(&(UProxy->processing));
			UProxy->checking = false;
		}
	} else {
		if (UProxy->pageTarget == NULL) {
			char *ip = IPv6MapToString(UProxy->ip); {
				Log(LOG_LEVEL_DEBUG, "RequestFree: Removing proxy %s and updating parent...", ip);
			} free(ip);

			if (!UProxy->checkSuccess)
				UProxyFailUpdateParentInfo(UProxy);
			else
				UProxySuccessUpdateParentInfo(UProxy);

			if (UProxy->singleCheckCallback != NULL)
				UProxy->singleCheckCallback(UProxy);
		}

		UProxyRemove(UProxy);
	}
}

typedef enum _SOCKS_TYPE {
	SOCKS_TYPE_CONNECT = 0x01,
	SOCKS_TYPE_BIND = 0x02,
	SOCKS_TYPE_UDP_ASSOCIATE = 0x03
} SOCKS_TYPE;

static bool SOCKS4(SOCKS_TYPE Type, UNCHECKED_PROXY *UProxy)
{
	if (Type == SOCKS_TYPE_UDP_ASSOCIATE)
		return false;

	if (UProxy->stage == 1) {
		/*
		field 1: SOCKS version number, 1 byte, must be 0x04 for this version
		field 2: command code, 1 byte:
		0x01 = establish a TCP/IP stream connection
		0x02 = establish a TCP/IP port binding
		field 3: network byte order port number, 2 bytes
		field 4: network byte order IP address, 4 bytes
		field 5: the user ID string, variable length, terminated with a null (0x00)
		*/
		char buff[1 + 1 + sizeof(uint16_t) + IPV4_SIZE + 1 /* ? */];
		buff[0] = 0x04;
		buff[1] = 0x01; // CONNECT
		*((uint16_t*)(&(buff[2]))) = htons(UProxy->targetPort);
		*((uint32_t*)(&(buff[4]))) = htonl(UProxy->targetIPv4->Data[3]);
		buff[8] = 0x00;

		bufferevent_write(UProxy->assocBufferEvent, buff, 9);
		bufferevent_setwatermark(UProxy->assocBufferEvent, EV_READ, 8, 0);
	} else if (UProxy->stage == 2) {
		size_t len = evbuffer_get_length(bufferevent_get_input(UProxy->assocBufferEvent));
		uint8_t data[2];
		if (len != 8)
			return false;

		evbuffer_remove(bufferevent_get_input(UProxy->assocBufferEvent), data, 2);
		Log(LOG_LEVEL_DEBUG, "SOCKS4: Stage 2 data[1]: %d", data[1]);
		return data[1] == 0x5A;
	}
}

static bool SOCKS5(SOCKS_TYPE Type, uint16_t *Port, UNCHECKED_PROXY *UProxy)
{
	/*
	field 1: SOCKS version number (must be 0x05 for this version)
	field 2: number of authentication methods supported, 1 byte
	field 3: authentication methods, variable length, 1 byte per method supported
	0x00: No authentication
	0x01: GSSAPI
	0x02: Username/Password
	0x03�0x7F: methods assigned by IANA
	0x80�0xFE: methods reserved for private use

	+----+-----+-------+------+----------+----------+
	|VER | CMD |  RSV  | ATYP | DST.ADDR | DST.PORT |
	+----+-----+-------+------+----------+----------+
	| 1  |  1  | X'00' |  1   | Variable |    2     |
	+----+-----+-------+------+----------+----------+


	o  VER    protocol version: X'05'
	o  CMD
	o		CONNECT X'01'
	o		BIND X'02'
	o		UDP ASSOCIATE X'03'
	o  RSV    RESERVED
	o  ATYP   address type of following address
	o		IP V4 address: X'01'
	o		DOMAINNAME: X'03'
	o		IP V6 address: X'04'
	o  DST.ADDR       desired destination address // network octet order? nope
	o  DST.PORT desired destination port in network octet order
	*/

	switch (UProxy->stage) {
		case 0:
		{
			uint8_t buff[3];
			buff[0] = 0x05;
			buff[1] = 1; // 1 auth
			buff[2] = 0x00; // no auth
			bufferevent_write(UProxy->assocBufferEvent, buff, 3);
			bufferevent_setwatermark(UProxy->assocBufferEvent, EV_READ, 2, 0);
			break;
		}
		case 1:
		{
			size_t len = evbuffer_get_length(bufferevent_get_input(UProxy->assocBufferEvent));
			uint8_t data[2];
			Log(LOG_LEVEL_DEBUG, "SOCKS5: Stage 1 data len: %d", len);
			if (len != 2)
				return false;

			evbuffer_remove(bufferevent_get_input(UProxy->assocBufferEvent), data, 2);
			Log(LOG_LEVEL_DEBUG, "SOCKS5: Stage 1 data[1]: %d", data[1]);
			return data[1] == 0x00;
			break;
		}
		case 2:
		{
			IP_TYPE ipType = GetIPType(UProxy->ip); // Prefered

			if (ipType == IPV4 && UProxy->targetIPv4 == NULL)
				ipType = IPV6;
			if (ipType == IPV6 && UProxy->targetIPv6 == NULL)
				ipType = IPV4;
			// -> Real

			uint8_t *buff;
			if (ipType == IPV4) {
				Log(LOG_LEVEL_DEBUG, "SOCKS5: Stage 2 IPV4");
				uint8_t tBuff[4 + IPV4_SIZE + sizeof(uint16_t)];
				buff = tBuff;
			} else {
				Log(LOG_LEVEL_DEBUG, "SOCKS5: Stage 2 IPV6");
				uint8_t tBuff[4 + IPV6_SIZE + sizeof(uint16_t)];
				buff = tBuff;
			}
			buff[0] = 0x05; // again?
			buff[1] = Type;
			buff[2] = 0x00; // RESERVED
			buff[3] = ipType == IPV4 ? 0x01 : 0x04; // who was 0x02?
			if (ipType == IPV4)
				(*(uint32_t*)(&(buff[4]))) = UProxy->targetIPv4->Data[3];
			else
				memcpy(&(buff[4]), UProxy->targetIPv6->Data, IPV6_SIZE);
			*((uint16_t*)&(buff[4 + (ipType == IPV4 ? IPV4_SIZE : IPV6_SIZE)])) = Type != SOCKS_TYPE_UDP_ASSOCIATE ? htons(*Port) : 0;

			bufferevent_write(UProxy->assocBufferEvent, buff, 4 + (ipType == IPV4 ? IPV4_SIZE : IPV6_SIZE) + sizeof(uint16_t));
			bufferevent_setwatermark(UProxy->assocBufferEvent, EV_READ, 10, 0);
			break;
		}
		case 3:
		{
			size_t len = evbuffer_get_length(bufferevent_get_input(UProxy->assocBufferEvent));
			uint8_t data[10];
			Log(LOG_LEVEL_DEBUG, "SOCKS5: Stage 3 data len: %d", len);
			if (len < 10)
				return false;

			evbuffer_remove(bufferevent_get_input(UProxy->assocBufferEvent), data, 10);

			Log(LOG_LEVEL_DEBUG, "SOCKS5: Stage 3 data[1]: %d", data[1]);
			Log(LOG_LEVEL_DEBUG, "SOCKS5: Stage 3 port: %d", ntohs(*((uint16_t*)&(data[8]))));
			*Port = ntohs(*((uint16_t*)&(data[8])));

			return data[1] == 0x00;
			break;
		}
	}
	return true;
}

void ProxyDNSResolved(int Err, struct evutil_addrinfo *Addr, UNCHECKED_PROXY *UProxy)
{
	if (Err) {
		bufferevent_setcb(UProxy->assocBufferEvent, NULL, NULL, NULL, NULL);
		printf("%s -> %s\n", UProxy->pageTarget, evutil_gai_strerror(Err));

		if (UProxy->timeout != NULL)
			event_active(UProxy->timeout, EV_TIMEOUT, 0);
	} else {
		struct evutil_addrinfo *ai;

		for (ai = Addr; ai; ai = ai->ai_next) {
			if (UProxy->targetIPv4 != NULL && UProxy->targetIPv6 != NULL)
				break;
			if (ai->ai_family == AF_INET && UProxy->targetIPv4 == NULL)
				UProxy->targetIPv4 = RawToIPv6Map((struct sockaddr_in *)ai->ai_addr);
			else if (ai->ai_family == AF_INET6 && UProxy->targetIPv6 == NULL)
				UProxy->targetIPv6 = RawToIPv6Map((struct sockaddr_in6 *)ai->ai_addr);
		}
		evutil_freeaddrinfo(Addr);
		ProxyHandleData(UProxy, EV_TYPE_CONNECT);
	}
}

static bool ProxyDNSResolve(UNCHECKED_PROXY *UProxy, char *Domain)
{
	if (UProxy->pageTarget == NULL)
		return false;
	struct evutil_addrinfo hints;
	struct evdns_getaddrinfo_request *req;
	struct user_data *user_data;
	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_UNSPEC;
	hints.ai_flags = EVUTIL_AI_CANONNAME;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_protocol = IPPROTO_TCP;

	if (evdns_getaddrinfo(levRequestDNSBase, Domain, NULL, &hints, ProxyDNSResolved, UProxy) == NULL)
		return false;
}

int sslCreated2 = 0;

static char *ProxyParseUrl(UNCHECKED_PROXY *UProxy, bool OnlyDomain, bool IncludePort)
{
	char *domain = strdup(UProxy->pageTarget);

	if (strstr(domain, "https://") == NULL && strstr(domain, "http://") == NULL && strstr(domain, "udp://") == NULL) {
		free(domain);
		return NULL;
	}

	domain = domain + (8 * sizeof(char));
	char *pathStart = strstr(domain, "/");
	if (pathStart != NULL)
		*pathStart = 0x00;

	char *portStart = strchr(domain, ":");
	if (portStart == NULL) {
		if (!OnlyDomain)
			UProxy->targetPort = strstr(UProxy->pageTarget, "https://") != NULL ? HTTPS_DEFAULT_PORT : HTTP_DEFAULT_PORT;
	} else {
		*portStart = 0x00;
		if (!OnlyDomain && IncludePort) {
			UProxy->targetPort = atoi(portStart + sizeof(char));
			if (UProxy->targetPort == 80 || UProxy->targetPort == 443)
				*portStart = ':';
		}
	}
	if (OnlyDomain)
		return domain;

	IPv6Map *ip = StringToIPv6Map(domain);
	if (ip != NULL) {
		if (GetIPType(ip) == IPV6)
			UProxy->targetIPv6 = ip;
		else
			UProxy->targetIPv4 = ip;
	}

	return domain;
}

void ProxyHandleData(UNCHECKED_PROXY *UProxy, PROXY_HANDLE_DATA_EV_TYPE EVType)
{
	Log(LOG_LEVEL_DEBUG, "ProxyHandleData: Proxy %s (%d), stage %d", ProxyGetTypeString(UProxy->type), UProxy->type, UProxy->stage);
	char *reqString;
#define EVTYPE_CASE(x) if (EVType != x) return;
#define EVTYPE_CASE_NOT(x) if (EVType == x) return;

	switch (UProxy->type) {
		case PROXY_TYPE_HTTP: {
			EVTYPE_CASE(EV_TYPE_CONNECT);
			UProxy->stage = 7;
			break;
		}
		case PROXY_TYPE_HTTPS: {
			switch (UProxy->stage) {
				case 0: {
					EVTYPE_CASE(EV_TYPE_CONNECT);

					if (UProxy->pageTarget == NULL) {
						char *host = GetHost(GetIPType(UProxy->ip), ProxyIsSSL(UProxy->type));

						reqString = StrReplaceToNew(RequestStringSSL, "{HOST}", host); {
							if (strstr(reqString, "{KEY_VAL}") != NULL) {
								char *key;
								size_t key64Len = Base64Encode(UProxy->hash, 512 / 8, &key); {
									StrReplaceOrig(&reqString, "{KEY_VAL}", key);
								} free(key);
							}

							bufferevent_write(UProxy->assocBufferEvent, (void*)reqString, strlen(reqString) * sizeof(char));
						} free(reqString);
					} else {
						char *domain = ProxyParseUrl(UProxy, true, true); {
							if (domain == NULL)
								goto fail;
							reqString = StrReplaceToNew(RequestStringSSL, "{HOST}", domain);
							if (strstr(reqString, "{KEY_VAL}") != NULL) {
								char *key;
								size_t key64Len = Base64Encode(UProxy->hash, 512 / 8, &key); {
									StrReplaceOrig(&reqString, "{KEY_VAL}", key);
								} free(key);
							}
						} free(domain);

						bufferevent_write(UProxy->assocBufferEvent, (void*)reqString, strlen(reqString) * sizeof(char));
						free(reqString);
					}

					UProxy->stage = 1;
					break;
				}
				case 1: {
					EVTYPE_CASE(EV_TYPE_READ);
					// HTTP/1.1 200

					size_t len = evbuffer_get_length(bufferevent_get_input(UProxy->assocBufferEvent));
					char data[12];
					if (len < 12)
						goto fail;

					evbuffer_remove(bufferevent_get_input(UProxy->assocBufferEvent), data, 12);
					if (strncmp(data + 9, "200", 3) != 0)
						goto fail;

					UProxy->stage = 6;
					break;
				}
			}
			break;
		}
		case PROXY_TYPE_SOCKS4:
		case PROXY_TYPE_SOCKS4A:
		case PROXY_TYPE_SOCKS4_TO_SSL:
		case PROXY_TYPE_SOCKS4A_TO_SSL: {
			if (GetIPType(UProxy->ip) == IPV4) // ???
				goto fail;

			switch (UProxy->stage) {
				case 0: {
					EVTYPE_CASE(EV_TYPE_CONNECT);

					if (UProxy->pageTarget == NULL) {
						SOCKS4(SOCKS_TYPE_CONNECT, UProxy);
						UProxy->stage = 1;
					} else {
						char *domain = ProxyParseUrl(UProxy, false, false); {
							if (domain == NULL)
								goto fail;
							if (UProxy->targetIPv4 == NULL && UProxy->targetIPv6 == NULL) {
								if (!ProxyDNSResolve(UProxy, domain)) {
									free(domain);
									goto fail;
								}
							} else {
								SOCKS4(SOCKS_TYPE_CONNECT, UProxy);
								UProxy->stage = 1;
							}
						} free(domain);
					}
					break;
				}
				case 1: {
					EVTYPE_CASE(EV_TYPE_READ);

					if (!SOCKS4(SOCKS_TYPE_CONNECT, UProxy))
						goto fail;
					UProxy->stage = ProxyIsSSL(UProxy->type) ? 6 : 7;
				}
			}
			break;
		}
		case PROXY_TYPE_SOCKS5:
		case PROXY_TYPE_SOCKS5_TO_SSL:
		case PROXY_TYPE_SOCKS5_WITH_UDP: {
			uint16_t port = UProxy->type == PROXY_TYPE_SOCKS5_WITH_UDP ? ServerPortUDP : (ProxyIsSSL(UProxy->type) ? SSLServerPort : ServerPort);
			SOCKS_TYPE socksType = UProxy->type == PROXY_TYPE_SOCKS5_WITH_UDP ? SOCKS_TYPE_UDP_ASSOCIATE : SOCKS_TYPE_CONNECT;

			Log(LOG_LEVEL_DEBUG, "SOCKS5 port %d", port);

			switch (UProxy->stage) {
				case 0: {
					EVTYPE_CASE(EV_TYPE_CONNECT);

					SOCKS5(0, NULL, UProxy);
					Log(LOG_LEVEL_DEBUG, "SOCKS5 advance to stage 1");
					UProxy->stage = 1;
					break;
				}
				case 1: {
					EVTYPE_CASE(EV_TYPE_READ);

					if (SOCKS5(0, NULL, UProxy)) {
						Log(LOG_LEVEL_DEBUG, "SOCKS5 advance to stage 2");
						UProxy->stage = 2;
						if (UProxy->pageTarget != NULL) {
							char *domain = ProxyParseUrl(UProxy, false, false); {
								if (domain == NULL)
									goto fail;

								if (UProxy->targetIPv4 == NULL && UProxy->targetIPv6 == NULL) {
									UProxy->stage = 2;
									if (!ProxyDNSResolve(UProxy, domain)) {
										free(domain);
										goto fail;
									}
									return;
								}
							} free(domain);
						}
						SOCKS5(socksType, &port, UProxy);
						Log(LOG_LEVEL_DEBUG, "SOCKS5 advance to stage 3");
						UProxy->stage = 3;

						// This handles two stages because after first stage, there's no one to send packet after receiving response
					} else
						goto fail;
					break;
				}
				case 2: {
					if (UProxy->pageTarget == NULL)
						goto fail;

					assert(UProxy->targetIPv4 != NULL || UProxy->targetIPv6 != NULL);

					// Execute stage 2 ending after DNS resolve
					SOCKS5(socksType, &port, UProxy);
					Log(LOG_LEVEL_DEBUG, "SOCKS5 advance to stage 3 (DNS)");
					UProxy->stage = 3;
					break;
				}
				case 3: {
					EVTYPE_CASE(EV_TYPE_READ);

					if (SOCKS5(socksType, &port, UProxy)) {
						if (UProxy->type == PROXY_TYPE_SOCKS5_WITH_UDP) {
							Log(LOG_LEVEL_DEBUG, "SOCKS5 advance to stage 4 (UDP)");
							UProxy->stage = 8;

							int hSock;

							UProxy->requestTimeHttpMs = GetUnixTimestampMilliseconds();

							if ((hSock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP)) != -1) {
								Log(LOG_LEVEL_DEBUG, "UDP socket");
								IP_TYPE ipType = GetIPType(UProxy->ip); // Preffered
								if (ipType == IPV4 && UProxy->targetIPv4 == NULL)
									ipType = IPV6;
								if (ipType == IPV6 && UProxy->targetIPv6 == NULL)
									ipType = IPV4;
								// -> Real

								Log(LOG_LEVEL_DEBUG, "UDP IP Type %d", ipType);

								uint8_t buff[512 / 8 + 6 + (ipType == IPV4 ? IPV4_SIZE : IPV6_SIZE)];
								buff[0] = 0x00;
								buff[1] = 0x00;
								buff[2] = 0x00;
								buff[3] = ipType == IPV4 ? 0x01 : 0x04;
								memcpy(&(buff[4]), ipType == IPV4 ? &(UProxy->targetIPv4->Data[3]) : UProxy->targetIPv6->Data, ipType == IPV4 ? IPV4_SIZE : IPV6_SIZE);
								*((uint16_t*)&(buff[4 + (ipType == IPV4 ? IPV4_SIZE : IPV6_SIZE)])) = htons(ServerPortUDP);
								memcpy(&(buff[6 + (ipType == IPV4 ? IPV4_SIZE : IPV6_SIZE)]), UProxy->hash, 512 / 8);

								Log(LOG_LEVEL_DEBUG, "UDP buff construct");
								struct sockaddr *sa = IPv6MapToRaw(UProxy->ip, port); {
									if (sendto(hSock, buff, 512 / 8 + 6 + (ipType == IPV4 ? IPV4_SIZE : IPV6_SIZE), 0, sa, GetIPType(UProxy->ip) == IPV4 ? sizeof(struct sockaddr_in) : sizeof(struct sockaddr_in6)) == -1) {
										Log(LOG_LEVEL_DEBUG, "UDP send fail");
										free(sa);
										close(hSock);
										goto fail;
									}
									Log(LOG_LEVEL_DEBUG, "UDP sent ;)");
								} free(sa);

								if (UProxy->pageTarget != NULL) {
									Log(LOG_LEVEL_DEBUG, "Waiting for UDP response...");
									UProxy->udpRead = event_new(levRequestBase, hSock, EV_READ | EV_PERSIST, EVRead, UProxy);
									event_add(UProxy->udpRead, NULL);
									UProxy->stage = 8;
								} else {
									close(hSock);
								}
							}
						} else {
							Log(LOG_LEVEL_DEBUG, "SOCKS5 advance to stage %d (%s)", ProxyIsSSL(UProxy->type) ? 6 : 7, ProxyIsSSL(UProxy->type) ? "SSL" : "HTTP");
							UProxy->stage = ProxyIsSSL(UProxy->type) ? 6 : 7;
						}
					} else
						goto fail;
					break;
				}
			}
			break;
		}
	}

	switch (UProxy->stage) {
		case 6: {
			struct evbuffer *buff = bufferevent_get_input(UProxy->assocBufferEvent);
			evbuffer_drain(buff, evbuffer_get_length(buff));
			Log(LOG_LEVEL_DEBUG, "Establishing SSL connection...");
			// Begin REAL SSL

			struct bufferevent *test = UProxy->assocBufferEvent;

			UProxy->assocBufferEvent = bufferevent_openssl_filter_new(levRequestBase,
																	  UProxy->assocBufferEvent,
																	  SSL_new(RequestBaseSSLCTX),
																	  BUFFEREVENT_SSL_CONNECTING,
																	  BEV_OPT_CLOSE_ON_FREE | BEV_OPT_THREADSAFE);
			Log(LOG_LEVEL_DEBUG, "SSL2 created %d", ++sslCreated2);
			bufferevent_openssl_set_allow_dirty_shutdown(UProxy->assocBufferEvent, 1);

			if (UProxy->assocBufferEvent != NULL) {
				if (bufferevent_openssl_get_ssl(UProxy->assocBufferEvent) == NULL) {
					Log(LOG_LEVEL_DEBUG, "SSL2 NULL!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!");
				} else {
					Log(LOG_LEVEL_DEBUG, "SSL2 UNDERLYING! -> %p", bufferevent_get_underlying(UProxy->assocBufferEvent));
					if (bufferevent_get_underlying(UProxy->assocBufferEvent) == test)
						Log(LOG_LEVEL_DEBUG, "SSL2 UNDERLYING POSITIVE (y) -> %p vs %p", bufferevent_get_underlying(UProxy->assocBufferEvent), test);
					else
						Log(LOG_LEVEL_DEBUG, "SSL2 UNDERLYING NEGATIVE, WHAT'S GOING ON???? (y) -> %p vs %p", bufferevent_get_underlying(UProxy->assocBufferEvent), test);
				}
				bufferevent_setcb(UProxy->assocBufferEvent, (bufferevent_data_cb)EVRead, (bufferevent_data_cb)EVWrite, (bufferevent_event_cb)EVEvent, UProxy);
				bufferevent_set_timeouts(UProxy->assocBufferEvent, &GlobalTimeoutTV, &GlobalTimeoutTV);
			} else {
				Log(LOG_LEVEL_DEBUG, "SSL BuffEvent fail");
				goto fail;
			}

			UProxy->stage = 7;
			// SSL handshake brings up EVEvent connect so this falls to stage 7 later
			break;
		}
		case 7: {
			EVTYPE_CASE_NOT(EV_TYPE_WRITE);

			if (ProxyIsSSL(UProxy->type)) {
				// SSL
				SSL *ssl = bufferevent_openssl_get_ssl(UProxy->assocBufferEvent);
				X509 *cert = SSL_get_peer_certificate(ssl); {
					uint8_t hash[EVP_MAX_MD_SIZE];
					size_t trash;

					X509_digest(cert, EVP_sha512(), hash, &trash);

					if (!MemEqual(hash, SSLFingerPrint, 512 / 8 /* SHA-512 */))
						UProxy->invalidCert = X509_dup(cert);
					else
						UProxy->invalidCert = NULL;
				} X509_free(cert);
			} else
				UProxy->invalidCert = NULL;

			UProxy->requestTimeHttpMs = GetUnixTimestampMilliseconds();
			Log(LOG_LEVEL_DEBUG, "Sending HTTP request");
			char *key;
			size_t key64Len = Base64Encode(UProxy->hash, 512 / 8, &key); {

				/*char *host = GetHost(GetIPType(UProxy->ip), ProxyIsSSL(UProxy->type));
				size_t rawOrigLen = strlen(RequestString);
				size_t baseLen = (rawOrigLen - 2 /* %s for Host header *) + strlen(host);
				size_t fullOrigLen = (sizeof(char) * rawOrigLen) + 1;

				reqString = malloc((sizeof(char) * (baseLen + key64Len + 4 /* \r\n\r\n *)) + 1 /* NUL *);
				memcpy(reqString, RequestString, fullOrigLen);

				char reqStringFormat[fullOrigLen];
				memcpy(reqStringFormat, reqString, fullOrigLen);
				sprintf(reqString, reqStringFormat, host);

				memcpy(reqString + baseLen, key, key64Len * sizeof(char));
				reqString[baseLen + key64Len] = '\r';
				reqString[baseLen + key64Len + 1] = '\n';
				reqString[baseLen + key64Len + 2] = '\r';
				reqString[baseLen + key64Len + 3] = '\n';
				reqString[baseLen + key64Len + 4] = 0x00;*/

				Log(LOG_LEVEL_DEBUG, "Page target: %s", UProxy->pageTarget);

				if (UProxy->pageTarget == NULL) {
					char *host = GetHost(GetIPType(UProxy->ip), ProxyIsSSL(UProxy->type));

					reqString = StrReplaceToNew(RequestString, "{HOST}", host);
				} else {
					char *domain = ProxyParseUrl(UProxy, true, true); {
						if (domain == NULL)
							goto fail;

						reqString = StrReplaceToNew(RequestString, "{HOST}", domain);
					} free(domain);
				}
				if (strstr(reqString, "{KEY_VAL}") != NULL)
					StrReplaceOrig(&reqString, "{KEY_VAL}", key);
				else
					goto fail;
			} free(key);

			bufferevent_write(UProxy->assocBufferEvent, (void*)reqString, strlen(reqString) * sizeof(char));
			free(reqString);
			UProxy->stage = 8;
			Log(LOG_LEVEL_DEBUG, "Advance to stage 8 (final)");
			break;
		}
		case 8:
		{
			// TODO: Handle chunked responses for HTTP?
			if (UProxy->pageTarget != NULL)
				UProxy->singleCheckCallback(UProxy);

			bufferevent_setcb(UProxy->assocBufferEvent, NULL, NULL, NULL, NULL);
			break;
		}
	}

	return;
fail:
	Log(LOG_LEVEL_DEBUG, "ProxyHandleData failure Proxy %s at stage %d", ProxyGetTypeString(UProxy->type), UProxy->stage);
	bufferevent_setcb(UProxy->assocBufferEvent, NULL, NULL, NULL, NULL);
	if (UProxy->timeout != NULL)
		event_active(UProxy->timeout, EV_TIMEOUT, 0);
}

void CALLBACK EVEvent(struct bufferevent *BuffEvent, uint16_t Event, UNCHECKED_PROXY *UProxy)
{
	Log(LOG_LEVEL_DEBUG, "EVEvent %02x", Event);

	if (Event == BEV_EVENT_CONNECTED) {
		char *ip = IPv6MapToString(UProxy->ip); {
			Log(LOG_LEVEL_DEBUG, "EVEvent: event connected %s (size %d)", ip, SizeUncheckedProxies);
		} free(ip);

		ProxyHandleData(UProxy, EV_TYPE_CONNECT);
	} else {
		if (ProxyIsSSL(UProxy->type))
			Log(LOG_LEVEL_DEBUG, "SSL stage %d error %02x", UProxy->stage, Event);

#if DEBUG
		char *ip = IPv6MapToString(UProxy->ip); {
			Log(LOG_LEVEL_DEBUG, "EVEvent: event timeout / fail %s", ip);
		} free(ip);
		Log(LOG_LEVEL_DEBUG, "EVEvent: BuffEvent: %08x event %02x", BuffEvent, Event);
#endif
		RequestFree(bufferevent_getfd(BuffEvent), Event, UProxy);
	}
}

void CALLBACK EVRead(struct bufferevent *BuffEvent, UNCHECKED_PROXY *UProxy)
{
	ProxyHandleData(UProxy, EV_TYPE_READ);
}

void CALLBACK EVWrite(struct bufferevent *BuffEvent, UNCHECKED_PROXY *UProxy)
{
	ProxyHandleData(UProxy, EV_TYPE_WRITE);
}

void RequestAsync(UNCHECKED_PROXY *UProxy)
{
	struct sockaddr *sa = IPv6MapToRaw(UProxy->ip, UProxy->port);

#if DEBUG
	char *ip = IPv6MapToString(UProxy->ip); {
		Log(LOG_LEVEL_DEBUG, "RequestAsync: [%s]:%d", ip, UProxy->port);
		if (GetIPType(UProxy->ip) == IPV4) {
			char *asd = calloc(1, 64 /* whatever */); {
				inet_ntop(AF_INET, &(((struct sockaddr_in*)sa)->sin_addr), asd, INET_ADDRSTRLEN);
				Log(LOG_LEVEL_DEBUG, "RequestAsync 2: [%s]:%d", asd, ntohs(((struct sockaddr_in*)sa)->sin_port));
			} free(asd);
		} else {
			char *asd = calloc(1, 64 /* whatever */); {
				inet_ntop(AF_INET6, &(((struct sockaddr_in6*)sa)->sin6_addr), asd, INET6_ADDRSTRLEN);
				Log(LOG_LEVEL_DEBUG, "RequestAsync 2: [%s]:%d", asd, ntohs(((struct sockaddr_in6*)sa)->sin6_port));
			} free(asd);
		}
	} free(ip);
#endif

	UProxy->assocBufferEvent = bufferevent_socket_new(levRequestBase, -1, BEV_OPT_CLOSE_ON_FREE | BEV_OPT_THREADSAFE);

	Log(LOG_LEVEL_DEBUG, "RequestAsync: new socket");

	bufferevent_set_timeouts(UProxy->assocBufferEvent, &GlobalTimeoutTV, &GlobalTimeoutTV);
	bufferevent_setcb(UProxy->assocBufferEvent, (bufferevent_data_cb)EVRead, (bufferevent_data_cb)EVWrite, (bufferevent_event_cb)EVEvent, UProxy);
	bufferevent_enable(UProxy->assocBufferEvent, EV_READ | EV_WRITE);

	UProxy->requestTimeMs = GetUnixTimestampMilliseconds();
	Log(LOG_LEVEL_DEBUG, "RequestAsync: UProxy request time: %llu", UProxy->requestTimeMs);

	InterlockedIncrement(&CurrentlyChecking, 1);

	UProxy->timeout = event_new(levRequestBase, -1, EV_TIMEOUT, (event_callback_fn)RequestFree, UProxy);
	event_add(UProxy->timeout, &GlobalTimeoutTV);

	UProxy->checking = true;
	assert(bufferevent_socket_connect(UProxy->assocBufferEvent, sa, sizeof(struct sockaddr_in6)) == 0); // socket creation should never fail, because IP is always valid (!= dead)
	free(sa);
}