/*
 * reputation - Provides a scoring system for "known users".
 * (C) Copyright 2015-2019 Bram Matthys (Syzop) and the UnrealIRCd team.
 * License: GPLv2
 *
 * How this works is simple:
 * Every 5 minutes the IP address of all the connected users receive
 * a point. Registered users receive 2 points every 5 minutes.
 * The total reputation score is then later used, by other modules, for
 * example to make decisions such as to reject or allow a user if the
 * server is under attack.
 * The reputation scores are saved in a database. By default this file
 * is data/reputation.db (often ~/unrealircd/data/reputation.db).
 *
 * See also https://www.unrealircd.org/docs/Connthrottle
 */

#include "unrealircd.h"

#define REPUTATION_VERSION "1.2"

#undef TEST

#undef BENCHMARK
/* Benchmark results (2GHz Xeon Skylake, compiled with -O2, Linux):
 * 10k random IP's with various expire times:
 * - load db:  23 ms
 * - expiry:    1 ms
 * - save db:   7 ms
 * 100k random IP's with various expire times:
 * - load db: 103 ms
 * - expiry:   10 ms
 * - save db:  32 ms
 * So, even for 100,000 unique IP's, the initial load of the database
 * would delay the UnrealIRCd boot process only for 0.1 second.
 * The writing of the db, which happens every 5 minutes, for such
 * amount of IP's takes 32ms (0.03 second).
 * Of course, exact figures will depend on the storage and cache.
 * That being said, the file for 100k random IP's is slightly under
 * 3MB, so not big, which likely means the timing will be similar
 * for a broad number of (storage) systems.
 */
 
#ifndef TEST
 #define BUMP_SCORE_EVERY	300
 #define DELETE_OLD_EVERY	605
 #define SAVE_DB_EVERY		902
#else
 #define BUMP_SCORE_EVERY 	3
 #define DELETE_OLD_EVERY	3
 #define SAVE_DB_EVERY		3
#endif

#ifndef CALLBACKTYPE_REPUTATION_STARTTIME
 #define CALLBACKTYPE_REPUTATION_STARTTIME 5
#endif

ModuleHeader MOD_HEADER
  = {
	"reputation",
	REPUTATION_VERSION,
	"Known IP's scoring system",
	"UnrealIRCd Team",
	"unrealircd-5",
    };

/* Defines */

#define MAXEXPIRES 10

#define REPUTATION_SCORE_CAP 10000

#define UPDATE_SCORE_MARGIN 1

#define REPUTATION_HASH_TABLE_SIZE 2048

#define Reputation(client)	moddata_client(client, reputation_md).l

/* Definitions (structs, etc.) */

struct cfgstruct {
	int expire_score[MAXEXPIRES];
	long expire_time[MAXEXPIRES];
	char *database;
};

typedef struct ReputationEntry ReputationEntry;

struct ReputationEntry {
	ReputationEntry *prev, *next;
	unsigned short score; /**< score for the user */
	long last_seen; /**< user last seen (unix timestamp) */
	int marker; /**< internal marker, not written to db */
	char ip[1]; /*< ip address */
};

/* Global variables */

static struct cfgstruct cfg; /**< Current configuration */
long reputation_starttime = 0;
long reputation_writtentime = 0;

static ReputationEntry *ReputationHashTable[REPUTATION_HASH_TABLE_SIZE];
static char siphashkey_reputation[SIPHASH_KEY_LENGTH];

static ModuleInfo ModInf;

ModDataInfo *reputation_md; /* Module Data structure which we acquire */

/* Forward declarations */
void reputation_md_free(ModData *m);
char *reputation_md_serialize(ModData *m);
void reputation_md_unserialize(char *str, ModData *m);
void config_setdefaults(void);
CMD_FUNC(reputation_cmd);
CMD_FUNC(reputationunperm);
int reputation_whois(Client *client, Client *target);
int reputation_set_on_connect(Client *client);
int reputation_pre_lconnect(Client *client);
int reputation_config_test(ConfigFile *cf, ConfigEntry *ce, int type, int *errs);
int reputation_config_run(ConfigFile *cf, ConfigEntry *ce, int type);
int reputation_config_posttest(int *errs);
static uint64_t hash_reputation_entry(char *ip);
void add_reputation_entry(ReputationEntry *e);
EVENT(delete_old_records);
EVENT(add_scores);
EVENT(save_db_evt);
void load_db(void);
void save_db(void);
int reputation_starttime_callback(void);

