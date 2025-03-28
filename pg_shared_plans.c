/*-------------------------------------------------------------------------
 *
 * pg_shared_plans.c: Implementation of plan cache in shared memory.
 *
 * This program is open source, licensed under the PostgreSQL license.
 * For license terms, see the LICENSE file.
 *
 * Copyright (C) 2021-2022: Julien Rouhaud
 *
 *-------------------------------------------------------------------------
 */


#include "postgres.h"

#include "access/parallel.h"
#include "access/relation.h"
#if PG_VERSION_NUM >= 160000
#include "catalog/pg_proc.h"
#endif
#if PG_VERSION_NUM < 130000
#include "catalog/pg_type_d.h"
#endif
#include "commands/explain.h"
#if PG_VERSION_NUM >= 130000
#include "common/hashfn.h"
#else
#include "utils/hashutils.h"
#endif
#include "optimizer/optimizer.h"
#include "optimizer/planner.h"
#include "pgstat.h"
#include "storage/ipc.h"
#include "storage/proc.h"
#if PG_VERSION_NUM >= 130000
#include "tcop/cmdtag.h"
#endif
#include "tcop/utility.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#if PG_VERSION_NUM >= 140000
#if PG_VERSION_NUM < 160000
#include "utils/queryjumble.h"
#else
#include "nodes/queryjumble.h"
#endif		/* pg16- */
#endif		/* pg14+ */
#include "utils/syscache.h"
#if PG_VERSION_NUM < 140000
#include "utils/timestamp.h"
#endif

#include "include/pg_shared_plans.h"
#include "include/pgsp_import.h"
#include "include/pgsp_rdepend.h"
#include "include/pgsp_utility.h"

#if PG_VERSION_NUM < 170000
#define hashRowType			hashTupleDesc
#endif

PG_MODULE_MAGIC;

#define PGSP_TRANCHE_NAME		"pg_shared_plans"
#define PGSP_USAGE_INIT			(1.0)
#define ASSUMED_MEDIAN_INIT		(10.0)	/* initial assumed median usage */
#define USAGE_DECREASE_FACTOR	(0.99)	/* decreased every entry_dealloc */
#define USAGE_DEALLOC_PERCENT	5		/* free this % of entries at once */

#define PLANCACHE_THRESHOLD		5

#define PGSP_USEDSMEM(size) {												\
	volatile pgspSharedState *s = (volatile pgspSharedState *) pgsp;		\
																			\
	SpinLockAcquire(&s->mutex);												\
	s->alloced_size += (size);												\
	SpinLockRelease(&s->mutex);												\
}

#define PGSP_FREEDSMEM(size) {												\
	volatile pgspSharedState *s = (volatile pgspSharedState *) pgsp;		\
																			\
	SpinLockAcquire(&s->mutex);												\
	Assert(s->alloced_size >= (size));										\
	s->alloced_size -= (size);												\
	SpinLockRelease(&s->mutex);												\
}

#define PGSP_FREERELEASEDSMEM(where,what,size,sizefield) {					\
	Assert(where->what != InvalidDsaPointer);								\
		dsa_free(pgsp_area, where->what);									\
		where->what = InvalidDsaPointer;									\
		PGSP_FREEDSMEM(size);												\
		where->sizefield = 0;												\
}

#define PGSP_TRANSFER(entry,context,field,counter) {						\
	entry->field = context->field;											\
	context->field = InvalidDsaPointer;										\
	entry->counter = context->counter;										\
	context->counter = 0;													\
}


typedef struct pgspDsaContext
{
	dsa_pointer		plan;
	dsa_pointer		rels;
	int				num_rdeps;
	size_t			len;
	int				num_rels;
	dsa_pointer		rdeps;
} pgspDsaContext;

typedef struct pgspWalkerContext
{
	uint32	constid;
	int		num_const;
} pgspWalkerContext;


/*---- Local variables ----*/

/* Saved hook values in case of unload */
static shmem_startup_hook_type prev_shmem_startup_hook = NULL;
#if PG_VERSION_NUM >= 150000
static shmem_request_hook_type prev_shmem_request_hook = NULL;
#endif
static planner_hook_type prev_planner_hook = NULL;
static ProcessUtility_hook_type prev_ProcessUtility = NULL;

/* Links to shared memory state */
pgspSharedState *pgsp = NULL;
HTAB *pgsp_hash = NULL;
dsa_area *pgsp_area = NULL;
dshash_table *pgsp_rdepend = NULL;

/*---- GUC variables ----*/

#ifdef USE_ASSERT_CHECKING
static bool pgsp_cache_all;
#endif
static bool pgsp_disable_plancache;
static bool pgsp_enabled;
static int	pgsp_max;
static int	pgsp_min_plantime;
extern int	pgsp_rdepend_max;
static bool	pgsp_ro;
static int	pgsp_threshold;
static bool pgsp_es_costs;
static int	pgsp_es_format;
static bool pgsp_es_verbose;

static void pgsp_assign_rdepend_max(int newval, void *extra);

static const struct config_enum_entry pgsp_explain_format_options[] =
{
	{"text", EXPLAIN_FORMAT_TEXT, false},
	{"json", EXPLAIN_FORMAT_JSON, false},
	{"xml", EXPLAIN_FORMAT_XML, false},
	{"yaml", EXPLAIN_FORMAT_YAML, false},
	{NULL, 0, false},
};

/*---- Function declarations ----*/

PGDLLEXPORT void _PG_init(void);

PG_FUNCTION_INFO_V1(pg_shared_plans_reset);
PG_FUNCTION_INFO_V1(pg_shared_plans_info);
PG_FUNCTION_INFO_V1(pg_shared_plans);

#if PG_VERSION_NUM >= 150000
static void pgsp_shmem_request(void);
#endif
static void pgsp_shmem_startup(void);
static PlannedStmt *pgsp_planner_hook(Query *parse,
#if PG_VERSION_NUM >= 130000
									  const char *query_string,
#endif
									  int cursorOptions,
									  ParamListInfo boundParams);
static void pgsp_ProcessUtility(PlannedStmt *pstmt, const char *queryString,
#if PG_VERSION_NUM >= 140000
								bool readOnlyTree,
#endif
								ProcessUtilityContext context,
								ParamListInfo params,
								QueryEnvironment *queryEnv,
								DestReceiver *dest,
#if PG_VERSION_NUM >= 130000
								QueryCompletion *qc
#else
								char *completionTag
#endif
								);

static void pgsp_attach_dsa(void);
static void pg_shared_plans_reset_internal(Oid userid, Oid dbid, uint64 queryid);

static void pgsp_accum_custom_plan(pgspHashKey *key, Cost cost);
static void pgsp_acquire_executor_locks(PlannedStmt *plannedstmt, bool acquire);
static bool pgsp_allocate_plan(Query *parse, PlannedStmt *stmt,
							   pgspDsaContext *context, pgspHashKey *key);
static bool pgsp_choose_cache_plan(pgspEntry *entry, bool *accum_custom_stats);
static const char *pgsp_get_plan(dsa_pointer plan);
static void pgsp_cache_plan(Query *parse, PlannedStmt *custom,
		PlannedStmt *generic, pgspHashKey *key, double plantime, int num_const);
static Size pgsp_memsize(void);
static pgspEntry *pgsp_entry_alloc(pgspHashKey *key, pgspDsaContext *context,
		double plantime, int num_const, Cost custom_cost, Cost generic_cost);
static void pgsp_entry_dealloc(void);
static void pgsp_entry_remove(pgspEntry *entry);
static bool pgsp_query_walker(Node *node, pgspWalkerContext *context);
static int entry_cmp(const void *lhs, const void *rhs);
static Datum do_showrels(dsa_pointer rels, int num_rels);
static char *do_showplans(dsa_pointer plan);


dshash_parameters pgsp_rdepend_params = {
	sizeof(pgspRdependKey),
	sizeof(pgspRdependEntry),
	pgsp_rdepend_fn_compare,
	pgsp_rdepend_fn_hash,
#if PG_VERSION_NUM >= 170000
	dshash_memcpy,
#endif
	-1, /* will be set at inittime */
};

/*
 * Module load callback
 */
