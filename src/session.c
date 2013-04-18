/**
 * \file session.c
 * \author Radek Krejci <rkrejci@cesnet.cz>
 * \brief Implementation of functions to handle NETCONF sessions.
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
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <stddef.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <pwd.h>

#ifndef DISABLE_LIBSSH
#	include <libssh2.h>
#endif

#include <libxml/tree.h>
#include <libxml/parser.h>
#include <libxml/xpath.h>
#include <libxml/xpathInternals.h>

#include "config.h"

#include "netconf_internal.h"
#include "messages.h"
#include "messages_internal.h"
#include "session.h"
#include "datastore.h"
#include "nacm.h"

#ifndef DISABLE_NOTIFICATIONS
#  include "notifications.h"
#endif

static const char rcsid[] __attribute__((used)) ="$Id: "__FILE__": "RCSID" $";

/* definition in datastore.c */
char **get_schemas_capabilities(void);

extern struct nc_shared_info *nc_info;

extern uid_t nacm_recovery_uid_; /* internal.c */

/**
 * @brief List of possible NETCONF transportation supported by libnetconf
 */
enum nc_transport {
	NC_TRTANSPORT_SSH /* netconf-ssh */
};

struct session_list_item {
	int offset_prev;
	int offset_next;
	int size;
	int active; /* flag if the non-dummy session is connected to this record */
	int scounter; /* number of sessions connected with this record */
	char session_id[SID_SIZE];
	enum nc_transport transport;
	struct nc_session_stats stats;
	char login_time[TIME_LENGTH];
	 /*
	  * variable length part - data actually contain 2 strings (username and
	  * source-host whose lengths can differ for each session)
	  */
	pthread_rwlock_t lock;
	char data[1];
};

struct session_list_map {
	/* start of the mapped file with session list */
	int size; /* current file size (may include gaps) */
	int count; /* current number of sessions */
	int first_offset;
	pthread_rwlock_t lock; /* lock for the all file - for resizing */
	struct session_list_item record[1]; /* first record of the session records list */
};

static int session_list_fd = -1;
static struct session_list_map *session_list = NULL;

/**
 * Sleep time in microseconds to wait between unsuccessful reading due to EAGAIN or EWOULDBLOCK
 */
#define NC_READ_SLEEP 100
#ifdef DISABLE_LIBSSH
#define NC_WRITE(session,buf,c) \
	if (session->fd_output != -1) {c += write (session->fd_output, (buf), strlen(buf));}
#else
#define NC_WRITE(session,buf,c) \
	if(session->ssh_channel){ \
		c += libssh2_channel_write (session->ssh_channel, (buf), strlen(buf)); \
	} else if (session->fd_output != -1) { \
		c += write (session->fd_output, (buf), strlen(buf));}
#endif

#define SIZE_STEP (1024*16)
int nc_session_monitoring_init(void)
{
	struct stat fdinfo;
	int first = 0, c;
	size_t size;
	pthread_rwlockattr_t rwlockattr;
	mode_t um;

	if (session_list != NULL) {
		ERROR("%s: session list already exists.", __func__);
		return (EXIT_FAILURE);
	}

	if (session_list_fd != -1) {
		close(session_list_fd);
	}

	um = umask(0000);
	session_list_fd = open(SESSIONSFILE_PATH, O_CREAT | O_RDWR, FILE_PERM);
	umask(um);
	if (session_list_fd == -1) {
		ERROR("Opening the sessions monitoring file failed (%s).", strerror(errno));
		return (EXIT_FAILURE);
	}

	/* get the file size */
	if (fstat(session_list_fd, &fdinfo) == -1) {
		ERROR("Unable to get the sessions monitoring file information (%s)", strerror(errno));
		close(session_list_fd);
		session_list_fd = -1;
		return (EXIT_FAILURE);
	}

	if (fdinfo.st_size == 0) {
		/* we have a new file, create some initial size using file gaps */
		first = 1;
		lseek(session_list_fd, SIZE_STEP - 1, SEEK_SET);
		while (((c = write(session_list_fd, "", 1)) == -1) && (errno == EAGAIN || errno == EINTR));
		if (c == -1) {
			WARN("%s: Preparing the session list file failed (%s).", __func__, strerror(errno));
		}
		lseek(session_list_fd, 0, SEEK_SET);
		size = SIZE_STEP;
	} else {
		size = fdinfo.st_size;
	}

	session_list = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, session_list_fd, 0);
	if (session_list == MAP_FAILED) {
		ERROR("Accessing the shared sessions monitoring file failed (%s)", strerror(errno));
		close(session_list_fd);
		session_list = NULL;
		session_list_fd = -1;
		return (EXIT_FAILURE);
	}

	if (first) {
		pthread_rwlockattr_init(&rwlockattr);
		pthread_rwlockattr_setpshared(&rwlockattr, PTHREAD_PROCESS_SHARED);
		pthread_rwlock_init(&(session_list->lock), &rwlockattr);
		pthread_rwlockattr_destroy(&rwlockattr);
		pthread_rwlock_wrlock(&(session_list->lock));
		session_list->size = SIZE_STEP;
		session_list->count = 0;
		pthread_rwlock_unlock(&(session_list->lock));
	}

	return (EXIT_SUCCESS);
}

void nc_session_monitoring_close(void)
{
	if (session_list) {
		munmap(session_list, session_list->size);
		close(session_list_fd);
		session_list = NULL;
		session_list_fd = -1;
	}
}

int nc_session_monitor(struct nc_session* session)
{
	struct session_list_item *litem = NULL, *litem_aux;
	pthread_rwlockattr_t rwlockattr;
	int prev, next, size, totalsize = 0;

	if (session == NULL || session->monitored || session_list == NULL) {
		return (EXIT_FAILURE);
	}

	if (session->status != NC_SESSION_STATUS_WORKING && session->status != NC_SESSION_STATUS_DUMMY) {
		ERROR("%s: specified session is in invalid state and cannot be monitored.", __func__);
		return (EXIT_FAILURE);
	}

	/* critical section */
	pthread_rwlock_wrlock(&(session_list->lock));
	if (session_list->count > 0) {
		/* find possible duplicity of the record */
		for(litem = (struct session_list_item*)((char*)(session_list->record) + session_list->first_offset);
				litem != NULL;
				litem = (struct session_list_item*)((char*)litem + litem->offset_next)) {
			if (strcmp(session->session_id, litem->session_id) == 0) {
				/* session is already monitored */

				/*
				 * allow to add the session only if the connecting
				 * session is dummy or if there is no real session
				 * connected with this record
				 */
				if (session->status == NC_SESSION_STATUS_DUMMY) {
					litem->scounter++;
					pthread_rwlock_unlock(&(session_list->lock));

					/* connect session statistics to the shared memory segment */
					free(session->stats);
					session->stats = &(litem->stats);
					session->monitored = 1;
					return (EXIT_SUCCESS);
				} else if (session->status == NC_SESSION_STATUS_WORKING && litem->active == 0) {
					litem->scounter++;
					litem->active = 1;
					pthread_rwlock_unlock(&(session_list->lock));

					/* connect session statistics to the shared memory segment */
					free(session->stats);
					session->stats = &(litem->stats);
					session->monitored = 1;
					return (EXIT_SUCCESS);
				} else {
					litem->scounter++;
					pthread_rwlock_unlock(&(session_list->lock));

					if (litem->active == 1) {
						ERROR("%s: specified session is already monitored.", __func__);
					} else {
						ERROR("%s: specified session is in invalid state and cannot be monitored.", __func__);
					}
					return (EXIT_FAILURE);
				}
			}

			if (litem->offset_next == 0) {
				/* we are on the end of the list */
				break;
			}
		}
	}

	/* find the place for a new record, try to place it into some gap */
	size = (sizeof(struct session_list_item) - 1) + (
	                (session->username != NULL) ? (strlen(session->username) + 1) : 1) + (
	                (session->hostname != NULL) ? (strlen(session->hostname) + 1) : 1);
	if (session_list->count == 0) {
		/* we add the first item into the list */
		litem = &(session_list->record[0]);
		litem->offset_prev = 0;
		litem->offset_next = 0;
		session_list->first_offset = 0;
	} else if (session_list->first_offset >= size) {
		/* the gap is in the beginning of the list */
		litem = &(session_list->record[0]);
		litem->offset_prev = 0;
		litem->offset_next = session_list->first_offset;
		session_list->first_offset = 0;
	} else {
		totalsize = session_list->first_offset;

		/* search for a gap inside the list */
		for (litem = (struct session_list_item*) ((char*) (session_list->record) + session_list->first_offset);
		                ;
		                litem = (struct session_list_item*) ((char*) litem + litem->offset_next)) {
			/* check if there is enough space to add new record */
			if ((totalsize + litem->size + size) > session_list->size) {
				ERROR("There is not enough space to monitor another NETCONF session.");
				pthread_rwlock_unlock(&(session_list->lock));
				return (EXIT_FAILURE);
			} else {
				totalsize += litem->offset_next;
			}

			if (litem->offset_next >= (size + litem->size)) {
				/* we have the efficient gap */

				/* correct links from predecessor */
				prev = litem->size;
				next = litem->offset_next - litem->size;
				litem->offset_next = litem->size;

				/* now go into the new record and create connection with sibling records */
				litem = (struct session_list_item*)((char*)litem + litem->offset_next);
				litem->offset_prev = prev;
				litem->offset_next = next;

				/* also correct links in successor */
				litem_aux = (struct session_list_item*)((char*)litem + litem->offset_next);
				litem_aux->offset_prev = litem->offset_next;
				break;
			} else if (litem->offset_next == 0) {
				/* we are on the end of the list */
				prev = litem->size;
				litem->offset_next = litem->size;

				/* now go into the new record */
				litem = (struct session_list_item*)((char*)litem + litem->offset_next);
				litem->offset_prev = prev;
				litem->offset_next = 0;
				break;
			}
			/* move to another item in the list */
		}
	}
	session_list->count++;

	/* fill new structure */
	litem->size = size;
	strncpy(litem->session_id, session->session_id, SID_SIZE);
	litem->transport = NC_TRTANSPORT_SSH;
	if (session->stats != NULL) {
		memcpy(&(litem->stats), session->stats, sizeof(struct nc_session_stats));
		free(session->stats);
	}
	session->stats = &(litem->stats);
	strncpy(litem->login_time, (session->logintime == NULL) ? "" : session->logintime, TIME_LENGTH);
	litem->login_time[TIME_LENGTH - 1] = 0; /* terminating null byte */

	strcpy(litem->data, (session->username == NULL) ? "" : session->username);
	strcpy(litem->data + 1 + strlen(litem->data), (session->hostname == NULL) ? "" : session->hostname);

	pthread_rwlockattr_init(&rwlockattr);
	pthread_rwlockattr_setpshared(&rwlockattr, PTHREAD_PROCESS_SHARED);
	pthread_rwlock_init(&(litem->lock), &rwlockattr);
	pthread_rwlockattr_destroy(&rwlockattr);

	if (session->status == NC_SESSION_STATUS_WORKING) {
		litem->active = 1;
	}

	litem->scounter = 1;
	session->monitored = 1;

	/* end of critical section, other processes now can access new record */
	pthread_rwlock_unlock(&(session_list->lock));

	return (EXIT_SUCCESS);
}

