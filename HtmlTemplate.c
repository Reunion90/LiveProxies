#include "HtmlTemplate.h"
#include "Logger.h"
#ifdef __linux__
	#include <dirent.h>
#elif defined _WIN32 || defined _WIN64
	#include <windows.h>
#endif
#include <stddef.h>
#include <string.h>
#include <stdint.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include "Global.h"
#include "ProxyLists.h"
#include "Interface.h"
#include "IPv6Map.h"
#include <libconfig.h>
#include <event2/buffer.h>
#include "Harvester.h"
#include <maxminddb.h>
#include "Websocket.h"
#include "Config.h"
#include <openssl/evp.h>
#include <openssl/x509.h>
#include "Stats.h"
#include "Server.h"
#include "Base64.h"

char *HtmlTemplateTags[] = {
	"{T_VERSION}",
	"{T_CURRENT_PAGE}",
	"{T_CFG_HOME_ACTIVE}",
	"{T_CFG_UPROXIES_ACTIVE}",
	"{T_CFG_PROXIES_ACTIVE}",
	"{T_CFG_SOURCES_ACTIVE}",
	"{T_CFG_STATS_ACTIVE}",
	"{T_USER}",
	"{T_COUNT_UPROXIES}",
	"{T_COUNT_PROXIES}",
	"{T_UPROXIES_HEAD}",
	"{T_UPROXIES_TABLE_ITEMS_START}",
	"{T_UPROXIES_TABLE_ITEMS_END}",
	"{T_CFG_TABLE_ODD}",
	"{T_CFG_TABLE_EVEN}",
	"{T_CFG_TABLE_OK}",
	"{T_CFG_TABLE_WARN}",
	"{T_CFG_TABLE_ERR}",
	"{T_UPROXIES_ITEM}",
	"{T_PROXIES_HEAD}",
	"{T_PROXIES_TABLE_ITEMS_START}",
	"{T_PROXIES_TABLE_ITEMS_END}",
	"{T_PROXIES_ITEM}",
	"{T_PRXSRC_HEAD}",
	"{T_PRXSRC_TABLE_ITEMS_START}",
	"{T_PRXSRC_TABLE_ITEMS_END}",
	"{T_PRXSRC_ITEM}",
	NULL,
	"{T_TABLE_BREAK}",
	"{T_STATS_GEO_HEAD}",
	"{T_STATS_GEO_TABLE_ITEMS_START}",
	"{T_STATS_GEO_TABLE_ITEMS_END}",
	"{T_STATS_GEO_ITEM}",
	"{T_CHECK_IP}",
	"{T_CHECK_PORT}",
	"{T_CHECK_TYPE}",
	"{T_CHECK_COUNTRY_LOWER}",
	"{T_CHECK_COUNTRY_UPPER}",
	"{T_CHECK_LIVE_SINCE}",
	"{T_CHECK_LAST_CHECKED}",
	"{T_CHECK_CONNECT_TIMEOUT}",
	"{T_CHECK_HTTP_S_TIMEOUT}",
	"{T_CHECK_SUCCESSFUL_CHECKS}",
	"{T_CHECK_FAILED_CHECKS}",
	"{T_CHECK_RETRIES}",
	"{T_CHECK_UID}",
	"{T_CHECK_COUNTRY_FULL}",
	"{T_SUB_SIZE_UPROXIES}",
	"{T_SUB_SIZE_PROXIES}",
	"{T_SUB_AUTH_COOKIE}",
	"{T_SUB_MSG_INTERVAL}",
	"{T_SUB_PROXY_ADD}",
	"{T_SUB_UPROXY_ADD}",
	"{T_SUB_PROXY_REMOVE}",
	"{T_SUB_UPROXY_REMOVE}",
	"{T_CFG_ENABLED}",
	"{T_CFG_DISABLED}",
	"{T_CFG_TOOLS_ACTIVE}",
	"{T_CHECK_COND_INVALID_CERT}",
	"{T_CHECK_COND_INVALID_CERT_FINGERPRINT}",
	"{T_CHECK_ELSE_COND_INVALID_CERT}",
	"{T_CHECK_END_COND_INVALID_CERT}",
	"{T_CHECK_COND_INVALID_CERT_INFO}",
	"{T_STATS_PCOUNT_HEAD}",
	"{T_STATS_PCOUNT_ITEMS_START}",
	"{T_STATS_PCOUNT_ITEMS_END}",
	"{T_STATS_PCOUNT_ITEM}",
	"{T_CPAGE_RAW_HTTP}",
	"{T_CFG_SETTINGS_ACTIVE}"
};

