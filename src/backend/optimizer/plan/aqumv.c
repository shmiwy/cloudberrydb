/*-------------------------------------------------------------------------
 *
 * aqumv.c
 *	  Answer Query Using Materialzed Views.
 *
 * Portions Copyright (c) 2023, HashData Technology Limited.
 * 
 * Author: Zhang Mingli <avamingli@gmail.com>
 *
 * IDENTIFICATION
 *	  src/backend/optimizer/plan/aqumv.c
 *
 *-------------------------------------------------------------------------
 */

#include "c.h"
#include "access/genam.h"
#include "access/htup_details.h"
#include "access/table.h"
#include "catalog/catalog.h"
#include "catalog/pg_class_d.h"
#include "catalog/pg_inherits.h"
#include "catalog/pg_rewrite.h"
#include "cdb/cdbllize.h"
#include "optimizer/optimizer.h"
#include "optimizer/planmain.h"
#include "optimizer/planner.h"
#include "optimizer/prep.h"
#include "optimizer/tlist.h"
#include "parser/analyze.h"
#include "parser/parsetree.h"
#include "parser/parse_node.h"
#include "tcop/tcopprot.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "nodes/pathnodes.h"
#include "nodes/pg_list.h"

RelOptInfo *answer_query_using_materialized_views(PlannerInfo *root,
												RelOptInfo *current_rel,
												query_pathkeys_callback qp_callback,
												void *qp_extra);

typedef struct
{
	int 	varno;
} aqumv_adjust_varno_context;

static bool aqumv_process_from_quals(Node *query_quals, Node *mv_quals, List** post_quals);
static void aqumv_adjust_varno(Query *parse, int delta);
static Node *aqumv_adjust_varno_mutator(Node *node, aqumv_adjust_varno_context *context);

typedef struct
{
	List	*mv_pure_vars;				/* List of pure Vars expression. */
	List	*mv_pure_vars_index; 		/* Index list of pure Vars. */
	List	*mv_nonpure_vars_index; 	/* Index list of nonpure Vars. */
	List	*mv_query_tlist;			/* mv query target list. */
	List	*mv_tlist;					/* mv relation's target list. */
	bool	has_unmatched;				/* True if we fail to rewrite an expression. */
} aqumv_equivalent_transformation_context;

static aqumv_equivalent_transformation_context* aqumv_init_context(List *view_tlist, List *mv_tlist);
static bool aqumv_process_targetlist(aqumv_equivalent_transformation_context *context, List *query_tlist, List **mv_final_tlist);
static void aqumv_process_nonpure_vars_expr(aqumv_equivalent_transformation_context* context);
static Node *aqumv_adjust_sub_matched_expr_mutator(Node *node, aqumv_equivalent_transformation_context *context);

typedef struct
{
	int	complexity;
} node_complexity_context;

typedef struct
{
	int	tlist_index;	/* Index of tlist, begin from 1 */
	int	count;			/* Count of subnodes in this expression */
} expr_to_sort;

static inline Var *copyVarFromTatgetList(List* tlist, int var_index);

/*
 * Answer Query Using Materialized Views(AQUMV).
 * This function modifies root(parse and etc.), current_rel in-place.
 */
