/*-------------------------------------------------------------------------
 * loose_quadtree.c (CURRENTLY JUST A COPY OF geo_spgist.c)
 * 
 * SP-GiST implementation of 4-dimensional quad tree over boxes
 *
 * This module provides SP-GiST implementation for boxes using quad tree
 * analogy in 4-dimensional space.  SP-GiST doesn't allow indexing of
 * overlapping objects.  We are making 2D objects never-overlapping in
 * 4D space.  This technique has some benefits compared to traditional
 * R-Tree which is implemented as GiST.  The performance tests reveal
 * that this technique especially beneficial with too much overlapping
 * objects, so called "spaghetti data".
 *
 * Unlike the original quad tree, we are splitting the tree into 16
 * quadrants in 4D space.  It is easier to imagine it as splitting space
 * two times into 4:
 *
 *				|	   |
 *				|	   |
 *				| -----+-----
 *				|	   |
 *				|	   |
 * -------------+-------------
 *				|
 *				|
 *				|
 *				|
 *				|
 *
 * We are using box datatype as the prefix, but we are treating them
 * as points in 4-dimensional space, because 2D boxes are not enough
 * to represent the quadrant boundaries in 4D space.  They however are
 * sufficient to point out the additional boundaries of the next
 * quadrant.
 *
 * We are using traversal values provided by SP-GiST to calculate and
 * to store the bounds of the quadrants, while traversing into the tree.
 * Traversal value has all the boundaries in the 4D space, and is capable
 * of transferring the required boundaries to the following traversal
 * values.  In conclusion, three things are necessary to calculate the
 * next traversal value:
 *
 *	(1) the traversal value of the parent
 *	(2) the quadrant of the current node
 *	(3) the prefix of the current node
 *
 * If we visualize them on our simplified drawing (see the drawing above);
 * transferred boundaries of (1) would be the outer axis, relevant part
 * of (2) would be the up right part of the other axis, and (3) would be
 * the inner axis.
 *
 * For example, consider the case of overlapping.  When recursion
 * descends deeper and deeper down the tree, all quadrants in
 * the current node will be checked for overlapping.  The boundaries
 * will be re-calculated for all quadrants.  Overlap check answers
 * the question: can any box from this quadrant overlap with the given
 * box?  If yes, then this quadrant will be walked.  If no, then this
 * quadrant will be skipped.
 *
 * This method provides restrictions for minimum and maximum values of
 * every dimension of every corner of the box on every level of the tree
 * except the root.  For the root node, we are setting the boundaries
 * that we don't yet have as infinity.
 *
 * Portions Copyright (c) 1996-2018, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *			src/backend/utils/adt/geo_spgist.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "math.h"
#include "access/spgist.h"
#include "access/stratnum.h"
#include "catalog/pg_type.h"
#include "utils/builtins.h"
#include "utils/geo_decls.h"

#define LOOSENESS 0.5
#ifdef PG_MODULE_MAGIC
PG_MODULE_MAGIC;
#endif

Datum spg_loose_quad_config(PG_FUNCTION_ARGS);
Datum spg_loose_quad_choose(PG_FUNCTION_ARGS);
Datum spg_loose_quad_picksplit(PG_FUNCTION_ARGS);
Datum spg_loose_quad_inner_consistent(PG_FUNCTION_ARGS);
Datum spg_loose_quad_leaf_consistent(PG_FUNCTION_ARGS);

PG_FUNCTION_INFO_V1(spg_loose_quad_config);
PG_FUNCTION_INFO_V1(spg_loose_quad_choose);
PG_FUNCTION_INFO_V1(spg_loose_quad_picksplit);
PG_FUNCTION_INFO_V1(spg_loose_quad_inner_consistent);
PG_FUNCTION_INFO_V1(spg_loose_quad_leaf_consistent);

/*
 * Comparator for qsort
 *
 * We don't need to use the floating point macros in here, because this
 * is going only going to be used in a place to effect the performance
 * of the index, not the correctness.
 */
static int
compareDoubles(const void *a, const void *b)
{
	double		x = *(double *) a;
	double		y = *(double *) b;

	if (x == y)
		return 0;
	return (x > y) ? 1 : -1;
}

typedef struct
{
	double      width;
	double      height;
} Dimensions;


typedef struct
{
	double		low;
	double		high;
} Range;

