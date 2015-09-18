/*
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
 * Copyright MariaDB Corporation Ab 2014-2015
 */

/**
 * @file blr_slave.c - contains code for the router to slave communication
 *
 * The binlog router is designed to be used in replication environments to
 * increase the replication fanout of a master server. It provides a transparant
 * mechanism to read the binlog entries for multiple slaves while requiring
 * only a single connection to the actual master to support the slaves.
 *
 * The current prototype implement is designed to support MySQL 5.6 and has
 * a number of limitations. This prototype is merely a proof of concept and
 * should not be considered production ready.
 *
 * @verbatim
 * Revision History
 *
 * Date		Who			Description
 * 14/04/2014	Mark Riddoch		Initial implementation
 * 18/02/2015	Massimiliano Pinto	Addition of DISCONNECT ALL and DISCONNECT SERVER server_id
 * 18/03/2015	Markus Makela		Better detection of CRC32 | NONE  checksum
 * 19/03/2015	Massimiliano Pinto	Addition of basic MariaDB 10 compatibility support
 * 07/05/2015   Massimiliano Pinto	Added MariaDB 10 Compatibility
 * 11/05/2015   Massimiliano Pinto	Only MariaDB 10 Slaves can register to binlog router with a MariaDB 10 Master
 * 25/05/2015	Massimiliano Pinto	Addition of BLRM_SLAVE_STOPPED state and blr_start/stop_slave.
 *					New commands STOP SLAVE, START SLAVE added.	
 * 29/05/2015	Massimiliano Pinto	Addition of CHANGE MASTER TO ...
 * 05/06/2015	Massimiliano Pinto	router->service->dbref->sever->name instead of master->remote
 *					in blr_slave_send_slave_status()
 * 08/06/2015	Massimiliano Pinto	blr_slave_send_slave_status() shows mysql_errno and error_msg
 * 15/06/2015	Massimiliano Pinto	Added constraints to CHANGE MASTER TO MASTER_LOG_FILE/POS
 * 23/06/2015	Massimiliano Pinto	Added utility routines for blr_handle_change_master
 *					Call create/use binlog in blr_start_slave() (START SLAVE)
 * 29/06/2015	Massimiliano Pinto	Successfully CHANGE MASTER results in updating master.ini
 *					in blr_handle_change_master()
 * 20/08/2015	Massimiliano Pinto	Added parsing and validation for CHANGE MASTER TO
 * 21/08/2015	Massimiliano Pinto	Added support for new config options:
 *					master_uuid, master_hostname, master_version
 *					If set those values are sent to slaves instead of
 *					saved master responses
 * 03/09/2015	Massimiliano Pinto	Added support for SHOW [GLOBAL] VARIABLES LIKE
 * 04/09/2015	Massimiliano Pinto	Added support for SHOW WARNINGS
 * 15/09/2015	Massimiliano Pinto	Added support for SHOW [GLOBAL] STATUS LIKE 'Uptime'
 * 25/09/2015	Massimiliano Pinto	Addition of slave heartbeat:
 *					the period set during registration is checked
 *					and heartbeat event might be sent to the affected slave.
 * 25/09/2015   Martin Brampton         Block callback processing when no router session in the DCB
 * 23/10/15	Markus Makela		Added current_safe_event
 *
 * @endverbatim
 */

#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <service.h>
#include <server.h>
#include <router.h>
#include <atomic.h>
#include <spinlock.h>
#include <blr.h>
#include <dcb.h>
#include <spinlock.h>
#include <housekeeper.h>
#include <sys/stat.h>
#include <skygw_types.h>
#include <skygw_utils.h>
#include <log_manager.h>
#include <version.h>

static void encode_value(unsigned char *data, unsigned int value, int len);
static int blr_slave_query(ROUTER_INSTANCE *router, ROUTER_SLAVE *slave, GWBUF *queue);
static int blr_slave_replay(ROUTER_INSTANCE *router, ROUTER_SLAVE *slave, GWBUF *master);
static void blr_slave_send_error(ROUTER_INSTANCE *router, ROUTER_SLAVE *slave, char  *msg);
static int blr_slave_send_timestamp(ROUTER_INSTANCE *router, ROUTER_SLAVE *slave);
static int blr_slave_register(ROUTER_INSTANCE *router, ROUTER_SLAVE *slave, GWBUF *queue);
static int blr_slave_binlog_dump(ROUTER_INSTANCE *router, ROUTER_SLAVE *slave, GWBUF *queue);
int blr_slave_catchup(ROUTER_INSTANCE *router, ROUTER_SLAVE *slave, bool large);
uint8_t *blr_build_header(GWBUF	*pkt, REP_HEADER *hdr);
int blr_slave_callback(DCB *dcb, DCB_REASON reason, void *data);
static int blr_slave_fake_rotate(ROUTER_INSTANCE *router, ROUTER_SLAVE *slave);
static void blr_slave_send_fde(ROUTER_INSTANCE *router, ROUTER_SLAVE *slave);
static int blr_slave_send_maxscale_version(ROUTER_INSTANCE *router, ROUTER_SLAVE *slave);
static int blr_slave_send_server_id(ROUTER_INSTANCE *router, ROUTER_SLAVE *slave);
static int blr_slave_send_maxscale_variables(ROUTER_INSTANCE *router, ROUTER_SLAVE *slave);
static int blr_slave_send_master_status(ROUTER_INSTANCE *router, ROUTER_SLAVE *slave);
static int blr_slave_send_slave_status(ROUTER_INSTANCE *router, ROUTER_SLAVE *slave);
static int blr_slave_send_slave_hosts(ROUTER_INSTANCE *router, ROUTER_SLAVE *slave);
static int blr_slave_send_fieldcount(ROUTER_INSTANCE *router, ROUTER_SLAVE *slave, int count);
static int blr_slave_send_columndef(ROUTER_INSTANCE *router, ROUTER_SLAVE *slave, char *name, int type, int len, uint8_t seqno);
static int blr_slave_send_eof(ROUTER_INSTANCE *router, ROUTER_SLAVE *slave, int seqno);
static int blr_slave_send_disconnected_server(ROUTER_INSTANCE *router, ROUTER_SLAVE *slave, int server_id, int found);
static int blr_slave_disconnect_all(ROUTER_INSTANCE *router, ROUTER_SLAVE *slave);
static int blr_slave_disconnect_server(ROUTER_INSTANCE *router, ROUTER_SLAVE *slave, int server_id);
static int blr_slave_send_ok(ROUTER_INSTANCE* router, ROUTER_SLAVE* slave);
void poll_fake_write_event(DCB *dcb);

extern int lm_enabled_logfiles_bitmask;
extern size_t         log_ses_count[];
extern __thread log_info_t tls_log_info;

/**
 * Process a request packet from the slave server.
 *
 * The router can handle a limited subset of requests from the slave, these
 * include a subset of general SQL queries, a slave registeration command and
 * the binlog dump command.
 *
 * The strategy for responding to these commands is to use caches responses
 * for the the same commands that have previously been made to the real master
 * if this is possible, if it is not then the router itself will synthesize a
 * response.
 *
 * @param router	The router instance this defines the master for this replication chain
 * @param slave		The slave specific data
 * @param queue		The incoming request packet
 */
int
blr_slave_request(ROUTER_INSTANCE *router, ROUTER_SLAVE *slave, GWBUF *queue)
{
	if (slave->state < 0 || slave->state > BLRS_MAXSTATE)
	{
        	MXS_ERROR("Invalid slave state machine state (%d) for binlog router.",
                          slave->state);
		gwbuf_consume(queue, gwbuf_length(queue));
		return 0;
	}

	slave->stats.n_requests++;
	switch (MYSQL_COMMAND(queue))
	{
	case COM_QUERY:
		slave->stats.n_queries++;
		return blr_slave_query(router, slave, queue);
		break;
	case COM_REGISTER_SLAVE:
		if (router->master_state == BLRM_UNCONFIGURED) {
			slave->state = BLRS_ERRORED;
			blr_slave_send_error_packet(slave,
				"Binlog router is not yet configured for replication", (unsigned int) 1597, NULL);

			MXS_ERROR("%s: Slave %s: Binlog router is not yet configured for replication",
                                  router->service->name,
                                  slave->dcb->remote);
			dcb_close(slave->dcb);
			return 1;
		}

		/*
		 * If Master is MariaDB10 don't allow registration from
		 * MariaDB/Mysql 5 Slaves
		 */

		if (router->mariadb10_compat && !slave->mariadb10_compat) {
			slave->state = BLRS_ERRORED;
			blr_send_custom_error(slave->dcb, 1, 0,
				"MariaDB 10 Slave is required for Slave registration", "42000", 1064);

			MXS_ERROR("%s: Slave %s: a MariaDB 10 Slave is required for Slave registration",
                                  router->service->name,
                                  slave->dcb->remote);

			dcb_close(slave->dcb);
			return 1;
		} else {
			/* Master and Slave version OK: continue with slave registration */
			return blr_slave_register(router, slave, queue);
		}
		break;
	case COM_BINLOG_DUMP:
		{
		char task_name[BLRM_TASK_NAME_LEN+1]="";
		int rc = 0;

		rc = blr_slave_binlog_dump(router, slave, queue);

		if (router->send_slave_heartbeat && rc && slave->heartbeat > 0) {
			snprintf(task_name, BLRM_TASK_NAME_LEN, "%s slaves heartbeat send", router->service->name);

			/* Add slave heartbeat check task: it runs with 1 second frequency */
			hktask_add(task_name, blr_send_slave_heartbeat, router, 1);
		}

		return rc;
		break;
		}
	case COM_STATISTICS:
		return blr_statistics(router, slave, queue);
		break;
	case COM_PING:
		return blr_ping(router, slave, queue);
		break;
	case COM_QUIT:
		MXS_DEBUG("COM_QUIT received from slave with server_id %d",
                          slave->serverid);
		break;
	default:
		blr_send_custom_error(slave->dcb, 1, 0,
			"You have an error in your SQL syntax; Check the syntax the MaxScale binlog router accepts.",
			"42000", 1064);
        	MXS_ERROR("Unexpected MySQL Command (%d) received from slave",
                          MYSQL_COMMAND(queue));
		break;
	}
	return 0;
}

/**
 * Handle a query from the slave. This is expected to be one of the "standard"
 * queries we expect as part of the registraton process. Most of these can
 * be dealt with by replying the stored responses we got from the master
 * when MaxScale registered as a slave. The exception to the rule is the
 * request to obtain the current timestamp value of the server.
 *
 * The original set added for the registration process has been enhanced in
 * order to support some commands that are useful for monitoring the binlog
 * router.
 *
 * Twelve select statements are currently supported:
 *	SELECT UNIX_TIMESTAMP();
 *	SELECT @master_binlog_checksum
 *	SELECT @@GLOBAL.GTID_MODE
 *	SELECT VERSION()
 *	SELECT 1
 *	SELECT @@version_comment limit 1
 *	SELECT @@hostname
 *	SELECT @@max_allowed_packet
 *	SELECT @@maxscale_version
 *	SELECT @@[GLOBAL.]server_id
 *	SELECT @@version
 *	SELECT @@[GLOBAL.]server_uuid
 *
 * Eight show commands are supported:
 *	SHOW [GLOBAL] VARIABLES LIKE 'SERVER_ID'
 *	SHOW [GLOBAL] VARIABLES LIKE 'SERVER_UUID'
 *	SHOW [GLOBAL] VARIABLES LIKE 'MAXSCALE%'
 *	SHOW SLAVE STATUS
 *	SHOW MASTER STATUS
 *	SHOW SLAVE HOSTS
 *	SHOW WARNINGS
 *	SHOW [GLOBAL] STATUS LIKE 'Uptime'
 *
 * Six set commands are supported:
 *	SET @master_binlog_checksum = @@global.binlog_checksum
 *	SET @master_heartbeat_period=...
 *	SET @slave_slave_uuid=...
 *	SET NAMES latin1
 *	SET NAMES utf8
 *	SET mariadb_slave_capability=...
 *
 * Four administrative commands are supported:
 *	STOP SLAVE
 *	START SLAVE
 *	CHANGE MASTER TO
 *	RESET SLAVE
 *
 * @param router        The router instance this defines the master for this replication chain
 * @param slave         The slave specific data
 * @param queue         The incoming request packet
 * @return		Non-zero if data has been sent
 */
