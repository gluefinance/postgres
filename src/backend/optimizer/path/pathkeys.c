/*-------------------------------------------------------------------------
 *
 * pathkeys.c
 *	  Utilities for matching and building path keys
 *
 * Portions Copyright (c) 1996-2000, PostgreSQL, Inc
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header$
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "nodes/makefuncs.h"
#include "optimizer/clauses.h"
#include "optimizer/joininfo.h"
#include "optimizer/pathnode.h"
#include "optimizer/paths.h"
#include "optimizer/tlist.h"
#include "optimizer/var.h"
#include "parser/parsetree.h"
#include "parser/parse_func.h"
#include "utils/lsyscache.h"

static PathKeyItem *makePathKeyItem(Node *key, Oid sortop);
static List *make_canonical_pathkey(Query *root, PathKeyItem *item);
static Var *find_indexkey_var(Query *root, RelOptInfo *rel,
							  AttrNumber varattno);


/*--------------------
 *	Explanation of Path.pathkeys
 *
 *	Path.pathkeys is a List of Lists of PathKeyItem nodes that represent
 *	the sort order of the result generated by the Path.  The n'th sublist
 *	represents the n'th sort key of the result.
 *
 *	In single/base relation RelOptInfo's, the Paths represent various ways
 *	of scanning the relation and the resulting ordering of the tuples.
 *	Sequential scan Paths have NIL pathkeys, indicating no known ordering.
 *	Index scans have Path.pathkeys that represent the chosen index's ordering,
 *  if any.  A single-key index would create a pathkey with a single sublist,
 *	e.g. ( (tab1.indexkey1/sortop1) ).  A multi-key index generates a sublist
 *	per key, e.g. ( (tab1.indexkey1/sortop1) (tab1.indexkey2/sortop2) ) which
 *	shows major sort by indexkey1 (ordering by sortop1) and minor sort by
 *	indexkey2 with sortop2.
 *
 *	Note that a multi-pass indexscan (OR clause scan) has NIL pathkeys since
 *	we can say nothing about the overall order of its result.  Also, an
 *	indexscan on an unordered type of index generates NIL pathkeys.  However,
 *	we can always create a pathkey by doing an explicit sort.  The pathkeys
 *	for a sort plan's output just represent the sort key fields and the
 *	ordering operators used.
 *
 *	Things get more interesting when we consider joins.  Suppose we do a
 *	mergejoin between A and B using the mergeclause A.X = B.Y.  The output
 *	of the mergejoin is sorted by X --- but it is also sorted by Y.  We
 *	represent this fact by listing both keys in a single pathkey sublist:
 *	( (A.X/xsortop B.Y/ysortop) ).  This pathkey asserts that the major
 *	sort order of the Path can be taken to be *either* A.X or B.Y.
 *	They are equal, so they are both primary sort keys.  By doing this,
 *	we allow future joins to use either var as a pre-sorted key, so upper
 *	Mergejoins may be able to avoid having to re-sort the Path.  This is
 *	why pathkeys is a List of Lists.
 *
 *	We keep a sortop associated with each PathKeyItem because cross-data-type
 *	mergejoins are possible; for example int4 = int8 is mergejoinable.
 *	In this case we need to remember that the left var is ordered by int4lt
 *	while the right var is ordered by int8lt.  So the different members of
 *	each sublist could have different sortops.
 *
 *	Note that while the order of the top list is meaningful (primary vs.
 *	secondary sort key), the order of each sublist is arbitrary.  Each sublist
 *	should be regarded as a set of equivalent keys, with no significance
 *	to the list order.
 *
 *	With a little further thought, it becomes apparent that pathkeys for
 *	joins need not only come from mergejoins.  For example, if we do a
 *	nestloop join between outer relation A and inner relation B, then any
 *	pathkeys relevant to A are still valid for the join result: we have
 *	not altered the order of the tuples from A.  Even more interesting,
 *	if there was a mergeclause (more formally, an "equijoin clause") A.X=B.Y,
 *	and A.X was a pathkey for the outer relation A, then we can assert that
 *	B.Y is a pathkey for the join result; X was ordered before and still is,
 *	and the joined values of Y are equal to the joined values of X, so Y
 *	must now be ordered too.  This is true even though we used no mergejoin.
 *
 *	More generally, whenever we have an equijoin clause A.X = B.Y and a
 *	pathkey A.X, we can add B.Y to that pathkey if B is part of the joined
 *	relation the pathkey is for, *no matter how we formed the join*.
 *
 *	In short, then: when producing the pathkeys for a merge or nestloop join,
 *	we can keep all of the keys of the outer path, since the ordering of the
 *	outer path will be preserved in the result.  Furthermore, we can add to
 *	each pathkey sublist any inner vars that are equijoined to any of the
 *	outer vars in the sublist; this works regardless of whether we are
 *	implementing the join using that equijoin clause as a mergeclause,
 *	or merely enforcing the clause after-the-fact as a qpqual filter.
 *
 *	Although Hashjoins also work only with equijoin operators, it is *not*
 *	safe to consider the output of a Hashjoin to be sorted in any particular
 *	order --- not even the outer path's order.  This is true because the
 *	executor might have to split the join into multiple batches.  Therefore
 *	a Hashjoin is always given NIL pathkeys.  (Also, we need to use only
 *	mergejoinable operators when deducing which inner vars are now sorted,
 *	because a mergejoin operator tells us which left- and right-datatype
 *	sortops can be considered equivalent, whereas a hashjoin operator
 *	doesn't imply anything about sort order.)
 *
 *	Pathkeys are also useful to represent an ordering that we wish to achieve,
 *	since they are easily compared to the pathkeys of a potential candidate
 *	path.  So, SortClause lists are turned into pathkeys lists for use inside
 *	the optimizer.
 *
 *	OK, now for how it *really* works:
 *
 *	We did implement pathkeys just as described above, and found that the
 *	planner spent a huge amount of time comparing pathkeys, because the
 *	representation of pathkeys as unordered lists made it expensive to decide
 *	whether two were equal or not.  So, we've modified the representation
 *	as described next.
 *
 *	If we scan the WHERE clause for equijoin clauses (mergejoinable clauses)
 *	during planner startup, we can construct lists of equivalent pathkey items
 *	for the query.  There could be more than two items per equivalence set;
 *	for example, WHERE A.X = B.Y AND B.Y = C.Z AND D.R = E.S creates the
 *	equivalence sets { A.X B.Y C.Z } and { D.R E.S } (plus associated sortops).
 *	Any pathkey item that belongs to an equivalence set implies that all the
 *	other items in its set apply to the relation too, or at least all the ones
 *	that are for fields present in the relation.  (Some of the items in the
 *	set might be for as-yet-unjoined relations.)  Furthermore, any multi-item
 *	pathkey sublist that appears at any stage of planning the query *must* be
 *	a subset of one or another of these equivalence sets; there's no way we'd
 *	have put two items in the same pathkey sublist unless they were equijoined
 *	in WHERE.
 *
 *	Now suppose that we allow a pathkey sublist to contain pathkey items for
 *	vars that are not yet part of the pathkey's relation.  This introduces
 *	no logical difficulty, because such items can easily be seen to be
 *	irrelevant; we just mandate that they be ignored.  But having allowed
 *	this, we can declare (by fiat) that any multiple-item pathkey sublist
 *	must be equal() to the appropriate equivalence set.  In effect, whenever
 *	we make a pathkey sublist that mentions any var appearing in an
 *	equivalence set, we instantly add all the other vars equivalenced to it,
 *	whether they appear yet in the pathkey's relation or not.  And we also
 *	mandate that the pathkey sublist appear in the same order as the
 *	equivalence set it comes from.  (In practice, we simply return a pointer
 *	to the relevant equivalence set without building any new sublist at all.)
 *	This makes comparing pathkeys very simple and fast, and saves a lot of
 *	work and memory space for pathkey construction as well.
 *
 *	Note that pathkey sublists having just one item still exist, and are
 *	not expected to be equal() to any equivalence set.  This occurs when
 *	we describe a sort order that involves a var that's not mentioned in
 *	any equijoin clause of the WHERE.  We could add singleton sets containing
 *	such vars to the query's list of equivalence sets, but there's little
 *	point in doing so.
 *
 *	By the way, it's OK and even useful for us to build equivalence sets
 *	that mention multiple vars from the same relation.  For example, if
 *	we have WHERE A.X = A.Y and we are scanning A using an index on X,
 *	we can legitimately conclude that the path is sorted by Y as well;
 *	and this could be handy if Y is the variable used in other join clauses
 *	or ORDER BY.  So, any WHERE clause with a mergejoinable operator can
 *	contribute to an equivalence set, even if it's not a join clause.
 *
 *	-- bjm & tgl
 *--------------------
 */


