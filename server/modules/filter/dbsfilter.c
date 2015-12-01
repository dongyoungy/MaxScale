/*
 * This file is distributed as part of MaxScale by MariaDB Corporation.  It is free
 * software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation,
 * version 2.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
 * details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program; if not, write to the Free Software Foundation, Inc., 51
 * Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Copyright MariaDB Corporation Ab 2014
 */

/**
 * @file dbsfilter.c - Query Log For DBSeer
 * @verbatim
 *
 * @endverbatim
 */
#include <stdio.h>
#include <fcntl.h>
#include <filter.h>
#include <modinfo.h>
#include <modutil.h>
#include <skygw_utils.h>
#include <log_manager.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>
#include <regex.h>
#include <atomic.h>

/** Defined in log_manager.cc */
extern int            lm_enabled_logfiles_bitmask;
extern size_t         log_ses_count[];
extern __thread log_info_t tls_log_info;

MODULE_INFO 	info = {
	MODULE_API_FILTER,
	MODULE_GA,
	FILTER_VERSION,
	"DBSeer query logging filter"
};

static char *version_str = "V0.1";

/*
 * The filter entry points
 */
static	FILTER	*createInstance(char **options, FILTER_PARAMETER **);
static	void	*newSession(FILTER *instance, SESSION *session);
static	void 	closeSession(FILTER *instance, void *session);
static	void 	freeSession(FILTER *instance, void *session);
static	void	setDownstream(FILTER *instance, void *fsession, DOWNSTREAM *downstream);
static	void	setUpstream(FILTER *instance, void *fsession, UPSTREAM *upstream);
static	int	routeQuery(FILTER *instance, void *fsession, GWBUF *queue);
static	int	clientReply(FILTER *instance, void *fsession, GWBUF *queue);
static	void	diagnostic(FILTER *instance, void *fsession, DCB *dcb);

static FILTER_OBJECT MyObject = {
    createInstance,
    newSession,
    closeSession,
    freeSession,
    setDownstream,
    setUpstream,
    routeQuery,
    clientReply,
    diagnostic,
};

/**
 * A instance structure, every instance will write to a same file.
 */
typedef struct {
	int	sessions;	/* Session count */
	char	*source;	/* The source of the client connection */
	char	*user;	/* The user name to filter on */
	char	*filename;	/* filename */
	char	*delimiter; /* delimiter for the log */

	FILE* fp;
} DBS_INSTANCE;

/**
 * The session structure for this DBS filter.
 * This stores the downstream filter information, such that the
 * filter is able to pass the query on to the next filter (or router)
 * in the chain.
 *
 * It also holds the file descriptor to which queries are written.
 */
typedef struct {
	DOWNSTREAM	down;
	UPSTREAM	up;
	int		active;
	char		*clientHost;
	char		*userName;
	char* sql;
	struct timeval	start;
	char		*current;
	int		n_statements;
	struct timeval	total;
	struct timeval	current_start;
	bool query_end;
} DBS_SESSION;

/**
 * Implementation of the mandatory version entry point
 *
 * @return version string of the module
 */
char *
version()
{
	return version_str;
}

/**
 * The module initialisation routine, called when the module
 * is first loaded.
 */
void
ModuleInit()
{
}

/**
 * The module entry point routine. It is this routine that
 * must populate the structure that is referred to as the
 * "module object", this is a structure with the set of
 * external entry points for this module.
 *
 * @return The module object
 */
FILTER_OBJECT *
GetModuleObject()
{
	return &MyObject;
}

/**
 * Create an instance of the filter for a particular service
 * within MaxScale.
 *
 * @param options	The options for this filter
 * @param params	The array of name/value pair parameters for the filter
 *
 * @return The instance data for this new instance
 */