static int
blr_slave_query(ROUTER_INSTANCE *router, ROUTER_SLAVE *slave, GWBUF *queue)
{
char	*qtext, *query_text;
char	*sep = " 	,=";
char	*word, *brkb;
int	query_len;
char    *ptr;
extern  char *strcasestr();

	qtext = GWBUF_DATA(queue);
	query_len = extract_field((uint8_t *)qtext, 24) - 1;
	qtext += 5;		// Skip header and first byte of the payload
	query_text = strndup(qtext, query_len);

	/* Don't log the full statement containg 'password', just trucate it */
	ptr = strcasestr(query_text, "password");
	if (ptr != NULL) {
		char *new_text = strdup(query_text);
		int trucate_at  = (ptr - query_text);
		if (trucate_at > 0) {
			if ( (trucate_at + 3) <= strlen(new_text)) {
				int i;
				for (i = 0; i < 3; i++) {
					new_text[trucate_at + i] = '.';
				}
				new_text[trucate_at+3] = '\0';
			} else {
				new_text[trucate_at] = '\0';
			}
		}

		MXS_INFO("Execute statement (truncated, it contains password)"
                         " from the slave '%s'", new_text);
		free(new_text);
	} else {
		MXS_INFO("Execute statement from the slave '%s'", query_text);
	}

	/*
	 * Implement a very rudimental "parsing" of the query text by extarcting the
	 * words from the statement and matchng them against the subset of queries we
	 * are expecting from the slave. We already have responses to these commands,
	 * except for the select of UNIX_TIMESTAMP(), that we have saved from MaxScale's
	 * own interaction with the real master. We simply replay these saved responses
	 * to the slave.
	 */
	if ((word = strtok_r(query_text, sep, &brkb)) == NULL)
	{
	
		MXS_ERROR("%s: Incomplete query.", router->service->name);
	}
	else if (strcasecmp(word, "SELECT") == 0)
	{
		if ((word = strtok_r(NULL, sep, &brkb)) == NULL)
		{
			MXS_ERROR("%s: Incomplete select query.",
                                  router->service->name);
		}
		else if (strcasecmp(word, "UNIX_TIMESTAMP()") == 0)
		{
			free(query_text);
			return blr_slave_send_timestamp(router, slave);
		}
		else if (strcasecmp(word, "@master_binlog_checksum") == 0)
		{
			free(query_text);
			return blr_slave_replay(router, slave, router->saved_master.chksum2);
		}
		else if (strcasecmp(word, "@@GLOBAL.GTID_MODE") == 0)
		{
			free(query_text);
			return blr_slave_replay(router, slave, router->saved_master.gtid_mode);
		}
		else if (strcasecmp(word, "1") == 0)
		{
			free(query_text);
			return blr_slave_replay(router, slave, router->saved_master.select1);
		}
		else if (strcasecmp(word, "VERSION()") == 0)
		{
			free(query_text);
			if (router->set_master_version)
				return blr_slave_send_var_value(router, slave, "VERSION()", router->set_master_version, BLR_TYPE_STRING);
			else
				return blr_slave_replay(router, slave, router->saved_master.selectver);
		}
		else if (strcasecmp(word, "@@version") == 0)
		{
			free(query_text);
			if (router->set_master_version)
				return blr_slave_send_var_value(router, slave, "@@version", router->set_master_version, BLR_TYPE_STRING);
			else {
				char *version = blr_extract_column(router->saved_master.selectver, 1);
				
				blr_slave_send_var_value(router, slave, "@@version", version == NULL ? "" : version, BLR_TYPE_STRING);
				free(version);
				return 1;
			}
		}
		else if (strcasecmp(word, "@@version_comment") == 0)
		{
			free(query_text);
			if (!router->saved_master.selectvercom)
				/* This will allow mysql client to get in when @@version_comment is not available */
				return blr_slave_send_ok(router, slave);
			else
				return blr_slave_replay(router, slave, router->saved_master.selectvercom);
		}
		else if (strcasecmp(word, "@@hostname") == 0)
		{
			free(query_text);
			if (router->set_master_hostname)
				return blr_slave_send_var_value(router, slave, "@@hostname", router->set_master_hostname, BLR_TYPE_STRING);
			else
				return blr_slave_replay(router, slave, router->saved_master.selecthostname);
		}
		else if ((strcasecmp(word, "@@server_uuid") == 0) || (strcasecmp(word, "@@global.server_uuid") == 0))
		{
			char	heading[40]; /* to ensure we match the case in query and response */
			strcpy(heading, word);

			free(query_text);
			if (router->set_master_uuid)
				return blr_slave_send_var_value(router, slave, heading, router->master_uuid, BLR_TYPE_STRING);
			else {
				char *master_uuid = blr_extract_column(router->saved_master.uuid, 2);
				blr_slave_send_var_value(router, slave, heading, master_uuid == NULL ? "" : master_uuid, BLR_TYPE_STRING);
				free(master_uuid);
				return 1;
			}
		}
		else if (strcasecmp(word, "@@max_allowed_packet") == 0)
		{
			free(query_text);
			return blr_slave_replay(router, slave, router->saved_master.map);
		}
		else if (strcasecmp(word, "@@maxscale_version") == 0)
		{
			free(query_text);
			return blr_slave_send_maxscale_version(router, slave);
		}
		else if ((strcasecmp(word, "@@server_id") == 0) || (strcasecmp(word, "@@global.server_id") == 0))
		{
			char    server_id[40];
			char	heading[40]; /* to ensure we match the case in query and response */

			sprintf(server_id, "%d", router->masterid);
			strcpy(heading, word);

			free(query_text);

			return blr_slave_send_var_value(router, slave, heading, server_id, BLR_TYPE_INT);
		}
	}
	else if (strcasecmp(word, "SHOW") == 0)
	{
		if ((word = strtok_r(NULL, sep, &brkb)) == NULL)
		{
			MXS_ERROR("%s: Incomplete show query.",
                                  router->service->name);
		}
		else if (strcasecmp(word, "WARNINGS") == 0)
		{
			free(query_text);
			return blr_slave_show_warnings(router, slave);
		}
		else if (strcasecmp(word, "GLOBAL") == 0)
		{
			if (router->master_state == BLRM_UNCONFIGURED) {
				free(query_text);
				return blr_slave_send_ok(router, slave);
			}

			if ((word = strtok_r(NULL, sep, &brkb)) == NULL)
			{
				MXS_ERROR("%s: Expected VARIABLES in SHOW GLOBAL",
                                          router->service->name);
			}
			else if (strcasecmp(word, "VARIABLES") == 0)
			{
				int rc = blr_slave_handle_variables(router, slave, brkb);

				/* if no var found, send empty result set */
				if (rc == 0)
					blr_slave_send_ok(router, slave);

				if (rc >= 0) {
					free(query_text);

					return 1;
				} else
					MXS_ERROR("%s: Expected LIKE clause in SHOW GLOBAL VARIABLES.",
                                                  router->service->name);
			}
			else if (strcasecmp(word, "STATUS") == 0)
			{
				int rc = blr_slave_handle_status_variables(router, slave, brkb);

				/* if no var found, send empty result set */
				if (rc == 0)
					blr_slave_send_ok(router, slave);

				if (rc >= 0) {
					free(query_text);

					return 1;
				} else
					MXS_ERROR("%s: Expected LIKE clause in SHOW GLOBAL STATUS.",
                                                  router->service->name);
			}
		}
		else if (strcasecmp(word, "VARIABLES") == 0)
		{
			int rc;
			if (router->master_state == BLRM_UNCONFIGURED) {
				free(query_text);
				return blr_slave_send_ok(router, slave);
			}

			rc = blr_slave_handle_variables(router, slave, brkb);

			/* if no var found, send empty result set */
			if (rc == 0)
				blr_slave_send_ok(router, slave);

			if (rc >= 0) {
				free(query_text);

				return 1;
			} else
				MXS_ERROR("%s: Expected LIKE clause in SHOW VARIABLES.",
                                          router->service->name);
		}
		else if (strcasecmp(word, "MASTER") == 0)
		{
			if ((word = strtok_r(NULL, sep, &brkb)) == NULL)
			{
				MXS_ERROR("%s: Expected SHOW MASTER STATUS command",
                                          router->service->name);
			}
			else if (strcasecmp(word, "STATUS") == 0)
			{
				free(query_text);

				/* if state is BLRM_UNCONFIGURED return empty result */

				if (router->master_state > BLRM_UNCONFIGURED)
					return blr_slave_send_master_status(router, slave);
				else
					return blr_slave_send_ok(router, slave);	
			}
		}
		else if (strcasecmp(word, "SLAVE") == 0)
		{
			if ((word = strtok_r(NULL, sep, &brkb)) == NULL)
			{
				MXS_ERROR("%s: Expected SHOW SLAVE STATUS command",
                                          router->service->name);
			}
			else if (strcasecmp(word, "STATUS") == 0)
			{
				free(query_text);
				/* if state is BLRM_UNCONFIGURED return empty result */
				if (router->master_state > BLRM_UNCONFIGURED)
					return blr_slave_send_slave_status(router, slave);
				else
					return blr_slave_send_ok(router, slave);	
			}
			else if (strcasecmp(word, "HOSTS") == 0)
			{
				free(query_text);
				/* if state is BLRM_UNCONFIGURED return empty result */
				if (router->master_state > BLRM_UNCONFIGURED)
					return blr_slave_send_slave_hosts(router, slave);
				else
					return blr_slave_send_ok(router, slave);	
			}
		}
		else if (strcasecmp(word, "STATUS") == 0)
		{
			int rc = blr_slave_handle_status_variables(router, slave, brkb);

			/* if no var found, send empty result set */
			if (rc == 0)
				blr_slave_send_ok(router, slave);

			if (rc >= 0) {
				free(query_text);

				return 1;
			} else
				MXS_ERROR("%s: Expected LIKE clause in SHOW STATUS.",
                                          router->service->name);
		}
	}
	else if (strcasecmp(query_text, "SET") == 0)
	{
		if ((word = strtok_r(NULL, sep, &brkb)) == NULL)
		{
			MXS_ERROR("%s: Incomplete set command.",
                                  router->service->name);
		}
		else if (strcasecmp(word, "@master_heartbeat_period") == 0)
		{
			int slave_heartbeat;
			int v_len = 0;
			word = strtok_r(NULL, sep, &brkb);
			if (word) {
				char *new_val;
				v_len = strlen(word);
				if (v_len > 6) {
					new_val = strndup(word, v_len - 6);	
					slave->heartbeat = atoi(new_val) / 1000;
				} else {
					new_val = strndup(word, v_len);
					slave->heartbeat = atoi(new_val) / 1000000;
				}

				free(new_val);
			}
			free(query_text);
			return blr_slave_replay(router, slave, router->saved_master.heartbeat);
		}
		else if (strcasecmp(word, "@mariadb_slave_capability") == 0)
                {
			/* mariadb10 compatibility is set for the slave */
			slave->mariadb10_compat=true;

                       	free(query_text);
			if (router->mariadb10_compat) {
				return blr_slave_replay(router, slave, router->saved_master.mariadb10);
			} else {
				return blr_slave_send_ok(router, slave);
			}
                }
		else if (strcasecmp(word, "@master_binlog_checksum") == 0)
		{
			word = strtok_r(NULL, sep, &brkb);
			if (word && (strcasecmp(word, "'none'") == 0))
				slave->nocrc = 1;
			else if (word && (strcasecmp(word, "@@global.binlog_checksum") == 0))
				slave->nocrc = !router->master_chksum;
			else
				slave->nocrc = 0;

			
			free(query_text);
			return blr_slave_replay(router, slave, router->saved_master.chksum1);
		}
		else if (strcasecmp(word, "@slave_uuid") == 0)
		{
			if ((word = strtok_r(NULL, sep, &brkb)) != NULL) {
				int len = strlen(word);
				char *word_ptr = word;
				if (len) {
					if (word[len-1] == '\'')
						word[len-1] = '\0';
					if (word[0] == '\'') {
						word[0] = '\0';
						word_ptr++;
					}
				}
				slave->uuid = strdup(word_ptr);
			}
			free(query_text);
			return blr_slave_replay(router, slave, router->saved_master.setslaveuuid);
		}
		else if (strcasecmp(word, "NAMES") == 0)
		{
			if ((word = strtok_r(NULL, sep, &brkb)) == NULL)
			{
				MXS_ERROR("%s: Truncated SET NAMES command.",
                                          router->service->name);
			}
			else if (strcasecmp(word, "latin1") == 0)
			{
				free(query_text);
				return blr_slave_replay(router, slave, router->saved_master.setnames);
			}
			else if (strcasecmp(word, "utf8") == 0)
			{
				free(query_text);
				return blr_slave_replay(router, slave, router->saved_master.utf8);
			}
		}
	} /* RESET current configured master */
        else if (strcasecmp(query_text, "RESET") == 0)
	{
		if ((word = strtok_r(NULL, sep, &brkb)) == NULL)
		{
			MXS_ERROR("%s: Incomplete RESET command.",
                                  router->service->name);
		}
		else if (strcasecmp(word, "SLAVE") == 0)
		{
			free(query_text);

			if (router->master_state == BLRM_SLAVE_STOPPED) {
				char path[PATH_MAX + 1] = "";
				char error_string[BINLOG_ERROR_MSG_LEN + 1] = "";
				MASTER_SERVER_CFG *current_master = NULL;
				int removed_cfg = 0;

				/* save current replication parameters */
				current_master = (MASTER_SERVER_CFG *)calloc(1, sizeof(MASTER_SERVER_CFG));

				if (!current_master) {
					snprintf(error_string, BINLOG_ERROR_MSG_LEN, "error allocating memory for blr_master_get_config");
					MXS_ERROR("%s: %s", router->service->name, error_string);
					blr_slave_send_error_packet(slave, error_string, (unsigned int)1201, NULL);

					return 1;
				}

				/* get current data */
				blr_master_get_config(router, current_master);

				MXS_NOTICE("%s: 'RESET SLAVE executed'. Previous state MASTER_HOST='%s', "
                                           "MASTER_PORT=%i, MASTER_LOG_FILE='%s', MASTER_LOG_POS=%lu, "
                                           "MASTER_USER='%s'",
                                           router->service->name,
                                           current_master->host,
                                           current_master->port,
                                           current_master->logfile,
                                           current_master->pos,
                                           current_master->user);

				/* remove master.ini */
				strncpy(path, router->binlogdir, PATH_MAX);

				strncat(path,"/master.ini", PATH_MAX);

				/* remove master.ini */
				removed_cfg = unlink(path);

				if (removed_cfg == -1) {
					char err_msg[STRERROR_BUFLEN];
					snprintf(error_string, BINLOG_ERROR_MSG_LEN, "Error removing %s, %s, errno %u", path, strerror_r(errno, err_msg, sizeof(err_msg)), errno);
					MXS_ERROR("%s: %s", router->service->name, error_string);
				}

				spinlock_acquire(&router->lock);

				router->master_state = BLRM_UNCONFIGURED;
				blr_master_set_empty_config(router);
				blr_master_free_config(current_master);

				spinlock_release(&router->lock);

				if (removed_cfg == -1) {
					blr_slave_send_error_packet(slave, error_string, (unsigned int)1201, NULL);
					return 1;
				} else {
					return blr_slave_send_ok(router, slave);
				}
			} else {
				if (router->master_state == BLRM_UNCONFIGURED)
					blr_slave_send_ok(router, slave);
				else
					blr_slave_send_error_packet(slave, "This operation cannot be performed with a running slave; run STOP SLAVE first", (unsigned int)1198, NULL);
				return 1;
			}
		}
	}
	/* start replication from the current configured master */
	else if (strcasecmp(query_text, "START") == 0)
	{
		if ((word = strtok_r(NULL, sep, &brkb)) == NULL)
		{
			MXS_ERROR("%s: Incomplete START command.",
                                  router->service->name);
		}
		else if (strcasecmp(word, "SLAVE") == 0)
		{
			free(query_text);
			return blr_start_slave(router, slave);
		}
	}
	/* stop replication from the current master*/
	else if (strcasecmp(query_text, "STOP") == 0)
	{
		if ((word = strtok_r(NULL, sep, &brkb)) == NULL)
		{
			MXS_ERROR("%s: Incomplete STOP command.", router->service->name);
		}
		else if (strcasecmp(word, "SLAVE") == 0)
		{
			free(query_text);
			return blr_stop_slave(router, slave);
		}
	}
	/* Change the server to replicate from */
	else if (strcasecmp(query_text, "CHANGE") == 0)
	{
		if ((word = strtok_r(NULL, sep, &brkb)) == NULL)
		{
			MXS_ERROR("%s: Incomplete CHANGE command.", router->service->name);
		}
		else if (strcasecmp(word, "MASTER") == 0)
		{
			if (router->master_state != BLRM_SLAVE_STOPPED && router->master_state != BLRM_UNCONFIGURED)
			{
				free(query_text);
				blr_slave_send_error_packet(slave, "Cannot change master with a running slave; run STOP SLAVE first", (unsigned int)1198, NULL);
				return 1;
			}
			else
			{
				int rc;
				char error_string[BINLOG_ERROR_MSG_LEN + 1] = "";
				MASTER_SERVER_CFG *current_master = NULL;

				current_master = (MASTER_SERVER_CFG *)calloc(1, sizeof(MASTER_SERVER_CFG));

				if (!current_master) {
					free(query_text);
					strcpy(error_string, "Error allocating memory for blr_master_get_config");
					MXS_ERROR("%s: %s", router->service->name, error_string);

					blr_slave_send_error_packet(slave, error_string, (unsigned int)1201, NULL);

					return 1;
				}

				blr_master_get_config(router, current_master);

				rc = blr_handle_change_master(router, brkb, error_string);

				free(query_text);

				if (rc < 0) {
					/* CHANGE MASTER TO has failed */
					blr_slave_send_error_packet(slave, error_string, (unsigned int)1234, "42000");
					blr_master_free_config(current_master);

					return 1;
				} else {
					int ret;
					char error[BINLOG_ERROR_MSG_LEN + 1];

					/* Write/Update master config into master.ini file */
					ret = blr_file_write_master_config(router, error);

                                        if (ret) {
						/* file operation failure: restore config */
						spinlock_acquire(&router->lock);

						blr_master_apply_config(router, current_master);
						blr_master_free_config(current_master);

						spinlock_release(&router->lock);

						snprintf(error_string, BINLOG_ERROR_MSG_LEN, "Error writing into %s/master.ini: %s", router->binlogdir, error);
						MXS_ERROR("%s: %s",
                                                          router->service->name, error_string);

						blr_slave_send_error_packet(slave, error_string, (unsigned int)1201, NULL);

						return 1;
					}

					/**
					 * check if router is BLRM_UNCONFIGURED
					 * and change state to BLRM_SLAVE_STOPPED
					 */
					if (rc == 1 || router->master_state == BLRM_UNCONFIGURED) {
						spinlock_acquire(&router->lock);

						router->master_state = BLRM_SLAVE_STOPPED;

						spinlock_release(&router->lock);
					}

					if (!router->trx_safe)
						blr_master_free_config(current_master);

					if (router->trx_safe && router->pending_transaction) {
						if (strcmp(router->binlog_name, router->prevbinlog) != 0)
						{
							char message[BINLOG_ERROR_MSG_LEN+1] = "";
							snprintf(message, BINLOG_ERROR_MSG_LEN, "1105:Partial transaction in file %s starting at pos %lu, ending at pos %lu will be lost with next START SLAVE command", current_master->logfile, current_master->safe_pos, current_master->pos);
							blr_master_free_config(current_master);

							return blr_slave_send_warning_message(router, slave, message);
						} else {
							blr_master_free_config(current_master);
							return blr_slave_send_ok(router, slave);
						}

					} else {
						return blr_slave_send_ok(router, slave);
					}
				}
			}
		}
	}
	else if (strcasecmp(query_text, "DISCONNECT") == 0)
	{
		if ((word = strtok_r(NULL, sep, &brkb)) == NULL)
		{
			MXS_ERROR("%s: Incomplete DISCONNECT command.",
                                  router->service->name);
		}
		else if (strcasecmp(word, "ALL") == 0)
		{
			free(query_text);
			return blr_slave_disconnect_all(router, slave);
		}
		else if (strcasecmp(word, "SERVER") == 0)
		{
			if ((word = strtok_r(NULL, sep, &brkb)) == NULL)
			{
				MXS_ERROR("%s: Expected DISCONNECT SERVER $server_id",
                                          router->service->name);
			} else {
				int serverid = atoi(word);
				free(query_text);
				return blr_slave_disconnect_server(router, slave, serverid);
			}
		}
	}

	free(query_text);

	query_text = strndup(qtext, query_len);
	MXS_ERROR("Unexpected query from '%s'@'%s': %s", slave->dcb->user, slave->dcb->remote, query_text);
	free(query_text);
	blr_slave_send_error(router, slave, "You have an error in your SQL syntax; Check the syntax the MaxScale binlog router accepts.");
	return 1;
}