typedef struct
{
	Range		left;
	Range		right;
} RangeBox;

typedef struct
{
	RangeBox	range_box_x;
	RangeBox	range_box_y;
} RectBox;

/*
 * Calculate the quadrant
 * 
 * The quadrant is an int set by where the new box's centroid is 
 * in relation to the centroid of the overall box. I don't think we have
 * to check the bounds of objects b/c we can't change the level of the nodes...
 * 
 * Quadrant labeling: 
 * 
 *				|
 *		  0		|    1
 *				|	    
 *    ----------+----------
 *	  			|
 *	  	  2		|    3
 *	  			|
 *  
 */
static int
getQuadrant(BOX *centroid, BOX *inBox)
{
	int quadrant = 0;

	elog(LOG, "[getQuadrant] BOX (minx, miny) = (%f, %f)  -->  (maxx, maxy) = (%f, %f)\n", inBox->low.x, inBox->low.y, inBox->high.x, inBox->high.y);
	elog(LOG, "[getQuadrant] CENTROID (minx, miny) = (%f, %f)  -->  (maxx, maxy) = (%f, %f)\n", centroid->low.x, centroid->low.y, centroid->high.x, centroid->high.y);

	double centroidX = centroid->low.x + (centroid->high.x - centroid->low.x) / 2.0;
	double centroidY = centroid->low.y + (centroid->high.y - centroid->low.y) / 2.0;
	double newCentroidX = inBox->low.x + (inBox->high.x - inBox->low.x) / 2.0;
	double newCentroidY = inBox->low.y + (inBox->high.y - inBox->low.y) / 2.0;

	if (newCentroidY >= centroidY) {
		// in either quadrant 0 or 1
		if (newCentroidX >= centroidX) {
			quadrant = 1;
		} else {
			quadrant = 0;
		}
	} else {
		// in either quadrant 2 or 3
		if (newCentroidX >= centroidX) {
			quadrant = 3;
		} else {
			quadrant = 2;
		}
	}
	
	elog(LOG, "Quadrant is: (%d)\n", quadrant);
	return quadrant;
}

/*
 * Get RangeBox using BOX
 * 
 * We are turning the BOX to our structures to emphasize their function
 * of representing points in 4D space.  It also is more convenient to
 * access the values with this structure.
 */
static RangeBox *
getRangeBox(BOX *box)
{
	RangeBox   *range_box = (RangeBox *) palloc(sizeof(RangeBox));

	range_box->left.low = box->low.x;
	range_box->left.high = box->high.x;

	range_box->right.low = box->low.y;
	range_box->right.high = box->high.y;

	return range_box;
}

/*
 * Initialize the traversal value
 *
 * In the beginning, we don't have any restrictions.  We have to
 * initialize the struct to cover the whole 4D space.
 */
static RectBox *
initRectBox(void)
{
	RectBox    *rect_box = (RectBox *) palloc(sizeof(RectBox));
	double		infinity = get_float8_infinity();

	rect_box->range_box_x.left.low = -infinity;
	rect_box->range_box_x.left.high = infinity;

	rect_box->range_box_x.right.low = -infinity;
	rect_box->range_box_x.right.high = infinity;

	rect_box->range_box_y.left.low = -infinity;
	rect_box->range_box_y.left.high = infinity;

	rect_box->range_box_y.right.low = -infinity;
	rect_box->range_box_y.right.high = infinity;

	return rect_box;
}

/*
 * Calculate the next traversal value
 *
 * All centroids are bounded by RectBox, but SP-GiST only keeps
 * boxes.  When we are traversing the tree, we must calculate RectBox,
 * using centroid and quadrant.
 */
static RectBox *
nextRectBox(RectBox *rect_box, RangeBox *centroid, uint8 quadrant)
{
	RectBox    *next_rect_box = (RectBox *) palloc(sizeof(RectBox));

	memcpy(next_rect_box, rect_box, sizeof(RectBox));

	if (quadrant & 0x8)
		next_rect_box->range_box_x.left.low = centroid->left.low;
	else
		next_rect_box->range_box_x.left.high = centroid->left.low;

	if (quadrant & 0x4)
		next_rect_box->range_box_x.right.low = centroid->left.high;
	else
		next_rect_box->range_box_x.right.high = centroid->left.high;

	if (quadrant & 0x2)
		next_rect_box->range_box_y.left.low = centroid->right.low;
	else
		next_rect_box->range_box_y.left.high = centroid->right.low;

	if (quadrant & 0x1)
		next_rect_box->range_box_y.right.low = centroid->right.high;
	else
		next_rect_box->range_box_y.right.high = centroid->right.high;

	return next_rect_box;
}

