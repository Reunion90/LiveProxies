#define _BSD_SOURCE

#include "Harvester.h"
#include "Logger.h"
#include "ProxyLists.h"
#include "IPv6Map.h"
#include "Global.h"
#include "Config.h"
#include <python2.7/Python.h>
#include <dirent.h>
#include <openssl/sha.h>

static char *last_strstr(const char *haystack, const char *needle)
{
	if (*needle == '\0')
		return (char *)haystack;

	char *result = NULL;
	for (;;) {
		char *p = strstr(haystack, needle);
		if (p == NULL)
			break;
		result = p;
		haystack = p + 1;
	}

	return result;
}

void HarvestLoop()
{
	// holy memory

	Py_Initialize();

	for (;;) {
		PyObject *pName, *pModule, *pFunc = NULL, *pResult;
		PyRun_SimpleString("import sys");
		char *sysPath = malloc(19 + strlen(HarvestersPath) + 1); {
			sprintf(sysPath, "sys.path.append(\"%s\")", HarvestersPath);
			PyRun_SimpleString(sysPath);
		} free(sysPath);
		DIR *d;
		struct dirent *ent;
		d = opendir(HarvestersPath);
		if (!d) {
			Log(LOG_LEVEL_ERROR, "Failed to open \"%s\", no proxies added", HarvestersPath);
			goto end;
		}
		while ((ent = readdir(d)) != NULL) {
			if (ent->d_type != DT_REG /* normal file */ || strlen(ent->d_name) < 4 || strcmp(ent->d_name + strlen(ent->d_name) - 3, ".py"))
				continue;
			char *path = (char*)malloc(10 + strlen(ent->d_name) + 1 /* NULL */);
			sprintf(path, "%s", ent->d_name);
			path[strlen(path) - 3] = '\0';

			Log(LOG_LEVEL_SUCCESS, "Executing %s...", path);

			pName = PyString_FromString(path);
			pModule = PyImport_Import(pName);
			Py_DECREF(pName);

			if (pModule == NULL) {
				PyErr_Print();
				goto freepath;
			}
			pFunc = PyObject_GetAttrString(pModule, "run");
			if (!pFunc) {
				PyErr_Print();
				goto freemodule;
			}
			pResult = PyObject_CallObject(pFunc, NULL);
			if (!pResult) {
				PyErr_Print();
				goto freefunc;
			}

			uint32_t added = 0, total = 0;
			char *result = PyString_AsString(pResult);
			char *tokSave = NULL;
			char *pch = strtok_r(result, "\r\n", &tokSave);
			char curType = PROXY_TYPE_HTTP;
			uint16_t curPort;
			while (pch != NULL) {
				if (pch[0] == '\0') {
					pch = strtok_r(NULL, "\r\n", &tokSave);
					continue;
				}
				if (strncmp(pch, "setType", 7) == 0) {
					curType = atoi(pch + 7);
				}

				char *delimiterOffset = last_strstr(pch, ":");

				if (delimiterOffset == NULL)
					goto next;

				curPort = atoi(delimiterOffset + 1);
				if (curPort == 0)
					goto next;

				pch[delimiterOffset - pch] = '\0';

				IPv6Map *map;
				if (pch[0] == '[' && pch[strlen(pch) - 1] == ']') {
					pch[strlen(pch) - 1] = '\0';
					map = StringToIPv6Map(pch + 1);
				} else
					map = StringToIPv6Map(pch);

				if (map == NULL)
					goto next;

				IP_TYPE type = GetIPType(map);

				if (type == IPV6 && DisableIPv6) {
					Log(LOG_LEVEL_WARNING, "Got IPv6 address from harvester %s, but IPv6 is disabled (DisableIPv6 == true)", path);
					free(map);
					goto next;
				}
				if (GlobalIp4 == NULL && type == IPV4) {
					Log(LOG_LEVEL_WARNING, "Got IPv4 address from harvester %s, but no IPv4 is provided (GlobalIp4)", path);
					free(map);
					goto next;
				}
				if (GlobalIp6 == NULL && type == IPV6) {
					Log(LOG_LEVEL_WARNING, "Got IPv6 address from harvester %s, but no IPv6 is provided (GlobalIp6)", path);
					free(map);
					goto next;
				}

				UNCHECKED_PROXY *up = malloc(sizeof(UNCHECKED_PROXY));
				up->type = curType;
				up->checking = false;
				up->requestTimeMs = 0;
				up->requestTimeHttpMs = 0;
				up->checkSuccess = false;
				up->retries = 0;
				up->timeout = NULL;
				pthread_mutex_init(&(up->processing), NULL);
				up->port = curPort;
				up->ip = map;
				GenerateHashForUProxy(up);
				up->associatedProxy = NULL;

				if (!UProxyAdd(up))
					UProxyFree(up);
				else
					added++;
				total++;

next:
				pch = strtok_r(NULL, "\n", &tokSave);
			}
			printf("Added %d (%d new) proxies from %s\n", total, added, path);
			Py_DECREF(pResult);
freefunc:
			Py_XDECREF(pFunc);
freemodule:
			Py_DECREF(pModule);
freepath:
			free(path);
		}
		closedir(d);
		if (sizeUncheckedProxies == 0)
			printf("Warning: no proxies to check, all threads will be inactive\n");
end:
		msleep(HARVEST_TIMEOUT);
	}
}