/**
 * Send a reply to a command we have received from the slave. The reply itself
 * is merely a copy of a previous message we received from the master when we
 * registered as a slave. Hence we just replay this saved reply.
 *
 * @param	router		The binlog router instance
 * @param	slave		The slave server to which we are sending the response
 * @param	master		The saved master response
 * @return	Non-zero if data was sent
 */
static int
blr_slave_replay(ROUTER_INSTANCE *router, ROUTER_SLAVE *slave, GWBUF *master)
{
GWBUF	*clone;

	if (router->master_state == BLRM_UNCONFIGURED)
		return blr_slave_send_ok(router, slave);

	if (!master)
		return 0;

	if ((clone = gwbuf_clone(master)) != NULL)
	{
		return slave->dcb->func.write(slave->dcb, clone);
	}
	else
	{
		MXS_ERROR("Failed to clone server response to send to slave.");
		return 0;
	}
}

/**
 * Construct an error response
 *
 * @param router	The router instance
 * @param slave		The slave server instance
 * @param msg		The error message to send
 */
static void
blr_slave_send_error(ROUTER_INSTANCE *router, ROUTER_SLAVE *slave, char  *msg)
{
GWBUF		*pkt;
unsigned char   *data;
int             len;

        if ((pkt = gwbuf_alloc(strlen(msg) + 13)) == NULL)
                return;
        data = GWBUF_DATA(pkt);
        len = strlen(msg) + 9;
        encode_value(&data[0], len, 24);	// Payload length
        data[3] = 1;				// Sequence id
						// Payload
        data[4] = 0xff;				// Error indicator
	encode_value(&data[5], 1064, 16);// Error Code
	strncpy((char *)&data[7], "#42000", 6);
        memcpy(&data[13], msg, strlen(msg));	// Error Message
	slave->dcb->func.write(slave->dcb, pkt);
}

/*
 * Some standard packets that have been captured from a network trace of server
 * interactions. These packets are the schema definition sent in response to
 * a SELECT UNIX_TIMESTAMP() statement and the EOF packet that marks the end
 * of transmission of the result set.
 */
static uint8_t timestamp_def[] = {
 0x01, 0x00, 0x00, 0x01, 0x01, 0x26, 0x00, 0x00, 0x02, 0x03, 0x64, 0x65, 0x66, 0x00, 0x00, 0x00,
 0x10, 0x55, 0x4e, 0x49, 0x58, 0x5f, 0x54, 0x49, 0x4d, 0x45, 0x53, 0x54, 0x41, 0x4d, 0x50, 0x28,
 0x29, 0x00, 0x0c, 0x3f, 0x00, 0x0b, 0x00, 0x00, 0x00, 0x08, 0x81, 0x00, 0x00, 0x00, 0x00, 0x05,
 0x00, 0x00, 0x03, 0xfe, 0x00, 0x00, 0x02, 0x00
};
static uint8_t timestamp_eof[] = { 0x05, 0x00, 0x00, 0x05, 0xfe, 0x00, 0x00, 0x02, 0x00 };

/**
 * Send a response to a "SELECT UNIX_TIMESTAMP()" request. This differs from the other
 * requests since we do not save a copy of the original interaction with the master
 * and simply replay it. We want to always send the current time. We have stored a typcial
 * response, which gives us the schema information normally returned. This is sent to the
 * client and then we add a dynamic part that will insert the current timestamp data.
 * Finally we send a preprepaed EOF packet to end the response stream.
 *
 * @param	router		The binlog router instance
 * @param	slave		The slave server to which we are sending the response
 * @return	Non-zero if data was sent
 */
static int
blr_slave_send_timestamp(ROUTER_INSTANCE *router, ROUTER_SLAVE *slave)
{
GWBUF	*pkt;
char	timestamp[20];
uint8_t *ptr;
int	len, ts_len;

	sprintf(timestamp, "%ld", time(0));
	ts_len = strlen(timestamp);
	len = sizeof(timestamp_def) + sizeof(timestamp_eof) + 5 + ts_len;
	if ((pkt = gwbuf_alloc(len)) == NULL)
		return 0;
	ptr = GWBUF_DATA(pkt);
	memcpy(ptr, timestamp_def, sizeof(timestamp_def));	// Fixed preamble
	ptr += sizeof(timestamp_def);
	encode_value(ptr, ts_len + 1, 24);			// Add length of data packet
	ptr += 3;
	*ptr++ = 0x04;						// Sequence number in response
	*ptr++ = ts_len;					// Length of result string
	strncpy((char *)ptr, timestamp, ts_len);		// Result string
	ptr += ts_len;
	memcpy(ptr, timestamp_eof, sizeof(timestamp_eof));	// EOF packet to terminate result
	return slave->dcb->func.write(slave->dcb, pkt);
}