/*
 * makePathKeyItem
 *		create a PathKeyItem node
 */
static PathKeyItem *
makePathKeyItem(Node *key, Oid sortop)
{
	PathKeyItem	   *item = makeNode(PathKeyItem);

	item->key = key;
	item->sortop = sortop;
	return item;
}

/*
 * add_equijoined_keys
 *	  The given clause has a mergejoinable operator, so its two sides
 *	  can be considered equal after restriction clause application; in
 *	  particular, any pathkey mentioning one side (with the correct sortop)
 *	  can be expanded to include the other as well.  Record the vars and
 *	  associated sortops in the query's equi_key_list for future use.
 *
 * The query's equi_key_list field points to a list of sublists of PathKeyItem
 * nodes, where each sublist is a set of two or more vars+sortops that have
 * been identified as logically equivalent (and, therefore, we may consider
 * any two in a set to be equal).  As described above, we will subsequently
 * use direct pointers to one of these sublists to represent any pathkey
 * that involves an equijoined variable.
 *
 * This code would actually work fine with expressions more complex than
 * a single Var, but currently it won't see any because check_mergejoinable
 * won't accept such clauses as mergejoinable.
 */
void
add_equijoined_keys(Query *root, RestrictInfo *restrictinfo)
{
	Expr	   *clause = restrictinfo->clause;
	PathKeyItem *item1 = makePathKeyItem((Node *) get_leftop(clause),
										 restrictinfo->left_sortop);
	PathKeyItem *item2 = makePathKeyItem((Node *) get_rightop(clause),
										 restrictinfo->right_sortop);
	List	   *newset,
			   *cursetlink;

	/* We might see a clause X=X; don't make a single-element list from it */
	if (equal(item1, item2))
		return;
	/*
	 * Our plan is to make a two-element set, then sweep through the existing
	 * equijoin sets looking for matches to item1 or item2.  When we find one,
	 * we remove that set from equi_key_list and union it into our new set.
	 * When done, we add the new set to the front of equi_key_list.
	 *
	 * This is a standard UNION-FIND problem, for which there exist better
	 * data structures than simple lists.  If this code ever proves to be
	 * a bottleneck then it could be sped up --- but for now, simple is
	 * beautiful.
	 */
	newset = lcons(item1, lcons(item2, NIL));

	foreach(cursetlink, root->equi_key_list)
	{
		List	   *curset = lfirst(cursetlink);

		if (member(item1, curset) || member(item2, curset))
		{
			/* Found a set to merge into our new set */
			newset = LispUnion(newset, curset);
			/* Remove old set from equi_key_list.  NOTE this does not change
			 * lnext(cursetlink), so the outer foreach doesn't break.
			 */
			root->equi_key_list = lremove(curset, root->equi_key_list);
			freeList(curset);	/* might as well recycle old cons cells */
		}
	}

	root->equi_key_list = lcons(newset, root->equi_key_list);
}

