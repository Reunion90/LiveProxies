#pragma once

#include <pthread.h>
#include <event2/bufferevent.h>
#include "IPv6Map.h"

#define HTTP_AUTHORIZATION_REALM "Live Proxies interface - private access"

typedef enum _INTERFACE_PAGES {
	INTERFACE_PAGE_HOME = 0,
	INTERFACE_PAGE_UPROXIES = 1,
	INTERFACE_PAGE_PROXIES = 2,
	INTERFACE_PAGE_PRXSRC = 3,
	INTERFACE_PAGE_STATS = 4
} INTERFACE_PAGES;

typedef struct _INTERFACE_PAGE {
	INTERFACE_PAGES page;
	char *name;
} INTERFACE_PAGE;

INTERFACE_PAGE *InterfacePages;
size_t InterfacePagesSize;

typedef struct _INTERFACE_INFO {
	char *user;
	INTERFACE_PAGE *currentPage;
} INTERFACE_INFO;

typedef struct _AUTH_WEB {
	char *username;
	char *rndVerify;
	uint64_t expiry;
	IPv6Map *ip;
} AUTH_WEB;

typedef struct _AUTH_LOCAL {
	const char *username;
	const char *password;
} AUTH_LOCAL;

pthread_mutex_t AuthWebLock;
AUTH_WEB **AuthWebList;
size_t AuthWebCount;

pthread_mutex_t AuthLocalLock;
AUTH_LOCAL **AuthLocalList;
size_t AuthLocalCount;

void InterfaceInit();
void InterfaceWeb(struct bufferevent *BuffEvent, char *Buff);
void InterfaceWebUnchecked(struct bufferevent *BuffEvent, char *Buff);
void InterfaceProxyRecheck(struct bufferevent *BuffEvent, char *Buff);
void InterfaceWebHome(struct bufferevent *BuffEvent, char *Buff);