char* nc_session_stats(void)
{
	char *aux, *sessions = NULL, *session = NULL;
	struct session_list_item *litem;

	if (session_list == NULL) {
		return (NULL);
	}

	pthread_rwlock_rdlock(&(session_list->lock));
	for (litem = (struct session_list_item*)((char*)(session_list->record) + session_list->first_offset); litem != NULL;) {
		aux = NULL;
		if (asprintf(&aux, "<session><session-id>%s</session-id>"
				"<transport>netconf-ssh</transport>"
				"<username>%s</username>"
				"<source-host>%s</source-host>"
				"<login-time>%s</login-time>"
				"<in-rpcs>%u</in-rpcs><in-bad-rpcs>%u</in-bad-rpcs>"
				"<out-rpc-errors>%u</out-rpc-errors>"
				"<out-notifications>%u</out-notifications></session>",
				litem->session_id,
				litem->data, /* username */
				litem->data + (strlen(litem->data) + 1), /* hostname */
				litem->login_time,
				litem->stats.in_rpcs,
				litem->stats.in_bad_rpcs,
				litem->stats.out_rpc_errors,
				litem->stats.out_notifications) == -1) {
			ERROR("asprintf() failed (%s:%d).", __FILE__, __LINE__);
		} else {
			if (session == NULL) {
				session = aux;
			} else {
				session = realloc(session, strlen(session) + strlen(aux) + 1);
				strcat(session, aux);
				free(aux);
			}
		}

		/* move to the next record */
		if (litem->offset_next == 0) {
			litem = NULL;
		} else {
			litem = (struct session_list_item*)((char*)litem + litem->offset_next);
		}
	}
	pthread_rwlock_unlock(&(session_list->lock));

	if (session != NULL) {
		if (asprintf(&sessions, "<sessions>%s</sessions>", session) == -1) {
			ERROR("asprintf() failed (%s:%d).", __FILE__, __LINE__);
			free(session);
			return (NULL);
		}
		free(session);
	}
	return (sessions);
}

const char* nc_session_get_id (const struct nc_session *session)
{
	if (session == NULL) {
		return (NULL);
	}
	return (session->session_id);
}

const char* nc_session_get_host(const struct nc_session* session)
{
	if (session == NULL) {
		return (NULL);
	}
	return (session->hostname);
}

const char* nc_session_get_port(const struct nc_session* session)
{
	if (session == NULL) {
		return (NULL);
	}
	return (session->port);
}

const char* nc_session_get_user(const struct nc_session* session)
{
	if (session == NULL) {
		return (NULL);
	}
	return (session->username);
}

int nc_session_get_version (const struct nc_session *session)
{
	if (session == NULL) {
		return (-1);
	}
	return (session->version);
}

int nc_session_get_eventfd (const struct nc_session *session)
{
	if (session == NULL) {
		return -1;
	}

	if (session->libssh2_socket != -1) {
		return (session->libssh2_socket);
	} else if (session->fd_input != -1) {
		return (session->fd_input);
	} else {
		return (-1);
	}
}

int nc_session_notif_allowed (const struct nc_session *session)
{
	if (session == NULL) {
		/* notification subscription is not allowed */
		return 0;
	}

#ifndef DISABLE_NOTIFICATIONS
	/* check capabilities */
	if (nc_cpblts_enabled(session, NC_CAP_NOTIFICATION_ID) == 1) {
		/* subscription is allowed only if another subscription is not active */
		return ((session->ntf_active == 0) ? 1 : 0);
	} else
#endif
	{
		/* notifications are not supported at all */
		return (0);
	}
}

void nc_cpblts_free(struct nc_cpblts *c)
{
	int i;

	if (c == NULL) {
		return;
	}

	if (c->list != NULL) {
		if (c->items <= c->list_size) {
			for(i = 0; i < c->items; i++) {
				if (c->list[i] != NULL) {
					free(c->list[i]);
				}
			}
		} else {
			WARN("nc_cpblts_free: invalid capabilities structure, some memory may not be freed.");
		}
		free(c->list);
	}
	free(c);
}

struct nc_cpblts *nc_cpblts_new(char* const* list)
{
	struct nc_cpblts *retval;
	char* item;
	int i;

	retval = calloc(1, sizeof(struct nc_cpblts));
	if (retval == NULL) {
		ERROR("Memory allocation failed: %s (%s:%d).", strerror (errno), __FILE__, __LINE__);
		return (NULL);
	}

	retval->list_size = 10; /* initial value */
	retval->list = malloc(retval->list_size * sizeof(char*));
	if (retval->list == NULL) {
		ERROR("Memory allocation failed: %s (%s:%d).", strerror (errno), __FILE__, __LINE__);
		free(retval);
		return (NULL);
	}
	retval->list[0] = NULL;

	if (list != NULL) {
		for (i = 0, item = list[i]; item != NULL; i++) {
			retval->list[i] = strdup (item);
			retval->items++;
			if (retval->items == retval->list_size) {
				/* resize the capacity of the capabilities list */
				errno = 0;
				retval->list = realloc (retval->list, retval->list_size * 2 * sizeof (char*));
				if (errno != 0) {
					nc_cpblts_free (retval);
					return (NULL);
				}
				retval->list_size *= 2;
			}
			retval->list[i + 1] = NULL;
			item = list[i+1];
		}
	}

	return (retval);
}

int nc_cpblts_add (struct nc_cpblts *capabilities, const char* capability_string)
{
	int i;
	char *s, *p = NULL;

	if (capabilities == NULL || capability_string == NULL) {
		return (EXIT_FAILURE);
	}

	if (capabilities->items > capabilities->list_size) {
		WARN("nc_cpblts_add: structure inconsistency! Some data may be lost.");
		return (EXIT_FAILURE);
	}

	/* get working copy of capability_string where the parameters will be ignored */
	s = strdup(capability_string);
	if ((p = strchr(s, '?')) != NULL) {
		/* in following comparison, ignore capability's parameters */
		*p = 0;
	}

	/* find duplicities */
	for (i = 0; i < capabilities->items; i++) {
		if (strncmp(capabilities->list[i], s, strlen(s)) == 0) {
			/* capability is already in the capabilities list, but
			 * parameters can differ, so substitute the current instance
			 * with the new one
			 */
			free(capabilities->list[i]);
			if (p != NULL) {
				*p = '?';
			}
			capabilities->list[i] = s;
			return (EXIT_SUCCESS);
		}
	}
	/* unhide capability's parameters */
	if (p != NULL) {
		*p = '?';
	}

	capabilities->list[capabilities->items] = s;
	capabilities->items++;
	if (capabilities->items == capabilities->list_size) {
		/* resize the capacity of the capabilities list */
		errno = 0;
		capabilities->list = realloc(capabilities->list, capabilities->list_size * 2 * sizeof (char*));
		if (errno != 0) {
			return (EXIT_FAILURE);
		}
		capabilities->list_size *= 2;
	}
	capabilities->list[capabilities->items] = NULL;

	return (EXIT_SUCCESS);
}

int nc_cpblts_remove (struct nc_cpblts *capabilities, const char* capability_string)
{
	int i;
	char* s, *p;

	if (capabilities == NULL || capability_string == NULL) {
		return (EXIT_FAILURE);
	}

	if (capabilities->items > capabilities->list_size) {
		WARN("nc_cpblts_add: structure inconsistency! Some data may be lost.");
		return (EXIT_FAILURE);
	}

	s = strdup(capability_string);
	if ((p = strchr(s, '?')) != NULL) {
		/* in comparison, ignore capability's parameters */
		p = 0;
	}

	for (i = 0; i < capabilities->items; i++) {
		if (capabilities->list[i] != NULL && strncmp(capabilities->list[i], s, strlen(s)) == 0) {
			break;
		}
	}
	free(s);

	if (i < capabilities->items) {
		free(capabilities->list[i]);
		/* move here the last item from the list */
		capabilities->list[i] = capabilities->list[capabilities->items - 1];
		/* and then set the last item in the list to NULL */
		capabilities->list[capabilities->items - 1] = NULL;
		capabilities->items--;
	}

	return (EXIT_SUCCESS);
}