/* Can any range from range_box overlap with this argument? */
static bool
overlap2D(RangeBox *range_box, Range *query)
{
	return FPge(range_box->right.high, query->low) &&
		FPle(range_box->left.low, query->high);
}

/* Can any rectangle from rect_box overlap with this argument? */
static bool
overlap4D(RectBox *rect_box, RangeBox *query)
{
	return overlap2D(&rect_box->range_box_x, &query->left) &&
		overlap2D(&rect_box->range_box_y, &query->right);
}

/* Can any range from range_box contain this argument? */
static bool
contain2D(RangeBox *range_box, Range *query)
{
	return FPge(range_box->right.high, query->high) &&
		FPle(range_box->left.low, query->low);
}

/* Can any rectangle from rect_box contain this argument? */
static bool
contain4D(RectBox *rect_box, RangeBox *query)
{
	return contain2D(&rect_box->range_box_x, &query->left) &&
		contain2D(&rect_box->range_box_y, &query->right);
}

/* Can any range from range_box be contained by this argument? */
static bool
contained2D(RangeBox *range_box, Range *query)
{
	return FPle(range_box->left.low, query->high) &&
		FPge(range_box->left.high, query->low) &&
		FPle(range_box->right.low, query->high) &&
		FPge(range_box->right.high, query->low);
}

/* Can any rectangle from rect_box be contained by this argument? */
static bool
contained4D(RectBox *rect_box, RangeBox *query)
{
	return contained2D(&rect_box->range_box_x, &query->left) &&
		contained2D(&rect_box->range_box_y, &query->right);
}

/* Can any range from range_box to be lower than this argument? */
static bool
lower2D(RangeBox *range_box, Range *query)
{
	return FPlt(range_box->left.low, query->low) &&
		FPlt(range_box->right.low, query->low);
}

/* Can any range from range_box not extend to the right side of the query? */
static bool
overLower2D(RangeBox *range_box, Range *query)
{
	return FPle(range_box->left.low, query->high) &&
		FPle(range_box->right.low, query->high);
}

/* Can any range from range_box to be higher than this argument? */
static bool
higher2D(RangeBox *range_box, Range *query)
{
	return FPgt(range_box->left.high, query->high) &&
		FPgt(range_box->right.high, query->high);
}

/* Can any range from range_box not extend to the left side of the query? */
static bool
overHigher2D(RangeBox *range_box, Range *query)
{
	return FPge(range_box->left.high, query->low) &&
		FPge(range_box->right.high, query->low);
}

/* Can any rectangle from rect_box be left of this argument? */
static bool
left4D(RectBox *rect_box, RangeBox *query)
{
	return lower2D(&rect_box->range_box_x, &query->left);
}

/* Can any rectangle from rect_box does not extend the right of this argument? */
static bool
overLeft4D(RectBox *rect_box, RangeBox *query)
{
	return overLower2D(&rect_box->range_box_x, &query->left);
}

/* Can any rectangle from rect_box be right of this argument? */
static bool
right4D(RectBox *rect_box, RangeBox *query)
{
	return higher2D(&rect_box->range_box_x, &query->left);
}

/* Can any rectangle from rect_box does not extend the left of this argument? */
static bool
overRight4D(RectBox *rect_box, RangeBox *query)
{
	return overHigher2D(&rect_box->range_box_x, &query->left);
}

/* Can any rectangle from rect_box be below of this argument? */
static bool
below4D(RectBox *rect_box, RangeBox *query)
{
	return lower2D(&rect_box->range_box_y, &query->right);
}

/* Can any rectangle from rect_box does not extend above this argument? */
static bool
overBelow4D(RectBox *rect_box, RangeBox *query)
{
	return overLower2D(&rect_box->range_box_y, &query->right);
}

/* Can any rectangle from rect_box be above of this argument? */
static bool
above4D(RectBox *rect_box, RangeBox *query)
{
	return higher2D(&rect_box->range_box_y, &query->right);
}