MOD_TEST()
{
	memcpy(&ModInf, modinfo, modinfo->size);
	memset(&cfg, 0, sizeof(cfg));
	HookAdd(modinfo->handle, HOOKTYPE_CONFIGTEST, 0, reputation_config_test);
	HookAdd(modinfo->handle, HOOKTYPE_CONFIGPOSTTEST, 0, reputation_config_posttest);
	CallbackAddEx(modinfo->handle, CALLBACKTYPE_REPUTATION_STARTTIME, reputation_starttime_callback);
	return MOD_SUCCESS;
}

MOD_INIT()
{
	ModDataInfo mreq;

	MARK_AS_OFFICIAL_MODULE(modinfo);
	ModuleSetOptions(modinfo->handle, MOD_OPT_PERM, 1);
	memset(&ReputationHashTable, 0, sizeof(ReputationHashTable));
	siphash_generate_key(siphashkey_reputation);

	memset(&mreq, 0, sizeof(mreq));
	mreq.name = "reputation";
	mreq.free = reputation_md_free;
	mreq.serialize = reputation_md_serialize;
	mreq.unserialize = reputation_md_unserialize;
	mreq.sync = 0; /* local! */
	mreq.type = MODDATATYPE_CLIENT;
	reputation_md = ModDataAdd(modinfo->handle, mreq);
	if (!reputation_md)
		abort();

	config_setdefaults();
	HookAdd(modinfo->handle, HOOKTYPE_CONFIGRUN, 0, reputation_config_run);
	HookAdd(modinfo->handle, HOOKTYPE_WHOIS, 0, reputation_whois);
	HookAdd(modinfo->handle, HOOKTYPE_HANDSHAKE, 0, reputation_set_on_connect);
	HookAdd(modinfo->handle, HOOKTYPE_PRE_LOCAL_CONNECT, 2000000000, reputation_pre_lconnect); /* (prio: last) */
	HookAdd(modinfo->handle, HOOKTYPE_REMOTE_CONNECT, -1000000000, reputation_set_on_connect); /* (prio: near-first) */
	CommandAdd(ModInf.handle, "REPUTATION", reputation_cmd, MAXPARA, CMD_USER|CMD_SERVER);
	CommandAdd(ModInf.handle, "REPUTATIONUNPERM", reputationunperm, MAXPARA, CMD_USER|CMD_SERVER);
	return MOD_SUCCESS;
}

MOD_LOAD()
{
	load_db();
	if (reputation_starttime == 0)
		reputation_starttime = TStime();
	EventAdd(ModInf.handle, "delete_old_records", delete_old_records, NULL, DELETE_OLD_EVERY*1000, 0);
	EventAdd(ModInf.handle, "add_scores", add_scores, NULL, BUMP_SCORE_EVERY*1000, 0);
	EventAdd(ModInf.handle, "save_db", save_db_evt, NULL, SAVE_DB_EVERY*1000, 0);
	return MOD_SUCCESS;
}

MOD_UNLOAD()
{
	save_db();
	return MOD_SUCCESS;
}

void config_setdefaults(void)
{
	/* data/reputation.db */
	safe_strdup(cfg.database, "reputation.db");
	convert_to_absolute_path(&cfg.database, PERMDATADIR);

	/* EXPIRES the following entries if the IP does appear for some time: */
	/* <=2 points after 1 hour */
	cfg.expire_score[0] = 2;
#ifndef TEST
	cfg.expire_time[0]   = 3600;
#else
	cfg.expire_time[0]   = 36;
#endif
	/* <=6 points after 7 days */
	cfg.expire_score[1] = 6;
	cfg.expire_time[1]   = 86400*7;
	/* ANY result that has not been seen for 30 days */
	cfg.expire_score[2] = -1;
	cfg.expire_time[2]   = 86400*30;
}