const char* nc_cpblts_get(const struct nc_cpblts *c, const char* capability_string)
{
	int i;
	char* s, *p;

	if (capability_string == NULL || c == NULL || c->list == NULL) {
		return (NULL);
	}

	s = strdup(capability_string);
	if ((p = strchr(s, '?')) != NULL) {
		/* in comparison, ignore capability's parameters */
		p = 0;
	}

	for (i = 0; c->list[i]; i++) {
		if (strncmp(s, c->list[i], strlen(s)) == 0) {
			free(s);
			return (c->list[i]);
		}
	}
	free(s);
	return (NULL);

}

int nc_cpblts_enabled(const struct nc_session* session, const char* capability_string)
{
	int i;
	char* s, *p;

	if (capability_string == NULL || session == NULL || session->capabilities == NULL) {
		return (0);
	}

	s = strdup(capability_string);
	if ((p = strchr(s, '?')) != NULL) {
		/* in comparison, ignore capability's parameters */
		p = 0;
	}

	for (i = 0; session->capabilities->list[i]; i++) {
		if (strncmp(s, session->capabilities->list[i], strlen(s)) == 0) {
			free(s);
			return (1);
		}
	}
	free(s);
	return (0);
}

void nc_cpblts_iter_start(struct nc_cpblts *c)
{
	if (c == NULL) {
		return;
	}
	c->iter = 0;
}

const char *nc_cpblts_iter_next(struct nc_cpblts *c)
{
	if (c == NULL || c->list == NULL) {
		return (NULL);
	}

	if (c->iter > (c->items - 1)) {
		return NULL;
	}

	return (c->list[c->iter++]);
}

int nc_cpblts_count(const struct nc_cpblts *c)
{
	if (c == NULL || c->list == NULL) {
		return 0;
	}
	return c->items;
}

struct nc_cpblts *nc_session_get_cpblts_default ()
{
	struct nc_cpblts *retval;
	char** nslist;
	int i;

	retval = nc_cpblts_new(NULL);
	if (retval == NULL) {
		return (NULL);
	}

	nc_cpblts_add(retval, NC_CAP_BASE10_ID);
	nc_cpblts_add(retval, NC_CAP_BASE11_ID);
	nc_cpblts_add(retval, NC_CAP_YANG_ID);
	nc_cpblts_add(retval, NC_CAP_WRUNNING_ID);
	nc_cpblts_add(retval, NC_CAP_CANDIDATE_ID);
	nc_cpblts_add(retval, NC_CAP_STARTUP_ID);
	nc_cpblts_add(retval, NC_CAP_INTERLEAVE_ID);
	nc_cpblts_add(retval, NC_CAP_MONITORING_ID);
#ifndef DISABLE_NOTIFICATIONS
	nc_cpblts_add(retval, NC_CAP_NOTIFICATION_ID);
#endif
	if (ncdflt_get_basic_mode() != NCWD_MODE_NOTSET) {
		nc_cpblts_add(retval, NC_CAP_WITHDEFAULTS_ID);
	}

	/* add namespaces of used datastores as announced capabilities */
	if ((nslist = get_schemas_capabilities()) != NULL) {
		for(i = 0; nslist[i] != NULL; i++) {
			nc_cpblts_add(retval, nslist[i]);
			free(nslist[i]);
		}
		free(nslist);
	}

	return (retval);
}

struct nc_cpblts* nc_session_get_cpblts (const struct nc_session* session)
{
	if (session == NULL) {
		return (NULL);
	}

	return (session->capabilities);
}

/**
 * @brief Parse with-defaults capability
 * This function is used in ssh.c
 */
void parse_wdcap(struct nc_cpblts *capabilities, NCWD_MODE *basic, int *supported)
{
	const char* cpblt;
	char* s;

	if ((cpblt = nc_cpblts_get(capabilities, NC_CAP_WITHDEFAULTS_ID)) != NULL) {
		if ((s = strstr(cpblt, "report-all")) != NULL) {
			if (s[-1] == '=' && s[-2] == 'e') {
				/* basic mode: basic-mode=report-all */
				*basic = NCWD_MODE_ALL;
			}
			*supported = *supported | NCWD_MODE_ALL;
		}
		if ((s = strstr(cpblt, "trim")) != NULL) {
			if (s[-1] == '=' && s[-2] == 'e') {
				/* basic mode: basic-mode=trim */
				*basic = NCWD_MODE_TRIM;
			}
			*supported = *supported | NCWD_MODE_TRIM;
		}
		if ((s = strstr(cpblt, "explicit")) != NULL) {
			if (s[-1] == '=' && s[-2] == 'e') {
				/* basic mode: basic-mode=explicit */
				*basic = NCWD_MODE_EXPLICIT;
			}
			*supported = *supported | NCWD_MODE_EXPLICIT;
		}
		if ((s = strstr(cpblt, "report-all-tagged")) != NULL) {
			*supported = *supported | NCWD_MODE_ALL_TAGGED;
		}
	} else {
		*basic = NCWD_MODE_NOTSET;
		*supported = 0;
	}
}

struct nc_session* nc_session_dummy(const char* sid, const char* username, const char* hostname, struct nc_cpblts *capabilities)
{
	struct nc_session * session;
	struct passwd* p;
	const char* cpblt;

	if (sid == NULL || username == NULL || capabilities == NULL) {
		return NULL;
	}

	if ((session = malloc (sizeof (struct nc_session))) == NULL) {
		ERROR("Memory allocation failed (%s)", strerror(errno));
		return NULL;
	}
	memset (session, 0, sizeof (struct nc_session));
	if ((session->stats = malloc (sizeof (struct nc_session_stats))) == NULL) {
		ERROR("Memory allocation failed (%s)", strerror(errno));
		free(session);
		return NULL;
	}

	/* set invalid fd values to prevent comunication */
	session->fd_input = -1;
	session->fd_output = -1;
	session->libssh2_socket = -1;

	/* init stats values */
	session->logintime = nc_time2datetime(time(NULL));
	session->monitored = 0;
	session->stats->in_rpcs = 0;
	session->stats->in_bad_rpcs = 0;
	session->stats->out_rpc_errors = 0;
	session->stats->out_notifications = 0;

	/*
	 * mutexes and queues fields are not initialized since dummy session
	 * cannot send or receive any data
	 */

	/* session is DUMMY */
	session->status = NC_SESSION_STATUS_DUMMY;
	/* copy session id */
	strncpy (session->session_id, sid, SID_SIZE);
	/* get system groups for the username */
	session->groups = nc_get_grouplist(username);
	/* if specified, copy hostname */
	if (hostname != NULL) {
		session->hostname = strdup (hostname);
	}
	/* copy user name */
	session->username = strdup (username);
	/* detect if user ID is 0 -> then the session is recovery */
	session->nacm_recovery = 0;
	if ((p = getpwnam(username)) != NULL) {
		if (p->pw_uid == nacm_recovery_uid_) {
			session->nacm_recovery = 1;
		}
	}
	/* create empty capabilities list */
	session->capabilities = nc_cpblts_new (NULL);
	/* initialize capabilities iterator */
	nc_cpblts_iter_start (capabilities);
	/* copy all capabilities */
	while ((cpblt = nc_cpblts_iter_next (capabilities)) != NULL) {
		nc_cpblts_add (session->capabilities, cpblt);
	}

	session->wd_basic = NCWD_MODE_NOTSET;
	session->wd_modes = 0;
	/* set with defaults capability flags */
	parse_wdcap(session->capabilities, &(session->wd_basic), &(session->wd_modes));

	return session;
}

void nc_session_close(struct nc_session* session, NC_SESSION_TERM_REASON reason)
{
	int i;
	nc_rpc *rpc_close = NULL;
	nc_reply *reply = NULL;
	struct nc_msg *qmsg, *qmsg_aux;
	NC_SESSION_STATUS sstatus = session->status;

	/* lock session due to accessing its status and other items */
	if (sstatus != NC_SESSION_STATUS_DUMMY) {
		DBG_LOCK("mut_session");
		pthread_mutex_lock(&(session->mut_session));
	}

	/* close the SSH session */
	if (session != NULL && session->status != NC_SESSION_STATUS_CLOSING && session->status != NC_SESSION_STATUS_CLOSED) {

#ifndef DISABLE_NOTIFICATIONS
		/* log closing of the session */
		if (sstatus != NC_SESSION_STATUS_DUMMY) {
			ncntf_event_new(-1, NCNTF_BASE_SESSION_END, session, reason, NULL);
		}
#endif

		if (strcmp(session->session_id, INTERNAL_DUMMY_ID) != 0) {
			/*
			 * break all datastore locks held by the session,
			 * libnetconf's internal dummy sessions are excluded
			 */
			ncds_break_locks(session);
		}

		/* close ssh session */
#ifdef DISABLE_LIBSSH
		if (session->status == NC_SESSION_STATUS_WORKING) {
			/* close NETCONF session */
			rpc_close = nc_rpc_closesession();
			if (rpc_close != NULL) {
				if (nc_session_send_rpc(session, rpc_close) != 0) {
					nc_session_recv_reply(session, 10000, &reply); /* wait max 10 seconds */
					if (reply != NULL) {
						nc_reply_free(reply);
					}
				}
				if (rpc_close != NULL) {
					nc_rpc_free(rpc_close);
				}
			}
		}
#else
		if (session->ssh_channel != NULL) {
			if (session->status == NC_SESSION_STATUS_WORKING && libssh2_channel_eof(session->ssh_channel) == 0) {
				/* prevent infinite recursion when socket is corrupted -> stack overflow */
				session->status = NC_SESSION_STATUS_CLOSING;

				/* close NETCONF session */
				rpc_close = nc_rpc_closesession();
				if (rpc_close != NULL) {
					if (nc_session_send_rpc(session, rpc_close) != 0) {
						nc_session_recv_reply(session, 10000, &reply); /* wait max 10 seconds */
						if (reply != NULL) {
							nc_reply_free(reply);
						}
					}
					if (rpc_close != NULL) {
						nc_rpc_free(rpc_close);
					}
				}
			}

			libssh2_channel_free(session->ssh_channel);
			session->ssh_channel = NULL;
		}

		if (session->ssh_session != NULL) {
			libssh2_session_disconnect(session->ssh_session, nc_session_term_string(reason));
			libssh2_session_free(session->ssh_session);
			session->ssh_session = NULL;
		}
#endif

		free(session->hostname);
		session->hostname = NULL;
		free(session->logintime);
		session->logintime = NULL;
		free(session->port);
		session->port = NULL;

		if (session->libssh2_socket != -1) {
			close(session->libssh2_socket);
			session->libssh2_socket = -1;
		}

		/* remove messages from the queues */
		for (i = 0, qmsg = session->queue_event; i < 2; i++, qmsg = session->queue_msg) {
			while (qmsg != NULL) {
				qmsg_aux = qmsg->next;
				nc_msg_free(qmsg);
				qmsg = qmsg_aux;
			}
		}

		/*
		 * username, capabilities and session_id are untouched
		 */

		/* successfully closed */
	}

	if (sstatus != NC_SESSION_STATUS_DUMMY) {
		session->status = NC_SESSION_STATUS_CLOSED;
		DBG_UNLOCK("mut_session");
		pthread_mutex_unlock(&(session->mut_session));
	} else {
		session->status = NC_SESSION_STATUS_CLOSED;

	}
}