RelOptInfo*
answer_query_using_materialized_views(PlannerInfo *root,
									RelOptInfo *current_rel,
									query_pathkeys_callback qp_callback,
									void *qp_extra)
{
	Query   		*parse = root->parse; /* Query of origin SQL. */
	Query			*mvQuery; /* Query of view. */
	Query			*mvRelQueryTree; /* Query tree of select from mv itself. */
	RelOptInfo 		*mv_final_rel = current_rel; /* Final rel after rewritten. */
	ListCell 		*lc;
	Node    		*jtnode;
	Node			*mvjtnode;
	int				varno;
	RangeTblEntry 	*rte;
	RangeTblEntry 	*mvrte;
	Relation		ruleDesc;
	Relation		matviewRel;
	SysScanDesc		rcscan;
	HeapTuple		tup;
	RewriteRule		*rule;
	Form_pg_rewrite	rewrite_tup;
	List			*actions;
	bool			need_close = false;
	PlannerInfo		*subroot;
	List			*mv_final_tlist = NIL; /* Final target list we want to rewrite to. */
	List 			*post_quals = NIL;
	aqumv_equivalent_transformation_context	*context;

	/* Group By without agg could be possible though IMMV doesn't support it yet. */
	bool can_not_use_mv = (parse->commandType != CMD_SELECT) ||
						  (parse->rowMarks != NIL) ||
						  parse->hasWindowFuncs ||
						  parse->hasDistinctOn ||
						  (parse->havingQual != NULL) ||
						  parse->hasModifyingCTE ||
						  parse->sortClause ||
						  (parse->parentStmtType == PARENTSTMTTYPE_REFRESH_MATVIEW) ||
						  (parse->parentStmtType == PARENTSTMTTYPE_CTAS) ||
						  parse->hasSubLinks;

	if (can_not_use_mv)
		return mv_final_rel;

	/*
	 * AQUMV_FIXME_MVP:
	 *	Single relation, excluding catalog/inherit/partition tables.
	 */
	if (list_length(parse->jointree->fromlist) != 1)
		return mv_final_rel;

	jtnode = (Node *) linitial(parse->jointree->fromlist);
	if (!IsA(jtnode, RangeTblRef))
		return mv_final_rel;

	varno = ((RangeTblRef *) jtnode)->rtindex;
	rte = root->simple_rte_array[varno];
	Assert(rte != NULL);

	if ((rte->rtekind != RTE_RELATION) || 
		IsSystemClassByRelid(rte->relid) ||
		has_superclass(rte->relid) ||
		has_subclass(rte->relid))
		return mv_final_rel;

	ruleDesc = table_open(RewriteRelationId, AccessShareLock);

	rcscan = systable_beginscan(ruleDesc, InvalidOid, false,
								NULL, 0, NULL);

	while (HeapTupleIsValid(tup = systable_getnext(rcscan)))
	{
		CHECK_FOR_INTERRUPTS();
		if (need_close)
			table_close(matviewRel, AccessShareLock);

		rewrite_tup = (Form_pg_rewrite) GETSTRUCT(tup);

		matviewRel = table_open(rewrite_tup->ev_class, AccessShareLock);
		need_close = true;

		/*
		 * AQUMV
		 * Currently the data of IVM is always up-to-date if there were.
		 * Take care of this when IVM defered-fefresh is supported.
		 */
		if (!(RelationIsIVM(matviewRel) && RelationIsPopulated(matviewRel)))
			continue;

		if (matviewRel->rd_rel->relhasrules == false ||
			matviewRel->rd_rules->numLocks != 1)
			continue;

		rule = matviewRel->rd_rules->rules[0];

		/* Filter a SELECT action, and not instead. */
		if ((rule->event != CMD_SELECT) || !(rule->isInstead))
			continue;

		actions = rule->actions;
		if (list_length(actions) != 1)
			continue;

		/*
		 * AQUMV
		 * We will do some Equivalet Transformation on the mvQuery which
		 * represents the mv's corresponding query.
		 *
		 * AQUMV_FIXME_MVP: mvQuery is a simple query too, like the parse query.
		 * mvQuery->sortClause is ok here, though we can't use the Order by
		 * clause of mvQuery, and we have disabled the parse->sortClause.
		 * The reason is: the Order by clause of materialized view's query is
		 * typically pointless. We can't rely on the order even we wrote the
		 * ordered data into mv, ex: some other access methods except heap.
		 * The Seqscan on a heap-storaged mv seems ordered, but it's a free lunch.
		 * A Parallel Seqscan breaks that hypothesis.
		 */
		mvQuery = copyObject(linitial_node(Query, actions));
		Assert(IsA(mvQuery, Query));
		if(mvQuery->hasAggs ||
			mvQuery->hasWindowFuncs ||
			mvQuery->hasDistinctOn ||
			mvQuery->hasModifyingCTE ||
			mvQuery->hasSubLinks)
			continue;

		if (list_length(mvQuery->jointree->fromlist) != 1)
			continue;

		mvjtnode = (Node *) linitial(mvQuery->jointree->fromlist);
		if (!IsA(mvjtnode, RangeTblRef))
			continue;

		/*
		 * AQUMV
		 * Require that the relation of mvQuery is a simple query too.
		 * We haven't do sth like: pull up sublinks or subqueries yet.
		 */
		varno = ((RangeTblRef*) mvjtnode)->rtindex;
		mvrte = rt_fetch(varno, mvQuery->rtable);
		Assert(mvrte != NULL);

		if (mvrte->rtekind != RTE_RELATION)
			continue;

		/*
		 * AQUMV_FIXME_MVP
		 * Must be same relation, recursiviely embeded mv is not supported now.
		 */
		if (mvrte->relid != rte->relid)
			continue;

		/*
		 * AQUMV_FIXME_MVP
		 * mv's query tree itself is needed to do the final replacement
		 * when we found corresponding column expression from mvQuery's
		 * TargetList by Query's.
		 * 
		 * A plain SELECT on all columns seems the easiest way, though
		 * some columns may not be needed.
		 * And we get a mvRelQueryTree represents SELECT * FROM mv.
		 */
		char *mvname = quote_qualified_identifier(get_namespace_name(RelationGetNamespace(matviewRel)),
							 RelationGetRelationName(matviewRel));
		StringInfoData query_mv;
		initStringInfo(&query_mv);
		appendStringInfo(&query_mv, "SELECT * FROM %s", mvname);
		List *raw_parsetree_list = pg_parse_query(query_mv.data);

		/*
		 * AQUMV_FIXME_MVP
		 * We should drop the mv if it has rules.
		 * Because mv's rules shouldn't apply to origin query.
		 */
		if (list_length(raw_parsetree_list) != 1)
			continue;

		ParseState *mv_pstate = make_parsestate(NULL);
		mv_pstate->p_sourcetext = query_mv.data;
		mvRelQueryTree = transformTopLevelStmt(mv_pstate, linitial(raw_parsetree_list));
		free_parsestate(mv_pstate);
		/* AQUMV_FIXME_MVP: free mvRelQueryTree? */

		subroot = (PlannerInfo *) palloc(sizeof(PlannerInfo));
		memcpy(subroot, root, sizeof(PlannerInfo));
		subroot->parent_root = root;
		/*
		 * AQUMV_FIXME_MVP:
		 * TODO: keep ECs and adjust varno?
		 */
		subroot->eq_classes = NIL;
		/* Reset subplan-related stuff */
		subroot->plan_params = NIL;
		subroot->outer_params = NULL;
		subroot->init_plans = NIL;
		if (!parse->hasAggs)
		{
			subroot->agginfos = NIL;
			subroot->aggtransinfos = NIL;
		}
		subroot->parse = mvQuery;

		/*
		 * AQUMV
		 * We have to rewrite now before we do the real Equivalent
		 * Transformation 'rewrite'.  
		 * Because actions sotored in rule is not a normal query tree,
		 * it can't be used directly, ex: new/old realtions used to
		 * refresh mv.
		 * Earse unused relatoins, keep the right one.
		 */
		foreach(lc, mvQuery->rtable)
		{
			RangeTblEntry* rtetmp = lfirst(lc);
			if ((rtetmp->relkind == RELKIND_MATVIEW) &&
				(rtetmp->alias != NULL) &&
				(strcmp(rtetmp->alias->aliasname, "new") == 0 ||
				strcmp(rtetmp->alias->aliasname,"old") == 0))
			{
				foreach_delete_current(mvQuery->rtable, lc);
			}
		}

		/*
		 * Now we have the right relation, adjust
		 * varnos in its query tree.
		 * AQUMV_FIXME_MVP: Only one single relation
		 * is supported now, we could assign varno
		 * to 1 opportunistically.
		 */
		aqumv_adjust_varno(mvQuery, 1);

		/*
		 * AQUMV_FIXME_MVP
		 * Are stable functions OK?
		 * A STABLE function cannot modify the database and is guaranteed to
		 * return the same results given the same arguments for all rows
		 * within a single statement.
		 * But AQUMV rewrites the query to a new SQL actually, though the same
		 * results is guaranteed.
		 * Its's unclear whether STABLE is OK, let's be conservative for now.
		 */
		if(contain_mutable_functions((Node *)mvQuery))
			continue;

		context = aqumv_init_context(mvQuery->targetList, mvRelQueryTree->targetList);

		/* Sort nonpure vars expression, prepare for Greedy Algorithm. */
		aqumv_process_nonpure_vars_expr(context);

		/*
		 * Process and rewrite target list, return false if failed.
		 */
		if(!aqumv_process_targetlist(context, parse->targetList, &mv_final_tlist))
			continue;

		/*
		 * We have successfully processed target list, all columns in Aggrefs could be
		 * computed from mvQuery.
		 * It's safe to set hasAggs here.
		 */
		mvQuery->hasAggs = parse->hasAggs;
		mvQuery->groupClause = parse->groupClause;
		mvQuery->groupingSets = parse->groupingSets;

		/*
		 * AQUMV
		 * Process all quals to conjunctive normal form.
		 * 
		 * We assume that the selection predicates of view and query expressions
		 * have been converted into conjunctive normal form(CNF) before we process
		 * them.
		 * AQUMV_MVP: no having quals now.
		 */
		preprocess_qual_conditions(subroot, (Node *) mvQuery->jointree);

		/*
		 * Process quals, return false if failed. 
		 * Else, post_quals are filled if there were. 
		 * Like process target list, post_quals is used later to see if we could
		 * rewrite and apply it to mv relation.
		 */
		if(!aqumv_process_from_quals(parse->jointree->quals, mvQuery->jointree->quals, &post_quals))
			continue;

		/* Rewrite post_quals, return false if failed. */
		post_quals = (List *)aqumv_adjust_sub_matched_expr_mutator((Node *)post_quals, context);
		if (context->has_unmatched)
			continue;

		/*
		 * Here! We succeed to rewrite a new SQL.
		 * Begin to replace all guts.
		 */
		mvQuery->targetList = mv_final_tlist;

		/*
		 * AQUMV
		 * NB: Update processed_tlist again in case that tlist has been changed. 
		 */
		preprocess_targetlist(subroot);

		/*
		 * AQUMV
		 * NB: Correct the final_locus as we select from another realtion now.
		 */
		PathTarget *newtarget = make_pathtarget_from_tlist(subroot->processed_tlist);
		subroot->final_locus = cdbllize_get_final_locus(subroot, newtarget);

		/* Rewrite with mv's query tree*/
		mvrte->relkind = RELKIND_MATVIEW;
		mvrte->relid = matviewRel->rd_rel->oid;
		/*
		 * AQUMV_FIXME_MVP
		 * Not sure where it's true from actions even it's not inherit tables.
		 */
		mvrte->inh = false;
		mvQuery->rtable = list_make1(mvrte); /* rewrite to SELECT FROM mv itself. */
		mvQuery->jointree->quals = (Node *)post_quals; /* Could be NULL, but doesn'y matter for now. */

		/*
		 * Build a plan of new SQL.
		 * AQUMV is cost-based, let planner decide which is better.
		 */
		mv_final_rel = query_planner(subroot, qp_callback, qp_extra);

		/* AQUMV_FIXME_MVP
		 * We don't use STD_FUZZ_FACTOR for cost comparisons like compare_path_costs_fuzzily here.
		 * The STD_FUZZ_FACTOR is used to reduce paths of a rel, and keep the significantly ones.
		 * But in AQUMV, we always have only one best path of rel at the last to compare.
		 * TODO: limit clause and startup_cost.
		 */
		if (mv_final_rel->cheapest_total_path->total_cost < current_rel->cheapest_total_path->total_cost)
		{
			root->parse = mvQuery;
			root->processed_tlist = subroot->processed_tlist;
			/*
			 * Update pathkeys which may be changed by qp_callback.
			 * Set belows if corresponding feature is supported.
			 * sort_pathkeys
			 * distinct_pathkey
			 * window_pathkeys
			 */
			root->group_pathkeys = subroot->group_pathkeys;
			root->query_pathkeys = subroot->query_pathkeys;

			/*
			 * AQUMV_FIXME_MVP
			 * Use new query's ecs.
			 * Equivalence Class is not supported now, we may lost some ECs if the mv_query has
			 * equal quals or implicit ones.
			 * But keeping them also introduces more complex as we should process them like target list.
			 * Another flaw: the generated Filter expressions by keeping them are pointless as all
			 * rows of mv have matched the filter expressions.
			 * See more in README.cbdb.aqumv
			 */
			root->eq_classes = subroot->eq_classes;
			/* Replace relation, don't close the right one. */
			current_rel = mv_final_rel;
			table_close(matviewRel, AccessShareLock);
			need_close = false;
		}
	}
	if (need_close)
		table_close(matviewRel, AccessShareLock);
	systable_endscan(rcscan);
	table_close(ruleDesc, AccessShareLock);
	
	return current_rel;
}