int reputation_config_test(ConfigFile *cf, ConfigEntry *ce, int type, int *errs)
{
	int errors = 0;
	ConfigEntry *cep;

	if (type != CONFIG_SET)
		return 0;
	
	/* We are only interrested in set::reputation.. */
	if (!ce || strcmp(ce->ce_varname, "reputation"))
		return 0;
	
	for (cep = ce->ce_entries; cep; cep = cep->ce_next)
	{
		if (!cep->ce_vardata)
		{
			config_error("%s:%i: blank set::reputation::%s without value",
				cep->ce_fileptr->cf_filename, cep->ce_varlinenum, cep->ce_varname);
			errors++;
			continue;
		} else
		if (!strcmp(cep->ce_varname, "database"))
		{
			convert_to_absolute_path(&cep->ce_vardata, PERMDATADIR);
		} else
		{
			config_error("%s:%i: unknown directive set::reputation::%s",
				cep->ce_fileptr->cf_filename, cep->ce_varlinenum, cep->ce_varname);
			errors++;
			continue;
		}
	}
	
	*errs = errors;
	return errors ? -1 : 1;
}

int reputation_config_run(ConfigFile *cf, ConfigEntry *ce, int type)
{
	ConfigEntry *cep;

	if (type != CONFIG_SET)
		return 0;
	
	/* We are only interrested in set::reputation.. */
	if (!ce || strcmp(ce->ce_varname, "reputation"))
		return 0;
	
	for (cep = ce->ce_entries; cep; cep = cep->ce_next)
	{
		if (!strcmp(cep->ce_varname, "database"))
		{
			safe_strdup(cfg.database, cep->ce_vardata);
		}
	}
	return 1;
}

int reputation_config_posttest(int *errs)
{
	int errors = 0;

	*errs = errors;
	return errors ? -1 : 1;
}

/** Parse database header and set variables appropriately */
int parse_db_header(char *buf)
{
	char *header=NULL, *version=NULL, *starttime=NULL, *writtentime=NULL;
	char *p=NULL;

	if (strncmp(buf, "REPDB", 5))
		return 0;

	header = strtoken(&p, buf, " ");
	if (!header)
		return 0;

	version = strtoken(&p, NULL, " ");
	if (!version || (atoi(version) != 1))
		return 0;

	starttime = strtoken(&p, NULL, " ");
	if (!starttime)
		return 0;

	writtentime = strtoken(&p, NULL, " ");
	if (!writtentime)
		return 0;

	reputation_starttime = atol(starttime);
	reputation_writtentime = atol(writtentime);

	return 1;
}

void load_db(void)
{
	FILE *fd;
	char buf[512], *p;
#ifdef BENCHMARK
	struct timeval tv_alpha, tv_beta;

	gettimeofday(&tv_alpha, NULL);
#endif

	fd = fopen(cfg.database, "r");
	if (!fd)
	{
		config_warn("WARNING: Could not open/read database '%s': %s", cfg.database, strerror(ERRNO));
		return;
	}
	
	memset(buf, 0, sizeof(buf));
	if (fgets(buf, 512, fd) == NULL)
	{
		config_error("WARNING: Database file corrupt ('%s')", cfg.database);
		fclose(fd);
		return;
	}
	
	/* Header contains: REPDB <version> <starttime> <writtentime>
	 * Where:
	 * REPDB:        Literally the string "REPDB".
	 * <version>     This is version 1 at the time of this writing.
	 * <starttime>   The time that recording of reputation started,
	 *               in other words: when this module was first loaded, ever.
	 * <writtentime> Time that the database was last written.
	 */
	if (!parse_db_header(buf))
	{
		config_error("WARNING: Cannot load database %s. Error reading header. "
		             "Database corrupt? Or are you downgrading from a newer "
		             "UnrealIRCd version perhaps? This is not supported.",
		             cfg.database);
		fclose(fd);
		return;
	}

	while(fgets(buf, 512, fd) != NULL)
	{
		char *ip = NULL, *score = NULL, *last_seen = NULL;
		ReputationEntry *e;
		
		stripcrlf(buf);
		/* Format: <ip> <score> <last seen> */
		ip = strtoken(&p, buf, " ");
		if (!ip)
			continue;
		score = strtoken(&p, NULL, " ");
		if (!score)
			continue;
		last_seen = strtoken(&p, NULL, " ");
		if (!last_seen)
			continue;
		
		e = safe_alloc(sizeof(ReputationEntry)+strlen(ip));
		strcpy(e->ip, ip); /* safe, see alloc above */
		e->score = atoi(score);
		e->last_seen = atol(last_seen);
		
		add_reputation_entry(e);
	}
	fclose(fd);

#ifdef BENCHMARK
	gettimeofday(&tv_beta, NULL);
	ircd_log(LOG_ERROR, "Reputation benchmark: LOAD DB: %lld microseconds",
		(long long)(((tv_beta.tv_sec - tv_alpha.tv_sec) * 1000000) + (tv_beta.tv_usec - tv_alpha.tv_usec)));
#endif
}