static	FILTER	*
createInstance(char **options, FILTER_PARAMETER **params)
{
int		i;
DBS_INSTANCE	*my_instance;

	if ((my_instance = calloc(1, sizeof(DBS_INSTANCE))) != NULL)
	{
		my_instance->source = NULL;
		my_instance->user = NULL;

		/* set default log filename */
		my_instance->filename = strdup("dbseer_query.log");
		/* set default delimiter */
		my_instance->delimiter = strdup("|");

		for (i = 0; params && params[i]; i++)
		{
			if (!strcmp(params[i]->name, "filename"))
			{
				free(my_instance->filename);
				my_instance->filename = strdup(params[i]->value);
			}
			else if (!strcmp(params[i]->name, "source"))
				my_instance->source = strdup(params[i]->value);
			else if (!strcmp(params[i]->name, "user"))
				my_instance->user = strdup(params[i]->value);
			else if (!strcmp(params[i]->name, "delimiter"))
			{
				free(my_instance->delimiter);
				my_instance->delimiter = strdup(params[i]->value);
			}
		}
		my_instance->sessions = 0;
	  my_instance->fp = fopen(my_instance->filename, "w");
	}
	return (FILTER *)my_instance;
}

/**
 * Associate a new session with this instance of the filter.
 *
 * Create the file to log to and open it.
 *
 * @param instance	The filter instance data
 * @param session	The session itself
 * @return Session specific data for this session
 */
static	void	*
newSession(FILTER *instance, SESSION *session)
{
DBS_INSTANCE	*my_instance = (DBS_INSTANCE *)instance;
DBS_SESSION	*my_session;
int		i;
char		*remote, *user;

	if ((my_session = calloc(1, sizeof(DBS_SESSION))) != NULL)
	{
		atomic_add(&my_instance->sessions,1);

		my_session->sql = NULL;
		my_session->n_statements = 0;
		my_session->total.tv_sec = 0;
		my_session->total.tv_usec = 0;
		my_session->current = NULL;
		if ((remote = session_get_remote(session)) != NULL)
			my_session->clientHost = strdup(remote);
		else
			my_session->clientHost = NULL;
		if ((user = session_getUser(session)) != NULL)
			my_session->userName = strdup(user);
		else
			my_session->userName = NULL;
		my_session->active = 1;
		if (my_instance->source && my_session->clientHost && strcmp(my_session->clientHost,
							my_instance->source))
			my_session->active = 0;
		if (my_instance->user && my_session->userName && strcmp(my_session->userName,
							my_instance->user))
			my_session->active = 0;
	}

	return my_session;
}

/**
 * Close a session with the filter, this is the mechanism
 * by which a filter may cleanup data structure etc.
 *
 * @param instance	The filter instance data
 * @param session	The session being closed
 */
static	void
closeSession(FILTER *instance, void *session)
{
	DBS_SESSION	*my_session = (DBS_SESSION *)session;
}

/**
 * Free the memory associated with the session
 *
 * @param instance	The filter instance
 * @param session	The filter session
 */
static void
freeSession(FILTER *instance, void *session)
{
DBS_SESSION	*my_session = (DBS_SESSION *)session;

	free(my_session->clientHost);
	free(my_session->userName);
	free(my_session->sql);
	free(session);
	return;
}

/**
 * Set the downstream filter or router to which queries will be
 * passed from this filter.
 *
 * @param instance	The filter instance data
 * @param session	The filter session
 * @param downstream	The downstream filter or router.
 */
static void
setDownstream(FILTER *instance, void *session, DOWNSTREAM *downstream)
{
DBS_SESSION	*my_session = (DBS_SESSION *)session;

	my_session->down = *downstream;
}

/**
 * Set the upstream filter or session to which results will be
 * passed from this filter.
 *
 * @param instance	The filter instance data
 * @param session	The filter session
 * @param upstream	The upstream filter or session.
 */
static void
setUpstream(FILTER *instance, void *session, UPSTREAM *upstream)
{
DBS_SESSION	*my_session = (DBS_SESSION *)session;

	my_session->up = *upstream;
}

/**
 * The routeQuery entry point. This is passed the query buffer
 * to which the filter should be applied. Once applied the
 * query should normally be passed to the downstream component
 * (filter or router) in the filter chain.
 *
 * @param instance	The filter instance data
 * @param session	The filter session
 * @param queue		The query data
 */
