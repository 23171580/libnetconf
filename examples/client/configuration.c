/*
 * configuration.c
 * Author Radek Krejci <rkrejci@cesnet.cz>
 *
 * NETCONF client configuration.
 *
 * Copyright (C) 2012 CESNET, z.s.p.o.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 * 3. Neither the name of the Company nor the names of its contributors
 *    may be used to endorse or promote products derived from this
 *    software without specific prior written permission.
 *
 * ALTERNATIVELY, provided that this notice is retained in full, this
 * product may be distributed under the terms of the GNU General Public
 * License (GPL) version 2 or later, in which case the provisions
 * of the GPL apply INSTEAD OF those given above.
 *
 * This software is provided ``as is, and any express or implied
 * warranties, including, but not limited to, the implied warranties of
 * merchantability and fitness for a particular purpose are disclaimed.
 * In no event shall the company or contributors be liable for any
 * direct, indirect, incidental, special, exemplary, or consequential
 * damages (including, but not limited to, procurement of substitute
 * goods or services; loss of use, data, or profits; or business
 * interruption) however caused and on any theory of liability, whether
 * in contract, strict liability, or tort (including negligence or
 * otherwise) arising in any way out of the use of this software, even
 * if advised of the possibility of such damage.
 *
 */

#define _GNU_SOURCE
#include <stdlib.h>
#include <stdio.h>
#include <readline/readline.h>
#include <readline/history.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <pwd.h>
#include <errno.h>
#include <fcntl.h>

#include <libnetconf.h>
#include <libnetconf_ssh.h>

#include <libxml/tree.h>

#include "configuration.h"
#include "commands.h"

static const char rcsid[] __attribute__((used)) ="$Id: "__FILE__": "RCSID" $";

/* NetConf Client home */
#define NCC_DIR ".netconf_client"