void save_db(void)
{
	FILE *fd;
	char tmpfname[512];
	int i;
	ReputationEntry *e;
#ifdef BENCHMARK
	struct timeval tv_alpha, tv_beta;

	gettimeofday(&tv_alpha, NULL);
#endif
	
#ifdef TEST
	sendto_realops("REPUTATION IS RUNNING IN TEST MODE. SAVING DB'S...");
#endif

	/* We write to a temporary file. Only to rename it later if everything was ok */
	snprintf(tmpfname, sizeof(tmpfname), "%s.tmp", cfg.database);
	
	fd = fopen(tmpfname, "w");
	if (!fd)
	{
		config_error("ERROR: Could not open/write database '%s': %s -- DATABASE *NOT* SAVED!!!", tmpfname, strerror(ERRNO));
		return;
	}

	if (fprintf(fd, "REPDB 1 %lld %lld\n", (long long)reputation_starttime, (long long)TStime()) < 0)
		goto write_fail;

	for (i = 0; i < REPUTATION_HASH_TABLE_SIZE; i++)
	{
		for (e = ReputationHashTable[i]; e; e = e->next)
		{
			if (fprintf(fd, "%s %d %lld\n", e->ip, (int)e->score, (long long)e->last_seen) < 0)
			{
write_fail:
				config_error("ERROR writing to '%s': %s -- DATABASE *NOT* SAVED!!!", tmpfname, strerror(ERRNO));
				fclose(fd);
				return;
			}
		}
	}

	if (fclose(fd) < 0)
	{
		config_error("ERROR writing to '%s': %s -- DATABASE *NOT* SAVED!!!", tmpfname, strerror(ERRNO));
		return;
	}
	
	/* Everything went fine. We rename our temporary file to the existing
	 * DB file (will overwrite), which is more or less an atomic operation.
	 */
#ifdef _WIN32
	/* The rename operation cannot be atomic on Windows as it will cause a "file exists" error */
	unlink(cfg.database);
#endif
	if (rename(tmpfname, cfg.database) < 0)
	{
		config_error("ERROR renaming '%s' to '%s': %s -- DATABASE *NOT* SAVED!!!",
			tmpfname, cfg.database, strerror(ERRNO));
		return;
	}

	reputation_writtentime = TStime();

#ifdef BENCHMARK
	gettimeofday(&tv_beta, NULL);
	ircd_log(LOG_ERROR, "Reputation benchmark: SAVE DB: %lld microseconds",
		(long long)(((tv_beta.tv_sec - tv_alpha.tv_sec) * 1000000) + (tv_beta.tv_usec - tv_alpha.tv_usec)));
#endif

	return;
}

static uint64_t hash_reputation_entry(char *ip)
{
	return siphash(ip, siphashkey_reputation) % REPUTATION_HASH_TABLE_SIZE;
}

void add_reputation_entry(ReputationEntry *e)
{
	int hashv = hash_reputation_entry(e->ip);

	AddListItem(e, ReputationHashTable[hashv]);
}

ReputationEntry *find_reputation_entry(char *ip)
{
	ReputationEntry *e;
	int hashv = hash_reputation_entry(ip);

	for (e = ReputationHashTable[hashv]; e; e = e->next)
		if (!strcmp(e->ip, ip))
			return e;

	return NULL;
}

/** Called when the user connects.
 * Locally: very early, just after the TCP/IP connection has
 * been established, before any data.
 * Remote user: early in the HOOKTYPE_REMOTE_CONNECT hook.
 */