/*
 * make_canonical_pathkey
 *	  Given a PathKeyItem, find the equi_key_list subset it is a member of,
 *	  if any.  If so, return a pointer to that sublist, which is the
 *	  canonical representation (for this query) of that PathKeyItem's
 *	  equivalence set.  If it is not found, return a single-element list
 *	  containing the PathKeyItem (when the item has no equivalence peers,
 *	  we just allow it to be a standalone list).
 *
 * Note that this function must not be used until after we have completed
 * scanning the WHERE clause for equijoin operators.
 */
static List *
make_canonical_pathkey(Query *root, PathKeyItem *item)
{
	List	   *cursetlink;

	foreach(cursetlink, root->equi_key_list)
	{
		List	   *curset = lfirst(cursetlink);

		if (member(item, curset))
			return curset;
	}
	return lcons(item, NIL);
}

/*
 * canonicalize_pathkeys
 *	   Convert a not-necessarily-canonical pathkeys list to canonical form.
 *
 * Note that this function must not be used until after we have completed
 * scanning the WHERE clause for equijoin operators.
 */
List *
canonicalize_pathkeys(Query *root, List *pathkeys)
{
	List	   *new_pathkeys = NIL;
	List	   *i;

	foreach(i, pathkeys)
	{
		List		   *pathkey = (List *) lfirst(i);
		PathKeyItem	   *item;

		/*
		 * It's sufficient to look at the first entry in the sublist;
		 * if there are more entries, they're already part of an
		 * equivalence set by definition.
		 */
		Assert(pathkey != NIL);
		item = (PathKeyItem *) lfirst(pathkey);
		new_pathkeys = lappend(new_pathkeys,
							   make_canonical_pathkey(root, item));
	}
	return new_pathkeys;
}