/**
 * Send a response the the SQL command SELECT @@MAXSCALE_VERSION
 *
 * @param	router		The binlog router instance
 * @param	slave		The slave server to which we are sending the response
 * @return	Non-zero if data was sent
 */
static int
blr_slave_send_maxscale_version(ROUTER_INSTANCE *router, ROUTER_SLAVE *slave)
{
GWBUF	*pkt;
char	version[80] = "";
uint8_t *ptr;
int	len, vers_len;

	sprintf(version, "%s", MAXSCALE_VERSION);
	vers_len = strlen(version);
	blr_slave_send_fieldcount(router, slave, 1);
	blr_slave_send_columndef(router, slave, "MAXSCALE_VERSION", BLR_TYPE_STRING, vers_len, 2);
	blr_slave_send_eof(router, slave, 3);

	len = 5 + vers_len;
	if ((pkt = gwbuf_alloc(len)) == NULL)
		return 0;
	ptr = GWBUF_DATA(pkt);
	encode_value(ptr, vers_len + 1, 24);			// Add length of data packet
	ptr += 3;
	*ptr++ = 0x04;						// Sequence number in response
	*ptr++ = vers_len;					// Length of result string
	strncpy((char *)ptr, version, vers_len);		// Result string
	ptr += vers_len;
	slave->dcb->func.write(slave->dcb, pkt);
	return blr_slave_send_eof(router, slave, 5);
}

/**
 * Send a response the the SQL command SELECT @@server_id
 *
 * @param	router		The binlog router instance
 * @param	slave		The slave server to which we are sending the response
 * @return	Non-zero if data was sent
 */
static int
blr_slave_send_server_id(ROUTER_INSTANCE *router, ROUTER_SLAVE *slave)
{
GWBUF	*pkt;
char	server_id[40];
uint8_t *ptr;
int	len, id_len;

	sprintf(server_id, "%d", router->masterid);
	id_len = strlen(server_id);
	blr_slave_send_fieldcount(router, slave, 1);
	blr_slave_send_columndef(router, slave, "SERVER_ID", BLR_TYPE_INT, id_len, 2);
	blr_slave_send_eof(router, slave, 3);

	len = 5 + id_len;
	if ((pkt = gwbuf_alloc(len)) == NULL)
		return 0;
	ptr = GWBUF_DATA(pkt);
	encode_value(ptr, id_len + 1, 24);			// Add length of data packet
	ptr += 3;
	*ptr++ = 0x04;						// Sequence number in response
	*ptr++ = id_len;					// Length of result string
	strncpy((char *)ptr, server_id, id_len);		// Result string
	ptr += id_len;
	slave->dcb->func.write(slave->dcb, pkt);
	return blr_slave_send_eof(router, slave, 5);
}


/**
 * Send the response to the SQL command "SHOW VARIABLES LIKE 'MAXSCALE%'
 *
 * @param	router		The binlog router instance
 * @param	slave		The slave server to which we are sending the response
 * @return	Non-zero if data was sent
 */
static int
blr_slave_send_maxscale_variables(ROUTER_INSTANCE *router, ROUTER_SLAVE *slave)
{
GWBUF	*pkt;
char	name[40];
char	version[80];
uint8_t *ptr;
int	len, vers_len, seqno = 2;

	blr_slave_send_fieldcount(router, slave, 2);
	blr_slave_send_columndef(router, slave, "Variable_name", BLR_TYPE_STRING, 40, seqno++);
	blr_slave_send_columndef(router, slave, "Value", BLR_TYPE_STRING, 40, seqno++);
	blr_slave_send_eof(router, slave, seqno++);

	sprintf(version, "%s", MAXSCALE_VERSION);
	vers_len = strlen(version);
	strcpy(name, "MAXSCALE_VERSION");
	len = 5 + vers_len + strlen(name) + 1;
	if ((pkt = gwbuf_alloc(len)) == NULL)
		return 0;
	ptr = GWBUF_DATA(pkt);
	encode_value(ptr, vers_len + 2 + strlen(name), 24);			// Add length of data packet
	ptr += 3;
	*ptr++ = seqno++;						// Sequence number in response
	*ptr++ = strlen(name);					// Length of result string
	strncpy((char *)ptr, name, strlen(name));		// Result string
	ptr += strlen(name);
	*ptr++ = vers_len;					// Length of result string
	strncpy((char *)ptr, version, vers_len);		// Result string
	ptr += vers_len;
	slave->dcb->func.write(slave->dcb, pkt);

	return blr_slave_send_eof(router, slave, seqno++);
}


/**
 * Send the response to the SQL command "SHOW MASTER STATUS"
 *
 * @param	router		The binlog router instance
 * @param	slave		The slave server to which we are sending the response
 * @return	Non-zero if data was sent
 */
static int
blr_slave_send_master_status(ROUTER_INSTANCE *router, ROUTER_SLAVE *slave)
{
GWBUF	*pkt;
char	file[40];
char	position[40];
uint8_t *ptr;
int	len, file_len;

	blr_slave_send_fieldcount(router, slave, 5);
	blr_slave_send_columndef(router, slave, "File", BLR_TYPE_STRING, 40, 2);
	blr_slave_send_columndef(router, slave, "Position", BLR_TYPE_STRING, 40, 3);
	blr_slave_send_columndef(router, slave, "Binlog_Do_DB", BLR_TYPE_STRING, 40, 4);
	blr_slave_send_columndef(router, slave, "Binlog_Ignore_DB", BLR_TYPE_STRING, 40, 5);
	blr_slave_send_columndef(router, slave, "Execute_Gtid_Set", BLR_TYPE_STRING, 40, 6);
	blr_slave_send_eof(router, slave, 7);

	sprintf(file, "%s", router->binlog_name);
	file_len = strlen(file);

	sprintf(position, "%lu", router->binlog_position);

	len = 5 + file_len + strlen(position) + 1 + 3;
	if ((pkt = gwbuf_alloc(len)) == NULL)
		return 0;
	ptr = GWBUF_DATA(pkt);
	encode_value(ptr, len - 4, 24);			// Add length of data packet
	ptr += 3;
	*ptr++ = 0x08;						// Sequence number in response
	*ptr++ = strlen(file);					// Length of result string
	strncpy((char *)ptr, file, strlen(file));		// Result string
	ptr += strlen(file);
	*ptr++ = strlen(position);					// Length of result string
	strncpy((char *)ptr, position, strlen(position));		// Result string
	ptr += strlen(position);
	*ptr++ = 0;					// Send 3 empty values
	*ptr++ = 0;
	*ptr++ = 0;
	slave->dcb->func.write(slave->dcb, pkt);
	return blr_slave_send_eof(router, slave, 9);
}

/*
 * Columns to send for a "SHOW SLAVE STATUS" command
 */
static char *slave_status_columns[] = {
	"Slave_IO_State", "Master_Host", "Master_User", "Master_Port", "Connect_Retry",
	"Master_Log_File", "Read_Master_Log_Pos", "Relay_Log_File", "Relay_Log_Pos",
	"Relay_Master_Log_File", "Slave_IO_Running", "Slave_SQL_Running", "Replicate_Do_DB",
	"Replicate_Ignore_DB", "Replicate_Do_Table", 
	"Replicate_Ignore_Table", "Replicate_Wild_Do_Table", "Replicate_Wild_Ignore_Table",
	"Last_Errno", "Last_Error", "Skip_Counter", "Exec_Master_Log_Pos", "Relay_Log_Space",
	"Until_Condition", "Until_Log_File", "Until_Log_Pos", "Master_SSL_Allowed",
	"Master_SSL_CA_File", "Master_SSL_CA_Path", "Master_SSL_Cert", "Master_SSL_Cipher",
	"Master_SSL_Key", "Seconds_Behind_Master",
	"Master_SSL_Verify_Server_Cert", "Last_IO_Errno", "Last_IO_Error", "Last_SQL_Errno",
	"Last_SQL_Error", "Replicate_Ignore_Server_Ids", "Master_Server_Id", "Master_UUID",
	"Master_Info_File", "SQL_Delay", "SQL_Remaining_Delay", "Slave_SQL_Running_State",
	"Master_Retry_Count", "Master_Bind", "Last_IO_Error_TimeStamp", 
	"Last_SQL_Error_Timestamp", "Master_SSL_Crl", "Master_SSL_Crlpath",
	"Retrieved_Gtid_Set", "Executed_Gtid_Set", "Auto_Position", NULL
};

/**
 * Send the response to the SQL command "SHOW SLAVE STATUS"
 *
 * @param	router		The binlog router instance
 * @param	slave		The slave server to which we are sending the response
 * @return	Non-zero if data was sent
 */