/*
 * AQUMV
 * Since tlist and quals rewrite are both based on mv query's tlist,
 * put all stuff into a context.
 */
static aqumv_equivalent_transformation_context*
aqumv_init_context(List *view_tlist, List *mv_tlist)
{
	aqumv_equivalent_transformation_context *context = palloc0(sizeof(aqumv_equivalent_transformation_context));
	List	*mv_pure_vars = NIL; /* TargetEntry[Var] in mv query. */
	List	*mv_pure_vars_index = NIL; /* Index of TargetEntry[Var] in mv query. */
	List	*mv_nonpure_vars_index = NIL; /* Index of nonpure[Var] expression in mv query. */
	Expr	*expr;
	ListCell *lc;

	/*
	 * Process mv_query's tlist to pure-Var and no pure-Var expressions.
	 * See details in README.cbdb.aqumv
	 */
	int i = 0;
	foreach(lc, view_tlist)
	{
		i++;
		TargetEntry* tle = lfirst_node(TargetEntry, lc);

		if (tle->resjunk)
			continue;

		expr = tle->expr;
		if(IsA(expr, Var))
		{
			mv_pure_vars = lappend(mv_pure_vars, expr);
			mv_pure_vars_index = lappend_int(mv_pure_vars_index, i);
		}
		else
			mv_nonpure_vars_index = lappend_int(mv_nonpure_vars_index, i);
	}

	context->mv_pure_vars = mv_pure_vars;
	context->mv_pure_vars_index = mv_pure_vars_index;
	context->mv_nonpure_vars_index = mv_nonpure_vars_index;
	context->mv_tlist = mv_tlist;
	context->mv_query_tlist = view_tlist;
	context->has_unmatched = false;
	return context;
}