void
_PG_init(void)
{
	if (!process_shared_preload_libraries_in_progress)
	{
		elog(ERROR, "This module can only be loaded via shared_preload_libraries");
		return;
	}

#if PG_VERSION_NUM >= 140000
	/*
	 * Inform the postmaster that we want to enable query_id calculation if
	 * compute_query_id is set to auto.
	 */
	EnableQueryId();
#endif

	/*
	 * Define (or redefine) custom GUC variables.
	 */
#ifdef USE_ASSERT_CHECKING
	DefineCustomBoolVariable("pg_shared_plans.cache_regular_statements",
							 "Enable or disable caching of regular statements.",
							 NULL,
							 &pgsp_cache_all,
							 false,
							 PGC_SUSET,
							 0,
							 NULL,
							 NULL,
							 NULL);
#endif

	DefineCustomBoolVariable("pg_shared_plans.disable_plan_cache",
							 "Completely bypass the core plancache for handled plans.",
							 NULL,
							 &pgsp_disable_plancache,
							 false,
							 PGC_SUSET,
							 0,
							 NULL,
							 NULL,
							 NULL);

	DefineCustomBoolVariable("pg_shared_plans.enabled",
							 "Enable or disable pg_shared_plans.",
							 NULL,
							 &pgsp_enabled,
							 true,
							 PGC_SUSET,
							 0,
							 NULL,
							 NULL,
							 NULL);

	DefineCustomIntVariable("pg_shared_plans.max",
							"Sets the maximum number of plans tracked by pg_shared_plans.",
							NULL,
							&pgsp_max,
							100,
							5,
							INT_MAX,
							PGC_POSTMASTER,
							0,
							NULL,
							NULL,
							NULL);

	DefineCustomIntVariable("pg_shared_plans.min_plan_time",
							"Sets the minimum planning time to save an entry (in ms).",
							NULL,
							&pgsp_min_plantime,
							10,
							0,
							INT_MAX,
							PGC_SUSET,
							GUC_UNIT_MS,
							NULL,
							NULL,
							NULL);

	DefineCustomBoolVariable("pg_shared_plans.read_only",
							 "Should pg_shared_plans cache new plans.",
							 NULL,
							 &pgsp_ro,
							 false,
							 PGC_USERSET,
							 0,
							 NULL,
							 NULL,
							 NULL);

	DefineCustomIntVariable("pg_shared_plans.threshold",
							"Minimum number of custom plans to generate before maybe choosing cached plans.",
							NULL,
							&pgsp_threshold,
							4,
							1,
							PLANCACHE_THRESHOLD,
							PGC_SUSET,
							0,
							NULL,
							NULL,
							NULL);

	DefineCustomIntVariable("pg_shared_plans.rdepend_max",
							"Sets the maximum number of entries to store per reverse dependency.",
							NULL,
							&pgsp_rdepend_max,
							50,
							PGSP_RDEPEND_INIT,
							10000,
							PGC_SIGHUP,
							0,
							NULL,
							pgsp_assign_rdepend_max,
							NULL);

	DefineCustomBoolVariable("pg_shared_plans.explain_costs",
							 "Display plans with COST option.",
							 NULL,
							 &pgsp_es_costs,
							 false,
							 PGC_SUSET,
							 0,
							 NULL,
							 NULL,
							 NULL);


	DefineCustomEnumVariable("pg_shared_plans.explain_format",
							 "Display plans with FORMAT option.",
							 NULL,
							 &pgsp_es_format,
							 EXPLAIN_FORMAT_TEXT,
							 pgsp_explain_format_options,
							 PGC_SUSET,
							 0,
							 NULL,
							 NULL,
							 NULL);

	DefineCustomBoolVariable("pg_shared_plans.explain_verbose",
							 "Display plans with VERBOSE option.",
							 NULL,
							 &pgsp_es_verbose,
							 false,
							 PGC_SUSET,
							 0,
							 NULL,
							 NULL,
							 NULL);

	EmitWarningsOnPlaceholders("pg_shared_plans");

#if PG_VERSION_NUM < 150000
	/*
	 * Request additional shared resources.  (These are no-ops if we're not in
	 * the postmaster process.)  We'll allocate or attach to the shared
	 * resources in pgsp_shmem_startup().
	 */
	RequestAddinShmemSpace(pgsp_memsize());
	RequestNamedLWLockTranche(PGSP_TRANCHE_NAME, 1);
#endif

	/* Install hooks */
#if PG_VERSION_NUM >= 150000
	prev_shmem_request_hook = shmem_request_hook;
	shmem_request_hook = pgsp_shmem_request;
#endif
	prev_shmem_startup_hook = shmem_startup_hook;
	shmem_startup_hook = pgsp_shmem_startup;
	prev_planner_hook = planner_hook;
	planner_hook = pgsp_planner_hook;
	prev_ProcessUtility = ProcessUtility_hook;
	ProcessUtility_hook = pgsp_ProcessUtility;
}

static void
pgsp_assign_rdepend_max(int newval, void *extra)
{
	if (!IsParallelWorker() && newval < pgsp_rdepend_max)
		elog(WARNING, "New value for pg_shared_plans.rdepend_max (%d)"
				" is lower than the previous value (%d). Existing entries"
				" won't be affected.", newval, pgsp_rdepend_max);
}

#if PG_VERSION_NUM >= 150000
static void
pgsp_shmem_request(void)
{
	if (prev_shmem_request_hook)
		prev_shmem_request_hook();

	RequestAddinShmemSpace(pgsp_memsize());
	RequestNamedLWLockTranche(PGSP_TRANCHE_NAME, 1);
}
#endif

/*
 * shmem_startup hook: allocate or attach to shared memory,
 */
static void
pgsp_shmem_startup(void)
{
	bool		found;
	HASHCTL		info;

	if (prev_shmem_startup_hook)
		prev_shmem_startup_hook();

	/* reset in case this is a restart within the postmaster */
	pgsp = NULL;
	pgsp_hash = NULL;

	/*
	 * Create or attach to the shared memory state, including hash table
	 */
	LWLockAcquire(AddinShmemInitLock, LW_EXCLUSIVE);

	pgsp = ShmemInitStruct("pg_shared_plans",
						   sizeof(pgspSharedState),
						   &found);

	if (!found)
	{
		int			trancheid;

		/* First time through ... */
		memset(pgsp, 0, sizeof(pgspSharedState));
		pgsp->lock = &(GetNamedLWLockTranche(PGSP_TRANCHE_NAME))->lock;
		pgsp->pgsp_dsa_handle = DSM_HANDLE_INVALID;
		pgsp->pgsp_rdepend_handle = InvalidDsaPointer;
		pgsp->cur_median_usage = ASSUMED_MEDIAN_INIT;
		pgsp->rdepend_num = 0;
		pgsp->alloced_size = 0;
		SpinLockInit(&pgsp->mutex);

		/* try to guess our trancheid */
		for (trancheid = LWTRANCHE_FIRST_USER_DEFINED; ; trancheid++)
		{
			if (strcmp(GetLWLockIdentifier(PG_WAIT_LWLOCK, trancheid),
					   PGSP_TRANCHE_NAME) == 0)
			{
				/* Found it! */
				break;
			}
			if ((trancheid - LWTRANCHE_FIRST_USER_DEFINED) > 50)
			{
				/* No point trying so hard, just give up. */
				trancheid = LWTRANCHE_FIRST_USER_DEFINED;
				break;
			}
		}
		Assert(trancheid >= LWTRANCHE_FIRST_USER_DEFINED);
		pgsp->LWTRANCHE_PGSP = trancheid;

	}

	info.keysize = sizeof(pgspHashKey);
	info.entrysize = sizeof(pgspEntry);
	info.hash = pgsp_hash_fn;
	info.match = pgsp_match_fn;
	pgsp_hash = ShmemInitHash("pg_shared_plans hash",
							  pgsp_max, pgsp_max,
							  &info,
							  HASH_ELEM | HASH_FUNCTION | HASH_COMPARE);

	LWLockRelease(AddinShmemInitLock);
}