void nc_session_free (struct nc_session* session)
{
	struct session_list_item *litem, *aux;
	int i;

	nc_session_close(session, NC_SESSION_TERM_OTHER);

	/* free items untouched by nc_session_close() */
	if (session->username != NULL) {
		free (session->username);
	}
	if (session->groups != NULL) {
		for (i = 0; session->groups[i] != NULL; i++) {
			free(session->groups[i]);
		}
		free(session->groups);
	}
	if (session->capabilities != NULL) {
		nc_cpblts_free(session->capabilities);
	}

	/* destroy mutexes */
	pthread_mutex_destroy(&(session->mut_in));
	pthread_mutex_destroy(&(session->mut_out));
	pthread_mutex_destroy(&(session->mut_mqueue));
	pthread_mutex_destroy(&(session->mut_equeue));
	pthread_mutex_destroy(&(session->mut_session));

	if (session_list != NULL && session->monitored == 1) {
		/* remove from internal list if session is monitored */
		pthread_rwlock_wrlock(&(session_list->lock));
		if (session_list->count > 0) {
			for (litem = (struct session_list_item*)((char*)(session_list->record) + session_list->first_offset);
					;
					litem = (struct session_list_item*)((char*)litem + litem->offset_next)) {
				if (strcmp(litem->session_id, session->session_id) == 0) {
					/* we have matching record */
					litem->scounter--;

					if (litem->scounter == 0) {
						/* reconnect the list */
						if (litem->offset_prev == 0) { /* first item in the list */
							session_list->first_offset += litem->offset_next;
						} else {
							aux = (struct session_list_item*) ((char*) litem - litem->offset_prev);
							aux->offset_next = (litem->offset_next == 0) ? 0 : (aux->offset_next + litem->offset_next);
						}
						aux = (struct session_list_item*) ((char*) litem + litem->offset_next);
						aux->offset_prev = (litem->offset_prev == 0) ? 0 : (aux->offset_prev + litem->offset_prev);

						session_list->count--;
					}

					/* remove link from session statistics into the mapped file */
					session->stats = NULL;

					/* we are done */
					break;
				}

				/* end loop condition */
				if (litem->offset_next == 0) {
					/* no other item in the list */

					/* the session's stats were not connected
					 * with internal monitoring list, so free it
					 */
					free(session->stats);

					/* end the loop */
					break; /* for loop */
				}
			}
		}
		pthread_rwlock_unlock(&(session_list->lock));
	} else {
		/* there is no internal session monitoring list so session's
		 * stats cannot be connected with it - free the structure
		 * directly from the session structure
		 */
		free(session->stats);
	}

	free (session);
}

NC_SESSION_STATUS nc_session_get_status (const struct nc_session* session)
{
	if (session == NULL) {
		return (NC_SESSION_STATUS_ERROR);
	}

	return (session->status);
}

int nc_session_send (struct nc_session* session, struct nc_msg *msg)
{
	ssize_t c = 0;
	int len;
	char *text;
	char buf[1024];

	if ((session->ssh_channel == NULL) && (session->fd_output == -1)) {
		return (EXIT_FAILURE);
	}

	/*
	 * maybe the previous check can be replaced by the following one, but
	 * using both cannot be wrong
	 */
	if (session->status != NC_SESSION_STATUS_WORKING &&
			session->status != NC_SESSION_STATUS_CLOSING) {
		return (EXIT_FAILURE);
	}

	/* lock the session for sending the data */
	DBG_LOCK("mut_out");
	pthread_mutex_lock(&(session->mut_out));

	xmlDocDumpFormatMemory (msg->doc, (xmlChar**) (&text), &len, NC_CONTENT_FORMATTED);
	DBG("Writing message: %s", text);

	/* if v1.1 send chunk information before message */
	if (session->version == NETCONFV11) {
		snprintf (buf, 1024, "\n#%d\n", (int) strlen (text));
		c = 0;
		do {
			NC_WRITE(session, &(buf[c]), c);
#ifndef DISABLE_LIBSSH
			if (c == LIBSSH2_ERROR_TIMEOUT) {
				DBG_UNLOCK("mut_out");
				pthread_mutex_unlock(&(session->mut_out));
				VERB("Writing data into the communication channel timeouted.");
				return (EXIT_FAILURE);
			}
#endif
		} while (c != (ssize_t) strlen (buf));
	}

	/* write the message */
	c = 0;
	do {
		NC_WRITE(session, &(text[c]), c);
#ifndef DISABLE_LIBSSH
		if (c == LIBSSH2_ERROR_TIMEOUT) {
			DBG_UNLOCK("mut_out");
			pthread_mutex_unlock(&(session->mut_out));
			VERB("Writing data into the communication channel timeouted.");
			return (EXIT_FAILURE);
		}
#endif
	} while (c != (ssize_t) strlen (text));
	free (text);

	/* close message */
	if (session->version == NETCONFV11) {
		text = NC_V11_END_MSG;
	} else { /* NETCONFV10 */
		text = NC_V10_END_MSG;
	}
	c = 0;
	do {
		NC_WRITE(session, &(text[c]), c);
#ifndef DISABLE_LIBSSH
		if (c == LIBSSH2_ERROR_TIMEOUT) {
			DBG_UNLOCK("mut_out");
			pthread_mutex_unlock(&(session->mut_out));
			VERB("Writing data into the communication channel timeouted.");
			return (EXIT_FAILURE);
		}
#endif
	} while (c != (ssize_t) strlen (text));

	/* unlock the session's output */
	DBG_UNLOCK("mut_out");
	pthread_mutex_unlock(&(session->mut_out));

	return (EXIT_SUCCESS);
}

static int nc_session_read_len (struct nc_session* session, size_t chunk_length, char **text, size_t *len)
{
#ifndef DISABLE_LIBSSH
	char *err_msg;
#endif
	char *buf;
	ssize_t c;
	size_t rd = 0;

	/* check if we can work with the session */
	if (session->status != NC_SESSION_STATUS_WORKING &&
			session->status != NC_SESSION_STATUS_CLOSING) {
		return (EXIT_FAILURE);
	}

	buf = malloc ((chunk_length + 1) * sizeof(char));
	if (buf == NULL) {
		ERROR("Memory reallocation failed (%s:%d).", __FILE__, __LINE__);
		*len = 0;
		*text = NULL;
		return (EXIT_FAILURE);
	}

	while (rd < chunk_length) {
#ifndef DISABLE_LIBSSH
		if (session->ssh_channel) {
			/* read via libssh2 */
			c = libssh2_channel_read(session->ssh_channel, &(buf[rd]), chunk_length - rd);
			if (c == LIBSSH2_ERROR_EAGAIN || c == LIBSSH2_ERROR_TIMEOUT) {
				usleep (NC_READ_SLEEP);
				continue;
			} else if (c < 0) {
				libssh2_session_last_error (session->ssh_session, &err_msg, NULL, 0);
				ERROR("Reading from the SSH channel failed (%zd: %s)", c, err_msg);
				free (buf);
				*len = 0;
				*text = NULL;
				return (EXIT_FAILURE);
			} else if (c == 0) {
				if (libssh2_channel_eof (session->ssh_channel)) {
					ERROR("Server has closed the communication socket");
					free (buf);
					*len = 0;
					*text = NULL;
					return (EXIT_FAILURE);
				}
				usleep (NC_READ_SLEEP);
				continue;
			}
		} else
#endif
		if (session->fd_input != -1) {
			/* read via file descriptor */
			c = read (session->fd_input, &(buf[rd]), chunk_length - rd);
			if (c == -1) {
				if (errno == EAGAIN) {
					usleep (NC_READ_SLEEP);
					continue;
				} else {
					ERROR("Reading from an input file descriptor failed (%s)", strerror(errno));
					free (buf);
					*len = 0;
					*text = NULL;
					return (EXIT_FAILURE);
				}
			}
		} else {
			ERROR("No way to read the input, fatal error.");
			free (buf);
			*len = 0;
			*text = NULL;
			return (EXIT_FAILURE);
		}

		rd += c;
	}

	/* add terminating null byte */
	buf[rd] = 0;

	*len = rd;
	*text = buf;
	return (EXIT_SUCCESS);
}