/****************************************************************************
 *		PATHKEY COMPARISONS
 ****************************************************************************/

/*
 * compare_pathkeys
 *	  Compare two pathkeys to see if they are equivalent, and if not whether
 *	  one is "better" than the other.
 *
 *	  A pathkey can be considered better than another if it is a superset:
 *	  it contains all the keys of the other plus more.  For example, either
 *	  ((A) (B)) or ((A B)) is better than ((A)).
 *
 *	  Because we actually only expect to see canonicalized pathkey sublists,
 *	  we don't have to do the full two-way-subset-inclusion test on each
 *	  pair of sublists that is implied by the above statement.  Instead we
 *	  just do an equal().  In the normal case where multi-element sublists
 *	  are pointers into the root's equi_key_list, equal() will be very fast:
 *	  it will recognize pointer equality when the sublists are the same,
 *	  and will fail at the first sublist element when they are not.
 *
 * Yes, this gets called enough to be worth coding it this tensely.
 */
PathKeysComparison
compare_pathkeys(List *keys1, List *keys2)
{
	List	   *key1,
			   *key2;

	for (key1 = keys1, key2 = keys2;
		 key1 != NIL && key2 != NIL;
		 key1 = lnext(key1), key2 = lnext(key2))
	{
		List	   *subkey1 = lfirst(key1);
		List	   *subkey2 = lfirst(key2);

		/* We will never have two subkeys where one is a subset of the other,
		 * because of the canonicalization explained above.  Either they are
		 * equal or they ain't.
		 */
		if (! equal(subkey1, subkey2))
			return PATHKEYS_DIFFERENT; /* no need to keep looking */
	}

	/* If we reached the end of only one list, the other is longer and
	 * therefore not a subset.  (We assume the additional sublist(s)
	 * of the other list are not NIL --- no pathkey list should ever have
	 * a NIL sublist.)
	 */
	if (key1 == NIL && key2 == NIL)
		return PATHKEYS_EQUAL;
	if (key1 != NIL)
		return PATHKEYS_BETTER1; /* key1 is longer */
	return PATHKEYS_BETTER2;	/* key2 is longer */
}

/*
 * pathkeys_contained_in
 *	  Common special case of compare_pathkeys: we just want to know
 *	  if keys2 are at least as well sorted as keys1.
 */
bool
pathkeys_contained_in(List *keys1, List *keys2)
{
	switch (compare_pathkeys(keys1, keys2))
	{
		case PATHKEYS_EQUAL:
		case PATHKEYS_BETTER2:
			return true;
		default:
			break;
	}
	return false;
}

/*
 * get_cheapest_path_for_pathkeys
 *	  Find the cheapest path (according to the specified criterion) that
 *	  satisfies the given pathkeys.  Return NULL if no such path.
 *
 * 'paths' is a list of possible paths that all generate the same relation
 * 'pathkeys' represents a required ordering (already canonicalized!)
 * 'cost_criterion' is STARTUP_COST or TOTAL_COST
 */
Path *
get_cheapest_path_for_pathkeys(List *paths, List *pathkeys,
							   CostSelector cost_criterion)
{
	Path	   *matched_path = NULL;
	List	   *i;

	foreach(i, paths)
	{
		Path	   *path = (Path *) lfirst(i);

		/*
		 * Since cost comparison is a lot cheaper than pathkey comparison,
		 * do that first.  (XXX is that still true?)
		 */
		if (matched_path != NULL &&
			compare_path_costs(matched_path, path, cost_criterion) <= 0)
			continue;

		if (pathkeys_contained_in(pathkeys, path->pathkeys))
			matched_path = path;
	}
	return matched_path;
}