/* Can any rectangle from rect_box does not extend below of this argument? */
static bool
overAbove4D(RectBox *rect_box, RangeBox *query)
{
	return overHigher2D(&rect_box->range_box_y, &query->right);
}

/*
 * SP-GiST loose quadtree config function
 */
Datum
spg_loose_quad_config(PG_FUNCTION_ARGS)
{
	spgConfigOut *cfg = (spgConfigOut *) PG_GETARG_POINTER(1);

	cfg->prefixType = BOXOID;
	cfg->labelType = VOIDOID;	/* We don't use a label */
	cfg->canReturnData = true;
	cfg->longValuesOK = false;

	PG_RETURN_VOID();
}

/*
 * SP-GiST loose quadtree choose function
 */
Datum
spg_loose_quad_choose(PG_FUNCTION_ARGS)
{
	elog(LOG, "in box quadtree choose function!!");
	spgChooseIn *in = (spgChooseIn *) PG_GETARG_POINTER(0);
	spgChooseOut *out = (spgChooseOut *) PG_GETARG_POINTER(1);
	BOX		   *centroid = DatumGetBoxP(in->prefixDatum),
			   *box = DatumGetBoxP(in->leafDatum);
	double currBoxLength, newElementRadius;
	int boxNewLevel, levelDifference;

	out->resultType = spgMatchNode;
	out->result.matchNode.restDatum = BoxPGetDatum(box);

	/* nodeN will be set by core, when allTheSame. */
	if (!in->allTheSame) {
		// this code performs the level choosing equation
		int centroidWidth = centroid->high.x - centroid->low.x;
		int centroidHeight = centroid->high.y - centroid->low.y;
		currBoxLength =  (centroidWidth >= centroidHeight) ? centroidWidth : centroidHeight;
		int boxWidth = box->high.x - box->low.x;
		int boxHeight = box->high.y - box->low.y;
		newElementRadius =  (boxWidth >= boxHeight) ? boxWidth : boxHeight;
		newElementRadius = newElementRadius / 2.0;
		int levelCandidate = floor(log2(currBoxLength / newElementRadius)) - 1;
		boxNewLevel = (levelCandidate >= 0) ? levelCandidate : 0;
		int candidateLevelDiff = boxNewLevel - in->level;
		levelDifference = (candidateLevelDiff >= 0) ? candidateLevelDiff : 0;
		elog(LOG, "[choose] current level is: (%d) , inserting box at level: (%d) with level diff of (%d)\n", in->level, boxNewLevel, levelDifference);
		out->result.matchNode.levelAdd = levelDifference;

		// the following code chooses the quadrant at the new level
		// we have to simulate traversing the tree, choosing quadrant at each level
		// and upate the centroid accordingly 
		// out->result.matchNode.nodeN = getQuadrant(centroid, box);

		BOX		*currentBox = palloc(sizeof(BOX));
		int		currentQuadrant = 0;
		currentBox->high.x = centroid->high.x;
		currentBox->high.y = centroid->high.y;
		currentBox->low.x = centroid->low.x;
		currentBox->low.y = centroid->low.y;
		for (int i=0; i < levelDifference; i++) {
			elog(LOG, "[choose] current centroid (minx, miny) = (%f, %f)  -->  (maxx, maxy) = (%f, %f)\n", currentBox->low.x, currentBox->low.y, currentBox->high.x, currentBox->high.y);
			currentQuadrant = getQuadrant(currentBox, box);
			int middleX = currentBox->low.x + (currentBox->high.x - currentBox->low.x) / 2.0;
			int middleY = currentBox->low.y + (currentBox->high.y - currentBox->low.y) / 2.0;
			if (currentQuadrant == 0) {
				currentBox->low.y = middleY;
				currentBox->high.x = middleX;
			} else if (currentQuadrant == 1) {
				currentBox->low.x = middleX;
				currentBox->low.y = middleY;
			} else if (currentQuadrant == 2) {
				currentBox->high.x = middleX;
				currentBox->high.y = middleY;
			} else if (currentQuadrant == 3) {
				currentBox->low.x = middleX;
				currentBox->high.y = middleY;
			} else {
				elog(LOG, "[choose] INVALID QUADRANT SELECTED");
			}
		
		}
		out->result.matchNode.nodeN = currentQuadrant;
	}




	PG_RETURN_VOID();
}