void load_config (struct nc_cpblts **cpblts)
{
	struct passwd * pw;
	char * user_home, *netconf_dir, * history_file, *config_file;
	char * tmp_cap;
	int ret, history_fd, config_fd;
	xmlDocPtr config_doc;
	xmlNodePtr config_cap, tmp_node;

#ifndef DISABLE_LIBSSH
	char * key_priv, * key_pub, *prio;
	xmlNodePtr tmp_auth, tmp_pref, tmp_key;
#endif

	(*cpblts) = nc_session_get_cpblts_default();

	if ((pw = getpwuid(getuid())) == NULL) {
		ERROR("load_config", "Determining home directory failed (%s).", strerror(errno));
		return;
	}
	user_home = pw->pw_dir;

	if (asprintf (&netconf_dir, "%s/%s", user_home, NCC_DIR) == -1) {
		ERROR("load_config", "asprintf() failed (%s:%d).", __FILE__, __LINE__);
		return;
	}

	ret = access (netconf_dir, R_OK|X_OK);
	if (ret == -1) {
		if (errno == ENOENT) {
			/* directory does not exist */
			ERROR ("load_config", "Configuration directory (%s) does not exist, creating it.", netconf_dir);
			if (mkdir (netconf_dir, 0777) != 0) {
				ERROR ("load_config", "Configuration directory (%s) cannot be created (%s)", netconf_dir, strerror(errno));
				free (netconf_dir);
				return;
			}
		} else {
			ERROR ("load_config", "Configuration directory (%s) exists but something else failed (%s)", netconf_dir, strerror(errno));
			free (netconf_dir);
			return;
		}
	}

	if (asprintf (&history_file, "%s/history", netconf_dir) == -1) {
		ERROR("load_config", "asprintf() failed (%s:%d).", __FILE__, __LINE__);
		ERROR("load_config", "Unable to load commands history due to the previous error.");
		history_file = NULL;
	} else {
		ret = access(history_file, R_OK);
		if (ret == -1) {
			if (errno == ENOENT) {
				ERROR("load_config", "History file (%s) does not exist, creating it", history_file);
				if ((history_fd = creat(history_file, 0666)) == -1) {
					ERROR("load_config", "History file cannot be created (%s)", strerror(errno));
				} else {
					close(history_fd);
				}
			} else {
				ERROR("load_config", "Accessing the history file failed (%s)", strerror(errno));
			}
		} else {
			/* file exist and is accessible */
			if (read_history(history_file)) {
				ERROR("load_config", "Failed to load history.");
			}
		}
	}

	if (asprintf (&config_file, "%s/config.xml", netconf_dir) == -1) {
		ERROR("load_config", "asprintf() failed (%s:%d).", __FILE__, __LINE__);
		ERROR("load_config", "Unable to load configuration due to the previous error.");
		config_file = NULL;
	} else {
		ret = access(config_file, R_OK);
		if (ret == -1) {
			if (errno == ENOENT) {
				ERROR("load_config", "Configuration file (%s) does not exits, creating it", config_file);
				if ((config_fd = creat(config_file, 0666)) == -1) {
					ERROR("load_config", "Configuration file cannot be created (%s)", strerror(errno));
				} else {
					close(config_fd);
				}
			} else {
				ERROR("load_config", "Configuration file cannot accessed (%s)", strerror(errno));
			}
		} else {
			/* file exist and is accessible */
			if ((config_doc = xmlReadFile(config_file, NULL, XML_PARSE_NOBLANKS | XML_PARSE_NSCLEAN)) == NULL) {
				ERROR("load_config", "Failed to load configuration of NETCONF client (xmlReadFile failed).");
			} else {
				/* doc -> <netconf-client/>*/
				if (config_doc->children != NULL && xmlStrEqual(config_doc->children->name, BAD_CAST "netconf-client")) {
					tmp_node = config_doc->children->children;
					while (tmp_node) {
						if (xmlStrEqual(tmp_node->name, BAD_CAST "capabilities")) {
							/* doc -> <netconf-client> -> <capabilities> */
							nc_cpblts_free(*cpblts);
							(*cpblts) = nc_cpblts_new(NULL);
							config_cap = tmp_node->children;
							while (config_cap) {
								tmp_cap = (char *) xmlNodeGetContent(config_cap);
								nc_cpblts_add(*cpblts, tmp_cap);
								free(tmp_cap);
								config_cap = config_cap->next;
							}
						}
#ifndef DISABLE_LIBSSH
						else if (xmlStrEqual(tmp_node->name, BAD_CAST "authentication")) {
							/* doc -> <netconf-client> -> <authentication> */
							tmp_auth = tmp_node->children;
							while (tmp_auth) {
								if (xmlStrEqual(tmp_auth->name, BAD_CAST "pref")) {
									tmp_pref = tmp_auth->children;
									while (tmp_pref) {
										prio = (char*) xmlNodeGetContent(tmp_pref);
										if (xmlStrEqual(tmp_pref->name, BAD_CAST "publickey")) {
											nc_ssh_pref(NC_SSH_AUTH_PUBLIC_KEYS, atoi(prio));
										} else if (xmlStrEqual(tmp_pref->name, BAD_CAST "interactive")) {
											nc_ssh_pref(NC_SSH_AUTH_INTERACTIVE, atoi(prio));
										} else if (xmlStrEqual(tmp_pref->name, BAD_CAST "password")) {
											nc_ssh_pref(NC_SSH_AUTH_PASSWORD, atoi(prio));
										}
										free(prio);
										tmp_pref = tmp_pref->next;
									}
								} else if (xmlStrEqual(tmp_auth->name, BAD_CAST "keys")) {
									tmp_key = tmp_auth->children;
									while (tmp_key) {
										if (xmlStrEqual(tmp_key->name, BAD_CAST "key-path")) {
											key_priv = (char*) xmlNodeGetContent(tmp_key);
											if (asprintf(&key_pub, "%s.pub", key_priv) == -1) {
												ERROR("load_config", "asprintf() failed (%s:%d).", __FILE__, __LINE__);
												ERROR("load_config", "Unable to set SSH keys pair due to the previous error.");
												key_pub = NULL;
												tmp_key = tmp_key->next;
												continue;
											}
											nc_set_keypair_path(key_priv, key_pub);
											free(key_priv);
											free(key_pub);
										}
										tmp_key = tmp_key->next;
									}
								}
								tmp_auth = tmp_auth->next;
							}
						}
#endif /* not DISABLE_LIBSSH */
						tmp_node = tmp_node->next;
					}
				}
				xmlFreeDoc(config_doc);
			}
		}
	}

	free (config_file);
	free (history_file);
	free (netconf_dir);
}