/*
 * get_cheapest_fractional_path_for_pathkeys
 *	  Find the cheapest path (for retrieving a specified fraction of all
 *	  the tuples) that satisfies the given pathkeys.
 *	  Return NULL if no such path.
 *
 * See compare_fractional_path_costs() for the interpretation of the fraction
 * parameter.
 *
 * 'paths' is a list of possible paths that all generate the same relation
 * 'pathkeys' represents a required ordering (already canonicalized!)
 * 'fraction' is the fraction of the total tuples expected to be retrieved
 */
Path *
get_cheapest_fractional_path_for_pathkeys(List *paths,
										  List *pathkeys,
										  double fraction)
{
	Path	   *matched_path = NULL;
	List	   *i;

	foreach(i, paths)
	{
		Path	   *path = (Path *) lfirst(i);

		/*
		 * Since cost comparison is a lot cheaper than pathkey comparison,
		 * do that first.
		 */
		if (matched_path != NULL &&
			compare_fractional_path_costs(matched_path, path, fraction) <= 0)
			continue;

		if (pathkeys_contained_in(pathkeys, path->pathkeys))
			matched_path = path;
	}
	return matched_path;
}

/****************************************************************************
 *		NEW PATHKEY FORMATION
 ****************************************************************************/

/*
 * build_index_pathkeys
 *	  Build a pathkeys list that describes the ordering induced by an index
 *	  scan using the given index.  (Note that an unordered index doesn't
 *	  induce any ordering; such an index will have no sortop OIDS in
 *	  its "ordering" field, and we will return NIL.)
 *
 * If 'scandir' is BackwardScanDirection, attempt to build pathkeys
 * representing a backwards scan of the index.  Return NIL if can't do it.
 */
List *
build_index_pathkeys(Query *root,
					 RelOptInfo *rel,
					 IndexOptInfo *index,
					 ScanDirection scandir)
{
	List	   *retval = NIL;
	int		   *indexkeys = index->indexkeys;
	Oid		   *ordering = index->ordering;
	PathKeyItem *item;
	Oid			sortop;

	if (!indexkeys || indexkeys[0] == 0 ||
		!ordering || ordering[0] == InvalidOid)
		return NIL;				/* unordered index? */

	if (index->indproc)
	{
		/* Functional index: build a representation of the function call */
		Func	   *funcnode = makeNode(Func);
		List	   *funcargs = NIL;

		funcnode->funcid = index->indproc;
		funcnode->functype = get_func_rettype(index->indproc);
		funcnode->funcisindex = false;
		funcnode->funcsize = 0;
		funcnode->func_fcache = NULL;
		/* we assume here that the function returns a base type... */
		funcnode->func_tlist = setup_base_tlist(funcnode->functype);
		funcnode->func_planlist = NIL;

		while (*indexkeys != 0)
		{
			funcargs = lappend(funcargs,
							   find_indexkey_var(root, rel, *indexkeys));
			indexkeys++;
		}

		sortop = *ordering;
		if (ScanDirectionIsBackward(scandir))
		{
			sortop = get_commutator(sortop);
			if (sortop == InvalidOid)
				return NIL;		/* oops, no reverse sort operator? */
		}

		/* Make a one-sublist pathkeys list for the function expression */
		item = makePathKeyItem((Node *) make_funcclause(funcnode, funcargs),
							   sortop);
		retval = lcons(make_canonical_pathkey(root, item), NIL);
	}
	else
	{
		/* Normal non-functional index */
		while (*indexkeys != 0 && *ordering != InvalidOid)
		{
			Var		*relvar = find_indexkey_var(root, rel, *indexkeys);

			sortop = *ordering;
			if (ScanDirectionIsBackward(scandir))
			{
				sortop = get_commutator(sortop);
				if (sortop == InvalidOid)
					break;		/* oops, no reverse sort operator? */
			}

			/* OK, make a sublist for this sort key */
			item = makePathKeyItem((Node *) relvar, sortop);
			retval = lappend(retval, make_canonical_pathkey(root, item));

			indexkeys++;
			ordering++;
		}
	}

	return retval;
}