/*
 * Process varno after we eliminate mv's actions("old" and "new" relation)
 * Correct rindex and all varnos with a delta.
 *
 * MV's actions query tree:
 *		[rtable]
 *				RangeTblEntry [rtekind=RTE_RELATION]
 *						[alias] Alias [aliasname="old"]
 *				RangeTblEntry [rtekind=RTE_RELATION]
 *						[alias] Alias [aliasname="new"]
 *				RangeTblEntry [rtekind=RTE_RELATION]
 *		[jointree]
 *				FromExpr []
 *						[fromlist]
 *								RangeTblRef [rtindex=3]
 *		[targetList]
 *				TargetEntry [resno=1 resname="c1"]
 *						Var [varno=3 varattno=1]
 *				TargetEntry [resno=2 resname="c2"]
 *						Var [varno=3 varattno=2]
 *------------------------------------------------------------------------------------------
 * MV's query tree after rewrite:
 *		[rtable]
 *				RangeTblEntry [rtekind=RTE_RELATION]
 *		[jointree]
 *				FromExpr []
 *						[fromlist]
 *								RangeTblRef [rtindex=3]
 *		[targetList]
 *				TargetEntry [resno=1 resname="c1"]
 *						Var [varno=3 varattno=1]
 *				TargetEntry [resno=2 resname="c2"]
 *						Var [varno=3 varattno=2]
 *------------------------------------------------------------------------------------------
 * MV's query tree after varno adjust:
 *		[rtable]
 *				RangeTblEntry [rtekind=RTE_RELATION]
 *		[jointree]
 *				FromExpr []
 *						[fromlist]
 *								RangeTblRef [rtindex=1]
 *		[targetList]
 *				TargetEntry [resno=1 resname="c1"]
 *						Var [varno=1 varattno=1]
 *				TargetEntry [resno=2 resname="c2"]
 *						Var [varno=1 varattno=2]
 *
 */