static int nc_session_read_until (struct nc_session* session, const char* endtag, unsigned int limit, char **text, size_t *len)
{
#ifndef DISABLE_LIBSSH
	char *err_msg;
#endif
	size_t rd = 0;
	ssize_t c;
	static char *buf = NULL;
	static size_t buflen = 0;

	/* check if we can work with the session */
	if (session->status != NC_SESSION_STATUS_WORKING &&
			session->status != NC_SESSION_STATUS_CLOSING) {
		return (EXIT_FAILURE);
	}

	if (endtag == NULL) {
		return (EXIT_FAILURE);
	}

	/* allocate memory according to so far maximum buffer size */
	if (buflen == 0) {
		/* set starting buffer size */
		buflen = 1024;
		buf = (char*) malloc (buflen * sizeof(char));
		if (buf == NULL) {
			ERROR("Memory reallocation failed (%s:%d).", __FILE__, __LINE__);
			return (EXIT_FAILURE);
		}
	}

	for (rd = 0;;) {
		if (limit > 0 && rd > limit) {
			WARN("%s: reading limit reached.", __func__);
			return (EXIT_FAILURE);
		}
#ifndef DISABLE_LIBSSH
		if (session->ssh_channel) {
			/* read via libssh2 */
			c = libssh2_channel_read(session->ssh_channel, &(buf[rd]), 1);
			if (c == LIBSSH2_ERROR_EAGAIN || c == LIBSSH2_ERROR_TIMEOUT) {
				usleep (NC_READ_SLEEP);
				continue;
			} else if (c < 0) {
				libssh2_session_last_error (session->ssh_session, &err_msg, NULL, 0);
				ERROR("Reading from the SSH channel failed (%zd: %s)", c, err_msg);
				free (buf);
				buflen = 0;
				if (len != NULL) {
					*len = 0;
				}
				if (text != NULL) {
					*text = NULL;
				}
				return (EXIT_FAILURE);
			} else if (c == 0) {
				if (libssh2_channel_eof (session->ssh_channel)) {
					ERROR("Server has closed the communication socket");
					free (buf);
					buflen = 0;
					if (len != NULL) {
						*len = 0;
					}
					if (text != NULL) {
						*text = NULL;
					}
					return (EXIT_FAILURE);
				}
				usleep (NC_READ_SLEEP);
				continue;
			}
		} else
#endif
		if (session->fd_input != -1) {
			/* read via file descriptor */
			c = read (session->fd_input, &(buf[rd]), 1);
			if (c == -1) {
				if (errno == EAGAIN) {
					usleep (NC_READ_SLEEP);
					continue;
				} else {
					ERROR("Reading from an input file descriptor failed (%s)", strerror(errno));
					free (buf);
					buflen = 0;
					if (len != NULL) {
						*len = 0;
					}
					if (text != NULL) {
						*text = NULL;
					}
					return (EXIT_FAILURE);
				}
			}
		} else {
			ERROR("No way to read the input, fatal error.");
			free (buf);
			buflen = 0;
			if (len != NULL) {
				*len = 0;
			}
			if (text != NULL) {
				*text = NULL;
			}
			return (EXIT_FAILURE);
		}

		rd += c; /* should be rd++ */
		buf[rd] = '\0';

		if ((rd) < strlen (endtag)) {
			/* not enough data to compare with endtag */
			continue;
		} else {
			/* compare with endtag */
			if (strcmp (endtag, &(buf[rd - strlen (endtag)])) == 0) {
				/* end tag found */
				if (len != NULL) {
					*len = rd;
				}
				if (text != NULL) {
					*text = strdup (buf);
				}
				return (EXIT_SUCCESS);
			}
		}

		/* resize buffer if needed */
		if (rd == (buflen-1)) {
			/* get more memory for the text */
			buf = (char*) realloc (buf, (2 * buflen) * sizeof(char));
			if (buf == NULL) {
				ERROR("Memory reallocation failed (%s:%d).", __FILE__, __LINE__);
				if (len != NULL) {
					*len = 0;
				}
				if (text != NULL) {
					*text = NULL;
				}
				return (EXIT_FAILURE);
			}
			buflen = 2 * buflen;
		}
	}

	/* no one could be here */
	ERROR("Reading NETCONF message fatal failure");
	if (buf != NULL) {
		free (buf);
		buflen = 0;
	}
	if (len != NULL) {
		*len = 0;
	}
	if (text != NULL) {
		*text = NULL;
	}
	return (EXIT_FAILURE);
}

/**
 * @brief Get the message id string from the NETCONF message
 *
 * @param[in] msg NETCONF message to parse.
 * @return 0 on error,\n message-id of the message on success.
 */
const nc_msgid nc_msg_parse_msgid(const struct nc_msg *msg)
{
	nc_msgid ret = NULL;
	xmlAttrPtr prop;

	/* parse and store message-id */
	prop = xmlHasProp(xmlDocGetRootElement(msg->doc), BAD_CAST "message-id");
	if (prop != NULL && prop->children != NULL && prop->children->content != NULL) {
		ret = (char*)prop->children->content;
	}
	if (ret == NULL) {
		if (xmlStrcmp (xmlDocGetRootElement(msg->doc)->name, BAD_CAST "hello") != 0) {
			WARN("Missing message-id in %s.", (char*)xmlDocGetRootElement(msg->doc)->name);
			ret = NULL;
		} else {
			ret = "hello";
		}
	}

	return (ret);
}