int reputation_set_on_connect(Client *client)
{
	char *ip = client->ip;
	ReputationEntry *e;

	if (ip)
	{
		e = find_reputation_entry(ip);
		if (e)
		{
			Reputation(client) = e->score; /* SET MODDATA */
		}
	}

	return 0;
}

int reputation_pre_lconnect(Client *client)
{
	/* User will likely be accepted. Inform other servers about the score
	 * we have for this user. For more information about this type of
	 * server to server traffic, see the reputation_server_cmd function.
	 */
	ReputationEntry *e = find_reputation_entry(GetIP(client));
	sendto_server(NULL, 0, 0, NULL, ":%s REPUTATION %s %d", me.id, GetIP(client), e ? (int)e->score : 0);

	return 0;
}

EVENT(add_scores)
{
	static int marker = 0;
	char *ip;
	Client *client;
	ReputationEntry *e;

	/* This marker is used so we only bump score for an IP entry
	 * once and not twice (or more) if there are multiple users
	 * with the same IP address.
	 */
	marker += 2;

	/* These macros make the code below easier to read. Also,
	 * this explains why we just did marker+=2 and not marker++.
	 */
	#define MARKER_UNREGISTERED_USER (marker)
	#define MARKER_REGISTERED_USER (marker+1)
	
	list_for_each_entry(client, &client_list, client_node)
	{
		if (!IsUser(client))
			continue; /* skip servers, unknowns, etc.. */

		ip = client->ip;
		if (!ip)
			continue;

		e = find_reputation_entry(ip);
		if (!e)
		{
			/* Create */
			e = safe_alloc(sizeof(ReputationEntry)+strlen(ip));
			strcpy(e->ip, ip); /* safe, allocated above */
			add_reputation_entry(e);
		}

		/* If this is not a duplicate entry, then bump the score.. */
		if ((e->marker != MARKER_UNREGISTERED_USER) && (e->marker != MARKER_REGISTERED_USER))
		{
			e->marker = MARKER_UNREGISTERED_USER;
			if (e->score < REPUTATION_SCORE_CAP)
			{
				/* Regular users receive a point. */
				e->score++;
				/* Registered users receive an additional point */
				if (IsLoggedIn(client) && (e->score < REPUTATION_SCORE_CAP))
				{
					e->score++;
					e->marker = MARKER_REGISTERED_USER;
				}
			}
		} else
		if ((e->marker == MARKER_UNREGISTERED_USER) && IsLoggedIn(client) && (e->score < REPUTATION_SCORE_CAP))
		{
			/* This is to catch a special case:
			 * If there are 2 or more users with the same IP
			 * address and the first user was not registered
			 * then the IP entry only received a score bump of +1.
			 * If the 2nd user (with same IP) is a registered
			 * user then the IP should actually receive a
			 * score bump of +2 (in total).
			 */
			e->score++;
			e->marker = MARKER_REGISTERED_USER;
		}

		e->last_seen = TStime();
		Reputation(client) = e->score; /* update moddata */
	}
}

/** Is this entry expired? */
static inline int is_reputation_expired(ReputationEntry *e)
{
	int i;
	for (i = 0; i < MAXEXPIRES; i++)
	{
		if (cfg.expire_time[i] == 0)
			break; /* end of all entries */
		if ((e->score <= cfg.expire_score[i]) && (TStime() - e->last_seen > cfg.expire_time[i]))
			return 1;
	}
	return 0;
}

EVENT(delete_old_records)
{
	int i;
	ReputationEntry *e, *e_next;
#ifdef BENCHMARK
	struct timeval tv_alpha, tv_beta;

	gettimeofday(&tv_alpha, NULL);
#endif
	
	for (i = 0; i < REPUTATION_HASH_TABLE_SIZE; i++)
	{
		for (e = ReputationHashTable[i]; e; e = e_next)
		{
			e_next = e->next;
			
			if (is_reputation_expired(e))
			{
#ifdef DEBUGMODE
				ircd_log(LOG_ERROR, "Deleting expired entry for '%s' (score %hd, last seen %lld seconds ago)",
				         e->ip, e->score, (long long)(TStime() - e->last_seen));
#endif
				DelListItem(e, ReputationHashTable[i]);
				safe_free(e);
			}
		}
	}

#ifdef BENCHMARK
	gettimeofday(&tv_beta, NULL);
	ircd_log(LOG_ERROR, "Reputation benchmark: EXPIRY IN MEM: %lld microseconds",
		(long long)(((tv_beta.tv_sec - tv_alpha.tv_sec) * 1000000) + (tv_beta.tv_usec - tv_alpha.tv_usec)));
#endif
}