static int
blr_slave_send_slave_status(ROUTER_INSTANCE *router, ROUTER_SLAVE *slave)
{
GWBUF	*pkt;
char	column[42];
uint8_t *ptr;
int	len, actual_len, col_len, seqno, ncols, i;
char    *dyn_column=NULL;

	/* Count the columns */
	for (ncols = 0; slave_status_columns[ncols]; ncols++);

	blr_slave_send_fieldcount(router, slave, ncols);
	seqno = 2;
	for (i = 0; slave_status_columns[i]; i++)
		blr_slave_send_columndef(router, slave, slave_status_columns[i], BLR_TYPE_STRING, 40, seqno++);
	blr_slave_send_eof(router, slave, seqno++);

	len = 5 + (ncols * 41) + 250;	// Max length + 250 bytes error message

	if ((pkt = gwbuf_alloc(len)) == NULL)
		return 0;
	ptr = GWBUF_DATA(pkt);
	encode_value(ptr, len - 4, 24);			// Add length of data packet
	ptr += 3;
	*ptr++ = seqno++;					// Sequence number in response

	sprintf(column, "%s", blrm_states[router->master_state]);
	col_len = strlen(column);
	*ptr++ = col_len;					// Length of result string
	strncpy((char *)ptr, column, col_len);		// Result string
	ptr += col_len;

	sprintf(column, "%s", router->service->dbref->server->name ? router->service->dbref->server->name : "");
	col_len = strlen(column);
	*ptr++ = col_len;					// Length of result string
	strncpy((char *)ptr, column, col_len);		// Result string
	ptr += col_len;

	sprintf(column, "%s", router->user ? router->user : "");
	col_len = strlen(column);
	*ptr++ = col_len;					// Length of result string
	strncpy((char *)ptr, column, col_len);		// Result string
	ptr += col_len;

	sprintf(column, "%d", router->service->dbref->server->port);
	col_len = strlen(column);
	*ptr++ = col_len;					// Length of result string
	strncpy((char *)ptr, column, col_len);		// Result string
	ptr += col_len;

	sprintf(column, "%d", 60);			// Connect retry
	col_len = strlen(column);
	*ptr++ = col_len;					// Length of result string
	strncpy((char *)ptr, column, col_len);		// Result string
	ptr += col_len;

	sprintf(column, "%s", router->binlog_name);
	col_len = strlen(column);
	*ptr++ = col_len;					// Length of result string
	strncpy((char *)ptr, column, col_len);		// Result string
	ptr += col_len;

	/* if router->trx_safe report current_pos*/
	if (router->trx_safe)
		sprintf(column, "%lu", router->current_pos);
	else
		sprintf(column, "%lu", router->binlog_position);

	col_len = strlen(column);
	*ptr++ = col_len;					// Length of result string
	strncpy((char *)ptr, column, col_len);		// Result string
	ptr += col_len;

	/* We have no relay log, we relay the binlog, so we will send the same data */
	sprintf(column, "%s", router->binlog_name);
	col_len = strlen(column);
	*ptr++ = col_len;					// Length of result string
	strncpy((char *)ptr, column, col_len);		// Result string
	ptr += col_len;

	sprintf(column, "%ld", router->binlog_position);
	col_len = strlen(column);
	*ptr++ = col_len;					// Length of result string
	strncpy((char *)ptr, column, col_len);		// Result string
	ptr += col_len;

	/* We have no relay log, we relay the binlog, so we will send the same data */
	sprintf(column, "%s", router->binlog_name);
	col_len = strlen(column);
	*ptr++ = col_len;					// Length of result string
	strncpy((char *)ptr, column, col_len);		// Result string
	ptr += col_len;

	if (router->master_state != BLRM_SLAVE_STOPPED) {
		if (router->master_state < BLRM_BINLOGDUMP)
			strcpy(column, "Connecting");
		else
			strcpy(column, "Yes");
	} else {
		strcpy(column, "No");
	}
	col_len = strlen(column);
	*ptr++ = col_len;					// Length of result string
	strncpy((char *)ptr, column, col_len);		// Result string
	ptr += col_len;

	if (router->master_state != BLRM_SLAVE_STOPPED)
		strcpy(column, "Yes");
	else
		strcpy(column, "No");
	col_len = strlen(column);
	*ptr++ = col_len;					// Length of result string
	strncpy((char *)ptr, column, col_len);		// Result string
	ptr += col_len;

	*ptr++ = 0;					// Send 6 empty values
	*ptr++ = 0;
	*ptr++ = 0;
	*ptr++ = 0;
	*ptr++ = 0;
	*ptr++ = 0;

	/* Last error information */
	sprintf(column, "%lu", router->m_errno);
	col_len = strlen(column);
	*ptr++ = col_len;					// Length of result string
	strncpy((char *)ptr, column, col_len);		// Result string
	ptr += col_len;

	/* Last error message */
	if (router->m_errmsg == NULL) {
		*ptr++ = 0;
	} else {
		dyn_column = (char*)router->m_errmsg;
		col_len = strlen(dyn_column);
		if (col_len > 250)
			col_len = 250;
		*ptr++ = col_len;                                       // Length of result string
		strncpy((char *)ptr, dyn_column, col_len);              // Result string
		ptr += col_len;
	}

	/* Skip_Counter */
	sprintf(column, "%d", 0);
	col_len = strlen(column);
	*ptr++ = col_len;					// Length of result string
	strncpy((char *)ptr, column, col_len);		// Result string
	ptr += col_len;

	sprintf(column, "%ld", router->binlog_position);
	col_len = strlen(column);
	*ptr++ = col_len;					// Length of result string
	strncpy((char *)ptr, column, col_len);		// Result string
	ptr += col_len;

	sprintf(column, "%ld", router->binlog_position);
	col_len = strlen(column);
	*ptr++ = col_len;					// Length of result string
	strncpy((char *)ptr, column, col_len);		// Result string
	ptr += col_len;

	strcpy(column, "None");
	col_len = strlen(column);
	*ptr++ = col_len;					// Length of result string
	strncpy((char *)ptr, column, col_len);		// Result string
	ptr += col_len;

	*ptr++ = 0;

	/* Until_Log_Pos */
	sprintf(column, "%d", 0);
	col_len = strlen(column);
	*ptr++ = col_len;					// Length of result string
	strncpy((char *)ptr, column, col_len);		// Result string
	ptr += col_len;

	/* Master_SSL_Allowed */
	strcpy(column, "No");
	col_len = strlen(column);
	*ptr++ = col_len;					// Length of result string
	strncpy((char *)ptr, column, col_len);		// Result string
	ptr += col_len;

	*ptr++ = 0;					// Empty SSL columns
	*ptr++ = 0;
	*ptr++ = 0;
	*ptr++ = 0;
	*ptr++ = 0;

	/* Seconds_Behind_Master */
	sprintf(column, "%d", 0);
	col_len = strlen(column);
	*ptr++ = col_len;					// Length of result string
	strncpy((char *)ptr, column, col_len);		// Result string
	ptr += col_len;

	/* Master_SSL_Verify_Server_Cert */
	strcpy(column, "No");
	col_len = strlen(column);
	*ptr++ = col_len;					// Length of result string
	strncpy((char *)ptr, column, col_len);		// Result string
	ptr += col_len;

	/* Last_IO_Error */
	sprintf(column, "%d", 0);
	col_len = strlen(column);
	*ptr++ = col_len;					// Length of result string
	strncpy((char *)ptr, column, col_len);		// Result string
	ptr += col_len;

	*ptr++ = 0;

	/* Last_SQL_Error */
	sprintf(column, "%d", 0);
	col_len = strlen(column);
	*ptr++ = col_len;					// Length of result string
	strncpy((char *)ptr, column, col_len);		// Result string
	ptr += col_len;

	*ptr++ = 0;
	*ptr++ = 0;

	/* Master_Server_Id */
	sprintf(column, "%d", router->masterid);
	col_len = strlen(column);
	*ptr++ = col_len;					// Length of result string
	strncpy((char *)ptr, column, col_len);		// Result string
	ptr += col_len;

	sprintf(column, "%s", router->master_uuid ?
			 router->master_uuid : router->uuid);
	col_len = strlen(column);
	*ptr++ = col_len;					// Length of result string
	strncpy((char *)ptr, column, col_len);		// Result string
	ptr += col_len;

	*ptr++ = 0;

	/* SQL_Delay*/
	sprintf(column, "%d", 0);
	col_len = strlen(column);
	*ptr++ = col_len;					// Length of result string
	strncpy((char *)ptr, column, col_len);		// Result string
	ptr += col_len;

	*ptr++ = 0xfb;				// NULL value

	/* Slave_Running_State */
	if (router->master_state == BLRM_SLAVE_STOPPED)
		strcpy(column, "Slave stopped");
	else if (!router->m_errno)
		strcpy(column, "Slave running");
	else {
		if (router->master_state < BLRM_BINLOGDUMP)
			strcpy(column, "Registering");
		else
			strcpy(column, "Error");
	}
	col_len = strlen(column);
	*ptr++ = col_len;					// Length of result string
	strncpy((char *)ptr, column, col_len);		// Result string
	ptr += col_len;

	/* Master_Retry_Count */
	sprintf(column, "%d", 1000);
	col_len = strlen(column);
	*ptr++ = col_len;					// Length of result string
	strncpy((char *)ptr, column, col_len);		// Result string
	ptr += col_len;

	*ptr++ = 0;			// Send 5 empty values
	*ptr++ = 0;
	*ptr++ = 0;
	*ptr++ = 0;
	*ptr++ = 0;

	// No GTID support send empty values
	*ptr++ = 0;
	*ptr++ = 0;
	*ptr++ = 0;
	*ptr++ = 0;

	actual_len = ptr - (uint8_t *)GWBUF_DATA(pkt);
	ptr = GWBUF_DATA(pkt);
	encode_value(ptr, actual_len - 4, 24);			// Add length of data packet

	pkt = gwbuf_rtrim(pkt, len - actual_len);		// Trim the buffer to the actual size

	slave->dcb->func.write(slave->dcb, pkt);
	return blr_slave_send_eof(router, slave, seqno++);
}

/**
 * Send the response to the SQL command "SHOW SLAVE HOSTS"
 *
 * @param	router		The binlog router instance
 * @param	slave		The slave server to which we are sending the response
 * @return	Non-zero if data was sent
 */
static int
blr_slave_send_slave_hosts(ROUTER_INSTANCE *router, ROUTER_SLAVE *slave)
{
GWBUF		*pkt;
char		server_id[40];
char		host[40];
char		port[40];
char		master_id[40];
char		slave_uuid[40];
uint8_t 	*ptr;
int		len, seqno;
ROUTER_SLAVE	*sptr;

	blr_slave_send_fieldcount(router, slave, 5);
	blr_slave_send_columndef(router, slave, "Server_id", BLR_TYPE_STRING, 40, 2);
	blr_slave_send_columndef(router, slave, "Host", BLR_TYPE_STRING, 40, 3);
	blr_slave_send_columndef(router, slave, "Port", BLR_TYPE_STRING, 40, 4);
	blr_slave_send_columndef(router, slave, "Master_id", BLR_TYPE_STRING, 40, 5);
	blr_slave_send_columndef(router, slave, "Slave_UUID", BLR_TYPE_STRING, 40, 6);
	blr_slave_send_eof(router, slave, 7);

	seqno = 8;
	spinlock_acquire(&router->lock);
	sptr = router->slaves;
	while (sptr)
	{
		if (sptr->state == BLRS_DUMPING || sptr->state == BLRS_REGISTERED)
		{
			sprintf(server_id, "%d", sptr->serverid);
			sprintf(host, "%s", sptr->hostname ? sptr->hostname : "");
			sprintf(port, "%d", sptr->port);
			sprintf(master_id, "%d", router->serverid);
			sprintf(slave_uuid, "%s", sptr->uuid ? sptr->uuid : "");
			len = 4 + strlen(server_id) + strlen(host) + strlen(port)
					+ strlen(master_id) + strlen(slave_uuid) + 5;
			if ((pkt = gwbuf_alloc(len)) == NULL)
				return 0;
			ptr = GWBUF_DATA(pkt);
			encode_value(ptr, len - 4, 24);			// Add length of data packet
			ptr += 3;
			*ptr++ = seqno++;						// Sequence number in response
			*ptr++ = strlen(server_id);					// Length of result string
			strncpy((char *)ptr, server_id, strlen(server_id));		// Result string
			ptr += strlen(server_id);
			*ptr++ = strlen(host);					// Length of result string
			strncpy((char *)ptr, host, strlen(host));		// Result string
			ptr += strlen(host);
			*ptr++ = strlen(port);					// Length of result string
			strncpy((char *)ptr, port, strlen(port));		// Result string
			ptr += strlen(port);
			*ptr++ = strlen(master_id);					// Length of result string
			strncpy((char *)ptr, master_id, strlen(master_id));		// Result string
			ptr += strlen(master_id);
			*ptr++ = strlen(slave_uuid);					// Length of result string
			strncpy((char *)ptr, slave_uuid, strlen(slave_uuid));		// Result string
			ptr += strlen(slave_uuid);
			slave->dcb->func.write(slave->dcb, pkt);
		}
		sptr = sptr->next;
	}
	spinlock_release(&router->lock);
	return blr_slave_send_eof(router, slave, seqno);
}

/**
 * Process a slave replication registration message.
 *
 * We store the various bits of information the slave gives us and generate
 * a reply message: OK packet.
 *
 * @param	router		The router instance
 * @param	slave		The slave server
 * @param	queue		The BINLOG_DUMP packet
 * @return			Non-zero if data was sent
 */
static int
blr_slave_register(ROUTER_INSTANCE *router, ROUTER_SLAVE *slave, GWBUF *queue)
{
GWBUF	*resp;
uint8_t	*ptr;
int	slen;

	ptr = GWBUF_DATA(queue);
	ptr += 4;		// Skip length and sequence number
	if (*ptr++ != COM_REGISTER_SLAVE)
		return 0;
	slave->serverid = extract_field(ptr, 32);
	ptr += 4;
	slen = *ptr++;
	if (slen != 0)
	{
		slave->hostname = strndup((char *)ptr, slen);
		ptr += slen;
	}
	else
		slave->hostname = NULL;
	slen = *ptr++;
	if (slen != 0)
	{
		ptr += slen;
		slave->user = strndup((char *)ptr, slen);
	}
	else
		slave->user = NULL;
	slen = *ptr++;
	if (slen != 0)
	{
		slave->passwd = strndup((char *)ptr, slen);
		ptr += slen;
	}
	else
		slave->passwd = NULL;
	slave->port = extract_field(ptr, 16);
	ptr += 2;
	slave->rank = extract_field(ptr, 32);

	slave->state = BLRS_REGISTERED;

	/*
	 * Send OK response
	 */
	return blr_slave_send_ok(router, slave);
}

/**
 * Process a COM_BINLOG_DUMP message from the slave. This is the
 * final step in the process of registration. The new master, MaxScale
 * must send a response packet and generate a fake BINLOG_ROTATE event
 * with the binlog file requested by the slave. And then send a
 * FORMAT_DESCRIPTION_EVENT that has been saved from the real master.
 *
 * Once send MaxScale must continue to send binlog events to the slave.
 *
 * @param	router		The router instance
 * @param	slave		The slave server
 * @param	queue		The BINLOG_DUMP packet
 * @return			The number of bytes written to the slave
 */