static PlannedStmt *
pgsp_planner_hook(Query *parse,
#if PG_VERSION_NUM >= 130000
				  const char *query_string,
#endif
				  int cursorOptions,
				  ParamListInfo boundParams)
{
	Query		   *generic_parse = NULL, *back_parse = NULL;
	PlannedStmt	   *result, *generic;
	pgspHashKey		key;
	pgspEntry	   *entry;
	instr_time		planstart,
					planduration;
	double			plantime;
	bool			accum_custom_stats = false;
	pgspWalkerContext context;

	if (!pgsp_enabled || parse->queryId == UINT64CONST(0) ||
#if PG_VERSION_NUM >= 140000
			/* 3rd party query_id implementation may not be suitable. */
			compute_query_id == COMPUTE_QUERY_ID_OFF ||
#endif
#ifdef USE_ASSERT_CHECKING
			(!pgsp_cache_all && boundParams == NULL)
#else
			boundParams == NULL
#endif
			)
		goto fallback;

	if (parse->utilityStmt != NULL)
		goto fallback;

	/* Create or attach to the dsa. */
	pgsp_attach_dsa();

	/* We need to create a per-user entry if there are RLS */
	if (parse->hasRowSecurity)
		key.userid = GetUserId();
	else
		key.userid = InvalidOid;
	key.dbid = MyDatabaseId;
	key.queryid = parse->queryId;

	context.constid = 0;
	context.num_const = 0;

	/*
	 * Ignore if the plan is not cacheable (e.g. contains a temp table
	 * reference)
	 */
	if (pgsp_query_walker((Node *) parse, &context))
		goto fallback;

#ifdef USE_ASSERT_CHECKING
	/*
	 * If we cache regular statements, we can't rely anymore on plancache
	 * checks preventing cached plans to be reused when the tuple descriptor
	 * has changed.
	 */
	if (pgsp_cache_all)
	{
		TupleDesc	desc;
		int			i;

		desc = ExecCleanTypeFromTL(parse->targetList);
		context.constid = hash_combine(context.constid, hashRowType(desc));

		/* We also need to take into account resname. */
		for (i = 0; i < desc->natts; i++)
		{
			char *attname = TupleDescAttr(desc, i)->attname.data;

			context.constid = hash_combine(context.constid,
					hash_any((const unsigned char *)attname, strlen(attname)));
		}
	}
#endif

	key.constid = context.constid;

	/* Lookup the hash table entry with shared lock. */
	LWLockAcquire(pgsp->lock, LW_SHARED);
	entry = (pgspEntry *) hash_search(pgsp_hash, &key, HASH_FIND, NULL);

	if (entry)
	{
		int64		discard = entry->discard;
		const char *local = pgsp_get_plan(entry->plan);

		if (local != NULL)
		{
			bool	use_cached;
			int		bypass;

			use_cached = pgsp_choose_cache_plan(entry, &accum_custom_stats);

			if (use_cached)
			{
				result = (PlannedStmt *) stringToNode(local);

				LWLockRelease(pgsp->lock);
				pgsp_acquire_executor_locks(result, true);

				/*
				 * Check that the entry is still valid after acquiring the
				 * locks.
				 */
				LWLockAcquire(pgsp->lock, LW_SHARED);
				entry = (pgspEntry *) hash_search(pgsp_hash, &key, HASH_FIND,
												  NULL);

				if (entry == NULL || entry->plan == InvalidDsaPointer ||
						entry->discard != discard)
					use_cached = false;
				else
					bypass = entry->bypass;

				entry = NULL;
				LWLockRelease(pgsp->lock);
			}

			/* Entry is still valid, keep going. */
			if (use_cached)
			{
				Cost	total_diff;
				Cost	diff;
				int		nb_rels;

				/*
				 * If our threshold is greater or equal than the plancache one,
				 * we won't be able to bypass it, so just return our plan as
				 * is.
				 *
				 * Note that this should not happen as our heuristics to choose
				 * a generic plan is the same as plancache.  So if plancache
				 * decided that a generic plan isn't suitable so should we.
				 */
				if (pgsp_threshold >= PLANCACHE_THRESHOLD)
					return result;

				/*
				 * Nullify plancache heuristics to make it believe that a
				 * custom plan (our shared cached one) should be preferred over
				 * a generic one.  This will unfortunately only work if the
				 * query is otherwise costly enough, as we don't want to return
				 * a negative cost unless pgsp_disable_plancache is set.
				 */
				nb_rels = list_length(result->rtable);

				/* Compute the total additional cost plancache will add. */
				total_diff = (1000.0 * cpu_operator_cost * (nb_rels + 1)) *
							  PLANCACHE_THRESHOLD;

				/*
				 * Compute how much that represent for the number of times
				 * we'll return our shared cached plan.
				 */
				diff = total_diff / (PLANCACHE_THRESHOLD - pgsp_threshold);

				/* And add a safety margin, just in case. */
				diff += 0.01;

				if (pgsp_disable_plancache)
				{
					/*
					 * If we already adjusted the cost enough to have an
					 * average custom cost less than -1, just use the negative
					 * cost to display a value that can be a bit more helpful,
					 * otherwise keep adjusting the accumulated diff cost to
					 * make sure that the average cost will be less than -1.
					 */
					if (bypass > (PLANCACHE_THRESHOLD - pgsp_threshold))
						diff = result->planTree->total_cost * 2;
					else
						diff += result->planTree->total_cost * 2 *
							pgsp_threshold;
				}

				/*
				 * And finally remove that from the plan's total cost, making
				 * sure that the total cost isn't negative if
				 * pgsp_disable_plancache isn't set.
				 */
				result->planTree->total_cost -= diff;
				if (!pgsp_disable_plancache)
				{
					if (result->planTree->total_cost <= 0.0)
						result->planTree->total_cost = 0.001;
				}

				return result;
			}
		}
		else
		{
			/*
			 * Plan was discarded, clear entry pointer.  Later code will
			 * detect the missing plan and save a fresh new one.
			 */
			entry = NULL;
		}
	}

	LWLockRelease(pgsp->lock);

	if (!entry)
	{
		generic_parse = copyObject(parse);
		back_parse = copyObject(parse);
		INSTR_TIME_SET_CURRENT(planstart);
	}

	if (prev_planner_hook)
		result = (*prev_planner_hook) (parse,
#if PG_VERSION_NUM >= 130000
									   query_string,
#endif
									   cursorOptions, boundParams);
	else
		result = standard_planner(parse,
#if PG_VERSION_NUM >= 130000
								  query_string,
#endif
								  cursorOptions, boundParams);

	if(!entry)
	{
		INSTR_TIME_SET_CURRENT(planduration);
		INSTR_TIME_SUBTRACT(planduration, planstart);
		plantime = INSTR_TIME_GET_DOUBLE(planduration) * 1000.0;
	}

	/* Save the plan if no one did it yet */
	if (!entry && plantime >= pgsp_min_plantime)
	{
		Assert(back_parse != NULL && generic_parse != NULL);
		/* Generate a generic plan */
		generic = standard_planner(generic_parse,
#if PG_VERSION_NUM >= 130000
								   query_string,
#endif
								   cursorOptions, NULL);
		pgsp_cache_plan(back_parse, result, generic, &key, plantime,
				context.num_const);
	}
	else if (accum_custom_stats)
	{
		Cost custom_cost = pgsp_cached_plan_cost(result, true);

		pgsp_accum_custom_plan(&key, custom_cost);
	}

	Assert(!LWLockHeldByMe(pgsp->lock));
	return result;

fallback:
	Assert(!LWLockHeldByMe(pgsp->lock));
	if (prev_planner_hook)
		return (*prev_planner_hook) (parse,
#if PG_VERSION_NUM >= 130000
									 query_string,
#endif
									 cursorOptions, boundParams);
	else
		return standard_planner(parse,
#if PG_VERSION_NUM >= 130000
								query_string,
#endif
								cursorOptions, boundParams);
}

/*
 * Inspect the UTILITY command being run and discard cached plan as needed.
 *
 * There is no guarantee that the transaction will eventually commit, so we may
 * discard a plan for nothing, but this should hopfully not be a common case.
 * Also the current backend can't cache new plans until its transaction is
 * finished, as it could otherwise leave invalid or inefficient plans if its
 * transaction is rollbacked.
 * Other backend won't be able to process any query against the impacted
 * relations until the transaction is finished as they will be blocked by the
 * AccessExclusiveLock acquired by this UTILITY, so there's no risk to create
 * invalid / inefficient plans from other backends.
 *
 * FIXME: this is far from being finished.  Minimal list of things that still
 * needs to be handled:
 *
 * - find all the impacted relations in case of CASCADEd utility
 * - only RELATIONs are handled.  What about functions, operators...
 * - client should not be able to override pg_shared_plans.read_only if we set
 *   it here.
 */
static void
pgsp_ProcessUtility(PlannedStmt *pstmt, const char *queryString,
#if PG_VERSION_NUM >= 140000
					bool readOnlyTree,
#endif
					ProcessUtilityContext context,
					ParamListInfo params,
					QueryEnvironment *queryEnv,
					DestReceiver *dest,
#if PG_VERSION_NUM >= 130000
					QueryCompletion *qc
#else
					char *completionTag
#endif
					)
{
	Node   *parsetree = pstmt->utilityStmt;
	pgspUtilityContext util;

	/* Create or attach to the dsa. */
	pgsp_attach_dsa();

	memset(&util, 0, sizeof(pgspUtilityContext));

	/*
	 * Process all nodes that have to be processed before the UTILITY
	 * execution, mostly DROP commands.
	 */
	pgsp_utility_pre_exec(parsetree, &util);

	/* Process the populated util.oids_lock if any. */
	pgsp_utility_do_lock(&util);

	/* Run the utility. */
	if (prev_ProcessUtility)
		prev_ProcessUtility(pstmt, queryString,
#if PG_VERSION_NUM >= 140000
				readOnlyTree,
#endif
				context, params, queryEnv,
				dest,
#if PG_VERSION_NUM >= 130000
				qc
#else
				completionTag
#endif
				);
	else
		standard_ProcessUtility(pstmt, queryString,
#if PG_VERSION_NUM >= 140000
				readOnlyTree,
#endif
				context, params, queryEnv,
				dest,
#if PG_VERSION_NUM >= 130000
				qc
#else
				completionTag
#endif
				);

	if (util.reset_current_db)
	{
		Assert(IsA(parsetree, AlterTSDictionaryStmt));
		Assert(!util.has_discard && !util.has_remove && !util.has_lock);

		pg_shared_plans_reset_internal(InvalidOid, MyDatabaseId,
				UINT64CONST (0));

		return;
	}

	/*
	 * Now that the command completed, process the rest of the nodes that
	 * weren't processed before the execution.
	 */
	pgsp_utility_post_exec(parsetree, &util);

	/*
	 * Discard any saved plan, or drop the entry referencing the underlying
	 * relation, or unlock the required entries depending on the original
	 * UTILITY.
	 */
	if (util.has_discard || util.has_remove || util.has_lock)
	{
		pgspOidsEntry  *entry;
		HASH_SEQ_STATUS oids_seq;
		ListCell	   *lc;

		if (util.has_lock)
			Assert(!util.has_discard && !util.has_remove);
		else
			LWLockAcquire(pgsp->lock, LW_EXCLUSIVE);

		hash_seq_init(&oids_seq, util.oids_hash);
		while ((entry = hash_seq_search(&oids_seq)) != NULL)
		{
			if (entry->key.kind != PGSP_DISCARD &&
				entry->key.kind != PGSP_EVICT)
				continue;

			foreach(lc, entry->oids)
			{
				pgsp_evict_by_oid(MyDatabaseId, entry->key.classid,
								  lfirst_oid(lc),
								  entry->key.kind);
			}
		}
		/*
		 * Also make sure that we don't cache a new plan as we don't know if
		 * the transaction will commit or not.
		 */
		set_config_option("pg_shared_plans.read_only", "on", PGC_USERSET,
				PGC_S_SESSION, GUC_ACTION_LOCAL, true, 0, false);

		LWLockRelease(pgsp->lock);
	}
}

/*
 * Create the dynamic shared area or attach to it.
 */