EVENT(save_db_evt)
{
	save_db();
}

CMD_FUNC(reputationunperm)
{
	if (!IsOper(client))
	{
		sendnumeric(client, ERR_NOPRIVILEGES);
		return;
	}

	ModuleSetOptions(ModInf.handle, MOD_OPT_PERM, 0);

	sendto_realops("%s used /REPUTATIONUNPERM. On next REHASH the module can be RELOADED or UNLOADED. "
	               "Note however that for a few minutes the scoring may be skipped, so don't do this too often.",
	               client->name);
}

int count_reputation_records(void)
{
	int i;
	ReputationEntry *e;
	int total = 0;

	for (i = 0; i < REPUTATION_HASH_TABLE_SIZE; i++)
		for (e = ReputationHashTable[i]; e; e = e->next)
			total++;

	return total;
}

CMD_FUNC(reputation_user_cmd)
{
	ReputationEntry *e;
	char *ip;

	if (!IsOper(client))
	{
		sendnumeric(client, ERR_NOPRIVILEGES);
		return;
	}
	
	if ((parc < 2) || BadPtr(parv[1]))
	{
		sendnotice(client, "Reputation module statistics:");
		sendnotice(client, "Recording for: %lld seconds (since unixtime %lld)",
			(long long)(TStime() - reputation_starttime),
			(long long)reputation_starttime);
		if (reputation_writtentime)
		{
			sendnotice(client, "Last successful db write: %lld seconds ago (unixtime %lld)",
				(long long)(TStime() - reputation_writtentime),
				(long long)reputation_writtentime);
		} else {
			sendnotice(client, "Last successful db write: never");
		}
		sendnotice(client, "Current number of records (IP's): %d", count_reputation_records());
		sendnotice(client, "-");
		sendnotice(client, "For more specific information, use: /REPUTATION [nick|IP-address]");
		return;
	}
	
	if (strchr(parv[1], '.') || strchr(parv[1], ':'))
	{
		ip = parv[1];
	} else {
		Client *target = find_person(parv[1], NULL);
		if (!target)
		{
			sendnumeric(client, ERR_NOSUCHNICK, parv[1]);
			return;
		}
		ip = target->ip;
		if (!ip)
		{
			sendnotice(client, "No IP address information available for user '%s'.", parv[1]); /* e.g. services */
			return;
		}
	}
	
	e = find_reputation_entry(ip);
	if (!e)
	{
		sendnotice(client, "No reputation record found for IP %s", ip);
		return;
	}

	sendnotice(client, "****************************************************");
	sendnotice(client, "Reputation record for IP %s:", ip);
	sendnotice(client, "    Score: %hd", e->score);
	sendnotice(client, "Last seen: %lld seconds ago (unixtime: %lld)",
		(long long)(TStime() - e->last_seen),
		(long long)e->last_seen);
	sendnotice(client, "****************************************************");
}

/** The REPUTATION server command handler.
 * Syntax: :server REPUTATION <ip> <score>
 * Where the <score> may be prefixed by an asterisk (*).
 *
 * The best way to explain this command is to illustrate by example:
 * :servera REPUTATION 1.2.3.4 0
 * Then serverb, which might have a score of 2 for this IP, will:
 * - Send back to the servera direction:  :serverb REPUTATION 1.2.3.4 *2
 *   So the original server (and direction) receive a score update.
 * - Propagate to non-servera direction: :servera REPUTATION 1.2.3.4 2
 *   So use the new higher score (2 rather than 0).
 * Then the next server may do the same. It MUST propagate to non-serverb
 * direction and MAY (again) update the score even higher.
 *
 * If the score is not prefixed by * then the server may do as above and
 * send back to the uplink an "update" of the score. If, however, the
 * score is prefixed by * then the server will NEVER send back to the
 * uplink, it may only propagate. This is to prevent loops.
 *
 * Note that some margin is used when deciding if the server should send
 * back score updates. This is defined by UPDATE_SCORE_MARGIN.
 * If this is for example set to 1 then a point difference of 1 will not
 * yield a score update since such a minor score update is not worth the
 * server to server traffic. Also, due to timing differences a score
 * difference of 1 is quite likely to hapen in normal circumstances.
 */