static	int
routeQuery(FILTER *instance, void *session, GWBUF *queue)
{
DBS_INSTANCE	*my_instance = (DBS_INSTANCE *)instance;
DBS_SESSION	*my_session = (DBS_SESSION *)session;
char		*ptr;
size_t i;

	if (my_session->active)
	{
		if (queue->next != NULL)
		{
			queue = gwbuf_make_contiguous(queue);
		}
		if ((ptr = modutil_get_SQL(queue)) != NULL)
		{
			my_session->query_end = false;
			/* check for commit and rollback */
			if (strlen(ptr) > 5)
			{
				size_t buf_size = strlen(ptr)+1;
				char* buf = (char*)malloc(buf_size);
				for (i=0; i < buf_size && i < 9; ++i)
				{
					buf[i] = tolower(ptr[i]);
				}
				if (strncmp(buf, "commit", 6) == 0)
				{
					my_session->query_end = true;
				}
				else if (strncmp(buf, "rollback", 8) == 0)
				{
					free(my_session->sql);
					my_session->sql = NULL;
					my_session->query_end = true;
				}
				free(buf);
			}

			/* for normal sql statements */
			if (!my_session->query_end)
			{
				/* first statement */
				if (my_session->sql == NULL)
				{
					my_session->sql = strdup(ptr);
					gettimeofday(&my_session->current_start, NULL);
				}
				/* distinguish statements with semicolon */
				else
				{
					size_t len = strlen(my_session->sql) + strlen(ptr) + 2;
					char* new_sql = (char*)malloc(len);
					memset(new_sql, 0x00, len);
					strcat(new_sql, my_session->sql);
					strcat(new_sql, ";");
					strcat(new_sql, ptr);
					free(my_session->sql);
					my_session->sql = new_sql;
				}
			}

			free(ptr);
		}
	}
	/* Pass the query downstream */
	return my_session->down.routeQuery(my_session->down.instance,
			my_session->down.session, queue);
}

static int
clientReply(FILTER *instance, void *session, GWBUF *reply)
{
DBS_INSTANCE	*my_instance = (DBS_INSTANCE *)instance;
DBS_SESSION	*my_session = (DBS_SESSION *)session;
struct		timeval		tv, diff;
int		i, inserted;

/* found 'commit' and sql statements exist. */
	if (my_session->query_end && my_session->sql != NULL)
	{
		gettimeofday(&tv, NULL);
		timersub(&tv, &(my_session->current_start), &diff);

		/* get latency */
		uint64_t millis = (diff.tv_sec * (uint64_t)1000 + diff.tv_usec / 1000);
		/* get timestamp */
		uint64_t timestamp = (tv.tv_sec + (tv.tv_usec / (1000*1000)));

		/* print to log. */
		fprintf(my_instance->fp, "%ld%s%s%s%s%s%ld%s%s\n",
				timestamp,
				my_instance->delimiter,
				my_session->clientHost,
				my_instance->delimiter,
				my_session->userName,
				my_instance->delimiter,
				millis,
				my_instance->delimiter,
				my_session->sql);
		free(my_session->sql);
		my_session->sql = NULL;
	}

	/* Pass the result upstream */
	return my_session->up.clientReply(my_session->up.instance,
			my_session->up.session, reply);
}

/**
 * Diagnostics routine
 *
 * If fsession is NULL then print diagnostics on the filter
 * instance as a whole, otherwise print diagnostics for the
 * particular session.
 *
 * @param	instance	The filter instance
 * @param	fsession	Filter session, may be NULL
 * @param	dcb		The DCB for diagnostic output
 */
static	void
diagnostic(FILTER *instance, void *fsession, DCB *dcb)
{
DBS_INSTANCE	*my_instance = (DBS_INSTANCE *)instance;
DBS_SESSION	*my_session = (DBS_SESSION *)fsession;
int		i;

	if (my_instance->source)
		dcb_printf(dcb, "\t\tLimit logging to connections from 	%s\n",
				my_instance->source);
	if (my_instance->user)
		dcb_printf(dcb, "\t\tLimit logging to user		%s\n",
				my_instance->user);
	if (my_session)
	{
		dcb_printf(dcb, "\t\tLogging to file %s.\n",
			my_instance->filename);
	}
}