/*
 * SP-GiST loose quadtree pick-split function
 *
 * The pick-split function is called when a leaf page runs out of space
 * and needs to create new leaf pages. These represent the quadrants in our 
 * loose quadtree. The method we choose for determining new quadrants is:
 * 
 * - Take the minimum bounding square around the space of all the boxes
 * - calculate which quadrant a box is in by comparing the centroid of the box
 *   to the quadrants, check bounds too?
 * 
 */
Datum
spg_loose_quad_picksplit(PG_FUNCTION_ARGS)
{
	elog(LOG, "in box quadtree picksplit function!!");
	spgPickSplitIn *in = (spgPickSplitIn *) PG_GETARG_POINTER(0);
	spgPickSplitOut *out = (spgPickSplitOut *) PG_GETARG_POINTER(1);
	BOX		   *centroid;
	int			median,
				i;

	double     minXY, maxXY;
	double	   *lowXs = palloc(sizeof(double) * in->nTuples);
	double	   *highXs = palloc(sizeof(double) * in->nTuples);
	double	   *lowYs = palloc(sizeof(double) * in->nTuples);
	double	   *highYs = palloc(sizeof(double) * in->nTuples);

	/* Calculate median of all 4D coordinates */
	for (i = 0; i < in->nTuples; i++)
	{
		BOX		   *box = DatumGetBoxP(in->datums[i]);

		lowXs[i] = box->low.x;
		highXs[i] = box->high.x;
		lowYs[i] = box->low.y;
		highYs[i] = box->high.y;
	}

	qsort(lowXs, in->nTuples, sizeof(double), compareDoubles);
	qsort(highXs, in->nTuples, sizeof(double), compareDoubles);
	qsort(lowYs, in->nTuples, sizeof(double), compareDoubles);
	qsort(highYs, in->nTuples, sizeof(double), compareDoubles);

	median = in->nTuples / 2;

	centroid = palloc(sizeof(BOX));

	minXY = (lowXs[0] <= lowYs[0]) ? lowXs[0] : lowYs[0];
	maxXY = (highXs[in->nTuples] >= highYs[in->nTuples]) ? highXs[in->nTuples-1] : highYs[in->nTuples-1];
	centroid->low.x = minXY;
	centroid->high.x = maxXY;
	centroid->low.y = minXY;
	centroid->high.y = maxXY;

	/* Fill the output */
	out->hasPrefix = true;
	out->prefixDatum = BoxPGetDatum(centroid);

	out->nNodes = 4;
	out->nodeLabels = NULL;		/* We don't need node labels. */

	out->mapTuplesToNodes = palloc(sizeof(int) * in->nTuples);
	out->leafTupleDatums = palloc(sizeof(Datum) * in->nTuples);

	elog(LOG, "in picksplit, current level is: %d\n", in->level);
	/*
	 * Assign ranges to corresponding nodes according to quadrants relative to
	 * the "centroid" range
	 */
	for (i = 0; i < in->nTuples; i++)
	{
		BOX		   *box = DatumGetBoxP(in->datums[i]);
		uint8		quadrant = getQuadrant(centroid, box);

		out->leafTupleDatums[i] = BoxPGetDatum(box);
		out->mapTuplesToNodes[i] = quadrant;
	}

	PG_RETURN_VOID();
}

/*
 * Check if result of consistent method based on bounding box is exact.
 */
static bool
is_bounding_box_test_exact(StrategyNumber strategy)
{
	switch (strategy)
	{
		case RTLeftStrategyNumber:
		case RTOverLeftStrategyNumber:
		case RTOverRightStrategyNumber:
		case RTRightStrategyNumber:
		case RTOverBelowStrategyNumber:
		case RTBelowStrategyNumber:
		case RTAboveStrategyNumber:
		case RTOverAboveStrategyNumber:
			return true;

		default:
			return false;
	}
}

/*
 * Get bounding box for ScanKey.
 */