static void
pgsp_attach_dsa(void)
{
	MemoryContext	oldcontext;

	Assert(!LWLockHeldByMe(pgsp->lock));

	/* Nothing to do if we're already attached to the dsa. */
	if (pgsp_area != NULL)
	{
		Assert(pgsp_rdepend != NULL);
		return;
	}

	oldcontext = MemoryContextSwitchTo(TopMemoryContext);

	LWLockAcquire(pgsp->lock, LW_EXCLUSIVE);
	if (pgsp->pgsp_dsa_handle == DSM_HANDLE_INVALID)
	{
		pgsp_area = dsa_create(pgsp->LWTRANCHE_PGSP);
		dsa_pin(pgsp_area);
		pgsp->pgsp_dsa_handle = dsa_get_handle(pgsp_area);
	}
	else
		pgsp_area = dsa_attach(pgsp->pgsp_dsa_handle);

	dsa_pin_mapping(pgsp_area);

	pgsp_rdepend_params.tranche_id = pgsp->LWTRANCHE_PGSP;
	if (pgsp->pgsp_rdepend_handle == InvalidDsaPointer)
	{
		pgsp_rdepend = dshash_create(pgsp_area, &pgsp_rdepend_params, NULL);
		pgsp->pgsp_rdepend_handle = dshash_get_hash_table_handle(pgsp_rdepend);
	}
	else
	{
		pgsp_rdepend = dshash_attach(pgsp_area, &pgsp_rdepend_params,
									 pgsp->pgsp_rdepend_handle, NULL);
	}
	LWLockRelease(pgsp->lock);

	MemoryContextSwitchTo(oldcontext);

	Assert(pgsp_area != NULL);
}

static void
pg_shared_plans_reset_internal(Oid userid, Oid dbid, uint64 queryid)
{
	HASH_SEQ_STATUS hash_seq;
	pgspEntry  *entry;
	long		num_entries;
	long		num_remove = 0;
	//pgspHashKey key;

	/* Create or attach to the dsa. */
	pgsp_attach_dsa();

	LWLockAcquire(pgsp->lock, LW_EXCLUSIVE);
	num_entries = hash_get_num_entries(pgsp_hash);

	/* No fastpath yet, should user care of a specific constid? */
	//if (userid != 0 && dbid != 0 && queryid != UINT64CONST(0))
	//{
	//	/* If all the parameters are available, use the fast path. */
	//	key.userid = userid;
	//	key.dbid = dbid;
	//	key.queryid = queryid;

	//	/* Remove the key if exists */
	//	entry = (pgspEntry *) hash_search(pgsp_hash, &key, HASH_FIND, NULL);

	//	if (entry)
	//	{
	//		pgsp_entry_remove(entry);
	//		num_remove++;
	//	}
	//}
	//else
	if (userid != 0 || dbid != 0 || queryid != UINT64CONST(0))
	{
		/* Remove entries corresponding to valid parameters. */
		hash_seq_init(&hash_seq, pgsp_hash);
		while ((entry = hash_seq_search(&hash_seq)) != NULL)
		{
			if ((!userid || entry->key.userid == userid) &&
				(!dbid || entry->key.dbid == dbid) &&
				(!queryid || entry->key.queryid == queryid))
			{
				pgsp_entry_remove(entry);
				num_remove++;
			}
		}
	}
	else
	{
		/* Remove all entries. */
		hash_seq_init(&hash_seq, pgsp_hash);
		while ((entry = hash_seq_search(&hash_seq)) != NULL)
		{
			pgsp_entry_remove(entry);
			num_remove++;
		}
	}

	/* All entries are removed? */
	if (num_entries == num_remove)
	{
		volatile pgspSharedState *s = (volatile pgspSharedState *) pgsp;

		/*
		 * Reset global statistics for pg_shared_plans since all entries are
		 * removed.
		 */
		TimestampTz stats_reset = GetCurrentTimestamp();

		SpinLockAcquire(&s->mutex);
		s->dealloc = 0;
		s->stats_reset = stats_reset;
		SpinLockRelease(&s->mutex);
	}

	LWLockRelease(pgsp->lock);
}

/*
 * Accumulate statistics for custom planing.  Caller mustn't hold the LWLock.
 *
 * Note that even though caller should only call that function if the number of
 * custom plans hasn't reached pgsp_threshold, it may not be the case anymore
 * when we acquire the spinlock.  In that case we still accumulate the data as
 * we generated a custom plan, so it seems worthy to have more preciser
 * information about it.
 */
static void
pgsp_accum_custom_plan(pgspHashKey *key, Cost custom_cost)
{
	pgspEntry *entry;

	Assert(!LWLockHeldByMe(pgsp->lock));
	LWLockAcquire(pgsp->lock, LW_SHARED);

	entry = hash_search(pgsp_hash, key, HASH_FIND, NULL);
	if (entry)
	{
		volatile pgspEntry *e = (volatile pgspEntry *) entry;

		SpinLockAcquire(&e->mutex);
		e->total_custom_cost += custom_cost;
		e->num_custom_plans += 1;
		SpinLockRelease(&e->mutex);
	}

	LWLockRelease(pgsp->lock);
}

/*
 * Acquire locks needed for execution of a cached plan;
 * or release them if acquire is false.
 *
 * Based on AcquireExecutorLocks()
 */
static void
pgsp_acquire_executor_locks(PlannedStmt *plannedstmt, bool acquire)
{
	ListCell   *lc2;

	if (plannedstmt->commandType == CMD_UTILITY)
	{
		/*
		 * Ignore utility statements, except those (such as EXPLAIN) that
		 * contain a parsed-but-not-planned query.  Note: it's okay to use
		 * ScanQueryForLocks, even though the query hasn't been through
		 * rule rewriting, because rewriting doesn't change the query
		 * representation.
		 */
		Query	   *query = UtilityContainsQuery(plannedstmt->utilityStmt);

		if (query)
			pgsp_ScanQueryForLocks(query, acquire);
		return;
	}

	foreach(lc2, plannedstmt->rtable)
	{
		RangeTblEntry *rte = (RangeTblEntry *) lfirst(lc2);

		if (rte->rtekind != RTE_RELATION)
			continue;

		/*
		 * Acquire the appropriate type of lock on each relation OID. Note
		 * that we don't actually try to open the rel, and hence will not
		 * fail if it's been dropped entirely --- we'll just transiently
		 * acquire a non-conflicting lock.
		 */
		if (acquire)
			LockRelationOid(rte->relid, rte->rellockmode);
		else
			UnlockRelationOid(rte->relid, rte->rellockmode);
	}
}

#define PGSP_ITEM_NOT_HANDLED(i)	((i)->cacheId != TYPEOID && \
									(i)->cacheId != PROCOID)
static bool
pgsp_allocate_plan(Query *parse, PlannedStmt *stmt, pgspDsaContext *context,
				   pgspHashKey *key)
{
	char	   *local;
	char	   *serialized;
	List	   *oids = NIL;
	List	   *invalItems = NIL, *rels = NIL;
	bool		hasRowSecurity;
	Oid		   *array = NULL;
	ListCell   *lc;
	bool		ok = true;
	int			i;
	int			nb_alloced_rels = 0;
	int			nb_alloced_inval;
	pgspRdependKey *rdeps, *rdeps_tmp;

	Assert(!LWLockHeldByMe(pgsp->lock));
	Assert(context->plan == InvalidDsaPointer);
	Assert(context->rels == InvalidDsaPointer);
	Assert(pgsp_area != NULL);

	/* First, allocate space to save a serialized version of the plan. */
	serialized = nodeToString(stmt);
	context->len = strlen(serialized) + 1;

	context->plan = dsa_allocate_extended(pgsp_area,
										  context->len,
										  DSA_ALLOC_NO_OOM);

	/* If we couldn't allocate memory for the plan, inform caller. */
	if (context->plan == InvalidDsaPointer)
		return false;

	PGSP_USEDSMEM(context->len);

	local = dsa_get_address(pgsp_area, context->plan);
	Assert(local != NULL);

	/* And copy the plan. */
	memcpy(local, serialized, context->len);

	/* Compute base relations the plan is referencing. */
	foreach(lc, stmt->rtable)
	{
		RangeTblEntry  *rte = lfirst_node(RangeTblEntry, lc);

		/* We only need to add dependency for real relations. */
		if (rte->rtekind != RTE_RELATION
#if PG_VERSION_NUM >= 160000
				&& !(rte->rtekind == RTE_SUBQUERY && OidIsValid(rte->relid))
#endif
		   )
		{
			continue;
		}

		Assert(OidIsValid(rte->relid));
		oids = list_append_unique_oid(oids, rte->relid);
	}
	context->num_rels = list_length(oids);

	/* Save the list of relations in shared memory if any. */
	if (list_length(oids) != 0)
	{
		size_t array_len;

		Assert(oids != NIL);

		array_len = sizeof(Oid) * list_length(oids);
		context->rels = dsa_allocate_extended(pgsp_area,
											  array_len,
											  DSA_ALLOC_NO_OOM);

		/*
		 * If we couldn't allocate memory for the rels, inform caller but only
		 * after releasing the plan we just alloc'ed.
		 */
		if (context->rels == InvalidDsaPointer)
		{
			ok = false;
			goto free_plan;
		}

		PGSP_USEDSMEM(array_len);

		array = dsa_get_address(pgsp_area, context->rels);

		i = 0;
		foreach(lc, oids)
			array[i++] = lfirst_oid(lc);

		Assert(i == list_length(oids));
	}

	LWLockAcquire(pgsp->lock, LW_EXCLUSIVE);

	/* Save the list of relation dependencies */
	for (i = 0; i < context->num_rels; i++)
	{
		ok = pgsp_entry_register_rdepend(MyDatabaseId, RELOID, array[i],
										 key);

		if (!ok)
		{
			/* We'll have to unregister up to previous relation. */
			nb_alloced_rels = i;
			goto free_rels;
		}
	}
	/* We'll have to unregister all relations. */
	nb_alloced_rels = i;

	/* Also save handled PlanInvanItem dependencies. */
	extract_query_dependencies((Node *) parse, &rels, &invalItems,
			&hasRowSecurity);
	invalItems = list_concat(invalItems, stmt->invalItems);

	nb_alloced_inval = 0;
	rdeps_tmp = (pgspRdependKey *) palloc(sizeof(pgspRdependKey) *
			list_length(invalItems));
	foreach(lc, invalItems)
	{
		PlanInvalItem *item = (PlanInvalItem *) lfirst(lc);

		if (PGSP_ITEM_NOT_HANDLED(item))
			continue;

		ok = pgsp_entry_register_rdepend(MyDatabaseId, item->cacheId,
										 item->hashValue, key);
		if (!ok)
			goto free_invals;

		rdeps_tmp[nb_alloced_inval].dbid = MyDatabaseId;
		rdeps_tmp[nb_alloced_inval].classid = item->cacheId;
		rdeps_tmp[nb_alloced_inval].oid = item->hashValue;
		nb_alloced_inval++;
	}

	if (nb_alloced_inval > 0)
	{
		context->rdeps = dsa_allocate_extended(pgsp_area,
				sizeof(pgspRdependKey) * nb_alloced_inval,
				DSA_ALLOC_NO_OOM);

		if (context->rdeps == InvalidDsaPointer)
		{
			ok = false;
			goto free_invals;
		}

		PGSP_USEDSMEM(nb_alloced_inval * sizeof(pgspRdependKey));

		rdeps = dsa_get_address(pgsp_area, context->rdeps);
		memcpy(rdeps, rdeps_tmp, sizeof(pgspRdependKey) * nb_alloced_inval);
	}

	context->num_rdeps = nb_alloced_inval;

free_invals:
	/*
	 * If we couldn't register a type dependency, free all previously
	 * allocated shared memory.
	 */
	if (!ok && nb_alloced_inval > 0)
	{
		Assert(context->rdeps == InvalidDsaPointer);

		i = 0;
		foreach(lc, invalItems)
		{
			PlanInvalItem *item = (PlanInvalItem *) lfirst(lc);

			if (PGSP_ITEM_NOT_HANDLED(item))
				continue;

			pgsp_entry_unregister_rdepend(MyDatabaseId, item->cacheId,
										  item->hashValue, key);

			i++;
			if (i > nb_alloced_inval)
				break;
		}
	}

free_rels:
	/*
	 * If we couldn't register a relation dependency, free all previously
	 * allocated shared memory.
	 */
	if (!ok && context->num_rels > 0)
	{
		Assert(context->plan != InvalidDsaPointer && context->len > 0);
		Assert(oids != NIL && list_length(oids) >= nb_alloced_rels);

		Assert(array != NULL);
		/* Free all saved rdepend. */
		for (i = 0; i < nb_alloced_rels; i++)
			pgsp_entry_unregister_rdepend(MyDatabaseId, RELOID, array[i], key);

		/* And free the array of Oid. */
		Assert(context->rels != InvalidDsaPointer);
		dsa_free(pgsp_area, context->rels);
		PGSP_FREEDSMEM(list_length(oids) * sizeof(Oid));
	}

free_plan:

	if (!ok)
	{
		/* Free the plan. */
		dsa_free(pgsp_area, context->plan);
		PGSP_FREEDSMEM(context->len);
	}

	LWLockRelease(pgsp->lock);

	return ok;
}