static void
aqumv_adjust_varno(Query* parse, int varno)
{
	aqumv_adjust_varno_context context;
	context.varno = varno;
	parse = query_tree_mutator(parse, aqumv_adjust_varno_mutator, &context, QTW_DONT_COPY_QUERY);
}

/*
 * Only for plain select * from mv;
 * All TargetEntrys are pure Var.
 * var_index start from 1
 */
static inline Var * 
copyVarFromTatgetList(List* tlist, int var_index)
{
	TargetEntry * tle = (TargetEntry *) list_nth(tlist, var_index - 1);
	Assert(IsA(tle->expr,Var));
	Var *var = copyObject((Var *)tle->expr);
	return var;
}

/* 
 * Adjust varno and rindex with delta. 
 */ 
static Node *aqumv_adjust_varno_mutator(Node *node, aqumv_adjust_varno_context *context)
{
	if (node == NULL)
		return NULL;
	if (IsA(node, Var))
		((Var *)node)->varno = context->varno;
	else if (IsA(node, RangeTblRef))
 		/* AQUMV_FIXME_MVP: currently we have only one relation */
		((RangeTblRef*) node)->rtindex = context->varno;
	return expression_tree_mutator(node, aqumv_adjust_varno_mutator, context);
}

/*
 * Compute a node complexity recursively.
 * Complexity of a node is the total times we enter walker function after all
 * subnodes are walked recursively.
 * It's used to sort the expression in mv's tlist.
 */