/**
 * \brief Store configuration and history
 */
void store_config (struct nc_cpblts * cpblts)
{
	struct passwd * pw;
	char * user_home, *netconf_dir, * history_file, *config_file;
	const char * cap;
	int history_fd, ret;
	xmlDocPtr config_doc;
	xmlNodePtr config_caps;
	FILE * config_f;

	if ((pw = getpwuid(getuid())) == NULL) {
		ERROR("store_config", "Determining home directory failed (%s).", strerror(errno));
		return;
	}
	user_home = pw->pw_dir;

	if (asprintf (&netconf_dir, "%s/%s", user_home, NCC_DIR) == -1) {
		ERROR("store_config", "asprintf() failed (%s:%d).", __FILE__, __LINE__);
		return;
	}

	ret = access (netconf_dir, R_OK|W_OK|X_OK);
	if (ret == -1) {
		if (errno == ENOENT) {
			/* directory does not exist, create it */
			if (mkdir (netconf_dir, 0777)) {
				/* directory can not be created */
				free (netconf_dir);
				ERROR("store_config", "Storing history failed (mkdir(): %s)", strerror(errno));
				return;
			}
		} else {
			/* directory exist but cannot be accessed */
			free (netconf_dir);
			ERROR("store_config", "Accessing the directory for storing the history failed (%s)", strerror(errno));
			return;
		}
	}

	if (asprintf (&history_file, "%s/history", netconf_dir) == -1) {
		ERROR("store_config", "asprintf() failed (%s:%d).", __FILE__, __LINE__);
		ERROR("store_config", "Unable to store commands history due to the previous error.");
		history_file = NULL;
	} else {
		ret = access(history_file, R_OK | W_OK);
		if (ret == -1) {
			if (errno == ENOENT) {
				/* file does not exit, create it */
				if ((history_fd = creat(history_file, 0666)) == -1) {
					/* history file can not be created */
				} else {
					close(history_fd);
				}
			}
			ERROR("store_config", "Accessing the history file failed (%s)", strerror(errno));
		}

		if (write_history(history_file)) {
			ERROR("save_config", "Failed to save history.");
		}
		free(history_file);
	}

	if (asprintf (&config_file, "%s/config.xml", netconf_dir) == -1) {
		ERROR("store_config", "asprintf() failed (%s:%d).", __FILE__, __LINE__);
		ERROR("store_config", "Unable to store configuration due to the previous error.");
		config_file = NULL;
	} else {
		if (access(config_file, R_OK | W_OK) == -1 || (config_doc = xmlReadFile(config_file, NULL, XML_PARSE_NOBLANKS | XML_PARSE_NSCLEAN | XML_PARSE_NOERROR)) == NULL) {
			config_doc = xmlNewDoc(BAD_CAST "1.0");
			config_doc->children = xmlNewDocNode(config_doc, NULL, BAD_CAST "netconf-client", NULL);
		}
		if (config_doc != NULL) {
			if (config_doc->children != NULL && xmlStrEqual(config_doc->children->name, BAD_CAST "netconf-client")) {
				config_caps = config_doc->children->children;
				while (config_caps != NULL && !xmlStrEqual(config_caps->name, BAD_CAST "capabilities")) {
					config_caps = config_caps->next;
				}
				if (config_caps != NULL) {
					xmlUnlinkNode(config_caps);
					xmlFreeNode(config_caps);
				}
				config_caps = xmlNewChild(config_doc->children, NULL, BAD_CAST "capabilities", NULL);
				nc_cpblts_iter_start(cpblts);
				while ((cap = nc_cpblts_iter_next(cpblts)) != NULL) {
					xmlNewChild(config_caps, NULL, BAD_CAST "capability", BAD_CAST cap);
				}
			}
			if ((config_f = fopen(config_file, "w")) == NULL || xmlDocFormatDump(config_f, config_doc, 1) < 0) {
				ERROR("store_config", "Cannot write configuration to file %s", config_file);
			} else {
				fclose(config_f);
			}
			xmlFreeDoc(config_doc);
		} else {
			ERROR("store_config", "Cannot write configuration to file %s", config_file);
		}
	}

	free (netconf_dir);
	free (config_file);
}