/*
 * Handle cache eviction.  3 mechanism are possible:
 * - PGSP_DISCARD, which will remove the cached plan but keep the entry
 * - PGSP_EVICT, which will remove the entry entirely
 * - PGSP_DISCARD_AND_LOCK/PGSP_UNLOCK, which will remove the cached plan, keep
 *   the entry but also temporarily lock the entry to prevent concurrent usage.
 *   This is needed for the UTILITY that have a CONCURRENTLY form
 */
void
pgsp_evict_by_oid(Oid dbid, Oid classid, Oid oid, pgspEvictionKind kind)
{
	pgspRdependKey		rkey = {dbid, classid, oid};
	pgspRdependEntry   *rentry;
	pgspHashKey		   *rkeys;
	size_t				size;
	int					i, num_keys;

	if (kind == PGSP_UNLOCK)
		Assert(LWLockHeldByMeInMode(pgsp->lock, LW_SHARED));
	else
		Assert(LWLockHeldByMeInMode(pgsp->lock, LW_EXCLUSIVE));

	Assert(pgsp_area != NULL);

	/*
	 * For non-rel reverse dependencies, we previously saved a hash rather than
	 * the oid as we get the value from PlanInvanItem infrastructure populated
	 * during planning.
	 */
	if (classid != RELOID)
	{
		switch (classid)
		{
			case TYPEOID:
				rkey.oid = GetSysCacheHashValue1(TYPEOID,
						ObjectIdGetDatum(oid));
				break;
			case PROCOID:
				rkey.oid = GetSysCacheHashValue1(PROCOID,
						ObjectIdGetDatum(oid));
				break;
			default:
				elog(ERROR, "rdepend classid %d not handled", classid);
		}
	}

	rentry = dshash_find(pgsp_rdepend, &rkey, true);

	if(!rentry)
		return;

	Assert(rentry->keys != InvalidDsaPointer);

	Assert(dsa_get_address(pgsp_area, rentry->keys) != NULL);

	num_keys = rentry->num_keys;
	size = sizeof(pgspHashKey) * num_keys;
	rkeys = (pgspHashKey *) palloc(size);
	memcpy(rkeys, dsa_get_address(pgsp_area, rentry->keys), size);

	/*
	 * We can release the lock now as we hold a (possibly shared) lock on
	 * pgsp->lock, so no other backend can discard plans or evict entries
	 * concurrently.
	 */
	dshash_release_lock(pgsp_rdepend, rentry);

	for(i = 0; i < num_keys; i++)
	{
		pgspEntry *entry;

		entry = hash_search(pgsp_hash, &rkeys[i], HASH_FIND, NULL);
		if (!entry)
			continue;

		if (kind == PGSP_UNLOCK)
		{
			uint32 prev_lockers PG_USED_FOR_ASSERTS_ONLY;

			prev_lockers = pg_atomic_fetch_sub_u32(&entry->lockers, 1);
			Assert(prev_lockers > 0);
		}
		else if (kind == PGSP_DISCARD_AND_LOCK)
		{
			pg_atomic_fetch_add_u32(&entry->lockers, 1);
			Assert(pg_atomic_read_u32(&entry->lockers) > 0);
		}

		if (kind != PGSP_UNLOCK)
		{
			Assert(LWLockHeldByMeInMode(pgsp->lock, LW_EXCLUSIVE));

			/* We only report a discard of a plan that was previously valid. */
			if (entry->plan != InvalidDsaPointer)
			{
				PGSP_FREERELEASEDSMEM(entry, plan, entry->len, len);

				if (kind != PGSP_EVICT)
					entry->discard++;
			}

			if(kind == PGSP_EVICT)
			{
				/* We don't hold any lock on the pgsp_rdepend at this point. */
				pgsp_entry_remove(entry);
			}
		}
	}
}

/*
 * Decide whether to use a cached plan or not, and if caller should accumulate
 * custom plan statistics.
 * Also takes care of maintaining bypass and usage counters.
 * Caller must hold a shared lock on pgsp->lock.
 */
static bool
pgsp_choose_cache_plan(pgspEntry *entry, bool *accum_custom_stats)
{
	/* Grab the spinlock while updating the counters. */
	volatile pgspEntry *e = (volatile pgspEntry *) entry;
	bool				use_cached = false;

	Assert(LWLockHeldByMeInMode(pgsp->lock, LW_SHARED));

	/*
	 * We should already have computed a custom plan, and other immutable
	 * fields values.
	 * NOTE: A plan can have a zero cost, if it's Result with a One-Time
	 * Filter: false
	 */
	Assert(e->generic_cost >= 0 && e->len > 0 && e->plan != InvalidDsaPointer
		   && e->plantime > 0);

	SpinLockAcquire(&e->mutex);

	if (e->num_custom_plans >= pgsp_threshold)
	{
		double				avg;

		avg = e->total_custom_cost / e->num_custom_plans;
		use_cached = (e->generic_cost < avg);

		if (use_cached)
		{
			e->bypass += 1;
			e->usage += e->plantime;
		}
	}
	else
	{
		/* Increment usage so that it doesn't get evicted too soon */
		e->usage += e->plantime;
		/* And tell caller to later accumulate custom plan statistics. */
		*accum_custom_stats = true;
	}
	SpinLockRelease(&e->mutex);

	return use_cached;
}

static const char *
pgsp_get_plan(dsa_pointer plan)
{
	Assert(LWLockHeldByMeInMode(pgsp->lock, LW_SHARED));
	Assert(pgsp_area != NULL);

	if (plan == InvalidDsaPointer)
		return NULL;

	return (const char *) dsa_get_address(pgsp_area, plan);
}

/*
 * Store a generic plan in shared memory, and allocate a new entry to associate
 * the plan with.
 */
static void
pgsp_cache_plan(Query *parse, PlannedStmt *custom, PlannedStmt *generic,
				pgspHashKey *key, double plantime, int num_const)
{
	pgspDsaContext context = {0};
	pgspEntry *entry PG_USED_FOR_ASSERTS_ONLY;

	Assert(!LWLockHeldByMe(pgsp->lock));

	/*
	 * We store the plan is shared memory before acquiring the lwlock.  It
	 * means that we may have to free it, but it avoids locking overhead.
	 * We don't allow interrupts here as we could otherwise leak memory
	 * permanently.
	 */
	HOLD_INTERRUPTS();
	if (!pgsp_allocate_plan(parse, generic, &context, key))
	{
		/*
		 * Don't try to allocate a new entry if we couldn't store the plan in
		 * shared memory.
		 */
		RESUME_INTERRUPTS();
		return;
	}

	LWLockAcquire(pgsp->lock, LW_EXCLUSIVE);
	entry = pgsp_entry_alloc(key, &context, plantime, num_const,
							 pgsp_cached_plan_cost(custom, true),
							 pgsp_cached_plan_cost(generic, false));
	Assert(entry);
	LWLockRelease(pgsp->lock);
	RESUME_INTERRUPTS();
}

