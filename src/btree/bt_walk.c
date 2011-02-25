/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wt_internal.h"

/*
 * There are two tree-walk implementations: a textbook, depth-first recursive
 * tree walk in __wt_tree_walk(), and a non-recursive, depth-first tree walk
 * in __wt_walk_{begin,end,next}().
 *
 * The simple recursive walk is sufficient in most cases -- a hazard reference
 * is obtained on each page in turn, a worker function is called on the page,
 * then the hazard reference is released.
 *
 * The complicated tree walk routine was added because the cache eviction code
 * needs:
 *    + to walk the tree a few pages at a time, that is, periodically wake,
 *	visit some pages, then go back to sleep, which requires enough state
 *	to restart the traversal at any point,
 *    + to only visit pages that currently appear in the cache,
 *    + to return the WT_REF structure (not the WT_PAGE structure),
 *    + to walk files not associated with the current WT_TOC's DB handle,
 *    + and finally, it doesn't require a hazard reference.
 *
 * My guess is we'll generalize a more complicated walk at some point, which
 * means some or all of those behaviors will become configurable, and that's
 * why the code lives here instead of in the eviction code.
 */

/*
 * __wt_tree_walk --
 *	Depth-first recursive walk of a btree, calling a worker function on
 *	each page.
 */
int
__wt_tree_walk(WT_TOC *toc, WT_REF *ref,
    uint32_t flags, int (*work)(WT_TOC *, WT_PAGE *, void *), void *arg)
{
	IDB *idb;
	WT_COL *cip;
	WT_OFF *off;
	WT_OFF_RECORD *off_record;
	WT_PAGE *page;
	WT_ROW *rip;
	uint32_t i;
	int ret;

	 WT_ENV_FCHK(
	     toc->env, "__wt_tree_walk", flags, WT_APIMASK_BT_TREE_WALK);

	idb = toc->db->idb;

	/*
	 * A NULL WT_REF means to start at the top of the tree -- it's just
	 * a convenience.
	 */
	page = ref == NULL ? idb->root_page.page : ref->page;

	/* Walk internal pages, descending through any off-page references. */
	switch (page->dsk->type) {
	case WT_PAGE_COL_INT:
		WT_INDX_FOREACH(page, cip, i) {
			/* cip references the subtree containing the record */
			ref = WT_COL_REF(page, cip);
			if (LF_ISSET(WT_WALK_CACHE) &&
			    ref->state != WT_REF_CACHE)
				continue;

			off_record = WT_COL_OFF(cip);
			switch (ret =
			    __wt_page_in(toc, page, ref, off_record, 0)) {
			case 0:				/* Valid page */
				ret =
				    __wt_tree_walk(toc, ref, flags, work, arg);
				__wt_hazard_clear(toc, ref->page);
				break;
			case WT_PAGE_DELETED:
				ret = 0;		/* Skip deleted pages */
				break;
			}
			if (ret != 0)
				return (ret);
		}
		break;
	case WT_PAGE_ROW_INT:
		WT_INDX_FOREACH(page, rip, i) {
			/* rip references the subtree containing the record */
			ref = WT_ROW_REF(page, rip);
			if (LF_ISSET(WT_WALK_CACHE) &&
			    ref->state != WT_REF_CACHE)
				continue;

			off = WT_ROW_OFF(rip);
			switch (ret = __wt_page_in(toc, page, ref, off, 0)) {
			case 0:				/* Valid page */
				ret =
				    __wt_tree_walk(toc, ref, flags, work, arg);
				__wt_hazard_clear(toc, ref->page);
				break;
			case WT_PAGE_DELETED:
				ret = 0;		/* Skip deleted pages */
				break;
			}
			if (ret != 0)
				return (ret);
		}
		break;
	}

	/*
	 * Don't call the worker function for any page until all of its children
	 * have been visited.   This allows the walker function to be used for
	 * the close/sync methods, where reconciling a modified child page will
	 * modify its parent.
	 */
	WT_RET(work(toc, page, arg));

	return (0);
}

/*
 * __wt_walk_begin --
 *	Start a tree walk.
 */
int
__wt_walk_begin(WT_TOC *toc, WT_REF *ref, WT_WALK *walk)
{
	ENV *env;

	env = toc->env;

	/*
	 * The caller may be restarting a walk, so the structure may already
	 * be allocated.  Allocate 20 slots: it's always going to be enough.
	 */
	if (walk->tree_len == 0)
		WT_RET(__wt_realloc(env, &walk->tree_len,
		    20 * sizeof(WT_WALK_ENTRY), &walk->tree));
	walk->tree_slot = 0;

	walk->tree[0].ref = ref;
	walk->tree[0].indx = 0;
	walk->tree[0].visited = 0;

	return (0);
}

/*
 * __wt_walk_end --
 *	End a tree walk.
 */
void
__wt_walk_end(ENV *env, WT_WALK *walk)
{
	__wt_free(env, walk->tree, walk->tree_len);
}

/*
 * __wt_walk_next --
 *	Return the next WT_REF/WT_PAGE in the tree, in a non-recursive way.
 */
int
__wt_walk_next(WT_TOC *toc, WT_WALK *walk, WT_REF **refp)
{
	ENV *env;
	WT_PAGE *page, *child;
	WT_REF *ref;
	WT_WALK_ENTRY *e;
	u_int elem;

	env = toc->env;

	e = &walk->tree[walk->tree_slot];
	page = e->ref->page;

	/*
	 * Coming into this function we have a tree internal page and we're
	 * walking the array of children.
	 *
	 * If we've reached the end of this page, and haven't yet returned it,
	 * do that now.  If the page has been returned, traversal is finished:
	 * pop the stack and call ourselve recursively, unless the entire tree
	 * has been traversed, in which case we return NULL.
	 */
	if (e->visited) {
		if (walk->tree_slot == 0) {
			*refp = NULL;
			return (0);
		} else {
			--walk->tree_slot;
			return (__wt_walk_next(toc, walk, refp));
		}
	} else
		if (e->indx == page->indx_count) {
eop:			e->visited = 1;
			*refp = e->ref;
			return (0);
		}

	/* Find the next WT_REF/WT_PAGE pair present in the cache. */
	for (;;) {
		ref = &page->u.ref[e->indx];

		/* We only care about pages in the cache. */
		if (ref->state == WT_REF_CACHE)
			break;

		/*
		 * If we don't find another WT_REF/WT_OFF pair, do the
		 * post-order visit.
		 */
		if (++e->indx == page->indx_count)
			goto eop;
	}

	/*
	 * Check to see if the page has sub-trees associated with it, in which
	 * case we traverse those pages.
	 */
	child = ref->page;
	switch (child->dsk->type) {
	case WT_PAGE_COL_INT:
	case WT_PAGE_ROW_INT:
		/*
		 * The page has children.
		 *
		 * First, move past this child, then push the child onto our
		 * stack, and recursively descend the tree.
		 */
		++e->indx;

		/* Check to see if we grew past the end of our stack. */
		elem = (u_int)(walk->tree_len / WT_SIZEOF32(WT_WALK_ENTRY));
		if (walk->tree_slot >= elem)
			WT_RET(__wt_realloc(env, &walk->tree_len,
			    (elem + 20) * sizeof(WT_WALK_ENTRY), &walk->tree));

		e = &walk->tree[++walk->tree_slot];
		e->ref = ref;
		e->indx = 0;
		e->visited = 0;
		return (__wt_walk_next(toc, walk, refp));
	}

	/* Return the child page, it's not interesting for further traversal. */
	++e->indx;
	*refp = ref;
	return (0);
}
