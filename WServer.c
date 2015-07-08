#include "Base64.h"
#include "Logger.h"
#include "IPv6Map.h"
#include "ProxyLists.h"
#include "Global.h"
#include "GeoIP.h"
#include "WServer.h"
#include "ProxyRequest.h"
#include "ProxyRemove.h"
#include "Interface.h"
#include "Config.h"
#include <openssl/bio.h>
#include <openssl/evp.h>
#include <pcre.h>
#include <assert.h>

static char *GetCountryByIPv6Map(IPv6Map *In) {
	if (GetIPType(In) == IPV4) {
		if (GeoIPDB == NULL)
			GeoIPDB = GeoIP_open("/usr/local/share/GeoIP/GeoIP.dat", GEOIP_MEMORY_CACHE);
		assert(GeoIPDB != NULL);
	} else {
		if (GeoIPDB6 == NULL)
			GeoIPDB6 = GeoIP_open("/usr/local/share/GeoIP/GeoIPv6.dat", GEOIP_MEMORY_CACHE);
		assert(GeoIPDB6 != NULL);
	}

	char *ret;
	if (GetIPType(In) == IPV4) {
		ret = GeoIP_country_name_by_ipnum(GeoIPDB, In->Data[3]);
	}
	else {
		geoipv6_t ip;
		memcpy(ip.s6_addr, In->Data, IPV6_SIZE);
		ret = GeoIP_country_name_by_ipnum_v6(GeoIPDB6, ip);
	}

	return ret == NULL ? "--" : ret;
}

void WServerLanding(struct evhttp_request *evRequest, void *arg) {
	Log(LOG_LEVEL_DEBUG, "WServer landing at your services!");
	UNCHECKED_PROXY *UProxy = NULL;

	/* Get UProxy pointer */ {

		char *keyRaw = evhttp_find_header(evRequest->input_headers, "LPKey");

		if (keyRaw == NULL) {
			Log(LOG_LEVEL_WARNING, "Request IP %s doesn't have LPKey", evRequest->remote_host);
			return;
			// Loose proxy is automatically free'd by EVWrite called timeout
		}

		char *key;
		char *hVal = malloc(sizeof(keyRaw)); {
			size_t len; // trash
			if (!Base64Decode(keyRaw, &key, &len)) {
				Log(LOG_LEVEL_WARNING, "LPKey invalid from request IP %s", evRequest->remote_host);
				return;
			}
		} free(hVal);

		sem_wait(&lockUncheckedProxies); {
			for (size_t x = 0; x < sizeUncheckedProxies; x++) {
				if (memcmp(key, uncheckedProxies[x]->hash, 512 / 8) == 0) {
					UProxy = uncheckedProxies[x];
				}
			}
		} sem_post(&lockUncheckedProxies);
		free(key);

		if (UProxy == NULL) {
			Log(LOG_LEVEL_WARNING, "Request IP %s doesn't have matching LPKey", evRequest->remote_host);
			return;
			// Loose proxy is automatically free'd by EVWrite called timeout
		}
		sem_wait(&(UProxy->processing));
	} /* End get UProxy pointer */

	/* Process headers */ {
		PROXY *proxy;

		if (UProxy->associatedProxy == NULL) {
			proxy = malloc(sizeof(PROXY));
			proxy->ip = malloc(sizeof(IPv6Map));
			memcpy(proxy->ip->Data, UProxy->ip->Data, IPV6_SIZE);

			proxy->port = UProxy->port;
			proxy->type = UProxy->type;
			proxy->country = GetCountryByIPv6Map(proxy->ip);
			proxy->liveSinceMs = proxy->lastChecked = GetUnixTimestampMilliseconds();
			proxy->failedChecks = 0;
			proxy->httpTimeoutMs = GetUnixTimestampMilliseconds() - UProxy->requestTimeHttpMs;
			proxy->timeoutMs = GetUnixTimestampMilliseconds() - UProxy->requestTimeMs;
			proxy->rechecking = false;
			proxy->retries = 0;
			proxy->successfulChecks = 1;
			proxy->anonymity = ANONYMITY_NONE;
		}

		struct evkeyvalq *headers = evRequest->input_headers;
		struct evkeyval *header;
		bool anonMax = true;

		for (header = headers->tqh_first; header; header = header->next.tqe_next) {
			char *val = evhttp_find_header(RequestHeaders, header->key); // fix

			if ((val == NULL || strcmp(val, header->value) != 0) && strcmp(header->key, "LPKey") != 0)
				anonMax = false;

			bool ipv4 = GetIPType(GlobalIp) == IPV4;

			if (proxy->anonymity != ANONYMITY_NONE)
				break;

			int subStrVec[256];
			int regexRet = pcre_exec(ipv4 ? ipv4Regex : ipv6Regex, ipv4 ? ipv4RegexEx : ipv6RegexEx, header->value, strlen(header->value), 0, 0, subStrVec, 256);

			if (regexRet != PCRE_ERROR_NOMATCH) {
				if (regexRet < 0) {
					Log(LOG_LEVEL_ERROR, "Couldn't execute PCRE %s regex on %s request", ipv4 ? "IPv4" : "IPv6", evRequest->remote_host);
					free(proxy);
					sem_post(&(UProxy->processing));
					continue;
				}
				else {
					// Match!
					char *foundIpStr;
					for (size_t x = 0; x < regexRet; x++) {
						pcre_get_substring(header->value, subStrVec, regexRet, x, &(foundIpStr));
						IPv6Map *foundIp = StringToIPv6Map(foundIpStr); {
							if (foundIp != NULL && memcmp(GlobalIp->Data, foundIp->Data, ipv4 ? IPV4_SIZE : IPV6_SIZE) == 0) {
								proxy->anonymity = ANONYMITY_TRANSPARENT;
								break;
							}
						} free(foundIp);
					}
					pcre_free_substring(foundIpStr);
				}
			}
		}

		if (proxy->anonymity == ANONYMITY_NONE && anonMax)
			proxy->anonymity = ANONYMITY_MAX;
		else if (proxy->anonymity == ANONYMITY_NONE)
			proxy->anonymity = ANONYMITY_ANONYMOUS;

		UProxy->checkSuccess = true;

		if (UProxy->associatedProxy == NULL)
			ProxyAdd(proxy);
		else
			UProxySuccessUpdateParentInfo(UProxy);

	} /* End process headers */

	sem_post(&(UProxy->processing));

	/* Output */ {
		struct evbuffer *buff = evbuffer_new(); {
			evbuffer_add_reference(buff, "OK\n", 3, NULL, NULL);
			evhttp_send_reply(evRequest, HTTP_OK, "OK", buff);
		} evbuffer_free(buff);
	}
}