/* Calculate a hash value for a given key. */
uint32
pgsp_hash_fn(const void *key, Size keysize)
{
	const pgspHashKey *k = (const pgspHashKey *) key;
	uint32 h;

	h = hash_combine(0, k->userid);
	h = hash_combine(h, k->dbid);
	h = hash_combine(h, k->queryid);
	h = hash_combine(h, k->constid);

	return h;
}

/* Compares two keys.  Zero means match. */
int
pgsp_match_fn(const void *key1, const void *key2, Size keysize)
{
	const pgspHashKey *k1 = (const pgspHashKey *) key1;
	const pgspHashKey *k2 = (const pgspHashKey *) key2;

	if (k1->userid == k2->userid
		&& k1->dbid == k2->dbid
		&& k1->queryid == k2->queryid
		&& k1->constid == k2->constid
	)
		return 0;
	else
		return 1;
}

/*
 * Estimate shared memory space needed.
 */
static Size
pgsp_memsize(void)
{
	Size		size;

	size = CACHELINEALIGN(sizeof(pgspSharedState));
	size = add_size(size, hash_estimate_size(pgsp_max, sizeof(pgspEntry)));

	return size;
}

/*
 * Allocate a new hashtable entry if no one did the job before, and associate
 * it with the given dsa_pointers that holds the generic plan (and the
 * underlying relations if any) for that entry.  Caller must hold an exclusive
 * lock on pgsp->lock
 */
static pgspEntry *
pgsp_entry_alloc(pgspHashKey *key, pgspDsaContext *context, double plantime,
				 int num_const, Cost custom_cost, Cost generic_cost)
{
	pgspEntry  *entry;
	bool		found;
	uint32		lockers;

	Assert(LWLockHeldByMeInMode(pgsp->lock, LW_EXCLUSIVE));
	Assert(context->plan != InvalidDsaPointer);
	Assert((context->num_rels == 0 && context->rels == InvalidDsaPointer) ||
		   (context->num_rels > 0 && context->rels != InvalidDsaPointer));
	Assert((context->num_rdeps == 0 && context->rdeps == InvalidDsaPointer) ||
		   (context->num_rdeps > 0 && context->rdeps != InvalidDsaPointer));

	/* Make space if needed */
	while (hash_get_num_entries(pgsp_hash) >= pgsp_max)
		pgsp_entry_dealloc();

	/* Find or create an entry with desired hash code */
	entry = (pgspEntry *) hash_search(pgsp_hash, key, HASH_ENTER, &found);

	if (!found)
	{
		/* New entry, initialize it */
		entry->len = context->len;
		entry->plan = context->plan;
		entry->num_rels = context->num_rels;
		entry->rels = context->rels;
		entry->num_rdeps = context->num_rdeps;
		entry->rdeps = context->rdeps;
		entry->num_const = num_const;
		entry->plantime = plantime;
		entry->generic_cost = generic_cost;
		entry->discard = 0;
		pg_atomic_init_u32(&entry->lockers, 0);

		/* re-initialize the mutex each time ... we assume no one using it */
		SpinLockInit(&entry->mutex);
		entry->bypass = 0;
		entry->usage = PGSP_USAGE_INIT;
		entry->total_custom_cost = custom_cost;
		entry->num_custom_plans = 1.0;

		/* The context DSM were moved to the entry */
		PGSP_TRANSFER(entry, context, plan, len);
		PGSP_TRANSFER(entry, context, rels, num_rels);
		PGSP_TRANSFER(entry, context, rdeps, num_rdeps);
	}
	else if (entry->plan == InvalidDsaPointer)
	{
		/*
		 * Plan was discarded, simply register the new one if the entry isn't
		 * lock.  Otherwise we're out of luck and we need to deallocate all the
		 * data.
		 */
		lockers = pg_atomic_read_u32(&entry->lockers);
		if (lockers != 0)
		{
			Oid			   *array = NULL;
			pgspRdependKey *rdeps = NULL;
			int		i;

			/* Free the plan. */
			PGSP_FREERELEASEDSMEM(context, plan, context->len, len);

			/* Free all saved rdepend. */
			if (context->num_rels > 0)
			{
				array = dsa_get_address(pgsp_area, context->rels);
				Assert(array != NULL);

				for (i = 0; i < context->num_rels; i++)
					pgsp_entry_unregister_rdepend(MyDatabaseId, RELOID,
												  array[i], key);
			}

			if (context->num_rdeps > 0)
			{
				rdeps = dsa_get_address(pgsp_area, context->rdeps);
				Assert(rdeps != NULL);

				for (i = 0; i < context->num_rdeps; i++)
				{
					Assert(rdeps[i].dbid == MyDatabaseId);
					pgsp_entry_unregister_rdepend(rdeps[i].dbid,
												  rdeps[i].classid,
												  rdeps[i].oid,
												  key);
				}
			}

			/* And free the rdepend arrays. */
			if (context->num_rels > 0)
			{
				PGSP_FREERELEASEDSMEM(context, rels,
						context->num_rels * sizeof(Oid), num_rels);
			}
			if (context->num_rdeps > 0)
			{
				PGSP_FREERELEASEDSMEM(context, rdeps,
						context->num_rdeps * sizeof(pgspRdependKey), num_rdeps);
			}
		}
		else
		{
			/* Transfer the plan to the entry */
			PGSP_TRANSFER(entry, context, plan, len);
		}
	}

	/* Free the plan if it wasn't transferred */
	if (context->plan != InvalidDsaPointer)
	{
		PGSP_FREERELEASEDSMEM(context, plan, context->len, len);
	}

	/* Update reverse dependencies */
	if (found && entry->plan != InvalidDsaPointer)
	{
		if (entry->rels != InvalidDsaPointer)
		{
			Oid *old = dsa_get_address(pgsp_area, entry->rels);
			Oid *new = NULL;
			int i;

			Assert(entry->num_rels > 0);

			if (context->rels != InvalidDsaPointer)
			{
				Assert(context->num_rels > 0);
				new = dsa_get_address(pgsp_area, context->rels);
			}
			else
			{
				Assert(context->num_rels == 0);
			}

			/* Remove rels that are no longer referenced */
			for (i = 0; i < entry->num_rels; i++)
			{
				int j;
				bool rel_found = false;

				Assert(OidIsValid(old[i]));

				for (j = 0; j < context->num_rels; j++)
				{
					if (old[i] == new[j])
					{
						rel_found = true;
						break;
					}
				}

				if (!rel_found)
				{
					pgsp_entry_unregister_rdepend(MyDatabaseId, RELOID,
												  old[i], key);
				}
			}
			PGSP_FREERELEASEDSMEM(entry, rels, entry->num_rels * sizeof(Oid),
					num_rels);
		}

		PGSP_TRANSFER(entry, context, rels, num_rels);

		if (entry->rdeps != InvalidDsaPointer)
		{
			pgspRdependKey *old = dsa_get_address(pgsp_area, entry->rdeps);
			pgspRdependKey *new = NULL;
			int i;

			Assert(entry->num_rdeps > 0);

			if (context->rdeps != InvalidDsaPointer)
			{
				Assert(context->num_rdeps > 0);
				new = dsa_get_address(pgsp_area, context->rdeps);
			}
			else
			{
				Assert(context->num_rdeps == 0);
			}

			/* Remove rdeps that are no longer referenced */
			for (i = 0; i < entry->num_rdeps; i++)
			{
				int j;
				bool rdep_found = false;

				for (j = 0; j < context->num_rdeps; j++)
				{
					if (pgsp_rdepend_fn_compare(&old[i], &new[j], 0, NULL) == 0)
					{
						rdep_found = true;
						break;
					}
				}

				if (!rdep_found)
				{
					Assert(old[i].dbid == MyDatabaseId);
					pgsp_entry_unregister_rdepend(old[i].dbid,
												  old[i].classid,
												  old[i].oid,
												  key);
				}
			}
			PGSP_FREERELEASEDSMEM(entry, rdeps,
					entry->num_rdeps * sizeof(pgspRdependKey),
					num_rdeps);
		}
		PGSP_TRANSFER(entry, context, rdeps, num_rdeps);
	}

	if (context->rels != InvalidDsaPointer)
	{
		PGSP_FREERELEASEDSMEM(context, rels,
				context->num_rels * sizeof(Oid),
				num_rels);
	}
	if (context->rdeps != InvalidDsaPointer)
	{
		PGSP_FREERELEASEDSMEM(context, rdeps,
				context->num_rdeps * sizeof(pgspRdependKey),
				num_rdeps);
	}

	/* We should always have a valid handle if the entry isn't locked */
	Assert(entry->plan != InvalidDsaPointer || lockers > 0);

	return entry;
}

/*
 * Deallocate least-used entries.
 *
 * Caller must hold an exclusive lock on pgsp->lock.
 */
