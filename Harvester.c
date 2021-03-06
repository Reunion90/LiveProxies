#define _BSD_SOURCE

#include "Harvester.h"
#include "Logger.h"
#include "ProxyLists.h"
#include "IPv6Map.h"
#include "Global.h"
#include "Config.h"
#if __linux__
	#include <dirent.h>
#elif defined _WIN32 || defined _WIN64
	#include <windows.h>
#endif
#include <curl/curl.h>
#include <string.h>
#include <assert.h>
#include <stdlib.h>

#if defined _WIN32 || defined _WIN64
char *strtok_r(char *str, const char *delim, char **save)
{
	char *res, *last;

	if (!save)
		return strtok(str, delim);
	if (!str && !(str = *save))
		return NULL;
	last = str + strlen(str);
	if ((*save = res = strtok(str, delim))) {
		*save += strlen(res);
		if (*save < last)
			(*save)++;
		else
			*save = NULL;
	}
	return res;
}
#endif

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

static size_t WriteMemoryCallback(void *contents, size_t size, size_t nmemb, char **result)
{
	size_t realsize = size * nmemb;

	*result = realloc(*result, strlen(*result) + realsize + 1);
	memcpy(&((*result)[strlen(*result)]), contents, realsize);

	return realsize;
}

char *SourceTypes[] = { "Script", "Static file", "URL" };

char *ProxySourceTypeToString(HARVESTER_PROXY_SOURCE_TYPE In)
{
	switch (In) {
		case SCRIPT: {
			return SourceTypes[0];
		}
		case STATIC: {
			return SourceTypes[1];
		}
		case URL: {
			return SourceTypes[2];
		}
		default: {
			assert(false);
			return NULL;
		}
	}
}