static int
blr_slave_binlog_dump(ROUTER_INSTANCE *router, ROUTER_SLAVE *slave, GWBUF *queue)
{
GWBUF		*resp;
uint8_t		*ptr;
int		len, rval, binlognamelen;
REP_HEADER	hdr;
uint32_t	chksum;

	ptr = GWBUF_DATA(queue);
	len = extract_field(ptr, 24);
	binlognamelen = len - 11;
	if (binlognamelen > BINLOG_FNAMELEN)
	{
        	MXS_ERROR("blr_slave_binlog_dump truncating binlog filename "
                          "from %d to %d",
                          binlognamelen, BINLOG_FNAMELEN);
		binlognamelen = BINLOG_FNAMELEN;
	}
	ptr += 4;		// Skip length and sequence number
	if (*ptr++ != COM_BINLOG_DUMP)
	{
        	MXS_ERROR("blr_slave_binlog_dump expected a COM_BINLOG_DUMP but received %d",
                          *(ptr-1));
		return 0;
	}

	slave->binlog_pos = extract_field(ptr, 32);
	ptr += 4;
	ptr += 2;
	ptr += 4;
	strncpy(slave->binlogfile, (char *)ptr, binlognamelen);
	slave->binlogfile[binlognamelen] = 0;

       	MXS_DEBUG("%s: COM_BINLOG_DUMP: binlog name '%s', length %d, "
                  "from position %lu.", router->service->name,
                  slave->binlogfile, binlognamelen,
                  (unsigned long)slave->binlog_pos);

	slave->seqno = 1;


	if (slave->nocrc)
		len = BINLOG_EVENT_HDR_LEN + 8 + binlognamelen;
	else
		len = BINLOG_EVENT_HDR_LEN + 8 + 4 + binlognamelen;

	// Build a fake rotate event
	resp = gwbuf_alloc(len + 5);
	hdr.payload_len = len + 1;
	hdr.seqno = slave->seqno++;
	hdr.ok = 0;
	hdr.timestamp = 0L;
	hdr.event_type = ROTATE_EVENT;
	hdr.serverid = router->masterid;
	hdr.event_size = len;
	hdr.next_pos = 0;
	hdr.flags = 0x20;
	ptr = blr_build_header(resp, &hdr);
	encode_value(ptr, slave->binlog_pos, 64);
	ptr += 8;
	memcpy(ptr, slave->binlogfile, binlognamelen);
	ptr += binlognamelen;

	if (!slave->nocrc)
	{
		/*
		 * Now add the CRC to the fake binlog rotate event.
		 *
		 * The algorithm is first to compute the checksum of an empty buffer
		 * and then the checksum of the event portion of the message, ie we do not
		 * include the length, sequence number and ok byte that makes up the first
		 * 5 bytes of the message. We also do not include the 4 byte checksum itself.
		 */
		chksum = crc32(0L, NULL, 0);
		chksum = crc32(chksum, GWBUF_DATA(resp) + 5, hdr.event_size - 4);
		encode_value(ptr, chksum, 32);
	}

	/* Send Fake Rotate Event */
	rval = slave->dcb->func.write(slave->dcb, resp);

	/* set lastEventReceived */
	slave->lastEventReceived = ROTATE_EVENT;

	/* set lastReply for slave heartbeat check */
	if (router->send_slave_heartbeat)
		slave->lastReply = time(0);

	/* Send the FORMAT_DESCRIPTION_EVENT */
	if (slave->binlog_pos != 4)
		blr_slave_send_fde(router, slave);

	/* set lastEventReceived */
	slave->lastEventReceived = FORMAT_DESCRIPTION_EVENT;

	slave->dcb->low_water  = router->low_water;
	slave->dcb->high_water = router->high_water;

	dcb_add_callback(slave->dcb, DCB_REASON_DRAINED, blr_slave_callback, slave);

	slave->state = BLRS_DUMPING;

	MXS_NOTICE("%s: Slave %s:%d, server id %d requested binlog file %s from position %lu",
		router->service->name, slave->dcb->remote,
		ntohs((slave->dcb->ipv4).sin_port),
		slave->serverid,
		slave->binlogfile, (unsigned long)slave->binlog_pos);

	if (slave->binlog_pos != router->binlog_position ||
			strcmp(slave->binlogfile, router->binlog_name) != 0)
	{
		spinlock_acquire(&slave->catch_lock);
		slave->cstate &= ~CS_UPTODATE;
		slave->cstate |= CS_EXPECTCB;
		spinlock_release(&slave->catch_lock);
		poll_fake_write_event(slave->dcb);
	}
	return rval;
}

/**
 * Encode a value into a number of bits in a MySQL packet
 *
 * @param	data	Pointer to location in target packet
 * @param	value	The value to encode into the buffer
 * @param	len	Number of bits to encode value into
 */
static void
encode_value(unsigned char *data, unsigned int value, int len)
{
	while (len > 0)
	{
		*data++ = value & 0xff;
		value >>= 8;
		len -= 8;
	}
}


/**
 * Populate a header structure for a replication message from a GWBUF structure.
 *
 * @param pkt	The incoming packet in a GWBUF chain
 * @param hdr	The packet header to populate
 * @return 	A pointer to the first byte following the event header
 */
uint8_t *
blr_build_header(GWBUF	*pkt, REP_HEADER *hdr)
{
uint8_t	*ptr;

	ptr = GWBUF_DATA(pkt);

	encode_value(ptr, hdr->payload_len, 24);
	ptr += 3;
	*ptr++ = hdr->seqno;
	*ptr++ = hdr->ok;
	encode_value(ptr, hdr->timestamp, 32);
	ptr += 4;
	*ptr++ = hdr->event_type;
	encode_value(ptr, hdr->serverid, 32);
	ptr += 4;
	encode_value(ptr, hdr->event_size, 32);
	ptr += 4;
	encode_value(ptr, hdr->next_pos, 32);
	ptr += 4;
	encode_value(ptr, hdr->flags, 16);
	ptr += 2;

	return ptr;
}

/**
 * We have a registered slave that is behind the current leading edge of the 
 * binlog. We must replay the log entries to bring this node up to speed.
 *
 * There may be a large number of records to send to the slave, the process
 * is triggered by the slave COM_BINLOG_DUMP message and all the events must
 * be sent without receiving any new event. This measn there is no trigger into
 * MaxScale other than this initial message. However, if we simply send all the
 * events we end up with an extremely long write queue on the DCB and risk
 * running the server out of resources.
 *
 * The slave catchup routine will send a burst of replication events per single
 * call. The paramter "long" control the number of events in the burst. The
 * short burst is intended to be used when the master receive an event and 
 * needs to put the slave into catchup mode. This prevents the slave taking
 * too much time away from the thread that is processing the master events.
 *
 * At the end of the burst a fake EPOLLOUT event is added to the poll event
 * queue. This ensures that the slave callback for processing DCB write drain
 * will be called and future catchup requests will be handled on another thread.
 *
 * @param	router		The binlog router
 * @param	slave		The slave that is behind
 * @param	large		Send a long or short burst of events
 * @return			The number of bytes written
 */
int
blr_slave_catchup(ROUTER_INSTANCE *router, ROUTER_SLAVE *slave, bool large)
{
GWBUF		*head, *record;
REP_HEADER	hdr;
int		written, rval = 1, burst;
int		rotating = 0;
unsigned long	burst_size;
uint8_t		*ptr;
char read_errmsg[BINLOG_ERROR_MSG_LEN+1];

	read_errmsg[BINLOG_ERROR_MSG_LEN] = '\0';

	if (large)
		burst = router->long_burst;
	else
		burst = router->short_burst;
	burst_size = router->burst_size;
	spinlock_acquire(&slave->catch_lock);
	if (slave->cstate & CS_BUSY)
	{
		spinlock_release(&slave->catch_lock);
		return 0;
	}
	slave->cstate |= CS_BUSY;
	spinlock_release(&slave->catch_lock);

	if (slave->file == NULL)
	{
		rotating = router->rotating;
		if ((slave->file = blr_open_binlog(router, slave->binlogfile)) == NULL)
		{
			char err_msg[BINLOG_ERROR_MSG_LEN+1];
			err_msg[BINLOG_ERROR_MSG_LEN] = '\0';

			if (rotating)
			{
				spinlock_acquire(&slave->catch_lock);
				slave->cstate |= CS_EXPECTCB;
				slave->cstate &= ~CS_BUSY;
				spinlock_release(&slave->catch_lock);
				poll_fake_write_event(slave->dcb);
				return rval;
			}
			MXS_ERROR("Slave %s:%i, server-id %d, binlog '%s': blr_slave_catchup "
				"failed to open binlog file",
				slave->dcb->remote, ntohs((slave->dcb->ipv4).sin_port), slave->serverid,
				slave->binlogfile);

			slave->cstate &= ~CS_BUSY;
			slave->state = BLRS_ERRORED;

			snprintf(err_msg, BINLOG_ERROR_MSG_LEN, "Failed to open binlog '%s'", slave->binlogfile);

			/* Send error that stops slave replication */
			blr_send_custom_error(slave->dcb, slave->seqno++, 0, err_msg, "HY000", 1236);

			dcb_close(slave->dcb);
			return 0;
		}
	}
	slave->stats.n_bursts++;

	while (burst-- && burst_size > 0 &&
		(record = blr_read_binlog(router, slave->file, slave->binlog_pos, &hdr, read_errmsg)) != NULL)
	{
		head = gwbuf_alloc(5);
		ptr = GWBUF_DATA(head);
		encode_value(ptr, hdr.event_size + 1, 24);
		ptr += 3;
		*ptr++ = slave->seqno++;
		*ptr++ = 0;		// OK
		head = gwbuf_append(head, record);
		slave->lastEventTimestamp = hdr.timestamp;
		slave->lastEventReceived = hdr.event_type;

		if (hdr.event_type == ROTATE_EVENT)
		{
			unsigned long beat1 = hkheartbeat;
			blr_close_binlog(router, slave->file);
			if (hkheartbeat - beat1 > 1)
				MXS_ERROR("blr_close_binlog took %lu maxscale beats",
                                          hkheartbeat - beat1);
			blr_slave_rotate(router, slave, GWBUF_DATA(record));
			beat1 = hkheartbeat;
			if ((slave->file = blr_open_binlog(router, slave->binlogfile)) == NULL)
			{
				char err_msg[BINLOG_ERROR_MSG_LEN+1];
				err_msg[BINLOG_ERROR_MSG_LEN] = '\0';
				if (rotating)
				{
					spinlock_acquire(&slave->catch_lock);
					slave->cstate |= CS_EXPECTCB;
					slave->cstate &= ~CS_BUSY;
					spinlock_release(&slave->catch_lock);
					poll_fake_write_event(slave->dcb);
					return rval;
				}
				MXS_ERROR("Slave %s:%i, server-id %d, binlog '%s': blr_slave_catchup "
					"failed to open binlog file in rotate event",
					slave->dcb->remote,
					ntohs((slave->dcb->ipv4).sin_port),
					slave->serverid,
					slave->binlogfile);

				slave->state = BLRS_ERRORED;

				snprintf(err_msg, BINLOG_ERROR_MSG_LEN, "Failed to open binlog '%s' in rotate event", slave->binlogfile);

				/* Send error that stops slave replication */
				blr_send_custom_error(slave->dcb, (slave->seqno - 1), 0, err_msg, "HY000", 1236);

				dcb_close(slave->dcb);
				break;
			}
			if (hkheartbeat - beat1 > 1)
				MXS_ERROR("blr_open_binlog took %lu beats",
                                          hkheartbeat - beat1);
		}
		slave->stats.n_bytes += gwbuf_length(head);
		written = slave->dcb->func.write(slave->dcb, head);
		if (written && hdr.event_type != ROTATE_EVENT)
		{
			slave->binlog_pos = hdr.next_pos;
		}
		rval = written;
		slave->stats.n_events++;
		burst_size -= hdr.event_size;

		/* set lastReply for slave heartbeat check */
		if (router->send_slave_heartbeat)
			slave->lastReply = time(0);
	}
	if (record == NULL) {
		slave->stats.n_failed_read++;

                if (hdr.ok == SLAVE_POS_BAD_FD) {
			MXS_ERROR("%s Slave %s:%i, server-id %d, binlog '%s', %s",
				router->service->name,
				slave->dcb->remote,
				ntohs((slave->dcb->ipv4).sin_port),
				slave->serverid,
				slave->binlogfile,
				read_errmsg);
		}

                if (hdr.ok == SLAVE_POS_READ_ERR) {
			MXS_ERROR("%s Slave %s:%i, server-id %d, binlog '%s', %s",
				router->service->name,
				slave->dcb->remote,
				ntohs((slave->dcb->ipv4).sin_port),
				slave->serverid,
				slave->binlogfile,
				read_errmsg);

                        spinlock_acquire(&slave->catch_lock);

                        slave->state = BLRS_ERRORED;

                        spinlock_release(&slave->catch_lock);

			/*
			 * Send an error that will stop slave replication
			 */
                        blr_send_custom_error(slave->dcb, slave->seqno++, 0, read_errmsg, "HY000", 1236);

                        dcb_close(slave->dcb);

                        return 0;
                }

		if (hdr.ok == SLAVE_POS_READ_UNSAFE) {

			MXS_ERROR("%s: Slave %s:%i, server-id %d, binlog '%s', %s",
				router->service->name,
				slave->dcb->remote,
				ntohs((slave->dcb->ipv4).sin_port),
				slave->serverid,
				slave->binlogfile,
				read_errmsg);

			/*
			 * Close the slave session and socket
			 * The slave will try to reconnect
			 */
			dcb_close(slave->dcb);

			return 0;
		}
	}
	spinlock_acquire(&slave->catch_lock);
	slave->cstate &= ~CS_BUSY;
	spinlock_release(&slave->catch_lock);

	if (record)
	{
		slave->stats.n_flows++;
		spinlock_acquire(&slave->catch_lock);
		slave->cstate |= CS_EXPECTCB;
		spinlock_release(&slave->catch_lock);
		poll_fake_write_event(slave->dcb);
	}
	else if (slave->binlog_pos == router->binlog_position &&
			strcmp(slave->binlogfile, router->binlog_name) == 0)
	{
		int state_change = 0;
		unsigned int cstate =0;
		spinlock_acquire(&router->binlog_lock);
		spinlock_acquire(&slave->catch_lock);

		cstate = slave->cstate;

		/*
		 * Now check again since we hold the router->binlog_lock
		 * and slave->catch_lock.
		 */
		if (slave->binlog_pos != router->binlog_position ||
			strcmp(slave->binlogfile, router->binlog_name) != 0)
		{
			slave->cstate &= ~CS_UPTODATE;
			slave->cstate |= CS_EXPECTCB;
			spinlock_release(&slave->catch_lock);
			spinlock_release(&router->binlog_lock);

			if ((cstate & CS_UPTODATE) == CS_UPTODATE)
			{
#ifdef STATE_CHANGE_LOGGING_ENABLED
				MXS_NOTICE("%s: Slave %s:%d, server-id %d transition from up-to-date to catch-up in blr_slave_catchup, binlog file '%s', position %lu.",
					router->service->name,
					slave->dcb->remote,
					ntohs((slave->dcb->ipv4).sin_port),
					slave->serverid,
					slave->binlogfile, (unsigned long)slave->binlog_pos);
#endif
			}

			poll_fake_write_event(slave->dcb);
		}
		else
		{
			if ((slave->cstate & CS_UPTODATE) == 0)
			{
				slave->stats.n_upd++;
				slave->cstate |= CS_UPTODATE;
				spinlock_release(&slave->catch_lock);
				spinlock_release(&router->binlog_lock);
				state_change = 1;
			}
			else
			{
				MXS_NOTICE("Execution entered branch were locks previously were NOT "
				           "released. Previously this would have caused a lock-up.");
				spinlock_release(&slave->catch_lock);
				spinlock_release(&router->binlog_lock);
			}
		}

		if (state_change)
		{
			slave->stats.n_caughtup++;
#ifdef STATE_CHANGE_LOGGING_ENABLED
                        // TODO: The % 50 should be removed. Now only every 50th state change is logged.
			if (slave->stats.n_caughtup == 1)
			{
				MXS_NOTICE("%s: Slave %s:%d, server-id %d is now up to date '%s', position %lu.",
					router->service->name,
					slave->dcb->remote,
					ntohs((slave->dcb->ipv4).sin_port),
					slave->serverid,
					slave->binlogfile, (unsigned long)slave->binlog_pos);
			}
			else if ((slave->stats.n_caughtup % 50) == 0)
			{
				MXS_NOTICE("%s: Slave %s:%d, server-id %d is up to date '%s', position %lu.",
					router->service->name,
					slave->dcb->remote,
					ntohs((slave->dcb->ipv4).sin_port),
					slave->serverid,
					slave->binlogfile, (unsigned long)slave->binlog_pos);
			}
#endif
		}
	}
	else
	{
		if (slave->binlog_pos >= blr_file_size(slave->file)
				&& router->rotating == 0
				&& strcmp(router->binlog_name, slave->binlogfile) != 0
				&& (blr_master_connected(router)
					|| blr_file_next_exists(router, slave)))
		{
			/* We may have reached the end of file of a non-current
			 * binlog file.
			 *
			 * Note if the master is rotating there is a window during
			 * which the rotate event has been written to the old binlog
			 * but the new binlog file has not yet been created. Therefore
			 * we ignore these issues during the rotate processing.
			 */
			MXS_ERROR("%s: Slave %s:%d, server-id %d reached end of file for binlog file %s "
                                  "at %lu which is not the file currently being downloaded. "
                                  "Master binlog is %s, %lu. This may be caused by a "
                                  "previous failure of the master.",
                                  router->service->name,
                                  slave->dcb->remote,
                                  ntohs((slave->dcb->ipv4).sin_port),
                                  slave->serverid,
                                  slave->binlogfile, (unsigned long)slave->binlog_pos,
                                  router->binlog_name, router->binlog_position);

			if (blr_slave_fake_rotate(router, slave))
			{
				spinlock_acquire(&slave->catch_lock);
				slave->cstate |= CS_EXPECTCB;
				spinlock_release(&slave->catch_lock);
				poll_fake_write_event(slave->dcb);
			}
			else
			{
				slave->state = BLRS_ERRORED;
				dcb_close(slave->dcb);
			}
		}
		else if (blr_master_connected(router))
		{
			spinlock_acquire(&slave->catch_lock);
			slave->cstate |= CS_EXPECTCB;
			spinlock_release(&slave->catch_lock);
			poll_fake_write_event(slave->dcb);
		}
	}
	return rval;
}