CMD_FUNC(reputation_server_cmd)
{
	ReputationEntry *e;
	char *ip;
	int score;
	int allow_reply;

	/* :server REPUTATION <ip> <score> */
	if ((parc < 3) || BadPtr(parv[2]))
	{
		sendnumeric(client, ERR_NEEDMOREPARAMS, "REPUTATION");
		return;
	}
	
	ip = parv[1];

	if (parv[2][0] == '*')
	{
		allow_reply = 0;
		score = atoi(parv[2]+1);
	} else {
		allow_reply = 1;
		score = atoi(parv[2]);
	}

	if (score > REPUTATION_SCORE_CAP)
		score = REPUTATION_SCORE_CAP;

	e = find_reputation_entry(ip);
	if (allow_reply && e && (e->score > score) && (e->score - score > UPDATE_SCORE_MARGIN))
	{
		/* We have a higher score, inform the client direction about it.
		 * This will prefix the score with a * so servers will never reply to it.
		 */
		sendto_one(client, NULL, ":%s REPUTATION %s *%d", me.id, parv[1], e->score);
#ifdef DEBUGMODE
		ircd_log(LOG_ERROR, "[reputation] Score for '%s' from %s is %d, but we have %d, sending back %d",
			ip, client->name, score, e->score, e->score);
#endif
		score = e->score; /* Update for propagation in the non-client direction */
	}

	/* Update our score if sender has a higher score */
	if (e && (score > e->score))
	{
#ifdef DEBUGMODE
		ircd_log(LOG_ERROR, "[reputation] Score for '%s' from %s is %d, but we have %d, updating our score to %d",
			ip, client->name, score, e->score, score);
#endif
		e->score = score;
	}

	/* If we don't have any entry for this IP, add it now. */
	if (!e && (score > 0))
	{
#ifdef DEBUGMODE
		ircd_log(LOG_ERROR, "[reputation] Score for '%s' from %s is %d, we had no entry, adding it",
			ip, client->name, score);
#endif
		e = safe_alloc(sizeof(ReputationEntry)+strlen(ip));
		strcpy(e->ip, ip); /* safe, see alloc above */
		e->score = score;
		e->last_seen = TStime();
		add_reputation_entry(e);
	}

	/* Propagate to the non-client direction (score may be updated) */
	sendto_server(client, 0, 0, NULL,
	              ":%s REPUTATION %s %s%d",
	              client->id,
	              parv[1],
	              allow_reply ? "" : "*",
	              score);
}

CMD_FUNC(reputation_cmd)
{
	if (MyUser(client))
		reputation_user_cmd(client, recv_mtags, parc, parv);
	else if (IsServer(client))
		reputation_server_cmd(client, recv_mtags, parc, parv);
}

int reputation_whois(Client *client, Client *target)
{
	int reputation = Reputation(target);

	if (!IsOper(client))
		return 0; /* only opers can see this.. */
	
	if (reputation > 0)
	{
		sendto_one(client, NULL, ":%s %d %s %s :is using an IP with a reputation score of %d",
			me.name, RPL_WHOISSPECIAL, client->name,
			target->name, reputation);
	}
	return 0;
}

void reputation_md_free(ModData *m)
{
	/* we have nothing to free actually, but we must set to zero */
	m->l = 0;
}

char *reputation_md_serialize(ModData *m)
{
	static char buf[32];
	if (m->i == 0)
		return NULL; /* not set (reputation always starts at 1) */
	snprintf(buf, sizeof(buf), "%d", m->i);
	return buf;
}

void reputation_md_unserialize(char *str, ModData *m)
{
	m->i = atoi(str);
}

int reputation_starttime_callback(void)
{
	/* NB: fix this by 2038 */
	return (int)reputation_starttime;
}