static void
pgsp_entry_dealloc(void)
{
	HASH_SEQ_STATUS hash_seq;
	pgspEntry **entries;
	pgspEntry  *entry;
	int			nvictims;
	int			i;

	Assert(LWLockHeldByMeInMode(pgsp->lock, LW_EXCLUSIVE));

	/*
	 * Sort entries by usage and deallocate USAGE_DEALLOC_PERCENT of them.
	 * While we're scanning the table, apply the decay factor to the usage
	 * values, and update the mean query length.
	 *
	 * Note that the mean query length is almost immediately obsolete, since
	 * we compute it before not after discarding the least-used entries.
	 * Hopefully, that doesn't affect the mean too much; it doesn't seem worth
	 * making two passes to get a more current result.  Likewise, the new
	 * cur_median_usage includes the entries we're about to zap.
	 */

	entries = palloc(hash_get_num_entries(pgsp_hash) * sizeof(pgspEntry *));

	i = 0;

	hash_seq_init(&hash_seq, pgsp_hash);
	while ((entry = hash_seq_search(&hash_seq)) != NULL)
	{
		entries[i++] = entry;
		entry->usage *= USAGE_DECREASE_FACTOR;
	}

	/* Sort into increasing order by usage */
	qsort(entries, i, sizeof(pgspEntry *), entry_cmp);

	/* Record the (approximate) median usage */
	if (i > 0)
		pgsp->cur_median_usage = entries[i / 2]->usage;

	/* Now zap an appropriate fraction of lowest-usage entries */
	nvictims = Max(10, i * USAGE_DEALLOC_PERCENT / 100);
	nvictims = Min(nvictims, i);

	for (i = 0; i < nvictims; i++)
	{
		entry = entries[i];

		pgsp_entry_remove(entry);
	}

	pfree(entries);

	/* Increment the number of times entries are deallocated */
	{
		volatile pgspSharedState *s = (volatile pgspSharedState *) pgsp;

		SpinLockAcquire(&s->mutex);
		s->dealloc += 1;
		SpinLockRelease(&s->mutex);
	}
}

/*
 * Completely remove an entry:
 * - free associated dsa pointers and underlying memory
 * - unregister all underlying reverse dependencies entries.
 * - remove the entry itself from the hash table
 */
static void
pgsp_entry_remove(pgspEntry *entry)
{
	Assert(LWLockHeldByMeInMode(pgsp->lock, LW_EXCLUSIVE));
	Assert(pgsp_area != NULL);

	/* Free the dsa allocated memory. */
	if (entry->plan != InvalidDsaPointer)
	{
		PGSP_FREERELEASEDSMEM(entry, plan, entry->len, len);
	}

	if (entry->num_rels > 0)
	{
		Oid *array = NULL;
		int i;

		Assert(entry->rels != InvalidDsaPointer);

		array = (Oid *) dsa_get_address(pgsp_area, entry->rels);

		for(i = 0; i < entry->num_rels; i++)
			pgsp_entry_unregister_rdepend(entry->key.dbid, RELOID,
										  array[i], &entry->key);

		PGSP_FREERELEASEDSMEM(entry, rels, entry->num_rels * sizeof(Oid),
				num_rels);
	}
	else
	{
		Assert(entry->rels == InvalidDsaPointer);
	}

	if (entry->num_rdeps > 0)
	{
		pgspRdependKey *rdeps;
		int i;

		Assert(entry->rdeps != InvalidDsaPointer);

		rdeps = (pgspRdependKey *) dsa_get_address(pgsp_area, entry->rdeps);
		for (i = 0; i < entry->num_rdeps; i++)
			pgsp_entry_unregister_rdepend(rdeps[i].dbid, rdeps[i].classid,
										  rdeps[i].oid, &entry->key);

		PGSP_FREERELEASEDSMEM(entry, rdeps,
				entry->num_rdeps * sizeof(pgspRdependKey),
				num_rdeps);
	}
	else
	{
		Assert(entry->rdeps == InvalidDsaPointer);
	}

	/* And remove the hash entry. */
	hash_search(pgsp_hash, &entry->key, HASH_REMOVE, NULL);
}

/*
 * Walker function for query_tree_walker and expression_tree_walker to find
 * anything incompatible with shared plans.  The problematic things are:
 *
 * - usage of temporary tables
 *
 * It will also compute the constid, used to distinguish different queries
 * having the same queryid, in case multiple prepared statements have the same
 * normalized queries but with different hardcoded values.
 */
static bool
pgsp_query_walker(Node *node, pgspWalkerContext *context)
{
	if (!node)
		return false;

	if (IsA(node, Query))
	{
		Query *query = (Query *) node;
		ListCell *lc;

		foreach(lc, query->rtable)
		{
			RangeTblEntry *rte = lfirst_node(RangeTblEntry, lc);

			if (rte->rtekind == RTE_RELATION)
			{
				Relation rel = relation_open(rte->relid, AccessShareLock);
				bool is_temp;

				is_temp = RelationUsesLocalBuffers(rel);
				relation_close(rel, NoLock);

				if (is_temp)
					return true;

				/*
				 * pg_stat_statements doesn't compute a different queryid for
				 * underlying queries issued by rules, so we can only handle
				 * simple views having only a single _RETURN rule.
				 */
				if (rel->rd_rules)
				{
					if (get_rel_relkind(rte->relid) != RELKIND_VIEW)
						return true;

					if (rel->rd_rules->numLocks > 1)
						return true;
				}
			}

#if PG_VERSION_NUM < 140000
			/*
			 * pg_stat_statements doesn't take into account inheritance query
			 * (FROM ONLY ... / FROM ... *) before pg14.
			 */
			context->constid = hash_combine(context->constid, rte->inh);
#endif

#if PG_VERSION_NUM >= 130000 && PG_VERSION_NUM < 140000
			/*
			 * pg_stat_statements doesn't take into account the limit option
			 * (ONLY / WITH TIES) before pg14.
			 */
			context->constid = hash_combine(context->constid,
											query->limitOption);
#endif

			/*
			 * pg_stat_statements doesn't take into account the alias colnames,
			 * which may sound sensible, but some things will actually return
			 * different result if the alias changes, like row_to_json().
			 */
			if (rte->alias && rte->alias->colnames)
			{
				ListCell *lc2;

				foreach(lc2, rte->alias->colnames)
				{
					unsigned char  *colname = (unsigned char *) strVal(lfirst(lc2));
					int				len = strlen((char *) colname);

					context->constid = hash_combine(context->constid,
													hash_any(colname, len));
				}
			}
		}

		/*
		 * pg_stat_statements doesn't take into account the alias colnames,
		 * which is normal, but some things will actually return
		 * different result if the alias changes, like row_to_json().
		 */
		foreach(lc, query->targetList)
		{
			TargetEntry	   *te = (TargetEntry *) lfirst(lc);
			unsigned char  *n;
			int				len;

			if (!te->resname)
				continue;

			n = (unsigned char *) te->resname;
			len = strlen(te->resname);

			context->constid = hash_combine(context->constid,
											hash_any(n, len));
		}

		return query_tree_walker(query, pgsp_query_walker, context, 0);
	}
	else if (IsA(node, Const))
	{
		char   *r = nodeToString(node);
		int		len = strlen(r);

		context->constid = hash_combine(context->constid,
										hash_any((unsigned char *) r, len));
		context->num_const++;
	}
	else if (IsA(node, FuncExpr))
	{
		Oid			funcid = ((FuncExpr *) node)->funcid;
		AclResult	aclresult;

#if PG_VERSION_NUM >= 160000
		aclresult = object_aclcheck(ProcedureRelationId, funcid, GetUserId(),
									ACL_EXECUTE);
#else
		aclresult = pg_proc_aclcheck(funcid, GetUserId(), ACL_EXECUTE);
#endif
		/*
		 * The query is going to error out,so abort now and let
		 * standard_planner raise the error.
		 */
		if (aclresult != ACLCHECK_OK)
			return true;
	}
#if PG_VERSION_NUM < 140000
	else if (IsA(node, GroupingFunc))
	{
		GroupingFunc *gf = (GroupingFunc *) node;

		/*
		 * pg_stat_statement doesn't take into account agglevelsup until pg14.
		 */
		context->constid = hash_combine(context->constid, gf->agglevelsup);
	}
#endif
	else if (IsA(node, XmlExpr))
	{
		XmlExpr *expr = (XmlExpr *) node;

		/*
		 * pg_stat_statement doesn't take into account the name in
		 * xml(NAME foo ...) syntaxes.
		 */
		if (expr->name)
		{
			context->constid = hash_combine(context->constid,
					hash_any((unsigned char *) expr->name, strlen(expr->name)));
		}
	}
	else if (IsA(node, Param))
	{
		Param *param = (Param *) node;

		/*
		 * This probably cannot lead to wrong result unless pgsp_cache_all is
		 * enabled, but let's be safe.
		 */
		context->constid = hash_combine(context->constid, param->paramcollid);
	}

	return expression_tree_walker(node, pgsp_query_walker, context);
}

/*
 * qsort comparator for sorting into increasing usage order
 */
static int
entry_cmp(const void *lhs, const void *rhs)
{
	double		l_usage = (*(pgspEntry *const *) lhs)->usage;
	double		r_usage = (*(pgspEntry *const *) rhs)->usage;

	if (l_usage < r_usage)
		return -1;
	else if (l_usage > r_usage)
		return +1;
	else
		return 0;
}

static Datum
do_showrels(dsa_pointer rels, int num_rels)
{
	Oid		   *oids;
	Datum	   *arrayelems;
	int			i;

	Assert(LWLockHeldByMeInMode(pgsp->lock, LW_SHARED));
	Assert(rels != InvalidDsaPointer);
	Assert(num_rels > 0);
	Assert(pgsp_area != NULL);

	arrayelems = (Datum *) palloc(sizeof(Datum) * num_rels);

	oids = (Oid *) dsa_get_address(pgsp_area, rels);

	for (i = 0; i < num_rels; i++)
		arrayelems[i] = ObjectIdGetDatum(oids[i]);

	PG_RETURN_ARRAYTYPE_P(construct_array(arrayelems, num_rels, OIDOID,
						  sizeof(Oid), true, TYPALIGN_INT));
}