static NC_MSG_TYPE nc_session_receive (struct nc_session* session, int timeout, struct nc_msg** msg)
{
	struct nc_msg *retval;
	nc_reply* reply;
	const char* id;
	char *text = NULL, *chunk = NULL;
	size_t len;
	unsigned long long int text_size = 0, total_len = 0;
	size_t chunk_length;
	struct pollfd fds;
	int status;
	unsigned long int revents;
	NC_MSG_TYPE msgtype;
	xmlNodePtr root;

#ifndef DISABLE_LIBSSH
	LIBSSH2_POLLFD fds_ssh;
#endif

	if (session == NULL || (session->status != NC_SESSION_STATUS_WORKING && session->status != NC_SESSION_STATUS_CLOSING)) {
		ERROR("Invalid session to receive data.");
		return (NC_MSG_UNKNOWN);
	}

	/* lock the session for receiving */
	DBG_LOCK("mut_in");
	pthread_mutex_lock(&(session->mut_in));

	/* use while for possibility of repeating test */
	while(1) {
		if (session->ssh_channel == NULL && session->fd_input != -1) {
			/* we are getting data from standard file descriptor */
			fds.fd = session->fd_input;
			fds.events = POLLIN;
			fds.revents = 0;
			status = poll(&fds, 1, timeout);

			revents = (unsigned long int) fds.revents;
		}
#ifndef DISABLE_LIBSSH
		else if (session->ssh_channel != NULL) {
			/* we are getting data from libssh's channel */
			fds_ssh.type = LIBSSH2_POLLFD_CHANNEL;
			fds_ssh.fd.channel = session->ssh_channel;
			fds_ssh.events = LIBSSH2_POLLFD_POLLIN;
			fds_ssh.revents = LIBSSH2_POLLFD_POLLIN;
			/*
			 * According to libssh2 documentation, standard poll should work, but it does not.
			 * It seems, that some data are stored in internal buffers and they are not seen
			 * by poll, but libssh2_poll on the channel.
			 */
			/*
			fds.fd = eventfd;
			fds.events = POLLIN;
			fds.revents = 0;
			status = poll(&fds, 1, 100);
			*/
			status = libssh2_poll(&fds_ssh, 1, timeout);

			revents = fds_ssh.revents;
		}
#endif

		/* process the result */
		if (status == 0) {
			/* timed out */
			DBG_UNLOCK("mut_in");
			pthread_mutex_unlock(&(session->mut_in));
			return (NC_MSG_WOULDBLOCK);
		} else if ((status == -1) && (errno == EINTR)) {
			/* poll was interrupted */
			continue;
		} else if (status < 0) {
			/* poll failed - something wrong happend, close this socket and wait for another request */
			ERROR("Input channel error");
			nc_session_close(session, NC_SESSION_TERM_DROPPED);
			DBG_UNLOCK("mut_in");
			pthread_mutex_unlock(&(session->mut_in));
			if (nc_info) {
				pthread_rwlock_wrlock(&(nc_info->lock));
				nc_info->stats.sessions_dropped++;
				pthread_rwlock_unlock(&(nc_info->lock));
			}
			return (NC_MSG_UNKNOWN);

		}
		/* status > 0 */
		/* check the status of the socket */
		/* if nothing to read and POLLHUP (EOF) or POLLERR set */
		if ((revents & POLLHUP) || (revents & POLLERR)) {
			/* close client's socket (it's probably already closed by client */
			ERROR("Input channel closed");
			nc_session_close(session, NC_SESSION_TERM_DROPPED);
			DBG_UNLOCK("mut_in");
			pthread_mutex_unlock(&(session->mut_in));
			if (nc_info) {
				pthread_rwlock_wrlock(&(nc_info->lock));
				nc_info->stats.sessions_dropped++;
				pthread_rwlock_unlock(&(nc_info->lock));
			}
			return (NC_MSG_UNKNOWN);
		}

		/* we have something to read */
		break;
	}

	switch (session->version) {
	case NETCONFV10:
		if (nc_session_read_until (session, NC_V10_END_MSG, 0, &text, &len) != 0) {
			goto malformed_msg;
		}
		text[len - strlen (NC_V10_END_MSG)] = 0;
		DBG("Received message: %s", text);
		break;
	case NETCONFV11:
		do {
			if (nc_session_read_until (session, "\n#", 2, NULL, NULL) != 0) {
				if (total_len > 0) {
					free (text);
				}
				goto malformed_msg;
			}
			if (nc_session_read_until (session, "\n", 0, &chunk, &len) != 0) {
				if (total_len > 0) {
					free (text);
				}
				goto malformed_msg;
			}
			if (strcmp (chunk, "#\n") == 0) {
				/* end of chunked framing message */
				free (chunk);
				break;
			}

			/* convert string to the size of the following chunk */
			chunk_length = strtoul (chunk, (char **) NULL, 10);
			if (chunk_length == 0) {
				ERROR("Invalid frame chunk size detected, fatal error.");
				goto malformed_msg;
			}
			free (chunk);
			chunk = NULL;

			/* now we have size of next chunk, so read the chunk */
			if (nc_session_read_len (session, chunk_length, &chunk, &len) != 0) {
				if (total_len > 0) {
					free (text);
				}
				goto malformed_msg;
			}

			/* realloc resulting text buffer if needed (always needed now) */
			if (text_size < (total_len + len + 1)) {
				text = realloc (text, total_len + len + 1);
				if (text == NULL) {
					ERROR("Memory reallocation failed (%s:%d).", __FILE__, __LINE__);
					goto malformed_msg;
				}
				text[total_len] = '\0';
				text_size = total_len + len + 1;
			}
			strcat (text, chunk);
			total_len = strlen (text); /* don't forget count terminating null byte */
			free (chunk);
			chunk = NULL;

		} while (1);
		DBG("Received message: %s", text);
		break;
	default:
		ERROR("Unsupported NETCONF protocol version (%d)", session->version);
		goto malformed_msg;
		break;
	}

	DBG_UNLOCK("mut_in");
	pthread_mutex_unlock(&(session->mut_in));

	retval = calloc (1, sizeof(struct nc_msg));
	if (retval == NULL) {
		ERROR("Memory reallocation failed (%s:%d).", __FILE__, __LINE__);
		free (text);
		goto malformed_msg;
	}

	/* store the received message in libxml2 format */
	retval->doc = xmlReadDoc (BAD_CAST text, NULL, NULL, XML_PARSE_NOBLANKS | XML_PARSE_NOERROR | XML_PARSE_NOWARNING);
	if (retval->doc == NULL) {
		free (retval);
		free (text);
		ERROR("Invalid XML data received.");
		goto malformed_msg;
	}
	free (text);

	/* create xpath evaluation context */
	if ((retval->ctxt = xmlXPathNewContext(retval->doc)) == NULL) {
		ERROR("%s: rpc message XPath context cannot be created.", __func__);
		nc_msg_free(retval);
		goto malformed_msg;
	}

	/* register base namespace for the rpc */
	if (xmlXPathRegisterNs(retval->ctxt, BAD_CAST NC_NS_BASE10_ID, BAD_CAST NC_NS_BASE10) != 0) {
		ERROR("Registering base namespace for the message xpath context failed.");
		nc_msg_free(retval);
		goto malformed_msg;
	}
	if (xmlXPathRegisterNs(retval->ctxt, BAD_CAST NC_NS_NOTIFICATIONS_ID, BAD_CAST NC_NS_NOTIFICATIONS) != 0) {
		ERROR("Registering notifications namespace for the message xpath context failed.");
		nc_msg_free(retval);
		goto malformed_msg;
	}
	if (xmlXPathRegisterNs(retval->ctxt, BAD_CAST NC_NS_WITHDEFAULTS_ID, BAD_CAST NC_NS_WITHDEFAULTS) != 0) {
		ERROR("Registering with-defaults namespace for the message xpath context failed.");
		nc_msg_free(retval);
		goto malformed_msg;
	}
	if (xmlXPathRegisterNs(retval->ctxt, BAD_CAST NC_NS_MONITORING_ID, BAD_CAST NC_NS_MONITORING) != 0) {
		ERROR("Registering monitoring namespace for the message xpath context failed.");
		nc_msg_free(retval);
		goto malformed_msg;
	}

	/* parse and store message type */
	root = xmlDocGetRootElement(retval->doc);
	if (xmlStrcmp (root->name, BAD_CAST "rpc-reply") == 0) {
		msgtype = NC_MSG_REPLY;

		/* set reply type flag */
		nc_reply_parse_type(retval);

	} else if (xmlStrcmp (root->name, BAD_CAST "rpc") == 0) {
		msgtype = NC_MSG_RPC;

		/* set rpc type flag */
		nc_rpc_parse_type(retval);

		/* set with-defaults if any */
		nc_rpc_parse_withdefaults(retval, NULL);
	} else if (xmlStrcmp (root->name, BAD_CAST "notification") == 0) {
		/* we have notification */
		msgtype = NC_MSG_NOTIFICATION;
	} else if (xmlStrcmp (root->name, BAD_CAST "hello") == 0) {
		/* set message type, we have <hello> message */
		retval->type.reply = NC_REPLY_HELLO;
		msgtype = NC_MSG_HELLO;
	} else {
		WARN("Unknown (unsupported) type of received message detected.");
		retval->type.rpc = NC_RPC_UNKNOWN;
		msgtype = NC_MSG_UNKNOWN;
	}

	if (msgtype == NC_MSG_RPC || msgtype == NC_MSG_REPLY) {
		/* parse and store message-id */
		if ((id = nc_msg_parse_msgid(retval)) == NULL) {
			retval->msgid = NULL;
		} else {
			retval->msgid = strdup(id);
		}
	} else {
		retval->msgid = NULL;
	}

	/* return the result */
	*msg = retval;
	return (msgtype);

malformed_msg:
	if (session->version == NETCONFV11 && session->ssh_session == NULL) {
		/* NETCONF version 1.1 define sending error reply from the server */
		reply = nc_reply_error(nc_err_new(NC_ERR_MALFORMED_MSG));
		if (reply == NULL) {
			ERROR("Unable to create the \'Malformed message\' reply");
			nc_session_close(session, NC_SESSION_TERM_OTHER);
			return (NC_MSG_UNKNOWN);
		}

		nc_session_send_reply(session, NULL, reply);
		nc_reply_free(reply);
	}

	ERROR("Malformed message received, closing the session %s.", session->session_id);
	nc_session_close(session, NC_SESSION_TERM_OTHER);

	return (NC_MSG_UNKNOWN);
}

static NC_MSG_TYPE nc_session_recv_msg (struct nc_session* session, int timeout, struct nc_msg** msg)
{
	NC_MSG_TYPE ret;

	ret = nc_session_receive (session, timeout, msg);
	switch (ret) {
	case NC_MSG_REPLY: /* regular reply received */
	case NC_MSG_HELLO:
	case NC_MSG_NOTIFICATION:
	case NC_MSG_WOULDBLOCK:
		/* do nothing, just return the type */
		break;
	default:
		ret = NC_MSG_UNKNOWN;
		break;
	}

	return (ret);
}

NC_MSG_TYPE nc_session_recv_reply (struct nc_session* session, int timeout, nc_reply** reply)
{
	struct nc_msg *msg_aux, *msg;
	NC_MSG_TYPE ret;
	int local_timeout;

	if (timeout == 0) {
		local_timeout = 0;
	} else {
		local_timeout = 100;
	}

	DBG_LOCK("mut_mqueue");
	pthread_mutex_lock(&(session->mut_mqueue));

try_again:
	if (session->queue_msg != NULL) {
		/* pop the oldest reply from the queue */
		*reply = (nc_reply*)(session->queue_msg);
		session->queue_msg = (*reply)->next;
		DBG_UNLOCK("mut_mqueue");
		pthread_mutex_unlock(&(session->mut_mqueue));
		(*reply)->next = NULL;
		return (NC_MSG_REPLY);
	}

	ret = nc_session_recv_msg(session, local_timeout, &msg);

	switch (ret) {
	case NC_MSG_REPLY: /* regular reply received */
		/* if specified callback for processing rpc-error, use it */
		if (nc_reply_get_type (msg) == NC_REPLY_ERROR &&
				callbacks.process_error_reply != NULL) {
			/* process rpc-error msg */
			callbacks.process_error_reply(msg->error->tag,
					msg->error->type,
					msg->error->severity,
					msg->error->apptag,
					msg->error->path,
					msg->error->message,
					msg->error->attribute,
					msg->error->element,
					msg->error->ns,
					msg->error->sid);
			/* free the data */
			nc_reply_free(msg);
			ret = NC_MSG_NONE;
		} else {
			*reply = (nc_reply*)msg;
		}
		break;
	case NC_MSG_HELLO: /* hello message received */
		*reply = (nc_reply*)msg;
		break;
	case NC_MSG_NONE:
		/* <rpc-reply> with error information was processed automatically */
		break;
	case NC_MSG_WOULDBLOCK:
		if ((timeout == -1) || ((timeout > 0) && ((local_timeout = local_timeout - 100) > 0))) {
			goto try_again;
		}
		break;
	case NC_MSG_NOTIFICATION:
		/* add event notification into the session's list of notification messages */
		msg_aux = session->queue_event;
		if (msg_aux == NULL) {
			msg->next = session->queue_event;
			session->queue_event = msg;
		} else {
			for (; msg_aux->next != NULL; msg_aux = msg_aux->next);
			msg_aux->next = msg;
		}

		break;
	default:
		ret = NC_MSG_UNKNOWN;
		break;
	}

	/* session lock is no more needed */
	DBG_UNLOCK("mut_mqueue");
	pthread_mutex_unlock(&(session->mut_mqueue));

	return (ret);
}

int nc_session_send_notif (struct nc_session* session, const nc_ntf* ntf)
{
	int ret;
	struct nc_msg *msg;

	if (session == NULL || (session->status != NC_SESSION_STATUS_WORKING && session->status != NC_SESSION_STATUS_CLOSING)) {
		ERROR("Invalid session to send <notification>.");
		return (EXIT_FAILURE);
	}

	msg = nc_msg_dup ((struct nc_msg*) ntf);

	/* send message */
	ret = nc_session_send (session, msg);

	nc_msg_free (msg);

	if (ret == EXIT_SUCCESS) {
		/* update stats */
		session->stats->out_notifications++;
		if (nc_info) {
			pthread_rwlock_wrlock(&(nc_info->lock));
			nc_info->stats.counters.out_notifications++;
			pthread_rwlock_unlock(&(nc_info->lock));
		}
	}

	return (ret);
}