/*
 * Find or make a Var node for the specified attribute of the rel.
 *
 * We first look for the var in the rel's target list, because that's
 * easy and fast.  But the var might not be there (this should normally
 * only happen for vars that are used in WHERE restriction clauses,
 * but not in join clauses or in the SELECT target list).  In that case,
 * gin up a Var node the hard way.
 */
static Var *
find_indexkey_var(Query *root, RelOptInfo *rel, AttrNumber varattno)
{
	List	   *temp;
	int			relid;
	Oid			reloid,
				vartypeid;
	int32		type_mod;

	foreach(temp, rel->targetlist)
	{
		Var	   *tle_var = get_expr(lfirst(temp));

		if (IsA(tle_var, Var) && tle_var->varattno == varattno)
			return tle_var;
	}

	relid = lfirsti(rel->relids);
	reloid = getrelid(relid, root->rtable);
	vartypeid = get_atttype(reloid, varattno);
	type_mod = get_atttypmod(reloid, varattno);

	return makeVar(relid, varattno, vartypeid, type_mod, 0);
}

/*
 * build_join_pathkeys
 *	  Build the path keys for a join relation constructed by mergejoin or
 *	  nestloop join.  These keys should include all the path key vars of the
 *	  outer path (since the join will retain the ordering of the outer path)
 *	  plus any vars of the inner path that are equijoined to the outer vars.
 *
 *	  Per the discussion at the top of this file, equijoined inner vars
 *	  can be considered path keys of the result, just the same as the outer
 *	  vars they were joined with; furthermore, it doesn't matter what kind
 *	  of join algorithm is actually used.
 *
 * 'outer_pathkeys' is the list of the outer path's path keys
 * 'join_rel_tlist' is the target list of the join relation
 * 'equi_key_list' is the query's list of pathkeyitem equivalence sets
 *
 * Returns the list of new path keys.
 */
List *
build_join_pathkeys(List *outer_pathkeys,
					List *join_rel_tlist,
					List *equi_key_list)
{
	/*
	 * This used to be quite a complex bit of code, but now that all
	 * pathkey sublists start out life canonicalized, we don't have to
	 * do a darn thing here!  The inner-rel vars we used to need to add
	 * are *already* part of the outer pathkey!
	 *
	 * I'd remove the routine entirely, but maybe someday we'll need it...
	 */
	return outer_pathkeys;
}

/****************************************************************************
 *		PATHKEYS AND SORT CLAUSES
 ****************************************************************************/

/*
 * make_pathkeys_for_sortclauses
 *		Generate a pathkeys list that represents the sort order specified
 *		by a list of SortClauses (GroupClauses will work too!)
 *
 * NB: the result is NOT in canonical form, but must be passed through
 * canonicalize_pathkeys() before it can be used for comparisons or
 * labeling relation sort orders.  (We do things this way because
 * union_planner needs to be able to construct requested pathkeys before
 * the pathkey equivalence sets have been created for the query.)
 *
 * 'sortclauses' is a list of SortClause or GroupClause nodes
 * 'tlist' is the targetlist to find the referenced tlist entries in
 */
List *
make_pathkeys_for_sortclauses(List *sortclauses,
							  List *tlist)
{
	List	   *pathkeys = NIL;
	List	   *i;

	foreach(i, sortclauses)
	{
		SortClause	   *sortcl = (SortClause *) lfirst(i);
		Node		   *sortkey;
		PathKeyItem	   *pathkey;

		sortkey = get_sortgroupclause_expr(sortcl, tlist);
		pathkey = makePathKeyItem(sortkey, sortcl->sortop);
		/*
		 * The pathkey becomes a one-element sublist, for now;
		 * canonicalize_pathkeys() might replace it with a longer
		 * sublist later.
		 */
		pathkeys = lappend(pathkeys, lcons(pathkey, NIL));
	}
	return pathkeys;
}

/****************************************************************************
 *		PATHKEYS AND MERGECLAUSES
 ****************************************************************************/