/**
 * The DCB callback used by the slave to obtain DCB_REASON_LOW_WATER callbacks
 * when the server sends all the the queue data for a DCB. This is the mechanism
 * that is used to implement the flow control mechanism for the sending of
 * large quantities of binlog records during the catchup process.
 *
 * @param dcb		The DCB of the slave connection
 * @param reason	The reason the callback was called
 * @param data		The user data, in this case the server structure
 */
int
blr_slave_callback(DCB *dcb, DCB_REASON reason, void *data)
{
ROUTER_SLAVE		*slave = (ROUTER_SLAVE *)data;
ROUTER_INSTANCE		*router = slave->router;
unsigned int cstate;

    if (NULL == dcb->session->router_session)
    {
        /*
         * The following processing will fail if there is no router session,
         * because the "data" parameter will not contain meaningful data,
         * so we have no choice but to stop here.
         */
        return 0;
    }
	if (reason == DCB_REASON_DRAINED)
	{
		if (slave->state == BLRS_DUMPING)
		{
			int do_return;

			spinlock_acquire(&router->binlog_lock);

			do_return = 0;
			cstate = slave->cstate;

			/* check for a pending transaction and not rotating */
			if (router->pending_transaction && strcmp(router->binlog_name, slave->binlogfile) == 0 &&
				(slave->binlog_pos > router->binlog_position) && !router->rotating) {
				do_return = 1;
			}

			spinlock_release(&router->binlog_lock);

			if (do_return) {
				spinlock_acquire(&slave->catch_lock);
				slave->cstate |= CS_EXPECTCB;
				spinlock_release(&slave->catch_lock);
				poll_fake_write_event(slave->dcb);

				return 0;
			}

			spinlock_acquire(&slave->catch_lock);
			cstate = slave->cstate;
			slave->cstate &= ~(CS_UPTODATE|CS_EXPECTCB);
			spinlock_release(&slave->catch_lock);

			if ((cstate & CS_UPTODATE) == CS_UPTODATE)
			{
#ifdef STATE_CHANGE_LOGGING_ENABLED
				MXS_NOTICE("%s: Slave %s:%d, server-id %d transition from up-to-date to catch-up in blr_slave_callback, binlog file '%s', position %lu.",
					router->service->name,
					slave->dcb->remote,
					ntohs((slave->dcb->ipv4).sin_port),
					slave->serverid,
					slave->binlogfile, (unsigned long)slave->binlog_pos);
#endif
			}

			slave->stats.n_dcb++;
			blr_slave_catchup(router, slave, true);
		}
		else
		{
        		MXS_DEBUG("Ignored callback due to slave state %s",
                                  blrs_states[slave->state]);
		}
	}

	if (reason == DCB_REASON_LOW_WATER)
	{
		if (slave->state == BLRS_DUMPING)
		{
			slave->stats.n_cb++;
			blr_slave_catchup(router, slave, true);
		}
		else
		{
			slave->stats.n_cbna++;
		}
	}
	return 0;
}

/**
 * Rotate the slave to the new binlog file
 *
 * @param slave 	The slave instance
 * @param ptr		The rotate event (minus header and OK byte)
 */
void
blr_slave_rotate(ROUTER_INSTANCE *router, ROUTER_SLAVE *slave, uint8_t *ptr)
{
int	len = EXTRACT24(ptr + 9);	// Extract the event length

	len = len - (BINLOG_EVENT_HDR_LEN + 8);		// Remove length of header and position
	if (router->master_chksum)
		len -= 4;
	if (len > BINLOG_FNAMELEN)
		len = BINLOG_FNAMELEN;
	ptr += BINLOG_EVENT_HDR_LEN;	// Skip header
	slave->binlog_pos = extract_field(ptr, 32);
	slave->binlog_pos += (((uint64_t)extract_field(ptr+4, 32)) << 32);
	memcpy(slave->binlogfile, ptr + 8, len);
	slave->binlogfile[len] = 0;
}

/**
 *  Generate an internal rotate event that we can use to cause the slave to move beyond
 * a binlog file that is misisng the rotate eent at the end.
 *
 * @param router	The router instance
 * @param slave		The slave to rotate
 * @return  Non-zero if the rotate took place
 */
static int
blr_slave_fake_rotate(ROUTER_INSTANCE *router, ROUTER_SLAVE *slave)
{
char		*sptr;
int		filenum;
GWBUF		*resp;
uint8_t		*ptr;
int		len, binlognamelen;
REP_HEADER	hdr;
uint32_t	chksum;

	if ((sptr = strrchr(slave->binlogfile, '.')) == NULL)
		return 0;
	blr_close_binlog(router, slave->file);
	filenum = atoi(sptr + 1);
	sprintf(slave->binlogfile, BINLOG_NAMEFMT, router->fileroot, filenum + 1);
	slave->binlog_pos = 4;
	if ((slave->file = blr_open_binlog(router, slave->binlogfile)) == NULL)
		return 0;

	binlognamelen = strlen(slave->binlogfile);
	len = BINLOG_EVENT_HDR_LEN + 8 + 4 + binlognamelen;
	/* no slave crc, remove 4 bytes */
	if (slave->nocrc)
		len -= 4;

	// Build a fake rotate event
	resp = gwbuf_alloc(len + 5);
	hdr.payload_len = len + 1;
	hdr.seqno = slave->seqno++;
	hdr.ok = 0;
	hdr.timestamp = 0L;
	hdr.event_type = ROTATE_EVENT;
	hdr.serverid = router->masterid;
	hdr.event_size = len;
	hdr.next_pos = 0;
	hdr.flags = 0x20;
	ptr = blr_build_header(resp, &hdr);
	encode_value(ptr, slave->binlog_pos, 64);
	ptr += 8;
	memcpy(ptr, slave->binlogfile, binlognamelen);
	ptr += binlognamelen;

	/* if slave has crc add the chksum */
	if (!slave->nocrc) {
		/*
		 * Now add the CRC to the fake binlog rotate event.
		 *
		 * The algorithm is first to compute the checksum of an empty buffer
		 * and then the checksum of the event portion of the message, ie we do not
		 * include the length, sequence number and ok byte that makes up the first
		 * 5 bytes of the message. We also do not include the 4 byte checksum itself.
		 */
		chksum = crc32(0L, NULL, 0);
		chksum = crc32(chksum, GWBUF_DATA(resp) + 5, hdr.event_size - 4);
		encode_value(ptr, chksum, 32);
	}

	slave->dcb->func.write(slave->dcb, resp);
	return 1;
}

/**
 * Send a "fake" format description event to the newly connected slave
 *
 * @param router	The router instance
 * @param slave		The slave to send the event to
 */
