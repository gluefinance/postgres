/*-------------------------------------------------------------------------
 *
 * mergeutils.c--
 *	  Utilities for finding applicable merge clauses and pathkeys
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header$
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "nodes/pg_list.h"
#include "nodes/relation.h"

#include "optimizer/internal.h"
#include "optimizer/paths.h"
#include "optimizer/clauses.h"
#include "optimizer/ordering.h"

/*
 * group-clauses-by-order--
 *	  If a join clause node in 'restrictinfo-list' is mergejoinable, store
 *	  it within a mergeinfo node containing other clause nodes with the same
 *	  mergejoin ordering.
 *
 * 'restrictinfo-list' is the list of restrictinfo nodes
 * 'inner-relid' is the relid of the inner join relation
 *
 * Returns the new list of mergeinfo nodes.
 *
 */
List *
group_clauses_by_order(List *restrictinfo_list,
					   int inner_relid)
{
	List	   *mergeinfo_list = NIL;
	List	   *xrestrictinfo = NIL;

	foreach(xrestrictinfo, restrictinfo_list)
	{
		RestrictInfo *restrictinfo = (RestrictInfo *) lfirst(xrestrictinfo);
		MergeOrder *merge_ordering = restrictinfo->mergejoinorder;

		if (merge_ordering)
		{

			/*
			 * Create a new mergeinfo node and add it to 'mergeinfo-list'
			 * if one does not yet exist for this merge ordering.
			 */
			PathOrder	*path_order;
			MergeInfo	*xmergeinfo;
			Expr	   *clause = restrictinfo->clause;
			Var		   *leftop = get_leftop(clause);
			Var		   *rightop = get_rightop(clause);
			JoinKey    *jmkeys;

			path_order = makeNode(PathOrder);
			path_order->ordtype = MERGE_ORDER;
			path_order->ord.merge = merge_ordering;
			xmergeinfo = match_order_mergeinfo(path_order, mergeinfo_list);
			if (inner_relid == leftop->varno)
			{
				jmkeys = makeNode(JoinKey);
				jmkeys->outer = rightop;
				jmkeys->inner = leftop;
			}
			else
			{
				jmkeys = makeNode(JoinKey);
				jmkeys->outer = leftop;
				jmkeys->inner = rightop;
			}

			if (xmergeinfo == NULL)
			{
				xmergeinfo = makeNode(MergeInfo);

				xmergeinfo->m_ordering = merge_ordering;
				mergeinfo_list = lcons(xmergeinfo,
									   mergeinfo_list);
			}

			((JoinMethod *) xmergeinfo)->clauses = lcons(clause,
					  ((JoinMethod *) xmergeinfo)->clauses);
			((JoinMethod *) xmergeinfo)->jmkeys = lcons(jmkeys,
					  ((JoinMethod *) xmergeinfo)->jmkeys);
		}
	}
	return mergeinfo_list;
}


/*
 * match-order-mergeinfo--
 *	  Searches the list 'mergeinfo-list' for a mergeinfo node whose order
 *	  field equals 'ordering'.
 *
 * Returns the node if it exists.
 *
 */
MergeInfo *
match_order_mergeinfo(PathOrder *ordering, List *mergeinfo_list)
{
	MergeOrder *xmergeorder;
	List	   *xmergeinfo = NIL;

	foreach(xmergeinfo, mergeinfo_list)
	{
		MergeInfo	   *mergeinfo = (MergeInfo *) lfirst(xmergeinfo);

		xmergeorder = mergeinfo->m_ordering;

		if ((ordering->ordtype == MERGE_ORDER &&
		 equal_merge_ordering(ordering->ord.merge, xmergeorder)) ||
			(ordering->ordtype == SORTOP_ORDER &&
		   equal_path_merge_ordering(ordering->ord.sortop, xmergeorder)))
		{

			return mergeinfo;
		}
	}
	return (MergeInfo *) NIL;
}