void HarvestLoop()
{
	// holy memory
	
	HarvesterStatsPrxsrc = NULL;
	SizeStatsHarvesterPrxsrc = 0;

	for (;;) {
#if __linux__

		DIR *d;
		struct dirent *ent;
		d = opendir(HarvestersPath);

#elif defined _WIN32 || defined _WIN64
		WIN32_FIND_DATA fdFile;
		HANDLE d = NULL;

		size_t harvestersPathLen = strlen(HarvestersPath);
		char *fullPath = malloc(harvestersPathLen + 3); {
			strcpy(fullPath, HarvestersPath);
			strcat(fullPath, "\\*");
			fullPath[harvestersPathLen + 2] = 0x00;

			if ((d = FindFirstFile(fullPath, &fdFile)) == INVALID_HANDLE_VALUE) {
				Log(LOG_LEVEL_ERROR, "WIN32: Failed to search files: %d (%s)", GetLastError(), fullPath);
				d = false;
			}
		} free(fullPath);
#endif

		if (!d) {
			Log(LOG_LEVEL_ERROR, "Failed to open \"%s\", no proxies added", HarvestersPath);
			goto end;
		}

		char *name;

#if __linux__
		while ((ent = readdir(d)) != NULL) {
			if (ent->d_type != DT_REG /* normal file */ || strlen(ent->d_name) < 4)
				continue;
			name = ent->d_name;
#elif defined _WIN32 || defined _WIN64
		do {
			name = fdFile.cFileName;
#endif

			HARVESTER_PROXY_SOURCE_TYPE sourceType = NONE;
			size_t fileNameLen = strlen(name);
			if (strcmp(name + fileNameLen - 4, ".cmd") == 0)
				sourceType = SCRIPT;
			if (strcmp(name + fileNameLen - 4, ".txt") == 0 || strcmp(name + fileNameLen - 4, ".prx") == 0)
				sourceType = STATIC;
			if (strcmp(name + fileNameLen - 4, ".url") == 0)
				sourceType = URL;
			if (sourceType == NONE)
				continue;
			Log(LOG_LEVEL_SUCCESS, "Executing %s... (%s)", name, ProxySourceTypeToString(sourceType));
			char *result;
			uint32_t added = 0, total = 0;
			PROXY_TYPE curType = PROXY_TYPE_HTTP;
			
			char pathFull[strlen(HarvestersPath) + 1 + fileNameLen];
			strcpy(pathFull, HarvestersPath);
#ifdef __linux__
			strcat(pathFull, "/");
#elif defined _WIN32 || defined _WIN64
			strcat(pathFull, "\\");
#endif
			strcat(pathFull, name);
			
			if (sourceType == SCRIPT) {
				FILE *file = fopen(pathFull, "r");
				
				fseek(file, 0, SEEK_END);
				size_t size = ftell(file);
				fseek(file, 0, SEEK_SET);
				
				char *cmd = malloc(size); {
					fread(cmd, 1, size, file);
					
					size_t cmdLen = strlen(cmd);
					if (cmd[cmdLen - 1] == '\n')
						cmd[cmdLen - 1] = '\0';
					
					FILE *scriptStdout = popen(cmd, "r"); 
					
					
					if (scriptStdout == NULL) {
						Log(LOG_LEVEL_ERROR, "Failed to execute %s", cmd);
						continue;
					}

					result = NULL;
					char buff[32];
					size_t curLen = 0;
					while (fgets(buff, sizeof(buff), scriptStdout) != 0)
					{
						size_t buffLen = strlen(buff);
						result = realloc(result, buffLen + curLen + 1);
						strcpy(result + curLen, buff);
						curLen += buffLen;
					}
					
					if (result == NULL) {
						pclose(scriptStdout);
						continue;
					}
					pclose(scriptStdout);
				} free(cmd);
			}
			if (sourceType == STATIC) {
				char pathFull[strlen(HarvestersPath) + 1 + fileNameLen];
				strcpy(pathFull, HarvestersPath);
#ifdef __linux__
				strcat(pathFull, "/");
#elif defined _WIN32 || defined _WIN64
				strcat(pathFull, "\\");
#endif
				strcat(pathFull, name);

				FILE *hFile = fopen(pathFull, "r"); {
					if (hFile == NULL) {
						Log(LOG_LEVEL_ERROR, "Failed to open %s", pathFull);
						continue;
					}
					fseek(hFile, 0, SEEK_END);
					size_t size = ftell(hFile);
					fseek(hFile, 0, SEEK_SET);

					result = malloc(size + 1);
					fread(result, size, 1, hFile);
					result[size] = 0x00;
				} fclose(hFile);
			}
			if (sourceType == URL) {
				char pathFull[strlen(HarvestersPath) + 1 + fileNameLen + 1];
				strcpy(pathFull, HarvestersPath);
#ifdef __linux__
				strcat(pathFull, "/");
#elif defined _WIN32 || defined _WIN64
				strcat(pathFull, "\\");
#endif
				strcat(pathFull, name);

				FILE *hFile = fopen(pathFull, "r"); {
					if (hFile == NULL)
						continue;
					fseek(hFile, 0, SEEK_END);
					size_t size = ftell(hFile);
					fseek(hFile, 0, SEEK_SET);

					char *contents = malloc(size + 1); {
						fread(contents, size, 1, hFile);
						contents[size] = 0x00;

						char *nl = strstr(contents, "\r\n");
						if (nl == NULL)
							nl = strchr(contents, '\n');

						if (nl == NULL) {
							Log(LOG_LEVEL_WARNING, "Malformed URL type proxy source");
							free(contents);
							continue;
						}
						*nl = 0x00;
						char *url = nl + 1;
						if (url[strlen(url) - 1] == '\n')
							url[strlen(url) - 1] = 0x00;

						result = malloc(0);
						CURL *hCurl = curl_easy_init(); {
							curl_easy_setopt(hCurl, CURLOPT_URL, url);
							curl_easy_setopt(hCurl, CURLOPT_WRITEFUNCTION, WriteMemoryCallback);
							curl_easy_setopt(hCurl, CURLOPT_WRITEDATA, &result);
							curl_easy_setopt(hCurl, CURLOPT_USERAGENT, REQUEST_UA);
							CURLcode res = curl_easy_perform(hCurl);
							if (res != CURLE_OK) {
								Log(LOG_LEVEL_WARNING, "Request to %s failed: %s", url, curl_easy_strerror(res));
							} else {
								if (strncmp(contents, "setType", 7) == 0)
									curType = atoll(contents + 8);
							}
						} curl_easy_cleanup(hCurl);
					} free(contents);
				} fclose(hFile);
			}

			char *tokSave = NULL;
			char *pch = strtok_r(result, "\n", &tokSave);
			while (pch != NULL) {
				if (pch[0] == '\0') {
					pch = strtok_r(NULL, "\n", &tokSave);
					continue;
				}

				added += AddProxyHarvesterFormat(pch, &curType);
				total++;

				pch = strtok_r(NULL, "\n", &tokSave);
			}

			free(result);
			
			HARVESTER_PRXSRC_STATS_ENTRY entry;
			entry.name = name;
			entry.added = total;
			entry.addedNew = added;
			entry.type = sourceType;

			pthread_mutex_lock(&LockStatsHarvesterPrxsrc); {
				if (SizeStatsHarvesterPrxsrc < ProxySourcesBacklog) {
					HarvesterStatsPrxsrc = SizeStatsHarvesterPrxsrc == 0 ? malloc(++SizeStatsHarvesterPrxsrc * sizeof(HARVESTER_PRXSRC_STATS_ENTRY)) :
						realloc(HarvesterStatsPrxsrc, ++SizeStatsHarvesterPrxsrc * sizeof(HARVESTER_PRXSRC_STATS_ENTRY));
					memcpy(&(HarvesterStatsPrxsrc[SizeStatsHarvesterPrxsrc - 1]), &entry, sizeof(HARVESTER_PRXSRC_STATS_ENTRY));
				} else {
					free(HarvesterStatsPrxsrc[SizeStatsHarvesterPrxsrc].name);
					for (size_t x = SizeStatsHarvesterPrxsrc - 1;x < SizeStatsHarvesterPrxsrc;x--) {
						memcpy(&(HarvesterStatsPrxsrc[x - 1]), &(HarvesterStatsPrxsrc[x]), sizeof(HARVESTER_PRXSRC_STATS_ENTRY));
					}
					memcpy(&(HarvesterStatsPrxsrc[0]), &entry, sizeof(HARVESTER_PRXSRC_STATS_ENTRY));
				}
			} pthread_mutex_unlock(&LockStatsHarvesterPrxsrc);

			printf("Added %d (%d new) proxies from %s\n", total, added, name);
#if __linux__
		}
		closedir(d);
#elif defined _WIN32 || defined _WIN64
		} while (FindNextFile(d, &fdFile));
		FindClose(d);
#endif
		if (SizeUncheckedProxies == 0)
			printf("Warning: no proxies to check, all threads will be inactive\n");
end:
		msleep(HARVEST_TIMEOUT);
	}
}