NC_MSG_TYPE nc_session_recv_notif (struct nc_session* session, int timeout, nc_ntf** ntf)
{
	struct nc_msg *msg_aux, *msg;
	NC_MSG_TYPE ret;
	int local_timeout;

	if (timeout == 0) {
		local_timeout = 0;
	} else {
		local_timeout = 100;
	}

	DBG_LOCK("mut_equeue");
	pthread_mutex_lock(&(session->mut_equeue));

try_again:
	if (session->queue_event != NULL) {
		/* pop the oldest reply from the queue */
		*ntf = (nc_reply*)(session->queue_event);
		session->queue_event = (*ntf)->next;
		DBG_UNLOCK("mut_equeue");
		pthread_mutex_unlock(&(session->mut_equeue));
		(*ntf)->next = NULL;
		return (NC_MSG_NOTIFICATION);
	}

	ret = nc_session_recv_msg(session, local_timeout, &msg);

	switch (ret) {
	case NC_MSG_REPLY: /* regular reply received */
		/* add reply into the session's list of reply messages */
		msg_aux = session->queue_msg;
		if (msg_aux == NULL) {
			msg->next = session->queue_msg;
			session->queue_msg = msg;
		} else {
			for (; msg_aux->next != NULL; msg_aux = msg_aux->next);
			msg_aux->next = msg;
		}
		break;
	case NC_MSG_NONE:
		/* <rpc-reply> with error information was processed
		 * automatically, but we are waiting for a notification
		 */
		break;
	case NC_MSG_WOULDBLOCK:
		if ((timeout == -1) || ((timeout > 0) && ((local_timeout = local_timeout - 100) > 0))) {
			goto try_again;
		}
		break;
	case NC_MSG_NOTIFICATION:
		*ntf = (nc_reply*)msg;
		break;
	default:
		ret = NC_MSG_UNKNOWN;
		break;
	}

	DBG_UNLOCK("mut_equeue");
	pthread_mutex_unlock(&(session->mut_equeue));

	return (ret);
}

NC_MSG_TYPE nc_session_recv_rpc (struct nc_session* session, int timeout, nc_rpc** rpc)
{
	NC_MSG_TYPE ret;
	struct nc_err* e = NULL;
	nc_reply* reply;
	int local_timeout;

	if (timeout == 0) {
		local_timeout = 0;
	} else {
		local_timeout = 100;
	}

try_again:
	ret = nc_session_receive (session, local_timeout, (struct nc_msg**) rpc);
	switch (ret) {
	case NC_MSG_RPC:
		(*rpc)->with_defaults = nc_rpc_parse_withdefaults(*rpc, session);

		/* check for with-defaults capability */
		if ((*rpc)->with_defaults != NCWD_MODE_NOTSET) {
			/* check if the session support this */
			if (session->wd_basic == NCWD_MODE_NOTSET) {
				ERROR("rpc requires the with-defaults capability, but the session does not support it.");
				e = nc_err_new(NC_ERR_INVALID_VALUE);
				nc_err_set(e, NC_ERR_PARAM_INFO_BADELEM, "with-defaults");
				nc_err_set(e, NC_ERR_PARAM_MSG, "rpc requires the with-defaults capability, but the session does not support it.");
			} else {
				switch ((*rpc)->with_defaults) {
				case NCWD_MODE_ALL:
					if ((session->wd_modes & NCWD_MODE_ALL) == 0) {
						ERROR("rpc requires the with-defaults capability report-all mode, but the session does not support it.");
						e = nc_err_new(NC_ERR_INVALID_VALUE);
						nc_err_set(e, NC_ERR_PARAM_INFO_BADELEM, "with-defaults");
						nc_err_set(e, NC_ERR_PARAM_MSG, "rpc requires the with-defaults capability report-all mode, but the session does not support it.");
					}
					break;
				case NCWD_MODE_ALL_TAGGED:
					if ((session->wd_modes & NCWD_MODE_ALL_TAGGED) == 0) {
						ERROR("rpc requires the with-defaults capability report-all-tagged mode, but the session does not support it.");
						e = nc_err_new(NC_ERR_INVALID_VALUE);
						nc_err_set(e, NC_ERR_PARAM_INFO_BADELEM, "with-defaults");
						nc_err_set(e, NC_ERR_PARAM_MSG, "rpc requires the with-defaults capability report-all-tagged mode, but the session does not support it.");
					}
					break;
				case NCWD_MODE_TRIM:
					if ((session->wd_modes & NCWD_MODE_TRIM) == 0) {
						ERROR("rpc requires the with-defaults capability trim mode, but the session does not support it.");
						e = nc_err_new(NC_ERR_INVALID_VALUE);
						nc_err_set(e, NC_ERR_PARAM_INFO_BADELEM, "with-defaults");
						nc_err_set(e, NC_ERR_PARAM_MSG, "rpc the requires with-defaults capability trim mode, but the session does not support it.");
					}
					break;
				case NCWD_MODE_EXPLICIT:
					if ((session->wd_modes & NCWD_MODE_EXPLICIT) == 0) {
						ERROR("rpc requires the with-defaults capability explicit mode, but the session does not support it.");
						e = nc_err_new(NC_ERR_INVALID_VALUE);
						nc_err_set(e, NC_ERR_PARAM_INFO_BADELEM, "with-defaults");
						nc_err_set(e, NC_ERR_PARAM_MSG, "rpc requires the with-defaults capability explicit mode, but the session does not support it.");
					}
					break;
				default: /* something weird */
					ERROR("rpc requires the with-defaults capability with an unknown mode.");
					e = nc_err_new(NC_ERR_INVALID_VALUE);
					nc_err_set(e, NC_ERR_PARAM_INFO_BADELEM, "with-defaults");
					nc_err_set(e, NC_ERR_PARAM_MSG, "rpc requires the with-defaults capability with an unknown mode.");
					break;
				}
			}

			if (e != NULL) {
				reply = nc_reply_error(e);
				nc_session_send_reply(session, *rpc, reply);
				nc_rpc_free(*rpc);
				*rpc = NULL;
				nc_reply_free(reply);

				/* update stats */
				session->stats->in_bad_rpcs++;
				if (nc_info) {
					pthread_rwlock_wrlock(&(nc_info->lock));
					nc_info->stats.counters.in_bad_rpcs++;
					pthread_rwlock_unlock(&(nc_info->lock));
				}

				return (NC_MSG_NONE); /* message processed internally */
			}
		}
		/* update statistics */
		session->stats->in_rpcs++;
		if (nc_info) {
			pthread_rwlock_wrlock(&(nc_info->lock));
			nc_info->stats.counters.in_rpcs++;
			pthread_rwlock_unlock(&(nc_info->lock));
		}

		/* NACM init */
		nacm_start(*rpc, session);

		/* NACM - check operation access */
		if (nacm_check_operation(*rpc) != NACM_PERMIT) {
			e = nc_err_new(NC_ERR_ACCESS_DENIED);
			nc_err_set(e, NC_ERR_PARAM_MSG, "Operation not permitted.");
			reply = nc_reply_error(e);
			nc_session_send_reply(session, *rpc, reply);
			nc_rpc_free(*rpc);
			*rpc = NULL;
			nc_reply_free(reply);
			/* update stats */
			if (nc_info) {
				pthread_rwlock_wrlock(&(nc_info->lock));
				nc_info->stats_nacm.denied_ops++;
				pthread_rwlock_unlock(&(nc_info->lock));
			}

			return (NC_MSG_NONE); /* message processed internally */
		}

		break;
	case NC_MSG_HELLO:
		/* do nothing, just return the type */
		break;
	case NC_MSG_WOULDBLOCK:
		if ((timeout == -1) || ((timeout > 0) && ((local_timeout = local_timeout - 100) > 0))) {
			goto try_again;
		}
		break;
	default:
		ret = NC_MSG_UNKNOWN;

		/* update stats */
		session->stats->in_bad_rpcs++;
		if (nc_info) {
			pthread_rwlock_wrlock(&(nc_info->lock));
			nc_info->stats.counters.in_bad_rpcs++;
			pthread_rwlock_unlock(&(nc_info->lock));
		}

		break;
	}

	return (ret);
}