/*
 * find_mergeclauses_for_pathkeys
 *	  This routine attempts to find a set of mergeclauses that can be
 *	  used with a specified ordering for one of the input relations.
 *	  If successful, it returns a list of mergeclauses.
 *
 * 'pathkeys' is a pathkeys list showing the ordering of an input path.
 *			It doesn't matter whether it is for the inner or outer path.
 * 'restrictinfos' is a list of mergejoinable restriction clauses for the
 *			join relation being formed.
 *
 * The result is NIL if no merge can be done, else a maximal list of
 * usable mergeclauses (represented as a list of their restrictinfo nodes).
 *
 * XXX Ideally we ought to be considering context, ie what path orderings
 * are available on the other side of the join, rather than just making
 * an arbitrary choice among the mergeclause orders that will work for
 * this side of the join.
 */
List *
find_mergeclauses_for_pathkeys(List *pathkeys, List *restrictinfos)
{
	List	   *mergeclauses = NIL;
	List	   *i;

	foreach(i, pathkeys)
	{
		List		   *pathkey = lfirst(i);
		RestrictInfo   *matched_restrictinfo = NULL;
		List		   *j;

		/*
		 * We can match any of the keys in this pathkey sublist,
		 * since they're all equivalent.  And we can match against
		 * either left or right side of any mergejoin clause we haven't
		 * used yet.  For the moment we use a dumb "greedy" algorithm
		 * with no backtracking.  Is it worth being any smarter to
		 * make a longer list of usable mergeclauses?  Probably not.
		 */
		foreach(j, pathkey)
		{
			PathKeyItem	   *keyitem = lfirst(j);
			Node		   *key = keyitem->key;
			Oid				keyop = keyitem->sortop;
			List		   *k;

			foreach(k, restrictinfos)
			{
				RestrictInfo   *restrictinfo = lfirst(k);

				Assert(restrictinfo->mergejoinoperator != InvalidOid);

				if (((keyop == restrictinfo->left_sortop &&
					  equal(key, get_leftop(restrictinfo->clause))) ||
					 (keyop == restrictinfo->right_sortop &&
					  equal(key, get_rightop(restrictinfo->clause)))) &&
					! member(restrictinfo, mergeclauses))
				{
					matched_restrictinfo = restrictinfo;
					break;
				}
			}
			if (matched_restrictinfo)
				break;
		}

		/*
		 * If we didn't find a mergeclause, we're done --- any additional
		 * sort-key positions in the pathkeys are useless.  (But we can
		 * still mergejoin if we found at least one mergeclause.)
		 */
		if (! matched_restrictinfo)
			break;
		/*
		 * If we did find a usable mergeclause for this sort-key position,
		 * add it to result list.
		 */
		mergeclauses = lappend(mergeclauses, matched_restrictinfo);
	}

	return mergeclauses;
}

/*
 * make_pathkeys_for_mergeclauses
 *	  Builds a pathkey list representing the explicit sort order that
 *	  must be applied to a path in order to make it usable for the
 *	  given mergeclauses.
 *
 * 'mergeclauses' is a list of RestrictInfos for mergejoin clauses
 *			that will be used in a merge join.
 * 'tlist' is a relation target list for either the inner or outer
 *			side of the proposed join rel.  (Not actually needed anymore)
 *
 * Returns a pathkeys list that can be applied to the indicated relation.
 *
 * Note that it is not this routine's job to decide whether sorting is
 * actually needed for a particular input path.  Assume a sort is necessary;
 * just make the keys, eh?
 */
List *
make_pathkeys_for_mergeclauses(Query *root,
							   List *mergeclauses,
							   List *tlist)
{
	List	   *pathkeys = NIL;
	List	   *i;

	foreach(i, mergeclauses)
	{
		RestrictInfo *restrictinfo = (RestrictInfo *) lfirst(i);
		Node	   *key;
		Oid			sortop;
		PathKeyItem *item;

		Assert(restrictinfo->mergejoinoperator != InvalidOid);

		/*
		 * Find the key and sortop needed for this mergeclause.
		 *
		 * Both sides of the mergeclause should appear in one of the
		 * query's pathkey equivalence classes, so it doesn't matter
		 * which one we use here.
		 */
		key = (Node *) get_leftop(restrictinfo->clause);
		sortop = restrictinfo->left_sortop;
		/*
		 * Add a pathkey sublist for this sort item
		 */
		item = makePathKeyItem(key, sortop);
		pathkeys = lappend(pathkeys, make_canonical_pathkey(root, item));
	}

	return pathkeys;
}