static bool
compute_node_complexity_walker(Node *node, node_complexity_context *context)
{
	if (node == NULL)
		return false;
	context->complexity++;
	return expression_tree_walker(node, compute_node_complexity_walker, (void *) context);
}

static int
nonpure_vars_expr_compare(const ListCell *a, const ListCell *b)
{
	expr_to_sort	*ets1 = (expr_to_sort *) lfirst(a);
	expr_to_sort	*ets2 = (expr_to_sort *) lfirst(b);
	return (ets1->count < ets2->count) ? 1 : (ets1->count == ets2->count) ? 0 : -1;
}

/*
 * In-place update order of mv_nonpure_vars_index List
 */
static void
aqumv_process_nonpure_vars_expr(aqumv_equivalent_transformation_context* context)
{
	ListCell* lc;
	List	*expr_to_sort_list = NIL;
	foreach(lc, context->mv_nonpure_vars_index)
	{
		int index = lfirst_int(lc);
		Node *expr = lfirst(list_nth_cell(context->mv_query_tlist, index -1));
		node_complexity_context *subnode_context = palloc0(sizeof(node_complexity_context));
		(void) compute_node_complexity_walker(expr, subnode_context);
		expr_to_sort *ets = palloc0(sizeof(expr_to_sort));
		ets->tlist_index = index;
		ets->count = subnode_context->complexity;
		expr_to_sort_list = lappend(expr_to_sort_list, ets);
	}

	/* Sort the expr list */
	list_sort(expr_to_sort_list, nonpure_vars_expr_compare);
	/* Reorder mv_nonpure_vars_index */
	context->mv_nonpure_vars_index = NIL;
	foreach(lc, expr_to_sort_list)
	{
		expr_to_sort *ets = (expr_to_sort *) lfirst(lc);
		context->mv_nonpure_vars_index = lappend_int(context->mv_nonpure_vars_index, ets->tlist_index);
	}
	return;
}

/*
 * Process query and materialized views' quals.
 * Return true if all mv_quals are in query_quals,
 * else return false.
 *
 * If return true, put quals in query_quals but not in mv_quals
 * into post_quals.
 *
 * Ex: create materialized view mv0 as select * from t1 where c1 = 1;
 * query: select * from t1 where c1 = 1 and c2 = 2;
 * post_quals = {c2 = 2}.
 *
 * AQUMV_FIXME_MVP: only support one relation now, so we don't need to
 * compare varno(both are 1 after aqumv_adjust_varno),
 * mv's query tree has been processed into one relation too.
 * 
 * Will return false if varattno in mv->query has different order with query's.
 * Ex: create materialized view mv0 as select c2, c1 from t1 where c1 = 1;
 * 		query: select c1, c2 from t1 where c1 = 1 and c2 = 2;
 * 
 * The returned post_quals may be or may not be used later, it's up to mv's targetList.
 * 
 */
static bool 
aqumv_process_from_quals(Node *query_quals, Node *mv_quals, List **post_quals)
{
	List *qlist = NIL;
	List *mlist = NIL;

	if (query_quals == NULL)
		return mv_quals == NULL;

	if(!IsA(query_quals, List))
		qlist = list_make1(query_quals);
	else
		qlist = (List *)query_quals;

	if (mv_quals == NULL)
	{
		*post_quals = qlist; 
		return true;
	}

	if(!IsA(mv_quals, List))
		mlist = list_make1(mv_quals);
	else
		mlist = (List *)mv_quals;

	if (list_difference(mlist, qlist) != NIL)
		return false;
	*post_quals = list_difference(qlist, mlist);
	return true;
}

/*
 * Adjust query expr's Vars
 * Replace Vars with corresponding attribute in mv relation.
 * Return a new expr after rewrite. 
 */