const nc_msgid nc_session_send_rpc (struct nc_session* session, nc_rpc *rpc)
{
	int ret;
	char msg_id_str[16];
	const char* wd;
	struct nc_msg *msg;
	NC_OP op;

	if (session == NULL || (session->status != NC_SESSION_STATUS_WORKING && session->status != NC_SESSION_STATUS_CLOSING)) {
		ERROR("Invalid session to send <rpc>.");
		return (NULL); /* failure */
	}

	if (rpc->type.rpc != NC_RPC_HELLO) {
		/* check for capabilities operations */
		op = nc_rpc_get_op(rpc);
		/* :notifications */
		switch (op) {
#ifndef DISABLE_NOTIFICATIONS
		case NC_OP_CREATESUBSCRIPTION:
			if (nc_cpblts_enabled(session, NC_CAP_NOTIFICATION_ID) == 0) {
				ERROR("RPC requires :notifications capability, but the session does not support it.");
				return (NULL); /* failure */
			}
			break;
#endif
		case NC_OP_COMMIT:
		case NC_OP_DISCARDCHANGES:
			if (nc_cpblts_enabled(session, NC_CAP_CANDIDATE_ID) == 0) {
				ERROR("RPC requires :candidate capability, but the session does not support it.");
				return (NULL); /* failure */
			}
			break;
		case NC_OP_GETSCHEMA:
			if (nc_cpblts_enabled(session, NC_CAP_MONITORING_ID) == 0) {
				ERROR("RPC requires :monitoring capability, but the session does not support it.");
				return (NULL); /* failure */
			}
			break;
		default:
			/* no check is needed */
			break;
		}

		/* check for with-defaults capability */
		if (rpc->with_defaults != NCWD_MODE_NOTSET) {
			/* check if the session support this */
			if ((wd = nc_cpblts_get(session->capabilities, NC_CAP_WITHDEFAULTS_ID)) == NULL) {
				ERROR("RPC requires :with-defaults capability, but the session does not support it.");
				return (NULL); /* failure */
			}
			switch (rpc->with_defaults) {
			case NCWD_MODE_ALL:
				if (strstr(wd, "report-all") == NULL) {
					ERROR("RPC requires the with-defaults capability report-all mode, but the session does not support it.");
					return (NULL); /* failure */
				}
				break;
			case NCWD_MODE_ALL_TAGGED:
				if (strstr(wd, "report-all-tagged") == NULL) {
					ERROR("RPC requires the with-defaults capability report-all-tagged mode, but the session does not support it.");
					return (NULL); /* failure */
				}
				break;
			case NCWD_MODE_TRIM:
				if (strstr(wd, "trim") == NULL) {
					ERROR("RPC requires the with-defaults capability trim mode, but the session does not support it.");
					return (NULL); /* failure */
				}
				break;
			case NCWD_MODE_EXPLICIT:
				if (strstr(wd, "explicit") == NULL) {
					ERROR("RPC requires the with-defaults capability explicit mode, but the session does not support it.");
					return (NULL); /* failure */
				}
				break;
			default: /* NCDFLT_MODE_DISABLED */
				/* nothing to check */
				break;
			}
		}
	}

	msg = nc_msg_dup ((struct nc_msg*) rpc);
	/* set message id */
	if (xmlStrcmp (xmlDocGetRootElement(msg->doc)->name, BAD_CAST "rpc") == 0) {
		/* lock the session due to accessing msgid item */
		DBG_LOCK("mut_session");
		pthread_mutex_lock(&(session->mut_session));
		sprintf (msg_id_str, "%llu", session->msgid++);
		DBG_UNLOCK("mut_session");
		pthread_mutex_unlock(&(session->mut_session));
		if (xmlNewProp(xmlDocGetRootElement(msg->doc), BAD_CAST "message-id", BAD_CAST msg_id_str) == NULL) {
			ERROR("xmlNewProp failed (%s:%d).", __FILE__, __LINE__);
			nc_msg_free (msg);
			return (NULL);
		}
	} else {
		/* hello message */
		sprintf (msg_id_str, "hello");
	}

	/* send message */
	ret = nc_session_send (session, msg);

	nc_msg_free (msg);

	if (ret != EXIT_SUCCESS) {
		if (rpc->type.rpc != NC_RPC_HELLO) {
			DBG_LOCK("mut_session");
			pthread_mutex_lock(&(session->mut_session));
			session->msgid--;
			DBG_UNLOCK("mut_session");
			pthread_mutex_unlock(&(session->mut_session));
		}
		return (NULL);
	} else {
		rpc->msgid = strdup(msg_id_str);
		return (rpc->msgid);
	}
}

const nc_msgid nc_session_send_reply (struct nc_session* session, const nc_rpc* rpc, const nc_reply *reply)
{
	int ret;
	struct nc_msg *msg;
	const nc_msgid retval = NULL;
	xmlNsPtr ns, ns_aux;
	xmlNodePtr msg_root, rpc_root;

	if (session == NULL || (session->status != NC_SESSION_STATUS_WORKING && session->status != NC_SESSION_STATUS_CLOSING)) {
		ERROR("Invalid session to send <rpc-reply>.");
		return (0); /* failure */
	}

	if (rpc == NULL) {
		ERROR("%s: Invalid <rpc> message to answer.", __func__);
		return (0); /* failure */
	}

	if (reply == NULL) {
		ERROR("%s: Invalid <reply> message to send.", __func__);
		return (0); /* failure */
	}

	if (rpc->msgid == 0) {
		/* parse and store message-id */
		retval = nc_msg_parse_msgid(rpc);
	} else {
		retval = rpc->msgid;
	}

	msg = nc_msg_dup ((struct nc_msg*) reply);

	if (rpc != NULL) {
		/* set message id */
		msg->msgid = strdup(retval);
		msg_root = xmlDocGetRootElement(msg->doc);
		rpc_root = xmlDocGetRootElement(rpc->doc);
		if (xmlStrEqual(msg_root->name, BAD_CAST "rpc-reply") &&
				xmlStrEqual(msg_root->ns->href, BAD_CAST NC_NS_BASE10)) {
			/* copy attributes from the rpc */
			msg_root->properties = xmlCopyPropList(msg_root, rpc_root->properties);
			if (msg_root->properties == NULL) {
				xmlNewProp(msg_root, BAD_CAST "message-id", BAD_CAST msg->msgid);
			}
			/* copy additional namespace definitions from rpc */
			for (ns = rpc_root->nsDef; ns != NULL; ns = ns->next) {
				if (ns->prefix == NULL) {
					/* skip default namespace */
					continue;
				}
				ns_aux = xmlNewNs(msg_root, ns->href, ns->prefix);
				xmlSetNs(msg_root, ns_aux);
			}

		}
	} else {
		msg_root = xmlDocGetRootElement(msg->doc);
		/* unknown message ID, send reply without it */
		if (xmlStrcmp(msg_root->name, BAD_CAST "rpc-reply") == 0) {
			xmlRemoveProp(xmlHasProp(msg_root, BAD_CAST "message-id"));
		}
	}

	/* send message */
	ret = nc_session_send (session, msg);

	nc_msg_free (msg);

	if (ret != EXIT_SUCCESS) {
		return (0);
	} else {
		if (reply->type.reply == NC_REPLY_ERROR) {
			/* update stats */
			session->stats->out_rpc_errors++;
			if (nc_info) {
				pthread_rwlock_wrlock(&(nc_info->lock));
				nc_info->stats.counters.out_rpc_errors++;
				pthread_rwlock_unlock(&(nc_info->lock));
			}
		}
		return (retval);
	}
}

int nc_msgid_compare (const nc_msgid id1, const nc_msgid id2)
{
	if (id1 == NULL || id2 == NULL) {
		return (-1);
	} else {
		return (strcmp(id1, id2));
	}
}

NC_MSG_TYPE nc_session_send_recv (struct nc_session* session, nc_rpc *rpc, nc_reply** reply)
{
	const nc_msgid msgid;
	NC_MSG_TYPE replytype;
	struct nc_msg* queue = NULL, *msg, *p = NULL;

	msgid = nc_session_send_rpc(session, rpc);
	if (msgid == NULL) {
		return (NC_MSG_UNKNOWN);
	}

	DBG_LOCK("mut_mqueue");
	pthread_mutex_lock(&(session->mut_mqueue));

	/* first, look into the session's list of previously received messages */
	if ((queue = session->queue_msg) != NULL) {
		/* search in the queue for the reply with required message ID */
		for (msg = queue; msg != NULL; msg = msg->next) {
			/* test message IDs */
			if (nc_msgid_compare(msgid, nc_reply_get_msgid((nc_reply*) msg)) == 0) {
				break;
			}

			/* store the predecessor */
			p = msg;
		}

		if (msg != NULL) {
			/* we have found the reply in the queue */
			(*reply) = (nc_reply*) msg;

			/* detach the reply from session's queue */
			if (p == NULL) {
				/* no predecessor - we detach the first item in the queue */
				session->queue_msg = msg->next;
			} else {
				p->next = msg->next;
			}
			msg->next = NULL;
			DBG_UNLOCK("mut_mqueue");
			pthread_mutex_unlock(&(session->mut_mqueue));
			return (NC_MSG_REPLY);
		} else {
			/*
			 * the queue does not contain required reply, we have to
			 * read it from the input, but first we have to hide
			 * session's queue from nc_session_recv_reply()
			 */
			session->queue_msg = NULL;
		}
	}
	DBG_UNLOCK("mut_mqueue");
	pthread_mutex_unlock(&(session->mut_mqueue));

	while (1) {
		replytype = nc_session_recv_reply(session, -1, reply);
		if (replytype == NC_MSG_REPLY) {
			/* compare message ID */
			if (nc_msgid_compare(msgid, nc_reply_get_msgid(*reply)) != 0) {
				/* reply with different message ID is expected */
				DBG_LOCK("mut_mqueue");
				pthread_mutex_lock(&(session->mut_mqueue));
				/* store this reply for the later use of someone else */
				if (queue == NULL) {
					queue = (struct nc_msg*) (*reply);
				} else {
					for (msg = queue; msg->next != NULL; msg = msg->next);
					msg->next = (struct nc_msg*) (*reply);
				}
				DBG_UNLOCK("mut_mqueue");
				pthread_mutex_unlock(&(session->mut_mqueue));
			} else {
				/* we have it! */
				break;
			}
		} else if (replytype == NC_MSG_UNKNOWN || replytype == NC_MSG_NONE) {
			/* some error occured */
			break;
		}
		/*
		 * else (e.g. Notification or an unexpected reply was received)
		 * read another message in the loop
		 */
	}

	if (queue != NULL) {
		DBG_LOCK("mut_mqueue");
		pthread_mutex_lock(&(session->mut_mqueue));
		/* reconnect hidden queue back to the session */
		session->queue_msg = queue;
		DBG_UNLOCK("mut_mqueue");
		pthread_mutex_unlock(&(session->mut_mqueue));
	}

	return (replytype);
}

const char* nc_session_term_string(NC_SESSION_TERM_REASON reason)
{
	switch(reason) {
	case NC_SESSION_TERM_CLOSED:
		return ("closed");
		break;
	case NC_SESSION_TERM_KILLED:
		return ("killed");
		break;
	case NC_SESSION_TERM_DROPPED:
		return ("dropped");
		break;
	case NC_SESSION_TERM_TIMEOUT:
		return ("timeout");
		break;
	case NC_SESSION_TERM_BADHELLO:
		return ("bad-hello");
		break;
	default:
		return ("other");
		break;
	}
	return (NULL);
}