void GenericCb(struct evhttp_request *evRequest, void *arg) {
	struct evbuffer *buff = evbuffer_new(); {
		evbuffer_add_printf(buff, "%s", "Not Found"); // ???
		evhttp_send_reply(evRequest, HTTP_NOTFOUND, "Not Found", buff);
	} evbuffer_free(buff);
}

void WServerBase() {
	evWServerBase = event_base_new();
	evWServerHTTP = evhttp_new(evWServerBase);

	evhttp_set_gencb(evWServerHTTP, GenericCb, NULL);
	Log(LOG_LEVEL_DEBUG, "/ifaceu set cb: %d", evhttp_set_cb(evWServerHTTP, "/ifaceu", InterfaceWebUnchecked, NULL));
	Log(LOG_LEVEL_DEBUG, "/iface set cb: %d", evhttp_set_cb(evWServerHTTP, "/iface", InterfaceWeb, NULL));
	Log(LOG_LEVEL_DEBUG, "/prxchk set cb %d", evhttp_set_cb(evWServerHTTP, "/prxchk", WServerLanding, NULL));

	struct timeval timeout;
	timeout.tv_sec = GlobalTimeout / 1000;
	timeout.tv_usec = (GlobalTimeout % 1000) * 1000;
	if (evhttp_bind_socket(evWServerHTTP, "0.0.0.0", ServerPort) != 0) {
		Log(LOG_LEVEL_ERROR, "Failed to bind to 0.0.0.0:%d, exiting...", ServerPort);
		exit(EXIT_FAILURE);
	}
	Log(LOG_LEVEL_DEBUG, "WServer base dispatch");
	event_base_dispatch(evWServerBase);
	Log(LOG_LEVEL_DEBUG, "WServer base dispatch end");
}