static char *
do_showplans(dsa_pointer plan)
{
	PlannedStmt *stmt;
	ExplainState   *es = NewExplainState();
	const char *local;

	local = pgsp_get_plan(plan);

	if (!local)
		return NULL;

	es->analyze = false;
	es->costs = pgsp_es_costs;
	es->verbose = pgsp_es_verbose;
	es->buffers = false;
#if PG_VERSION_NUM >= 130000
	es->wal = false;
#endif
	es->timing = false;
	es->summary = false;
	es->format = pgsp_es_format;

	stmt = (PlannedStmt *) stringToNode(local);

	pgsp_acquire_executor_locks(stmt, true);
	ExplainBeginOutput(es);
	ExplainOnePlan(stmt, NULL, es, "", NULL, NULL, NULL
#if PG_VERSION_NUM >= 130000
				   ,NULL
#endif
#if PG_VERSION_NUM >= 170000
				   ,NULL
#endif
				   );
	pgsp_acquire_executor_locks(stmt, false);

	return es->str->data;
}

Datum
pg_shared_plans_reset(PG_FUNCTION_ARGS)
{
	Oid			userid;
	Oid			dbid;
	uint64		queryid;

	userid = PG_GETARG_OID(0);
	dbid = PG_GETARG_OID(1);
	queryid = (uint64) PG_GETARG_INT64(2);

	if (!pgsp || !pgsp_hash)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("pg_shared_plans must be loaded via shared_preload_libraries")));

	pg_shared_plans_reset_internal(userid, dbid, queryid);

	PG_RETURN_VOID();
}

/* Number of output arguments (columns) for pg_shared_plans_info */
#define PG_SHARED_PLANS_INFO_COLS	4

/*
 * Return statistics of pg_shared_plans.
 */
Datum
pg_shared_plans_info(PG_FUNCTION_ARGS)
{
	TupleDesc	tupdesc;
	Datum		values[PG_SHARED_PLANS_INFO_COLS];
	bool		nulls[PG_SHARED_PLANS_INFO_COLS];

	if (!pgsp || !pgsp_hash)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("pg_shared_plans must be loaded via shared_preload_libraries")));

	/* Build a tuple descriptor for our result type */
	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");

	MemSet(values, 0, sizeof(values));
	MemSet(nulls, 0, sizeof(nulls));

	/* Read global statistics for pg_shared_plans */
	{
		volatile pgspSharedState *s = (volatile pgspSharedState *) pgsp;
		int i = 0;

		SpinLockAcquire(&s->mutex);
		values[i++] = Int32GetDatum(s->rdepend_num);
		values[i++] = Int64GetDatum(s->alloced_size);
		values[i++] = Int64GetDatum(s->dealloc);
		values[i++] = TimestampTzGetDatum(s->stats_reset);
		SpinLockRelease(&s->mutex);
	}

	PG_RETURN_DATUM(HeapTupleGetDatum(heap_form_tuple(tupdesc, values, nulls)));
}

#define PG_SHARED_PLANS_COLS			17
Datum
pg_shared_plans(PG_FUNCTION_ARGS)
{
	bool		showrels = PG_GETARG_BOOL(0);
	bool		showplan = PG_GETARG_BOOL(1);
	Oid			dbid = PG_GETARG_OID(2);
	Oid			relid = PG_GETARG_OID(3);
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	TupleDesc	tupdesc;
	Tuplestorestate *tupstore;
	MemoryContext per_query_ctx;
	MemoryContext oldcontext;
	pgspHashKey	   *rkeys;
	int				rkeys_max, rkeys_cpt;
	HASH_SEQ_STATUS hash_seq;
	pgspEntry  *entry;

	/* check to see if caller supports us returning a tuplestore */
	if (rsinfo == NULL || !IsA(rsinfo, ReturnSetInfo))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("set-valued function called in context that cannot accept a set")));
	if (!(rsinfo->allowedModes & SFRM_Materialize))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("materialize mode required, but it is not " \
						"allowed in this context")));

	/* Create or attach to the dsa. */
	pgsp_attach_dsa();

	/* Switch into long-lived context to construct returned data structures */
	per_query_ctx = rsinfo->econtext->ecxt_per_query_memory;
	oldcontext = MemoryContextSwitchTo(per_query_ctx);

	/* Build a tuple descriptor for our result type */
	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");

	tupstore = tuplestore_begin_heap(true, false, work_mem);
	rsinfo->returnMode = SFRM_Materialize;
	rsinfo->setResult = tupstore;
	rsinfo->setDesc = tupdesc;

	MemoryContextSwitchTo(oldcontext);

	/* Default to current database. */
	if (OidIsValid(relid) && !OidIsValid(dbid))
		dbid = MyDatabaseId;

	/*
	 * Get shared lock and iterate over the hashtable entries.
	 *
	 * With a large hash table, we might be holding the lock rather longer than
	 * one could wish.  However, this only blocks creation of new hash table
	 * entries, and the larger the hash table the less likely that is to be
	 * needed.  So we can hope this is okay.  Perhaps someday we'll decide we
	 * need to partition the hash table to limit the time spent holding any one
	 * lock.
	 */
	LWLockAcquire(pgsp->lock, LW_SHARED);

	/* Fast path if a specific relation is asked. */
	if(OidIsValid(relid))
	{
		pgspRdependKey		rkey = {dbid, RELOID, relid};
		pgspRdependEntry   *rentry;
		pgspHashKey		   *tmp_rkeys;
		Size				size;

		rentry = dshash_find(pgsp_rdepend, &rkey, false);

		/* No corresponding entry, clean up and return the tuplestore. */
		if (rentry == NULL)
		{
			LWLockRelease(pgsp->lock);
#if PG_VERSION_NUM < 170000
			/* Should be a no-op anyway. */
			tuplestore_donestoring(tupstore);
#endif
			return (Datum) 0;
		}

		rkeys_max = rentry->num_keys;
		Assert(rkeys_max > 0);
		rkeys_cpt = 0;
		tmp_rkeys = (pgspHashKey *) dsa_get_address(pgsp_area, rentry->keys);
		Assert(tmp_rkeys != NULL);

		size = sizeof(pgspHashKey) * rentry->num_keys;
		rkeys = (pgspHashKey *) palloc(size);
		memcpy(rkeys, tmp_rkeys, size);

		/*
		 * Release the rdepend entry, we'll iterate over the copy of the stored
		 * hash keys in the main loop.
		 */
		dshash_release_lock(pgsp_rdepend, rentry);
	}
	else
	{
		/* We'll go through the whole hash. */
		hash_seq_init(&hash_seq, pgsp_hash);
		rkeys = NULL;
	}

	while (true)
	{
		Datum		values[PG_SHARED_PLANS_COLS];
		bool		nulls[PG_SHARED_PLANS_COLS];
		int			i = 0;
		volatile pgspEntry *e;
		int64		queryid;
		int64		bypass;
		int64		len;
		double		plantime;
		double		total_custom_cost;
		int64		num_custom_plans;
		double		generic_cost;
		int64		discard;

		memset(values, 0, sizeof(values));
		memset(nulls, 0, sizeof(nulls));

		/*
		 * Get the next entry.  If user asked for entries with a dependency on
		 * a specific relation, keep iterating on those, otherwise it goes
		 * through the hash seq.
		 */
		if (rkeys != NULL)
		{
			if(rkeys_cpt == rkeys_max)
				break;

			entry = hash_search(pgsp_hash, &rkeys[rkeys_cpt++], HASH_FIND,
								NULL);
			Assert(entry);
		}
		else
		{
			entry = hash_seq_search(&hash_seq);
			if (!entry)
				break;
		}

		e = (volatile pgspEntry *) entry;

		queryid = entry->key.queryid;
		len = entry->len;
		plantime = entry->plantime;
		generic_cost = entry->generic_cost;
		discard = entry->discard;

		SpinLockAcquire(&e->mutex);
		bypass = entry->bypass;
		total_custom_cost = entry-> total_custom_cost;
		num_custom_plans = entry->num_custom_plans;
		SpinLockRelease(&e->mutex);

		if (OidIsValid(entry->key.userid))
			values[i++] = ObjectIdGetDatum(entry->key.userid);
		else
			nulls[i++] = true;
		values[i++] = ObjectIdGetDatum(entry->key.dbid);
		values[i++] = Int64GetDatumFast(queryid);
		if (OidIsValid(entry->key.constid))
			values[i++] = ObjectIdGetDatum(entry->key.constid);
		else
			nulls[i++] = true;
		values[i++] = Int32GetDatum(entry->num_const);
		values[i++] = Int64GetDatumFast(bypass);
		values[i++] = Int64GetDatumFast(len);
		values[i++] = Float8GetDatumFast(plantime);
		values[i++] = Float8GetDatumFast(total_custom_cost);
		values[i++] = Int64GetDatumFast(num_custom_plans);
		values[i++] = Float8GetDatumFast(generic_cost);
		values[i++] = Int32GetDatum(entry->num_rels);
		values[i++] = Int32GetDatum(entry->num_rdeps);
		values[i++] = Int64GetDatumFast(discard);
		values[i++] = UInt32GetDatum(pg_atomic_read_u32(&entry->lockers));

		if (showrels)
		{
			if (entry->num_rels == 0)
				nulls[i++] = true;
			else
				values[i++] = do_showrels(entry->rels, entry->num_rels);
		}
		else
			nulls[i++] = true;

		if (showplan)
		{
			char *local = do_showplans(entry->plan);

			if (local)
				values[i++] = CStringGetTextDatum(local);
			else
				values[i++] = CStringGetTextDatum("<discarded>");
		}
		else
			nulls[i++] = true;

		Assert(i == PG_SHARED_PLANS_COLS);
		tuplestore_putvalues(tupstore, tupdesc, values, nulls);
	}

	if (rkeys)
	{
		Assert(rkeys_cpt == rkeys_max);
		pfree(rkeys);
	}

	/* clean up and return the tuplestore */
	LWLockRelease(pgsp->lock);
#if PG_VERSION_NUM < 170000
			/* Should be a no-op anyway. */
	tuplestore_donestoring(tupstore);
#endif
	return (Datum) 0;
}
