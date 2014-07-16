/*
 * This file is distributed as part of the SkySQL Gateway.  It is free
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
 * Copyright SkySQL Ab 2013
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <service.h>
#include <session.h>
#include <router.h>
#include <modules.h>
#include <modinfo.h>
#include <atomic.h>
#include <spinlock.h>
#include <dcb.h>
#include <poll.h>
#include <skygw_utils.h>
#include <log_manager.h>
#include <sapi/embed/php_embed.h>

#ifdef ZTS
    void ***tsrm_ls;
#endif

    PHP_FUNCTION(sample_hello_world) {
        php_printf("Hello World!\n");
        ZVAL_LONG(return_value, 42);
        return;
    }
    
    static function_entry maxapi_functions[] = {
        PHP_FE(sample_hello_world, NULL)
        {   NULL, NULL, NULL    }
    };
    
/* Extension bits */
    zend_module_entry php_mymod_module_entry = {
        STANDARD_MODULE_HEADER,
        "maxapi",   /* extension name */
        maxapi_functions,       /* function entries */
        NULL,       /* MINIT */
        NULL,       /* MSHUTDOWN */
        NULL,       /* RINIT */
        NULL,       /* RSHUTDOWN */
        NULL,       /* MINFO */
        "1.0",      /* Version */
        STANDARD_MODULE_PROPERTIES
    };    
    
static char *version_str = "V1.0.0";

MODULE_INFO 	info = {
	MODULE_API_ROUTER,
	MODULE_IN_DEVELOPMENT,
	ROUTER_VERSION,
	"The API router"
};

static	ROUTER	*createInstance(SERVICE *service, char **options);
static	void	*newSession(ROUTER *instance, SESSION *session);
static	void 	closeSession(ROUTER *instance, void *session);
static	void 	freeSession(ROUTER *instance, void *session);
static	int	routeQuery(ROUTER *instance, void *session, GWBUF *queue);
static	void	diagnostic(ROUTER *instance, DCB *dcb);
static  uint8_t getCapabilities (ROUTER* inst, void* router_session);


static ROUTER_OBJECT MyObject = {
    createInstance,
    newSession,
    closeSession,
    freeSession,
    routeQuery,
    diagnostic,
    NULL,
    NULL,
    getCapabilities
};

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
ROUTER_OBJECT *
GetModuleObject()
{
	return &MyObject;
}

/**
 * Create an instance of the router for a particular service
 * within the gateway.
 * 
 * @param service	The service this router is being create for
 * @param options	The options for this query router
 *
 * @return The instance data for this new instance
 */
static	ROUTER	*
createInstance(SERVICE *service, char **options)
{
	static int i = 0;
	ROUTER *inst = &i;
	/* Once off processing at the beginning */
	return (ROUTER *)inst;
}

/**
 * Associate a new session with this instance of the router.
 *
 * @param instance	The router instance data
 * @param session	The session itself
 * @return Session specific data for this session
 */
static	void	*
newSession(ROUTER *instance, SESSION *session)
{
static int i=0, *inst = &i;
	/* Every time a user connects to the service */
	int argc = 2;
	char *argv[] = {"maxapi.php", "hello"};
	
        php_embed_init(argc, argv PTSRMLS_CC);
        zend_startup_module(&php_mymod_module_entry);
	session->state = SESSION_STATE_READY;
	dcb_printf(session->client, "Welcome the SkySQL MaxScale API Interface (%s).\n",
		version_str);
	return (void *)inst;
}

/**
 * Close a session with the router, this is the mechanism
 * by which a router may cleanup data structure etc.
 *
 * @param instance	The router instance data
 * @param session	The session being closed
 */
static	void 	
closeSession(ROUTER *instance, void *session)
{
	/* When a user session comes to an end */
}

static void freeSession(
        ROUTER* router_instance,
        void*   router_client_session)
{
	/* Called after all components of the session have been closed */
        php_embed_shutdown(TSRMLS_C);
        return;
}

static	int	
routeQuery(ROUTER *instance, void *session, GWBUF *queue)
{
	/* This happens for every request */
	/* GWBUF passes data around - a buffer with start and end point - is a string */

	char cmdbuf[80];
	char *filename;
	
	/* Extract the characters */
	while (queue)
	{
		strncat(cmdbuf, GWBUF_DATA(queue), GWBUF_LENGTH(queue));
		queue = gwbuf_consume(queue, GWBUF_LENGTH(queue));
	}

	/* filename = strcat(strcat(getenv("MAXSCALE_HOME"), "/api/"), argv[0]); */
	/* filename = strcat(strcat(getenv("MAXSCALE_HOME"), "/api/"), "maxapi.php"); */
 
        zend_first_try {
		char *include_script;
		spprintf(&include_script, 0, "$argv[1] = '%s'; include '%s';", cmdbuf, "/home/mbrampton/MaxScaleHome/api/maxapi.php");
		zend_eval_string(include_script, NULL, "/home/mbrampton/MaxScaleHome/api/maxapi.php" TSRMLS_CC);
		efree(include_script);
        } zend_catch {
            int exit_status = EG(exit_status);
            return exit_status;
        } zend_end_try();
	return 0;
}

/**
 * Diagnostics routine
 *
 * @param	instance	The router instance
 * @param	dcb		The DCB for diagnostic output
 */
static	void
diagnostic(ROUTER *instance, DCB *dcb)
{
	/* Called when user asks to show service */
}

static uint8_t getCapabilities(
        ROUTER*  inst,
        void*    router_session)
{
	/* Not relevant for API */
        return 0;
}