size_t AddProxyHarvesterFormat(char *In, PROXY_TYPE *CurrentType)
{
	// TODO: Add in proxy sources
	if (strncmp(In, "setType", 7) == 0)
		*CurrentType = atoll(In + 8);

	if (ProxyIsSSL(*CurrentType) && !SSLEnabled) {
		Log(LOG_LEVEL_WARNING, "Got SSL proxy, but SSL is disabled");
		return 0;
	}

	char *delimiterOffset = last_strstr(In, ":");

	if (delimiterOffset == NULL)
		return 0;

	uint16_t curPort = atoi(delimiterOffset + 1);
	if (curPort == 0)
		return 0;

	In[delimiterOffset - In] = '\0';

	IPv6Map *map;
	if (In[0] == '[' && In[strlen(In) - 1] == ']') {
		In[strlen(In) - 1] = '\0';
		map = StringToIPv6Map(In + 1);
	} else
		map = StringToIPv6Map(In);

	if (map == NULL)
		return 0;

	IP_TYPE type = GetIPType(map);

	if (GlobalIp4 == NULL && type == IPV4) {
		Log(LOG_LEVEL_WARNING, "Got IPv4 address, but no IPv4 is provided (GlobalIp4)");
		free(map);
		return 0;
	}
	if (GlobalIp6 == NULL && type == IPV6) {
		Log(LOG_LEVEL_WARNING, "Got IPv6 address, but no IPv6 is provided (GlobalIp6)");
		free(map);
		return 0;
	}

	UNCHECKED_PROXY *up = AllocUProxy(map, curPort, *CurrentType, NULL, NULL);

	size_t added = UProxyAdd(up);

	if (added == 0)
		UProxyFree(up);

	return added;
}