void HtmlTemplateLoadAll()
{
	HtmlComponentEnabled = NULL;
	HtmlComponentDisabled = NULL;

	uint8_t itemsFound = 0;
	bool fullPath = false;

#ifdef __linux__
	DIR *d;
	struct dirent *dir;

	d = opendir(LINUX_LOCAL_HTML_PATH);
	if (!d) {
		d = opendir(LINUX_GLOBAL_HTML_PATH);
		fullPath = true;
	}
#elif defined _WIN32 || defined _WIN64
	WIN32_FIND_DATA fdFile;
	HANDLE d = NULL;

	if ((d = FindFirstFile(WINDOWS_LOCAL_HTML_PATH"\\*", &fdFile)) == INVALID_HANDLE_VALUE) {
		fullPath = true;

		Log(LOG_LEVEL_DEBUG, "WIN32: Failed to search files: %d (%s)", GetLastError(), WINDOWS_LOCAL_HTML_PATH"\\*");

		size_t lenAppData = strlen(WinAppData);
		char *appDataPath = malloc(lenAppData + strlen(WINDOWS_GLOBAL_HTML_PATH) + 2 + 1); {
			strcpy(appDataPath, WinAppData);
			strcat(appDataPath, WINDOWS_GLOBAL_HTML_PATH"\\*");

			if ((d = FindFirstFile(appDataPath, &fdFile)) == INVALID_HANDLE_VALUE) {
				Log(LOG_LEVEL_ERROR, "WIN32: Failed to search files: %d (%s)", GetLastError(), appDataPath);
				d = false;
			} else {
				Log(LOG_LEVEL_DEBUG, "Opened HTML (%s) in global", appDataPath);
			}
		} free(appDataPath);
	} else {
		Log(LOG_LEVEL_DEBUG, "Opened HTML in local");
	}
#endif

	char *files[] = { "head.tmpl", "foot.tmpl", "home.tmpl", "iface.tmpl", "ifaceu.tmpl", "prxsrc.tmpl", "stats.tmpl", "check.tmpl", "tools.tmpl", "cpageraw.tmpl", "settings.tmpl" };

	if (d) {
		config_t cfg;
		config_init(&cfg);

#ifdef __linux__
		if (config_read_file(&cfg, LINUX_LOCAL_HTML_PATH"/html.conf") == CONFIG_FALSE) {
			Log(LOG_LEVEL_DEBUG, "Failed to open html.conf in working directory, opening in global...: %s (line %d)", config_error_text(&cfg), config_error_line(&cfg));

			if (config_read_file(&cfg, LINUX_GLOBAL_HTML_PATH"/html.conf") == CONFIG_FALSE) {
				Log(LOG_LEVEL_ERROR, "Failed to open %s: %s (line %d)", LINUX_GLOBAL_HTML_PATH"/html.conf", config_error_text(&cfg), config_error_line(&cfg));

#elif defined _WIN32 || defined _WIN64
		if (config_read_file(&cfg, WINDOWS_LOCAL_HTML_PATH"/html.conf") == CONFIG_FALSE) {
			Log(LOG_LEVEL_DEBUG, "WIN: Failed to open %s: %s (line %d)", WINDOWS_LOCAL_HTML_PATH"\\html.conf", config_error_text(&cfg), config_error_line(&cfg));

			char *appDataPath;
			appDataPath = malloc(strlen(WinAppData) + strlen(WINDOWS_GLOBAL_HTML_PATH) + 10 + 1);
			strcpy(appDataPath, WinAppData);
			strcat(appDataPath, WINDOWS_GLOBAL_HTML_PATH"\\html.conf");

			if (config_read_file(&cfg, appDataPath) == CONFIG_FALSE) {
				Log(LOG_LEVEL_ERROR, "Failed to open %s: %s (line %d)", appDataPath, config_error_text(&cfg), config_error_line(&cfg));
#endif
				exit(1);
			}
#if defined _WIN32 || defined _WIN64
			free(appDataPath);
#endif
		}

		config_setting_t *cfgRoot = config_root_setting(&cfg);

		char *name;

#ifdef __linux__
		while ((dir = readdir(d)) != NULL) {
			name = dir->d_name;
#elif defined _WIN32 || defined _WIN64
		do {
			name = fdFile.cFileName;
#endif
			for (size_t x = 0;x < arrlen(files);x++) {
				if (strcmp(name, files[x]) == 0) {
#ifdef __linux__
					char fullName[strlen(name) + (fullPath ? strlen(LINUX_GLOBAL_HTML_PATH) : strlen(LINUX_LOCAL_HTML_PATH)) + 1];
					sprintf(fullName, "%s/%s", fullPath ? LINUX_GLOBAL_HTML_PATH : LINUX_LOCAL_HTML_PATH, files[x]);
#elif defined _WIN32 || defined _WIN64

					char *fullName;

					if (fullPath) {
						fullName = malloc(strlen(WinAppData) + strlen(WINDOWS_GLOBAL_HTML_PATH) + 1 + strlen(files[x]) + 1);

						strcpy(fullName, WinAppData);
						strcat(fullName, WINDOWS_GLOBAL_HTML_PATH"\\");
						strcat(fullName, files[x]);
						fullName[strlen(WinAppData) + strlen(WINDOWS_GLOBAL_HTML_PATH) + 1 + strlen(files[x])] = 0x00;
					} else {
						fullName = malloc(strlen(WINDOWS_LOCAL_HTML_PATH) + 1 + strlen(files[x]) + 1);
						strcpy(fullName, WINDOWS_LOCAL_HTML_PATH"\\");
						strcat(fullName, files[x]);
						fullName[strlen(WINDOWS_LOCAL_HTML_PATH) + 1 + strlen(files[x])] = 0x00;
					}
#endif
					Log(LOG_LEVEL_DEBUG, "Opening %s...", fullName);

					FILE *hFile = fopen(fullName, "r");
					if (hFile != NULL) {
						Log(LOG_LEVEL_DEBUG, "Parsing %s...", name);

						if (x == 0)
							HtmlTemplateParse(hFile, &HtmlTemplateHead, &HtmlTemplateHeadSize, cfgRoot);
						else if (x == 1)
							HtmlTemplateParse(hFile, &HtmlTemplateFoot, &HtmlTemplateFootSize, cfgRoot);
						else if (x == 2)
							HtmlTemplateParse(hFile, &HtmlTemplateHome, &HtmlTemplateHomeSize, cfgRoot);
						else if (x == 3)
							HtmlTemplateParse(hFile, &HtmlTemplateProxies, &HtmlTemplateProxiesSize, cfgRoot);
						else if (x == 4)
							HtmlTemplateParse(hFile, &HtmlTemplateUProxies, &HtmlTemplateUProxiesSize, cfgRoot);
						else if (x == 5)
							HtmlTemplateParse(hFile, &HtmlTemplateProxySources, &HtmlTemplateProxySourcesSize, cfgRoot);
						else if (x == 6)
							HtmlTemplateParse(hFile, &HtmlTemplateStats, &HtmlTemplateStatsSize, cfgRoot);
						else if (x == 7)
							HtmlTemplateParse(hFile, &HtmlTemplateCheck, &HtmlTemplateCheckSize, cfgRoot);
						else if (x == 8)
							HtmlTemplateParse(hFile, &HtmlTemplateTools, &HtmlTemplateToolsSize, cfgRoot);
						else if (x == 9)
							HtmlTemplateParse(hFile, &HtmlTemplateCPageRaw, &HtmlTemplateCPageRawSize, cfgRoot);
						else if (x == 10)
							HtmlTemplateParse(hFile, &HtmlTemplateSettings, &HtmlTemplateSettingsSize, cfgRoot);

						itemsFound++;
					} else {
#if defined _WIN32 || defined _WIN64
						Log(LOG_LEVEL_DEBUG, "WIN: fopen %s (r) failed (%d)", fullName, GetLastError());
#endif
					}
					fclose(hFile);
#if defined _WIN32 || defined _WIN64
					Log(LOG_LEVEL_DEBUG, "WIN: free %s ", fullName);
					free(fullName);
#endif
				}
			}
#ifdef __linux__
		}
#elif defined _WIN32 || defined _WIN64
		} while (FindNextFile(d, &fdFile));
#endif

		//config_destroy(&cfg); // TODO: fix segfault here

		if (itemsFound != 11) {
			Log(LOG_LEVEL_ERROR, "Not all HTML templates found. (%d of 11)", itemsFound);
			exit(1);
		}

#ifdef __linux__
		closedir(d);
#elif defined _WIN32 || defined _WIN64
		FindClose(d);
#endif
		Log(LOG_LEVEL_DEBUG, "Parsed all HTML tempalates");
	} else {
		Log(LOG_LEVEL_ERROR, "Cannot open HTML template dir");
		exit(1);
	}
}

void HtmlTemplateMimeTypesInit()
{
	config_t cfg;
	config_init(&cfg);
	HtmlTemplateMimeTypes = NULL;
	HtmlTemplateMimeTypesSize = 0;

#ifdef __linux__
	if (config_read_file(&cfg, LINUX_LOCAL_HTML_PATH"/html.conf") == CONFIG_FALSE) {
		Log(LOG_LEVEL_DEBUG, "Failed to open html.conf in working directory, opening in global...: %s (line %d)", config_error_text(&cfg), config_error_line(&cfg));

		if (config_read_file(&cfg, LINUX_GLOBAL_HTML_PATH"/html.conf") == CONFIG_FALSE) {
			Log(LOG_LEVEL_ERROR, "Failed to open %s: %s (line %d), using stock HTML...", LINUX_GLOBAL_HTML_PATH"/html.conf", config_error_text(&cfg), config_error_line(&cfg));

#elif defined _WIN32 || defined _WIN64
	if (config_read_file(&cfg, WINDOWS_LOCAL_HTML_PATH"/html.conf") == CONFIG_FALSE) {
		Log(LOG_LEVEL_DEBUG, "WIN: Failed to open %s: %s (line %d)", WINDOWS_LOCAL_HTML_PATH"\\html.conf", config_error_text(&cfg), config_error_line(&cfg));

		char *appDataPath;
		appDataPath = malloc(strlen(WinAppData) + strlen(WINDOWS_GLOBAL_HTML_PATH) + 10 + 1);
		strcpy(appDataPath, WinAppData);
		strcat(appDataPath, WINDOWS_GLOBAL_HTML_PATH"\\html.conf");

		if (config_read_file(&cfg, appDataPath) == CONFIG_FALSE) {
			Log(LOG_LEVEL_ERROR, "Failed to open %s: %s (line %d), using stock HTML...", appDataPath, config_error_text(&cfg), config_error_line(&cfg));
#endif
			exit(1);
			return;
		}
#if defined _WIN32 || defined _WIN64
		free(appDataPath);
#endif
	}

	config_setting_t *cfgRoot = config_root_setting(&cfg);

	size_t x = 0;
	config_setting_t *currentBlock;

	config_setting_t *mimeTypes = config_setting_get_member(cfgRoot, "MimeTypes");
	if (mimeTypes != NULL) {
		while ((currentBlock = config_setting_get_elem(mimeTypes, x)) != NULL) {
			const char *val = config_setting_get_string(currentBlock);
			if (x % 2 == 0) {
				if (HtmlTemplateMimeTypes == NULL)
					HtmlTemplateMimeTypes = malloc(++HtmlTemplateMimeTypesSize * sizeof(HTML_TEMPLATE_MIME_TYPE));
				else
					HtmlTemplateMimeTypes = realloc(HtmlTemplateMimeTypes, ++HtmlTemplateMimeTypesSize * sizeof(HTML_TEMPLATE_MIME_TYPE));

				HtmlTemplateMimeTypes[HtmlTemplateMimeTypesSize - 1].extension = malloc(strlen(val) + 1);
				strcpy(HtmlTemplateMimeTypes[HtmlTemplateMimeTypesSize - 1].extension, val);
			} else {
				HtmlTemplateMimeTypes[HtmlTemplateMimeTypesSize - 1].type = malloc(strlen(val) + 1);
				strcpy(HtmlTemplateMimeTypes[HtmlTemplateMimeTypesSize - 1].type, config_setting_get_string(currentBlock));
			}
			x++;
		}
	}
	config_destroy(&cfg);
}

static void HtmlTemplateFindFirst(char *Contents, OUT HTML_TEMPLATE_COMPONENT_IDENTIFIER *Identifier, OUT char **Offset)
{
	*Offset = (char*)SIZE_MAX;
	*Identifier = HTML_TEMPLATE_COMPONENT_IDENTIFIER_INVALID;

	for (size_t x = 0;x < arrlen(HtmlTemplateTags);x++) {
		if (HtmlTemplateTags[x] == NULL)
			continue;

		char *cur = strstr(Contents, HtmlTemplateTags[x]);

		if (cur != NULL && cur < *Offset) {
			Log(LOG_LEVEL_DEBUG, "TAG %s (%d)", HtmlTemplateTags[x], x);
			*Offset = cur;
			*Identifier = x;
		}
	}
}

static void HtmlTemplateComponentPush(HTML_TEMPLATE_COMPONENT **Components, size_t *CurrentSize, HTML_TEMPLATE_COMPONENT Component)
{
	(*CurrentSize)++;

	*Components = *CurrentSize == 1 ? malloc(*CurrentSize * sizeof(HTML_TEMPLATE_COMPONENT)) : realloc(*Components, *CurrentSize * sizeof(HTML_TEMPLATE_COMPONENT));
	memcpy(&((*Components)[*CurrentSize - 1]), &Component, sizeof(HTML_TEMPLATE_COMPONENT));
}

void HtmlTemplateParse(FILE *hFile, HTML_TEMPLATE_COMPONENT **Template, size_t *SizeRef, config_setting_t *CfgRoot)
{
	fseek(hFile, 0, SEEK_END);
	size_t size = ftell(hFile);
	fseek(hFile, 0, SEEK_SET);

	char *string = malloc(size + 1);
	fread(string, size, 1, hFile);
	string[size] = 0;

	*SizeRef = 0;
	*Template = NULL;

	char *curEnd = string;

	HTML_TEMPLATE_COMPONENT_IDENTIFIER identifier;
	HTML_TEMPLATE_COMPONENT comp;
	char *offset;

	while (1) {
		HtmlTemplateFindFirst(curEnd, &identifier, &offset);

		if (identifier == HTML_TEMPLATE_COMPONENT_IDENTIFIER_INVALID) {
			// No more components
			comp.identifier = HTML_TEMPLATE_COMPONENT_IDENTIFIER_STATIC;
			comp.content = malloc(strlen(curEnd) + 1);
			strcpy(comp.content, curEnd);
			HtmlTemplateComponentPush(Template, SizeRef, comp);
			break;
		} else {
			if (offset != curEnd) {
				// Push static content in front of component
				comp.identifier = HTML_TEMPLATE_COMPONENT_IDENTIFIER_STATIC;
				comp.content = malloc(offset - curEnd + 1);
				strncpy(comp.content, curEnd, offset - curEnd);
				((char*)(comp.content))[offset - curEnd] = 0x00;

				HtmlTemplateComponentPush(Template, SizeRef, comp);
				curEnd = offset;
			}
			if (offset == curEnd) {
				// Push dynamic content
				comp.identifier = identifier;

#define CONFIG_STRING(cfg, svar, var) const char *val; if (config_setting_lookup_string(cfg, svar, &(val)) == CONFIG_FALSE) { Log(LOG_LEVEL_ERROR, "Failed to lookup %s", svar); exit(1); } else { var = malloc(strlen(val) + 1); strcpy(var, val); }
#define CONFIG_INT(cfg, svar, var) if (config_setting_lookup_int(cfg, svar, (int*)(&(var))) == CONFIG_FALSE) { Log(LOG_LEVEL_ERROR, "Failed to lookup %s", svar); exit(1); }

				switch (identifier) {
					case HTML_TEMPLATE_COMPONENT_IDENTIFIER_VERSION: {
						comp.content = VERSION;
						break;
					}
					case HTML_TEMPLATE_COMPONENT_IDENTIFIER_CFG_HOME_ACTIVE: {
						CONFIG_STRING(CfgRoot, "HomeActive", comp.content);
						break;
					}
					case HTML_TEMPLATE_COMPONENT_IDENTIFIER_CFG_UPROXIES_ACTIVE: {
						CONFIG_STRING(CfgRoot, "UProxiesActive", comp.content);
						break;
					}
					case HTML_TEMPLATE_COMPONENT_IDENTIFIER_CFG_PROXIES_ACTIVE: {
						CONFIG_STRING(CfgRoot, "ProxiesActive", comp.content);
						break;
					}
					case HTML_TEMPLATE_COMPONENT_IDENTIFIER_CFG_SOURCES_ACTIVE: {
						CONFIG_STRING(CfgRoot, "SourcesActive", comp.content);
						break;
					}
					case HTML_TEMPLATE_COMPONENT_IDENTIFIER_CFG_STATS_ACTIVE: {
						CONFIG_STRING(CfgRoot, "StatsActive", comp.content);
						break;
					}
					case HTML_TEMPLATE_COMPONENT_IDENTIFIER_CFG_TOOLS_ACTIVE: {
						CONFIG_STRING(CfgRoot, "ToolsActive", comp.content);
						break;
					}
					case HTML_TEMPLATE_COMPONENT_IDENTIFIER_CFG_SETTINGS_ACTIVE: {
						CONFIG_STRING(CfgRoot, "SettingsActive", comp.content);
						break;
					}
					case HTML_TEMPLATE_COMPONENT_IDENTIFIER_CFG_TABLE_ODD: {
						CONFIG_STRING(CfgRoot, "TableOdd", comp.content);
						break;
					}
					case HTML_TEMPLATE_COMPONENT_IDENTIFIER_CFG_TABLE_EVEN: {
						CONFIG_STRING(CfgRoot, "TableEven", comp.content);
						break;
					}
					case HTML_TEMPLATE_COMPONENT_IDENTIFIER_CFG_TABLE_OK: {
						CONFIG_STRING(CfgRoot, "TableOk", comp.content);
						break;
					}
					case HTML_TEMPLATE_COMPONENT_IDENTIFIER_CFG_TABLE_WARN: {
						CONFIG_STRING(CfgRoot, "TableWarn", comp.content);
						break;
					}
					case HTML_TEMPLATE_COMPONENT_IDENTIFIER_CFG_TABLE_ERR: {
						CONFIG_STRING(CfgRoot, "TableError", comp.content);
						break;
					}
					case HTML_TEMPLATE_COMPONENT_IDENTIFIER_CFG_ENABLED: {
						if (HtmlComponentEnabled == NULL) {
							CONFIG_STRING(CfgRoot, "Enabled", HtmlComponentEnabled);
						}
						comp.content = HtmlComponentEnabled;
						break;
					}
					case HTML_TEMPLATE_COMPONENT_IDENTIFIER_CFG_DISABLED: {
						if (HtmlComponentDisabled == NULL) {
							CONFIG_STRING(CfgRoot, "Disabled", HtmlComponentDisabled);
						}
						comp.content = HtmlComponentDisabled;
						break;
					}
					case HTML_TEMPLATE_COMPONENT_IDENTIFIER_SUB_MSG_INTERVAL: {
						comp.content = (void*)WSMessageInterval;
						break;
					}
					case HTML_TEMPLATE_COMPONENT_IDENTIFIER_COUNT_PROXIES: {
						comp.content = (void*)(&SizeCheckedProxies);
						break;
					}
					case HTML_TEMPLATE_COMPONENT_IDENTIFIER_COUNT_UPROXIES: {
						comp.content = (void*)(&SizeUncheckedProxies);
						break;
					}
					case HTML_TEMPLATE_COMPONENT_IDENTIFIER_SUB_SIZE_PROXIES: {
						comp.content = (void*)WEBSOCKET_SERVER_COMMAND_SIZE_PROXIES;
						break;
					}
					case HTML_TEMPLATE_COMPONENT_IDENTIFIER_SUB_SIZE_UPROXIES: {
						comp.content = (void*)WEBSOCKET_SERVER_COMMAND_SIZE_UPROXIES;
						break;
					}
					case HTML_TEMPLATE_COMPONENT_IDENTIFIER_SUB_PROXY_ADD: {
						comp.content = (void*)WEBSOCKET_SERVER_COMMAND_PROXY_ADD;
						break;
					}
					case HTML_TEMPLATE_COMPONENT_IDENTIFIER_SUB_UPROXY_ADD: {
						comp.content = (void*)WEBSOCKET_SERVER_COMMAND_UPROXY_ADD;
						break;
					}
					case HTML_TEMPLATE_COMPONENT_IDENTIFIER_SUB_PROXY_REMOVE: {
						comp.content = (void*)WEBSOCKET_SERVER_COMMAND_PROXY_REMOVE;
						break;
					}
					case HTML_TEMPLATE_COMPONENT_IDENTIFIER_SUB_UPROXY_REMOVE: {
						comp.content = (void*)WEBSOCKET_SERVER_COMMAND_UPROXY_REMOVE;
						break;
					}
					case HTML_TEMPLATE_COMPONENT_IDENTIFIER_SUB_AUTH_COOKIE: {
						comp.content = AUTH_COOKIE;
						break;
					}
					default: {
						break;
					}
				}

#undef CONFIG_STRING
				HtmlTemplateComponentPush(Template, SizeRef, comp);
				curEnd = offset + strlen(HtmlTemplateTags[identifier]);
			}
		}
		// Log(LOG_LEVEL_DEBUG, "Size: %d", *SizeRef);
	}
	free(string);
}

void HtmlTemplateBufferInsert(struct evbuffer *Buffer, HTML_TEMPLATE_COMPONENT *Components, size_t Size, WEB_INTERFACE_INFO Info, HTML_TEMPALTE_TABLE_INFO TableInfo)
{
	uint8_t rowStatus = 0;

	if (TableInfo.inTable) {
		switch (Info.currentPage->page) {
			case INTERFACE_PAGE_UPROXIES: {
				// No color
				rowStatus = 0;
				break;
			}
			case INTERFACE_PAGE_PROXIES: {
				// Switch on anonymity
				switch (((PROXY*)(TableInfo.tableObject))->anonymity) {
					case ANONYMITY_MAX: {
						rowStatus = 1;
						break;
					}
					case ANONYMITY_ANONYMOUS: {
						rowStatus = 2;
						break;
					}
					case ANONYMITY_TRANSPARENT: {
						rowStatus = 3;
						break;
					}
					default: {
						break;
					}
				}
				break;
			}
			default: {
				break;
			}
		}
	}

	/*Log(LOG_LEVEL_DEBUG, "TableInfo:");
	Log(LOG_LEVEL_DEBUG, ".x: %d", TableInfo.currentComponentIteration);
	Log(LOG_LEVEL_DEBUG, ".inTable: %s", TableInfo.inTable ? "true" : "false");
	Log(LOG_LEVEL_DEBUG, ".iteration: %d", TableInfo.tableObjectIteration);
	Log(LOG_LEVEL_DEBUG, ".tableObject: %p", TableInfo.tableObject);

	Log(LOG_LEVEL_DEBUG, "Size: %d", Size);*/

	for (size_t x = TableInfo.currentComponentIteration;x < Size;x++) {
		/*Log(LOG_LEVEL_DEBUG, "Component (%d): ", x);
		Log(LOG_LEVEL_DEBUG, ".content: %s", Components[x].content);
		Log(LOG_LEVEL_DEBUG, ".identifier: %d", Components[x].identifier);*/

		if (Info.currentPage->page == INTERFACE_PAGE_CPAGE_RAW && (size_t)TableInfo.tableObject == 1) {
			do {
				x++;
				Log(LOG_LEVEL_DEBUG, "CPAGE ++ -> %d (%d)", x, Components[x].identifier);
			} while (Components[x].identifier != HTML_TEMPLATE_COMPONENT_IDENTIFIER_CPAGE_RAW_HTTP && x < Size);
			if (x >= Size)
				break;
		}

		switch (Components[x].identifier) {
			case HTML_TEMPLATE_COMPONENT_IDENTIFIER_CURRENT_PAGE: {
				evbuffer_add(Buffer, Info.currentPage->name, strlen(Info.currentPage->name));
				break;
			}
			case HTML_TEMPLATE_COMPONENT_IDENTIFIER_CFG_HOME_ACTIVE: {
				if (Info.currentPage->page != INTERFACE_PAGE_HOME)
					continue;
				break;
			}
			case HTML_TEMPLATE_COMPONENT_IDENTIFIER_CFG_UPROXIES_ACTIVE: {
				if (Info.currentPage->page != INTERFACE_PAGE_UPROXIES)
					continue;
				break;
			}
			case HTML_TEMPLATE_COMPONENT_IDENTIFIER_CFG_PROXIES_ACTIVE: {
				if (Info.currentPage->page != INTERFACE_PAGE_PROXIES)
					continue;
				break;
			}
			case HTML_TEMPLATE_COMPONENT_IDENTIFIER_CFG_SOURCES_ACTIVE: {
				if (Info.currentPage->page != INTERFACE_PAGE_PRXSRC)
					continue;
				break;
			}
			case HTML_TEMPLATE_COMPONENT_IDENTIFIER_CFG_STATS_ACTIVE: {
				if (Info.currentPage->page != INTERFACE_PAGE_STATS)
					continue;
				break;
			}
			case HTML_TEMPLATE_COMPONENT_IDENTIFIER_CFG_TOOLS_ACTIVE: {
				if (Info.currentPage->page != INTERFACE_PAGE_TOOLS)
					continue;
				break;
			}
			case HTML_TEMPLATE_COMPONENT_IDENTIFIER_CFG_SETTINGS_ACTIVE: {
				if (Info.currentPage->page != INTERFACE_PAGE_SETTINGS)
					continue;
				break;
			}
			case HTML_TEMPLATE_COMPONENT_IDENTIFIER_CFG_TABLE_ODD: {
				if (TableInfo.inTable && TableInfo.tableObjectIteration % 2 == 0)
					continue;
				break;
			}
			case HTML_TEMPLATE_COMPONENT_IDENTIFIER_CFG_TABLE_EVEN: {
				if (TableInfo.inTable && TableInfo.tableObjectIteration % 2 != 0)
					continue;
				break;
			}
			case HTML_TEMPLATE_COMPONENT_IDENTIFIER_CFG_TABLE_OK: {
				if (TableInfo.inTable && rowStatus != 1)
					continue;
				break;
			}
			case HTML_TEMPLATE_COMPONENT_IDENTIFIER_CFG_TABLE_WARN: {
				if (TableInfo.inTable && rowStatus != 2)
					continue;
				break;
			}
			case HTML_TEMPLATE_COMPONENT_IDENTIFIER_CFG_TABLE_ERR: {
				if (TableInfo.inTable && rowStatus != 3)
					continue;
				break;
			}
			case HTML_TEMPLATE_COMPONENT_IDENTIFIER_USER: {
				evbuffer_add(Buffer, Info.user, strlen(Info.user));
				break;
			}
			case HTML_TEMPLATE_COMPONENT_IDENTIFIER_COUNT_UPROXIES:
			case HTML_TEMPLATE_COMPONENT_IDENTIFIER_COUNT_PROXIES: {
				evbuffer_add_printf(Buffer, "%zu", *((size_t*)(Components[x].content)));
				break;
			}
			case HTML_TEMPLATE_COMPONENT_IDENTIFIER_SUB_SIZE_PROXIES:
			case HTML_TEMPLATE_COMPONENT_IDENTIFIER_SUB_SIZE_UPROXIES:
			case HTML_TEMPLATE_COMPONENT_IDENTIFIER_SUB_MSG_INTERVAL:
			case HTML_TEMPLATE_COMPONENT_IDENTIFIER_SUB_UPROXY_ADD:
			case HTML_TEMPLATE_COMPONENT_IDENTIFIER_SUB_PROXY_ADD:
			case HTML_TEMPLATE_COMPONENT_IDENTIFIER_SUB_UPROXY_REMOVE:
			case HTML_TEMPLATE_COMPONENT_IDENTIFIER_SUB_PROXY_REMOVE: {
				evbuffer_add_printf(Buffer, "%zu", (size_t)Components[x].content);
				break;
			}
			case HTML_TEMPLATE_COMPONENT_IDENTIFIER_UPROXIES_HEAD:
			case HTML_TEMPLATE_COMPONENT_IDENTIFIER_UPROXIES_ITEM: {
				bool item = Components[x].identifier == HTML_TEMPLATE_COMPONENT_IDENTIFIER_UPROXIES_ITEM;
				UNCHECKED_PROXY *uproxy = (UNCHECKED_PROXY*)TableInfo.tableObject;

				switch (TableInfo.tableHeadOrItemIteration) {
					case 0: {
						if (item) {
							char *ip = IPv6MapToString2(uproxy->ip); {
								evbuffer_add_printf(Buffer, "%s:%d", ip, uproxy->port);
							} free(ip);
						} else
							evbuffer_add(Buffer, "IP:Port", 7);
						break;
					}
					case 1: {
						if (item)
							evbuffer_add_printf(Buffer, "%s", ProxyGetTypeString(uproxy->type));
						else
							evbuffer_add(Buffer, "Type", 4);
						break;
					}
					case 2: {
						if (item)
							evbuffer_add(Buffer, uproxy->checking ? HtmlComponentEnabled : HtmlComponentDisabled, (uproxy->checking ? strlen(HtmlComponentEnabled) : strlen(HtmlComponentDisabled)));
						else
							evbuffer_add(Buffer, "Currently checking", 18);
						break;
					}
					case 3: {
						if (item)
							evbuffer_add_printf(Buffer, "%d", uproxy->retries);
						else
							evbuffer_add(Buffer, "Retries", 7);
						break;
					}
					case 4: {
						if (item)
							evbuffer_add(Buffer, uproxy->associatedProxy != NULL ? HtmlComponentEnabled : HtmlComponentDisabled, (uproxy->associatedProxy != NULL ? strlen(HtmlComponentEnabled) : strlen(HtmlComponentDisabled)));
						else
							evbuffer_add(Buffer, "Rechecking", 10);
						TableInfo.tableHeadOrItemIteration = -1; // line+4 sets it to 0
						break;
					}
				}
				TableInfo.tableHeadOrItemIteration++; // << line+4
				break;
			}
			case HTML_TEMPLATE_COMPONENT_IDENTIFIER_PROXIES_HEAD:
			case HTML_TEMPLATE_COMPONENT_IDENTIFIER_PROXIES_ITEM: {
				bool item = Components[x].identifier == HTML_TEMPLATE_COMPONENT_IDENTIFIER_PROXIES_ITEM;
				PROXY *proxy = (PROXY*)TableInfo.tableObject;

				switch (TableInfo.tableHeadOrItemIteration) {
					case 0: {
						if (item) {
							char *ip = IPv6MapToString2(proxy->ip); {
								evbuffer_add_printf(Buffer, "%s:%d", ip, proxy->port);
							} free(ip);
						} else
							evbuffer_add(Buffer, "IP:Port", 7);
						break;
					}
					case 1: {
						if (item)
							evbuffer_add_printf(Buffer, "%s", ProxyGetTypeString(proxy->type));
						else
							evbuffer_add(Buffer, "Type", 4);
						break;
					}
					case 2: {
						if (item)
							evbuffer_add(Buffer, proxy->country, 2);
						else
							evbuffer_add(Buffer, "Country", 7);
						break;
					}
					case 3: {
						if (item) {
							if (proxy->anonymity == ANONYMITY_MAX)
								evbuffer_add(Buffer, "Max", 3);
							else if (proxy->anonymity == ANONYMITY_ANONYMOUS)
								evbuffer_add(Buffer, "Anonymous", 9);
							else if (proxy->anonymity == ANONYMITY_TRANSPARENT)
								evbuffer_add(Buffer, "Transparent", 11);
							else
								evbuffer_add(Buffer, "N/A", 3);
						} else
							evbuffer_add(Buffer, "Anonymity", 9);
						break;
					}
					case 4: {
						if (item) {
							evbuffer_add_printf(Buffer, "%"PRIu64, proxy->timeoutMs);
						} else
							evbuffer_add(Buffer, "Connection latency (ms)", 23);
						break;
					}
					case 5: {
						if (item)
							evbuffer_add_printf(Buffer, "%"PRIu64, proxy->httpTimeoutMs);
						else
							evbuffer_add(Buffer, "HTTP/S latency (ms)", 19);
						break;
					}
					case 6: {
						if (item) {
							char *time = FormatTime(proxy->liveSinceMs); {
								evbuffer_add_printf(Buffer, "%s", time);
							} free(time);
						} else
							evbuffer_add(Buffer, "Live since", 10);
						break;
					}
					case 7: {
						if (item) {
							char *time = FormatTime(proxy->lastCheckedMs); {
								evbuffer_add_printf(Buffer, "%s", time);
							} free(time);
						} else
							evbuffer_add(Buffer, "Last checked", 12);
						break;
					}
					case 8: {
						if (item)
							evbuffer_add_printf(Buffer, "%d", proxy->retries);
						else
							evbuffer_add(Buffer, "Retries", 7);
						break;
					}
					case 9: {
						if (item)
							evbuffer_add_printf(Buffer, "%d", proxy->successfulChecks);
						else
							evbuffer_add(Buffer, "Successful checks", 17);
						break;
					}
					case 10: {
						if (item)
							evbuffer_add_printf(Buffer, "%d", proxy->failedChecks);
						else
							evbuffer_add(Buffer, "Failed checks", 13);
						break;
					}
					case 11: {
						if (item) {
							char *identifierb64;
							Base64Encode(proxy->identifier, PROXY_IDENTIFIER_LEN, &identifierb64); {
								evbuffer_add_printf(Buffer, "<a href=\"/recheck?uid=%s\">Check</a>", identifierb64);
							} free(identifierb64);
						} else
							evbuffer_add(Buffer, "Full check", 10);
						TableInfo.tableHeadOrItemIteration = -1; // line+4 sets it to 0
						break;
					}
				}
				TableInfo.tableHeadOrItemIteration++; // << line+4
				break;
			}
			case HTML_TEMPLATE_COMPONENT_IDENTIFIER_PRXSRC_HEAD:
			case HTML_TEMPLATE_COMPONENT_IDENTIFIER_PRXSRC_ITEM: {
				bool item = Components[x].identifier == HTML_TEMPLATE_COMPONENT_IDENTIFIER_PRXSRC_ITEM;
				HARVESTER_PRXSRC_STATS_ENTRY *entry = (HARVESTER_PRXSRC_STATS_ENTRY*)TableInfo.tableObject;

				switch (TableInfo.tableHeadOrItemIteration) {
					case 0: {
						if (item) {
							evbuffer_add(Buffer, entry->name, strlen(entry->name));
						} else
							evbuffer_add(Buffer, "Name", 4);
						break;
					}
					case 1: {
						if (item)
							evbuffer_add_printf(Buffer, "%s", ProxySourceTypeToString(entry->type));
						else
							evbuffer_add(Buffer, "Type", 4);
						break;
					}
					case 2: {
						if (item)
							evbuffer_add_printf(Buffer, "%"PRIu64, entry->addedNew);
						else
							evbuffer_add(Buffer, "New proxies", 11);
						break;
					}
					case 3: {
						if (item)
							evbuffer_add_printf(Buffer, "%"PRIu64, entry->added);
						else
							evbuffer_add(Buffer, "Total proxies", 13);
						TableInfo.tableHeadOrItemIteration = -1; // line+4 sets it to 0
						break;
					}
				}
				TableInfo.tableHeadOrItemIteration++; // << line+4
				break;
			}
			case HTML_TEMPLATE_COMPONENT_IDENTIFIER_STATS_GEO_HEAD:
			case HTML_TEMPLATE_COMPONENT_IDENTIFIER_STATS_GEO_ITEM: {
				bool item = Components[x].identifier == HTML_TEMPLATE_COMPONENT_IDENTIFIER_STATS_GEO_ITEM;
				HTML_TEMPLATE_TABLE_STATS_GEO *entry = (HTML_TEMPLATE_TABLE_STATS_GEO*)TableInfo.tableObject;

				switch (TableInfo.tableHeadOrItemIteration) {
					case 0: {
						if (item) {
							evbuffer_add(Buffer, entry->countryCode, 2);
						} else
							evbuffer_add(Buffer, "Country", 7);
						break;
					}
					case 1: {
						if (item)
							evbuffer_add_printf(Buffer, "%"PRIu64, entry->count);
						else
							evbuffer_add(Buffer, "Proxies", 7);
						TableInfo.tableHeadOrItemIteration = -1; // line+4 sets it to 0
						break;
					}
				}
				TableInfo.tableHeadOrItemIteration++; // << line+4
				break;
			}
			case HTML_TEMPLATE_COMPONENT_IDENTIFIER_STATS_PCOUNT_HEAD:
			case HTML_TEMPLATE_COMPONENT_IDENTIFIER_STATS_PCOUNT_ITEM: {
				bool item = Components[x].identifier == HTML_TEMPLATE_COMPONENT_IDENTIFIER_STATS_PCOUNT_ITEM;
				STATS_PROXY_COUNT *entry = (STATS_PROXY_COUNT*)TableInfo.tableObject;

				switch (TableInfo.tableHeadOrItemIteration) {
					case 0: {
						if (item) {
							char *time = FormatTime(entry->Time); {
								evbuffer_add_printf(Buffer, "%s", time);
							} free(time);
						} else
							evbuffer_add(Buffer, "Time", 4);
						break;
					}
					case 1: {
						if (item) {
							evbuffer_add_printf(Buffer, "%"PRIu64, entry->Proxy);
						} else
							evbuffer_add(Buffer, "Checked proxies", 15);
						break;
					}
					case 2: {
						if (item)
							evbuffer_add_printf(Buffer, "%"PRIu64, entry->UProxy);
						else
							evbuffer_add(Buffer, "Unchecked proxies", 17);
						TableInfo.tableHeadOrItemIteration = -1; // line+4 sets it to 0
						break;
					}
				}
				TableInfo.tableHeadOrItemIteration++; // << line+4
				break;
			}
			case HTML_TEMPLATE_COMPONENT_IDENTIFIER_UPROXIES_TABLE_ITEMS_START: {
				pthread_mutex_lock(&LockUncheckedProxies); {
					for (uint64_t i = 0;i < SizeUncheckedProxies;i++) {
						HTML_TEMPALTE_TABLE_INFO tableInfo;
						tableInfo.inTable = true;
						tableInfo.currentComponentIteration = x + 1;
						tableInfo.tableObjectIteration = (size_t)i;
						tableInfo.tableHeadOrItemIteration = 0;
						tableInfo.tableObject = UncheckedProxies[i];
						HtmlTemplateBufferInsert(Buffer, Components, Size, Info, tableInfo);
					}
				} pthread_mutex_unlock(&LockUncheckedProxies);
				while (Components[x].identifier != HTML_TEMPLATE_COMPONENT_IDENTIFIER_UPROXIES_TABLE_ITEMS_END && x < Size)
					x++;
				break;
			}
			case HTML_TEMPLATE_COMPONENT_IDENTIFIER_PROXIES_TABLE_ITEMS_START: {
				pthread_mutex_lock(&LockCheckedProxies); {
					for (uint64_t i = 0;i < SizeCheckedProxies;i++) {
						HTML_TEMPALTE_TABLE_INFO tableInfo;
						tableInfo.inTable = true;
						tableInfo.currentComponentIteration = x + 1;
						tableInfo.tableObjectIteration = (size_t)i;
						tableInfo.tableHeadOrItemIteration = 0;
						tableInfo.tableObject = CheckedProxies[i];
						HtmlTemplateBufferInsert(Buffer, Components, Size, Info, tableInfo);
					}
				} pthread_mutex_unlock(&LockCheckedProxies);
				while (Components[x].identifier != HTML_TEMPLATE_COMPONENT_IDENTIFIER_PROXIES_TABLE_ITEMS_END && x < Size)
					x++;
				break;
			}
			case HTML_TEMPLATE_COMPONENT_IDENTIFIER_PRXSRC_TABLE_ITEMS_START: {
				pthread_mutex_lock(&LockStatsHarvesterPrxsrc); {
					for (size_t i = 0;i < SizeStatsHarvesterPrxsrc;i++) {
						HTML_TEMPALTE_TABLE_INFO tableInfo;
						tableInfo.inTable = true;
						tableInfo.currentComponentIteration = x + 1;
						tableInfo.tableObjectIteration = (size_t)i;
						tableInfo.tableHeadOrItemIteration = 0;
						tableInfo.tableObject = &(HarvesterStatsPrxsrc[i]);
						HtmlTemplateBufferInsert(Buffer, Components, Size, Info, tableInfo);
					}
				} pthread_mutex_unlock(&LockStatsHarvesterPrxsrc);
				while (Components[x].identifier != HTML_TEMPLATE_COMPONENT_IDENTIFIER_PRXSRC_TABLE_ITEMS_END && x < Size)
					x++;
				break;
			}
			case HTML_TEMPLATE_COMPONENT_IDENTIFIER_STATS_GEO_TABLE_ITEMS_START: {
				HTML_TEMPLATE_TABLE_STATS_GEO *statsGeo = NULL;
				size_t statsGeoSize = 0;
				pthread_mutex_lock(&LockCheckedProxies); {
					for (uint64_t i = 0;i < SizeCheckedProxies;i++) {
						ssize_t foundIndex = -1;
						for (size_t a = 0;a < statsGeoSize;a++) {
							if (strncmp(statsGeo[a].countryCode, CheckedProxies[i]->country, 2) == 0) {
								foundIndex = a;
								break;
							}
						}
						if (foundIndex != -1) {
							statsGeo[foundIndex].count++;
						} else {
							statsGeo = statsGeoSize == 0 ? malloc(++statsGeoSize * sizeof(HTML_TEMPLATE_TABLE_STATS_GEO)) : realloc(statsGeo, ++statsGeoSize * sizeof(HTML_TEMPLATE_TABLE_STATS_GEO));
							statsGeo[statsGeoSize - 1].countryCode = CheckedProxies[i]->country;
							statsGeo[statsGeoSize - 1].count = 1;
						}
					}
				} pthread_mutex_unlock(&LockCheckedProxies);
				for (size_t i = 0;i < statsGeoSize;i++) {
					HTML_TEMPALTE_TABLE_INFO tableInfo;
					tableInfo.inTable = true;
					tableInfo.currentComponentIteration = x + 1;
					tableInfo.tableObjectIteration = i;
					tableInfo.tableHeadOrItemIteration = 0;
					tableInfo.tableObject = &(statsGeo[i]);
					HtmlTemplateBufferInsert(Buffer, Components, Size, Info, tableInfo);
				}
				free(statsGeo);
				while (Components[x].identifier != HTML_TEMPLATE_COMPONENT_IDENTIFIER_STATS_GEO_TABLE_ITEMS_END && x < Size)
					x++;
				break;
			}
			case HTML_TEMPLATE_COMPONENT_IDENTIFIER_STATS_PCOUNT_TABLE_ITEMS_START: {
				pthread_mutex_lock(&LockStatsProxyCount); {
					for (size_t i = 0;i < StatsProxyCountSize;i++) {
						HTML_TEMPALTE_TABLE_INFO tableInfo;
						tableInfo.inTable = true;
						tableInfo.currentComponentIteration = x + 1;
						tableInfo.tableObjectIteration = i;
						tableInfo.tableHeadOrItemIteration = 0;
						tableInfo.tableObject = &(StatsProxyCount[i]);
						HtmlTemplateBufferInsert(Buffer, Components, Size, Info, tableInfo);
					}
				} pthread_mutex_unlock(&LockStatsProxyCount);
				while (Components[x].identifier != HTML_TEMPLATE_COMPONENT_IDENTIFIER_STATS_PCOUNT_TABLE_ITEMS_END && x < Size)
					x++;
				break;
			}
			case HTML_TEMPLATE_COMPONENT_IDENTIFIER_CHECK_IP: {
				char *ip = IPv6MapToString2(((PROXY*)(TableInfo.tableObject))->ip); {
					evbuffer_add_printf(Buffer, "%s", ip);
				} free(ip);
				break;
			}
			case HTML_TEMPLATE_COMPONENT_IDENTIFIER_CHECK_TYPE: {
				evbuffer_add_printf(Buffer, "%s", ProxyGetTypeString(((PROXY*)(TableInfo.tableObject))->type));
				break;
			}
			case HTML_TEMPLATE_COMPONENT_IDENTIFIER_CHECK_COUNTRY_LOWER: {
				PROXY *proxy = (PROXY*)(TableInfo.tableObject);
				evbuffer_add_printf(Buffer, "%c%c", tolower(proxy->country[0]), tolower(proxy->country[1]));
				break;
			}
			case HTML_TEMPLATE_COMPONENT_IDENTIFIER_CHECK_COUNTRY_UPPER: {
				evbuffer_add_printf(Buffer, "%s", ((PROXY*)(TableInfo.tableObject))->country);
				break;
			}
			case HTML_TEMPLATE_COMPONENT_IDENTIFIER_CHECK_LIVE_SINCE: {
				char *time = FormatTime(((PROXY*)(TableInfo.tableObject))->liveSinceMs); {
					evbuffer_add_printf(Buffer, "%s", time);
				} free(time);
				break;
			}
			case HTML_TEMPLATE_COMPONENT_IDENTIFIER_CHECK_LAST_CHECKED: {
				char *time = FormatTime(((PROXY*)(TableInfo.tableObject))->lastCheckedMs); {
					evbuffer_add_printf(Buffer, "%s", time);
				} free(time);
				break;
			}
			case HTML_TEMPLATE_COMPONENT_IDENTIFIER_CHECK_UID: {
				char *identifierb64;
				Base64Encode(((PROXY*)(TableInfo.tableObject))->identifier, PROXY_IDENTIFIER_LEN, &identifierb64); {
					evbuffer_add_printf(Buffer, "%s", identifierb64);
				} free(identifierb64);
				break;
			}
			case HTML_TEMPLATE_COMPONENT_IDENTIFIER_CHECK_PORT: {
				evbuffer_add_printf(Buffer, "%"PRIu16, ((PROXY*)(TableInfo.tableObject))->port);
				break;
			}
			case HTML_TEMPLATE_COMPONENT_IDENTIFIER_CHECK_CONNECT_TIMEOUT: {
				evbuffer_add_printf(Buffer, "%"PRIu64, ((PROXY*)(TableInfo.tableObject))->timeoutMs);
				break;
			}
			case HTML_TEMPLATE_COMPONENT_IDENTIFIER_CHECK_HTTP_S_TIMEOUT: {
				evbuffer_add_printf(Buffer, "%"PRIu64, ((PROXY*)(TableInfo.tableObject))->httpTimeoutMs);
				break;
			}
			case HTML_TEMPLATE_COMPONENT_IDENTIFIER_CHECK_SUCCESSFUL_CHECKS: {
				evbuffer_add_printf(Buffer, "%"PRIu32, ((PROXY*)(TableInfo.tableObject))->successfulChecks);
				break;
			}
			case HTML_TEMPLATE_COMPONENT_IDENTIFIER_CHECK_FAILED_CHECKS: {
				evbuffer_add_printf(Buffer, "%"PRIu32, ((PROXY*)(TableInfo.tableObject))->failedChecks);
				break;
			}
			case HTML_TEMPLATE_COMPONENT_IDENTIFIER_CHECK_RETRIES: {
				evbuffer_add_printf(Buffer, "%"PRIu8, ((PROXY*)(TableInfo.tableObject))->retries);
				break;
			}
			case HTML_TEMPLATE_COMPONENT_IDENTIFIER_CHECK_COUNTRY_FULL: {

				const char *country;
				size_t len;
				int status;
				MMDB_lookup_result_s data;
				struct sockaddr* sa = IPv6MapToRaw(((PROXY*)(TableInfo.tableObject))->ip, 0); {
					data = MMDB_lookup_sockaddr(&GeoIPDB, sa, &status);
				} free(sa);

				if (status != MMDB_SUCCESS) {
					country = "Unknown";
					len = 7;
					goto countryEnd;
				}

				if (data.found_entry) {
					MMDB_entry_data_s entry;

					int status = MMDB_get_value(&data.entry, &entry, "country", "names", "en", NULL);
					if (status != MMDB_SUCCESS) {
						country = "Unknown";
						len = 7;
					} else if (entry.has_data) {
						country = entry.utf8_string;
						len = entry.data_size;
					} else {
						country = "Unknown";
						len = 7;
						goto countryEnd;
					}
				} else {
					country = "Unknown";
					len = 7;
				}
countryEnd:

				evbuffer_add_printf(Buffer, "%.*s", (int)len, country);
				break;
			}
			case HTML_TEMPLATE_COMPONENT_IDENTIFIER_UPROXIES_TABLE_ITEMS_END:
			case HTML_TEMPLATE_COMPONENT_IDENTIFIER_PROXIES_TABLE_ITEMS_END:
			case HTML_TEMPLATE_COMPONENT_IDENTIFIER_PRXSRC_TABLE_ITEMS_END:
			case HTML_TEMPLATE_COMPONENT_IDENTIFIER_STATS_GEO_TABLE_ITEMS_END:
			case HTML_TEMPLATE_COMPONENT_IDENTIFIER_STATS_PCOUNT_TABLE_ITEMS_END: {
				return;
				break;
			}
			case HTML_TEMPLATE_COMPONENT_IDENTIFIER_CHECK_COND_INVALID_CERT_INFO: {
				if (Info.currentPage->page != INTERFACE_PAGE_RECHECK)
					continue;

				BIO* bio = BIO_new(BIO_s_mem());
				X509_print_ex(bio, ((PROXY*)(TableInfo.tableObject))->invalidCert, XN_FLAG_COMPAT, X509_FLAG_COMPAT);
				char *data;
				size_t size = BIO_get_mem_data(bio, &data);
				evbuffer_add(Buffer, data, size);
				BIO_free_all(bio);
				break;
			}
			case HTML_TEMPLATE_COMPONENT_IDENTIFIER_CHECK_COND_INVALID_CERT_FINGERPRINT: {
				if (Info.currentPage->page != INTERFACE_PAGE_RECHECK)
					continue;

				uint8_t hash[EVP_MAX_MD_SIZE];
				unsigned int trash;

				X509_digest(((PROXY*)(TableInfo.tableObject))->invalidCert, EVP_sha1(), hash, &trash);
				for (size_t x = 0; x < 160 / 8;x++)
					evbuffer_add_printf(Buffer, "%s%02x", x == 0 ? "" : ":", hash[x]);
				break;
			}
			case HTML_TEMPLATE_COMPONENT_IDENTIFIER_CHECK_COND_INVALID_CERT: {
				if (Info.currentPage->page != INTERFACE_PAGE_RECHECK)
					continue;

				if (((PROXY*)(TableInfo.tableObject))->invalidCert == NULL) {
					do {
						x++;
					} while (Components[x].identifier != HTML_TEMPLATE_COMPONENT_IDENTIFIER_CHECK_ELSE_COND_INVALID_CERT);
				}
				break;
			}
			case HTML_TEMPLATE_COMPONENT_IDENTIFIER_CHECK_ELSE_COND_INVALID_CERT: {
				if (Info.currentPage->page != INTERFACE_PAGE_RECHECK)
					continue;

				if (((PROXY*)(TableInfo.tableObject))->invalidCert != NULL) {
					do {
						x++;
					} while (Components[x].identifier != HTML_TEMPLATE_COMPONENT_IDENTIFIER_CHECK_END_COND_INVALID_CERT);
				}
				break;
			}
			case HTML_TEMPLATE_COMPONENT_IDENTIFIER_CPAGE_RAW_HTTP: {
				if (Info.currentPage->page == INTERFACE_PAGE_CPAGE_RAW && TableInfo.tableObject == 0)
					return;
				break;
			}
			case HTML_TEMPLATE_COMPONENT_IDENTIFIER_TABLE_BREAK: {
				TableInfo.tableHeadOrItemIteration = 0;
				break;
			}
			default: {
				break;
			}
		}
		switch (Components[x].identifier) {
			case HTML_TEMPLATE_COMPONENT_IDENTIFIER_STATIC:
			case HTML_TEMPLATE_COMPONENT_IDENTIFIER_VERSION:
			case HTML_TEMPLATE_COMPONENT_IDENTIFIER_CFG_HOME_ACTIVE:
			case HTML_TEMPLATE_COMPONENT_IDENTIFIER_CFG_UPROXIES_ACTIVE:
			case HTML_TEMPLATE_COMPONENT_IDENTIFIER_CFG_PROXIES_ACTIVE:
			case HTML_TEMPLATE_COMPONENT_IDENTIFIER_CFG_SOURCES_ACTIVE:
			case HTML_TEMPLATE_COMPONENT_IDENTIFIER_CFG_STATS_ACTIVE:
			case HTML_TEMPLATE_COMPONENT_IDENTIFIER_CFG_SETTINGS_ACTIVE:
			case HTML_TEMPLATE_COMPONENT_IDENTIFIER_CFG_TABLE_ODD:
			case HTML_TEMPLATE_COMPONENT_IDENTIFIER_CFG_TABLE_EVEN:
			case HTML_TEMPLATE_COMPONENT_IDENTIFIER_CFG_TABLE_OK:
			case HTML_TEMPLATE_COMPONENT_IDENTIFIER_CFG_TABLE_WARN:
			case HTML_TEMPLATE_COMPONENT_IDENTIFIER_CFG_TABLE_ERR:
			case HTML_TEMPLATE_COMPONENT_IDENTIFIER_CFG_ENABLED:
			case HTML_TEMPLATE_COMPONENT_IDENTIFIER_CFG_DISABLED:
			case HTML_TEMPLATE_COMPONENT_IDENTIFIER_SUB_AUTH_COOKIE: {
				evbuffer_add(Buffer, Components[x].content, strlen(Components[x].content));
				break;
			}
			default: {
				break;
			}
		}
	}
}