static Node *aqumv_adjust_sub_matched_expr_mutator(Node *node, aqumv_equivalent_transformation_context *context)
{
	if (!node || context->has_unmatched)
		return node;
	
	bool is_targetEntry = IsA(node, TargetEntry);
	Expr *node_expr = is_targetEntry ? ((TargetEntry *)node)->expr : (Expr *)node;

	/* Don't select Const results form mv, bypass it to upper when projection. */
	if (IsA(node_expr, Const))
		return is_targetEntry ? node : (Node *)node_expr;

	ListCell 	*lc = NULL;
	foreach(lc, context->mv_nonpure_vars_index)
	{
		int index = lfirst_int(lc);
		TargetEntry *tle = list_nth_node(TargetEntry, context->mv_query_tlist, index - 1);
		if(equal(node_expr, tle->expr))
		{
			Var *newVar = copyVarFromTatgetList(context->mv_tlist, index);
			newVar->location = -2; /* hack here, use -2 to indicate already rewrited by mv rel Vars. */
			if (is_targetEntry)
			{
				TargetEntry *qtle = (TargetEntry *) node;
				/*
				 * AQUMV_FIXME_MVP:
				 * resorigtbl, resorigcol, resjunck in mv_query is also rejunck in mv table itself ?
				 */
				TargetEntry *mvtle = makeTargetEntry((Expr *)newVar, qtle->resno, qtle->resname, qtle->resjunk);
				return (Node *) mvtle;
			}
			else
				return (Node *) newVar;
		}
	}

	/*
	 * We didn't find matched nonpure-Var expr.
	 * And if expr doesn't have Vars, return it to upper.
	 */
	List	*expr_vars = pull_var_clause((Node *)node_expr,
									 PVC_RECURSE_AGGREGATES |
									 PVC_RECURSE_WINDOWFUNCS |
									 PVC_INCLUDE_PLACEHOLDERS);

	/* Keep TargetEntry expr no changed in case for count(*). */
	if (expr_vars == NIL)
		return is_targetEntry ? node : (Node *)node_expr;
	list_free(expr_vars);
	
	/* Try match with mv_pure_vars_index, but do not disturb already rewrited exprs(Var->location = -2)  */
	if (IsA(node_expr, Var))
	{
		Var *var = (Var *)node_expr;
		if (var->location == -2)
			return node;
		lc = NULL;
		int i = 0;
		foreach(lc, context->mv_pure_vars)
		{
			Var *pure_var = lfirst_node(Var,lc);
			if (equal(node_expr, pure_var))
			{
				int j = list_nth_int(context->mv_pure_vars_index, i);
				Var *newvar = copyVarFromTatgetList(context->mv_tlist, j);
				if (is_targetEntry)
				{
					((TargetEntry *)node)->expr = (Expr *)newvar;
					return node;
				}
				else
					return (Node *)newvar;
			}
			i++;
		}
		context->has_unmatched = true;
	}
	
	return expression_tree_mutator(node, aqumv_adjust_sub_matched_expr_mutator, context);
}

/*
 * Process query and materialized views' target list.
 * Return true if all query_tlist are in mv_tlist.
 * else return false.
 * 
 * If return true, put tlist in mv_quals but not in query_tlist
 * into post_tlist.
 *
 * Ex: create materialized view mv0 as select c1, c2 from t1 where c1 = 1;
 * query: select c2 from t1 where c1 = 1;
 * post_tlist= {1}.
 * 
 * AQUMV_FIXME_MVP: strict match with same resno?
 * MVP0: expression replace 
 *	mv: select c1, c2 from t1 where c1 = 50;
 *	select c1 from t1 where c1 = 50 and abs(t1.c2) = 51;
 *	rewrite: select c1 from mv where abs(mv.c2) = 51; 
 *
 * MVP1: expression eliminate 
 *	mv: select c1, abs(c2) as c2 from t1 where c1 = 50;
 *	select c1 from t1 where c1 = 50 and abs(c2) = 51;
 *	rewrite: select c1 from mv where c2 = 51; 
 * 
 * mv_final_tlist is the final targetList of mvQuery.
 * 
 */
static bool
aqumv_process_targetlist(aqumv_equivalent_transformation_context *context, List *query_tlist, List **mv_final_tlist)
{
	*mv_final_tlist = (List *)aqumv_adjust_sub_matched_expr_mutator((Node *)(copyObject(query_tlist)), context);
	if (context->has_unmatched)
		pfree(*mv_final_tlist);
	
	return !context->has_unmatched;
}