static BOX *
spg_box_quad_get_scankey_bbox(ScanKey sk, bool *recheck)
{
	switch (sk->sk_subtype)
	{
		case BOXOID:
			return DatumGetBoxP(sk->sk_argument);

		case POLYGONOID:
			if (recheck && !is_bounding_box_test_exact(sk->sk_strategy))
				*recheck = true;
			return &DatumGetPolygonP(sk->sk_argument)->boundbox;

		default:
			elog(ERROR, "unrecognized scankey subtype: %d", sk->sk_subtype);
			return NULL;
	}
}

/*
 * SP-GiST loose quadtree inner consistent function
 */
Datum
spg_loose_quad_inner_consistent(PG_FUNCTION_ARGS)
{
	spgInnerConsistentIn *in = (spgInnerConsistentIn *) PG_GETARG_POINTER(0);
	spgInnerConsistentOut *out = (spgInnerConsistentOut *) PG_GETARG_POINTER(1);
	int			i;
	MemoryContext old_ctx;
	RectBox    *rect_box;
	uint8		quadrant;
	RangeBox   *centroid,
			  **queries;

	if (in->allTheSame)
	{
		/* Report that all nodes should be visited */
		out->nNodes = in->nNodes;
		out->nodeNumbers = (int *) palloc(sizeof(int) * in->nNodes);
		for (i = 0; i < in->nNodes; i++)
			out->nodeNumbers[i] = i;

		PG_RETURN_VOID();
	}

	/*
	 * We are saving the traversal value or initialize it an unbounded one, if
	 * we have just begun to walk the tree.
	 */
	if (in->traversalValue)
		rect_box = in->traversalValue;
	else
		rect_box = initRectBox();

	/*
	 * We are casting the prefix and queries to RangeBoxes for ease of the
	 * following operations.
	 */
	centroid = getRangeBox(DatumGetBoxP(in->prefixDatum));
	queries = (RangeBox **) palloc(in->nkeys * sizeof(RangeBox *));
	for (i = 0; i < in->nkeys; i++)
	{
		BOX		   *box = spg_box_quad_get_scankey_bbox(&in->scankeys[i], NULL);

		queries[i] = getRangeBox(box);
	}

	/* Allocate enough memory for nodes */
	out->nNodes = 0;
	out->nodeNumbers = (int *) palloc(sizeof(int) * in->nNodes);
	out->traversalValues = (void **) palloc(sizeof(void *) * in->nNodes);

	/*
	 * We switch memory context, because we want to allocate memory for new
	 * traversal values (next_rect_box) and pass these pieces of memory to
	 * further call of this function.
	 */
	old_ctx = MemoryContextSwitchTo(in->traversalMemoryContext);

	for (quadrant = 0; quadrant < in->nNodes; quadrant++)
	{
		RectBox    *next_rect_box = nextRectBox(rect_box, centroid, quadrant);
		bool		flag = true;

		for (i = 0; i < in->nkeys; i++)
		{
			StrategyNumber strategy = in->scankeys[i].sk_strategy;

			switch (strategy)
			{
				case RTOverlapStrategyNumber:
					flag = overlap4D(next_rect_box, queries[i]);
					break;

				case RTContainsStrategyNumber:
					flag = contain4D(next_rect_box, queries[i]);
					break;

				case RTSameStrategyNumber:
				case RTContainedByStrategyNumber:
					flag = contained4D(next_rect_box, queries[i]);
					break;

				case RTLeftStrategyNumber:
					flag = left4D(next_rect_box, queries[i]);
					break;

				case RTOverLeftStrategyNumber:
					flag = overLeft4D(next_rect_box, queries[i]);
					break;

				case RTRightStrategyNumber:
					flag = right4D(next_rect_box, queries[i]);
					break;

				case RTOverRightStrategyNumber:
					flag = overRight4D(next_rect_box, queries[i]);
					break;

				case RTAboveStrategyNumber:
					flag = above4D(next_rect_box, queries[i]);
					break;

				case RTOverAboveStrategyNumber:
					flag = overAbove4D(next_rect_box, queries[i]);
					break;

				case RTBelowStrategyNumber:
					flag = below4D(next_rect_box, queries[i]);
					break;

				case RTOverBelowStrategyNumber:
					flag = overBelow4D(next_rect_box, queries[i]);
					break;

				default:
					elog(ERROR, "unrecognized strategy: %d", strategy);
			}

			/* If any check is failed, we have found our answer. */
			if (!flag)
				break;
		}

		if (flag)
		{
			out->traversalValues[out->nNodes] = next_rect_box;
			out->nodeNumbers[out->nNodes] = quadrant;
			out->nNodes++;
		}
		else
		{
			/*
			 * If this node is not selected, we don't need to keep the next
			 * traversal value in the memory context.
			 */
			pfree(next_rect_box);
		}
	}

	/* Switch back */
	MemoryContextSwitchTo(old_ctx);

	PG_RETURN_VOID();
}