static void
blr_slave_send_fde(ROUTER_INSTANCE *router, ROUTER_SLAVE *slave)
{
BLFILE		*file;
REP_HEADER	hdr;
GWBUF		*record, *head;
uint8_t		*ptr;
uint32_t	chksum;
char err_msg[BINLOG_ERROR_MSG_LEN+1];

	err_msg[BINLOG_ERROR_MSG_LEN] = '\0';

	memset(&hdr, 0, BINLOG_EVENT_HDR_LEN);

	if ((file = blr_open_binlog(router, slave->binlogfile)) == NULL)
		return;
	if ((record = blr_read_binlog(router, file, 4, &hdr, err_msg)) == NULL)
	{
		if (hdr.ok != SLAVE_POS_READ_OK) {
			MXS_ERROR("Slave %s:%i, server-id %d, binlog '%s', blr_read_binlog failure: %s",
				slave->dcb->remote,
				ntohs((slave->dcb->ipv4).sin_port),
				slave->serverid,
				slave->binlogfile,
				err_msg);
		}

		blr_close_binlog(router, file);
		return;
	}
	blr_close_binlog(router, file);
	head = gwbuf_alloc(5);
	ptr = GWBUF_DATA(head);
	encode_value(ptr, hdr.event_size + 1, 24); // Payload length
	ptr += 3;
	*ptr++ = slave->seqno++;
	*ptr++ = 0;		// OK
	head = gwbuf_append(head, record);
	ptr = GWBUF_DATA(record);
	encode_value(ptr, time(0), 32);		// Overwrite timestamp
	ptr += 13;
	encode_value(ptr, 0, 32);		// Set next position to 0
	/*
	 * Since we have changed the timestamp we must recalculate the CRC
	 *
	 * Position ptr to the start of the event header,
	 * calculate a new checksum
	 * and write it into the header
	 */
	ptr = GWBUF_DATA(record) + hdr.event_size - 4;
	chksum = crc32(0L, NULL, 0);
	chksum = crc32(chksum, GWBUF_DATA(record), hdr.event_size - 4);
	encode_value(ptr, chksum, 32);

	slave->dcb->func.write(slave->dcb, head);
}



/**
 * Send the field count packet in a response packet sequence.
 *
 * @param router	The router
 * @param slave		The slave connection
 * @param count		Number of columns in the result set
 * @return		Non-zero on success
 */
static int
blr_slave_send_fieldcount(ROUTER_INSTANCE *router, ROUTER_SLAVE *slave, int count)
{
GWBUF	*pkt;
uint8_t *ptr;

	if ((pkt = gwbuf_alloc(5)) == NULL)
		return 0;
	ptr = GWBUF_DATA(pkt);
	encode_value(ptr, 1, 24);			// Add length of data packet
	ptr += 3;
	*ptr++ = 0x01;					// Sequence number in response
	*ptr++ = count;					// Length of result string
	return slave->dcb->func.write(slave->dcb, pkt);
}


/**
 * Send the column definition packet in a response packet sequence.
 *
 * @param router	The router
 * @param slave		The slave connection
 * @param name		Name of the column
 * @param type		Column type
 * @param len		Column length
 * @param seqno		Packet sequence number
 * @return		Non-zero on success
 */
static int
blr_slave_send_columndef(ROUTER_INSTANCE *router, ROUTER_SLAVE *slave, char *name, int type, int len, uint8_t seqno)
{
GWBUF	*pkt;
uint8_t *ptr;

	if ((pkt = gwbuf_alloc(26 + strlen(name))) == NULL)
		return 0;
	ptr = GWBUF_DATA(pkt);
	encode_value(ptr, 22 + strlen(name), 24);	// Add length of data packet
	ptr += 3;
	*ptr++ = seqno;					// Sequence number in response
	*ptr++ = 3;					// Catalog is always def
	*ptr++ = 'd';
	*ptr++ = 'e';
	*ptr++ = 'f';
	*ptr++ = 0;					// Schema name length
	*ptr++ = 0;					// virtual table name length
	*ptr++ = 0;					// Table name length
	*ptr++ = strlen(name);				// Column name length;
	while (*name)
		*ptr++ = *name++;			// Copy the column name
	*ptr++ = 0;					// Orginal column name
	*ptr++ = 0x0c;					// Length of next fields always 12
	*ptr++ = 0x3f;					// Character set
	*ptr++ = 0;
	encode_value(ptr, len, 32);			// Add length of column
	ptr += 4;
	*ptr++ = type;
	*ptr++ = 0x81;					// Two bytes of flags
	if (type == 0xfd)
		*ptr++ = 0x1f;
	else
		*ptr++ = 0x00;
	*ptr++= 0;
	*ptr++= 0;
	*ptr++= 0;
	return slave->dcb->func.write(slave->dcb, pkt);
}


/**
 * Send an EOF packet in a response packet sequence.
 *
 * @param router	The router
 * @param slave		The slave connection
 * @param seqno		The sequence number of the EOF packet
 * @return		Non-zero on success
 */
static int
blr_slave_send_eof(ROUTER_INSTANCE *router, ROUTER_SLAVE *slave, int seqno)
{
GWBUF	*pkt;
uint8_t *ptr;

	if ((pkt = gwbuf_alloc(9)) == NULL)
		return 0;
	ptr = GWBUF_DATA(pkt);
	encode_value(ptr, 5, 24);			// Add length of data packet
	ptr += 3;
	*ptr++ = seqno;					// Sequence number in response
	*ptr++ = 0xfe;					// Length of result string
	encode_value(ptr, 0, 16);			// No errors
	ptr += 2;
	encode_value(ptr, 2, 16);			// Autocommit enabled
	return slave->dcb->func.write(slave->dcb, pkt);
}

/**
 * Send the reply only to the SQL command "DISCONNECT SERVER $server_id'
 *
 * @param	router		The binlog router instance
 * @param	slave		The slave server to which we are sending the response
 * @return	Non-zero if data was sent
 */
static int
blr_slave_send_disconnected_server(ROUTER_INSTANCE *router, ROUTER_SLAVE *slave, int server_id, int found)
{
GWBUF	*pkt;
char	state[40];
char	serverid[40];
uint8_t *ptr;
int	len, id_len, seqno = 2;

	sprintf(serverid, "%d", server_id);
	if (found)
		strcpy(state, "disconnected");
	else
		strcpy(state, "not found");

	id_len = strlen(serverid);
	len = 4 + (1 + id_len) + (1 + strlen(state));

	if ((pkt = gwbuf_alloc(len)) == NULL)
		return 0;

	blr_slave_send_fieldcount(router, slave, 2);
	blr_slave_send_columndef(router, slave, "server_id", BLR_TYPE_INT, 40, seqno++);
	blr_slave_send_columndef(router, slave, "state", BLR_TYPE_STRING, 40, seqno++);
	blr_slave_send_eof(router, slave, seqno++);

	ptr = GWBUF_DATA(pkt);
	encode_value(ptr, len - 4), 24);	// Add length of data packet
	ptr += 3;
	*ptr++ = seqno++;					// Sequence number in response

	*ptr++ = id_len;					// Length of result string
	strncpy((char *)ptr, serverid, id_len);			// Result string
	ptr += id_len;

	*ptr++ = strlen(state);					// Length of result string
	strncpy((char *)ptr, state, strlen(state));		// Result string
	ptr += strlen(state);

	slave->dcb->func.write(slave->dcb, pkt);

	return blr_slave_send_eof(router, slave, seqno++);
}


/**
 * Send the response to the SQL command "DISCONNECT SERVER $server_id'
 * and close the connection to that server
 *
 * @param	router		The binlog router instance
 * @param	slave		The slave server to which we are sending the response
 * @param	server_id	The slave server_id to disconnect
 * @return	Non-zero if data was sent to the client
 */
static int
blr_slave_disconnect_server(ROUTER_INSTANCE *router, ROUTER_SLAVE *slave, int server_id)
{
	ROUTER_OBJECT *router_obj= router->service->router;
	ROUTER_SLAVE    *sptr;
	int n;
	int server_found  = 0;

	spinlock_acquire(&router->lock);

	sptr = router->slaves;
	/* look for server_id among all registered slaves */
	while (sptr)
	{
		/* don't examine slaves with state = 0 */
		if ((sptr->state == BLRS_REGISTERED || sptr->state == BLRS_DUMPING) &&
			sptr->serverid == server_id)
		{
			/* server_id found */
			server_found = 1;
			MXS_NOTICE("%s: Slave %s, server id %d, disconnected by %s@%s",
                                   router->service->name,
                                   sptr->dcb->remote,
                                   server_id,
                                   slave->dcb->user,
                                   slave->dcb->remote);

			/* send server_id with disconnect state to client */
			n = blr_slave_send_disconnected_server(router, slave, server_id, 1);

			sptr->state = BLRS_UNREGISTERED;
			dcb_close(sptr->dcb);

			break;
		} else {
			sptr = sptr->next;
		}
	}

	spinlock_release(&router->lock);

	/** server id was not found
	 * send server_id with not found state to the client
	 */
	if (!server_found)
	{
		n = blr_slave_send_disconnected_server(router, slave, server_id, 0);
	}

	if (n == 0) {
		MXS_ERROR("gwbuf memory allocation in "
                          "DISCONNECT SERVER server_id [%d]",
                          sptr->serverid);

		blr_slave_send_error(router, slave, "Memory allocation error for DISCONNECT SERVER");
	}

	return 1;
}

/**
 * Send the response to the SQL command "DISCONNECT ALL'
 * and close the connection to all slave servers
 *
 * @param	router		The binlog router instance
 * @param	slave		The slave server to which we are sending the response
 * @return	Non-zero if data was sent to the client
 */
static int
blr_slave_disconnect_all(ROUTER_INSTANCE *router, ROUTER_SLAVE *slave)
{
	ROUTER_OBJECT *router_obj= router->service->router;
	ROUTER_SLAVE    *sptr;
	char server_id[40];
	char state[40];
	uint8_t *ptr;
	int len, seqno;
	GWBUF *pkt;

       /* preparing output result */
	blr_slave_send_fieldcount(router, slave, 2);
	blr_slave_send_columndef(router, slave, "server_id", BLR_TYPE_INT, 40, 2);
	blr_slave_send_columndef(router, slave, "state", BLR_TYPE_STRING, 40, 3);
	blr_slave_send_eof(router, slave, 4);
	seqno = 5;

	spinlock_acquire(&router->lock);
	sptr = router->slaves;

	while (sptr)
	{
		/* skip servers with state = 0 */
		if (sptr->state == BLRS_REGISTERED || sptr->state == BLRS_DUMPING)
		{
			sprintf(server_id, "%d", sptr->serverid);
			sprintf(state, "disconnected");

			len = 5 + strlen(server_id) + strlen(state) + 1;

			if ((pkt = gwbuf_alloc(len)) == NULL) {
				MXS_ERROR("gwbuf memory allocation in "
                                          "DISCONNECT ALL for [%s], server_id [%d]",
                                          sptr->dcb->remote, sptr->serverid);

				spinlock_release(&router->lock);

				blr_slave_send_error(router, slave, "Memory allocation error for DISCONNECT ALL");

				return 1;
			}

			MXS_NOTICE("%s: Slave %s, server id %d, disconnected by %s@%s",
                                   router->service->name,
                                   sptr->dcb->remote, sptr->serverid, slave->dcb->user, slave->dcb->remote);

			ptr = GWBUF_DATA(pkt);
			encode_value(ptr, len - 4, 24);                         // Add length of data packet

			ptr += 3;
			*ptr++ = seqno++;                                       // Sequence number in response
			*ptr++ = strlen(server_id);                             // Length of result string
			strncpy((char *)ptr, server_id, strlen(server_id));     // Result string
			ptr += strlen(server_id);
			*ptr++ = strlen(state);                                 // Length of result string
			strncpy((char *)ptr, state, strlen(state));             // Result string
			ptr += strlen(state);

			slave->dcb->func.write(slave->dcb, pkt);

			sptr->state = BLRS_UNREGISTERED;
			dcb_close(sptr->dcb);

		}
		sptr = sptr->next;
	}

	spinlock_release(&router->lock);

	blr_slave_send_eof(router, slave, seqno);

	return 1;
}

 /**
 * Send a MySQL OK packet to the slave backend
 *
 * @param	router		The binlog router instance
 * @param	slave		The slave server to which we are sending the response
 *
 * @return result of a write call, non-zero if write was successful
 */

static int
blr_slave_send_ok(ROUTER_INSTANCE* router, ROUTER_SLAVE* slave)
{
GWBUF   *pkt;
uint8_t *ptr;
uint8_t ok_packet[] = {7, 0, 0, // Payload length
			1, // Seqno,
			0, // OK,
			0, 0, 2, 0, 0, 0};

	if ((pkt = gwbuf_alloc(sizeof(ok_packet))) == NULL)
		return 0;

	memcpy(GWBUF_DATA(pkt), ok_packet, sizeof(ok_packet));

	return slave->dcb->func.write(slave->dcb, pkt);
}