/*
 * SP-GiST loose quadtree inner consistent function
 */
Datum
spg_loose_quad_leaf_consistent(PG_FUNCTION_ARGS)
{
	spgLeafConsistentIn *in = (spgLeafConsistentIn *) PG_GETARG_POINTER(0);
	spgLeafConsistentOut *out = (spgLeafConsistentOut *) PG_GETARG_POINTER(1);
	Datum		leaf = in->leafDatum;
	bool		flag = true;
	int			i;

	/* All tests are exact. */
	out->recheck = false;

	/* leafDatum is what it is... */
	out->leafValue = in->leafDatum;

	/* Perform the required comparison(s) */
	for (i = 0; i < in->nkeys; i++)
	{
		StrategyNumber strategy = in->scankeys[i].sk_strategy;
		BOX		   *box = spg_box_quad_get_scankey_bbox(&in->scankeys[i],
														&out->recheck);
		Datum		query = BoxPGetDatum(box);

		switch (strategy)
		{
			case RTOverlapStrategyNumber:
				flag = DatumGetBool(DirectFunctionCall2(box_overlap, leaf,
														query));
				break;

			case RTContainsStrategyNumber:
				flag = DatumGetBool(DirectFunctionCall2(box_contain, leaf,
														query));
				break;

			case RTContainedByStrategyNumber:
				flag = DatumGetBool(DirectFunctionCall2(box_contained, leaf,
														query));
				break;

			case RTSameStrategyNumber:
				flag = DatumGetBool(DirectFunctionCall2(box_same, leaf,
														query));
				break;

			case RTLeftStrategyNumber:
				flag = DatumGetBool(DirectFunctionCall2(box_left, leaf,
														query));
				break;

			case RTOverLeftStrategyNumber:
				flag = DatumGetBool(DirectFunctionCall2(box_overleft, leaf,
														query));
				break;

			case RTRightStrategyNumber:
				flag = DatumGetBool(DirectFunctionCall2(box_right, leaf,
														query));
				break;

			case RTOverRightStrategyNumber:
				flag = DatumGetBool(DirectFunctionCall2(box_overright, leaf,
														query));
				break;

			case RTAboveStrategyNumber:
				flag = DatumGetBool(DirectFunctionCall2(box_above, leaf,
														query));
				break;

			case RTOverAboveStrategyNumber:
				flag = DatumGetBool(DirectFunctionCall2(box_overabove, leaf,
														query));
				break;

			case RTBelowStrategyNumber:
				flag = DatumGetBool(DirectFunctionCall2(box_below, leaf,
														query));
				break;

			case RTOverBelowStrategyNumber:
				flag = DatumGetBool(DirectFunctionCall2(box_overbelow, leaf,
														query));
				break;

			default:
				elog(ERROR, "unrecognized strategy: %d", strategy);
		}

		/* If any check is failed, we have found our answer. */
		if (!flag)
			break;
	}

	PG_RETURN_BOOL(flag);
}


/*
 * SP-GiST config function for 2-D types that are lossy represented by their
 * bounding boxes
 */
Datum
spg_bbox_quad_config(PG_FUNCTION_ARGS)
{
	spgConfigOut *cfg = (spgConfigOut *) PG_GETARG_POINTER(1);

	cfg->prefixType = BOXOID;	/* A type represented by its bounding box */
	cfg->labelType = VOIDOID;	/* We don't need node labels. */
	cfg->leafType = BOXOID;
	cfg->canReturnData = false;
	cfg->longValuesOK = false;

	PG_RETURN_VOID();
}

/*
 * SP-GiST compress function for polygons
 */
Datum
spg_poly_quad_compress(PG_FUNCTION_ARGS)
{
	POLYGON    *polygon = PG_GETARG_POLYGON_P(0);
	BOX		   *box;

	box = box_copy(&polygon->boundbox);

	PG_RETURN_BOX_P(box);
}