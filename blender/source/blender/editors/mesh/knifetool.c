/*
 * ***** BEGIN GPL LICENSE BLOCK *****
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 * The Original Code is Copyright (C) 2007 Blender Foundation.
 * All rights reserved.
 *
 * 
 * Contributor(s): Joseph Eagar, Joshua Leung
 *
 * ***** END GPL LICENSE BLOCK *****
 */

#define _USE_MATH_DEFINES


#include "MEM_guardedalloc.h"


#include "BLI_blenlib.h"
#include "BLI_array.h"
#include "BLI_math.h"
#include "BLI_rand.h"
#include "BLI_smallhash.h"
#include "BLI_scanfill.h"

#include "BKE_DerivedMesh.h"
#include "BKE_context.h"
#include "BKE_depsgraph.h"

#include "BIF_gl.h"
#include "BIF_glutil.h" /* for paint cursor */


#include "ED_screen.h"
#include "ED_space_api.h"
#include "ED_view3d.h"
#include "ED_mesh.h"

#include "WM_api.h"
#include "WM_types.h"

#include "DNA_scene_types.h"
#include "DNA_mesh_types.h"
#include "DNA_object_types.h"
#include "BKE_tessmesh.h"

#include "mesh_intern.h"

/* this code here is kindof messy. . .I might need to eventually rework it - joeedh */

#define MAXGROUP	30
#define KMAXDIST	10	/* max mouse distance from edge before not detecting it */

/* knifetool operator */
typedef struct KnifeVert {
	BMVert *v; /* non-NULL if this is an original vert */
	ListBase edges;

	float co[3], cageco[3], sco[3]; /* sco is screen coordinates for cageco */
	short flag, draw, isface, inspace;
} KnifeVert;

typedef struct Ref {
	struct Ref *next, *prev;
	void *ref;
} Ref;

typedef struct KnifeEdge {
	KnifeVert *v1, *v2;
	BMFace *basef; /* face to restrict face fill to */
	ListBase faces;
	int draw;
	
	BMEdge *e, *oe; /* non-NULL if this is an original edge */
} KnifeEdge;

typedef struct BMEdgeHit {
	KnifeEdge *kfe;
	float hit[3], cagehit[3];
	float realhit[3]; /* used in midpoint mode */
	float schit[3];
	float l; /* lambda along cut line */
	float perc; /* lambda along hit line */
	KnifeVert *v; /* set if snapped to a vert */
	BMFace *f;
} BMEdgeHit;

/* struct for properties used while drawing */
typedef struct knifetool_opdata {
	ARegion *ar;		/* region that knifetool was activated in */
	void *draw_handle;	/* for drawing preview loop */
	ViewContext vc;
	bContext *C;
	
	Object *ob;
	BMEditMesh *em;
	
	MemArena *arena;

	GHash *origvertmap;
	GHash *origedgemap;
	
	GHash *kedgefacemap;
	
	BMBVHTree *bmbvh;

	BLI_mempool *kverts;
	BLI_mempool *kedges;
	
	float vthresh;
	float ethresh;
	
	float vertco[3], vertcage[3];
	float prevco[3], prevcage[3];
	
	/* used for drag-cutting */
	BMEdgeHit *linehits;
	int totlinehit;
	
	/* if curedge is NULL, attach to curvert;
	 * if curvert is NULL, attach to curbmface,
	 * otherwise create null vert */
	KnifeEdge *curedge, *prevedge;
	KnifeVert *curvert, *prevvert;
	BMFace *curbmface, *prevbmface;

	int totkedge, totkvert, cutnr;
	
	BLI_mempool *refs;
	
	float projmat[4][4];
	int is_ortho;
	int cut_through;
	float clipsta, clipend;
	
	enum {
		MODE_IDLE,
		MODE_DRAGGING,
		MODE_CONNECT,
		MODE_PANNING
	} mode;
	
	int snap_midpoints, prevmode, extend;
	int ignore_edge_snapping, ignore_vert_snapping;
	int prevmval[2];
	
	enum {
		ANGLE_FREE,
		ANGLE_0,
		ANGLE_45,
		ANGLE_90,
		ANGLE_135
	} angle_snapping;

	int is_space, prev_is_space; /*1 if current cut location, vertco, isn't on the mesh */
	float (*cagecos)[3];
} knifetool_opdata;

static ListBase *knife_get_face_kedges(knifetool_opdata *kcd, BMFace *f);

static void knife_input_ray_cast(knifetool_opdata *kcd, const int mval_i[2],
                                 float r_origin[3], float r_ray[3]);

static void knife_project_v3(knifetool_opdata *kcd, const float co[3], float sco[3])
{
	ED_view3d_project_float(kcd->ar, co, sco, kcd->projmat);
}

static ListBase *knife_empty_list(knifetool_opdata *kcd) {
	ListBase *lst;

	lst = BLI_memarena_alloc(kcd->arena, sizeof(ListBase));
	lst->first = lst->last = NULL;
	return lst;
}

static void knife_append_list(knifetool_opdata *kcd, ListBase *lst, void *elem) {
	Ref *ref;

	ref = BLI_mempool_calloc(kcd->refs);
	ref->ref = elem;
	BLI_addtail(lst, ref);
}

static KnifeEdge *new_knife_edge(knifetool_opdata *kcd)
{
	kcd->totkedge++;
	return BLI_mempool_calloc(kcd->kedges);
}

static void knife_add_to_vert_edges(knifetool_opdata *kcd, KnifeEdge* kfe)
{
	knife_append_list(kcd, &kfe->v1->edges, kfe);
	knife_append_list(kcd, &kfe->v2->edges, kfe);
}

static KnifeVert *new_knife_vert(knifetool_opdata *kcd, float *co, float *cageco)
{
	KnifeVert *kfv = BLI_mempool_calloc(kcd->kverts);
	
	kcd->totkvert++;
	
	copy_v3_v3(kfv->co, co);
	copy_v3_v3(kfv->cageco, cageco);
	copy_v3_v3(kfv->sco, co);

	knife_project_v3(kcd, kfv->co, kfv->sco);

	return kfv;
}

/* get a KnifeVert wrapper for an existing BMVert */
static KnifeVert *get_bm_knife_vert(knifetool_opdata *kcd, BMVert *v)
{
	KnifeVert *kfv = BLI_ghash_lookup(kcd->origvertmap, v);
	
	if (!kfv) {
		kfv = new_knife_vert(kcd, v->co, kcd->cagecos[BM_elem_index_get(v)]);
		kfv->v = v;
		BLI_ghash_insert(kcd->origvertmap, v, kfv);
	}
	
	return kfv;
}

/* get a KnifeEdge wrapper for an existing BMEdge */
static KnifeEdge *get_bm_knife_edge(knifetool_opdata *kcd, BMEdge *e)
{
	KnifeEdge *kfe = BLI_ghash_lookup(kcd->origedgemap, e);
	if (!kfe) {
		BMIter iter;
		BMFace *f;
		
		kfe = new_knife_edge(kcd);
		kfe->e = e;
		kfe->v1 = get_bm_knife_vert(kcd, e->v1);
		kfe->v2 = get_bm_knife_vert(kcd, e->v2);

		knife_add_to_vert_edges(kcd, kfe);
		
		BLI_ghash_insert(kcd->origedgemap, e, kfe);
		
		BM_ITER(f, &iter, kcd->em->bm, BM_FACES_OF_EDGE, e) {
			knife_append_list(kcd, &kfe->faces, f);
			
			/* ensures the kedges lst for this f is initialized,
			 * it automatically adds kfe by itself */
			knife_get_face_kedges(kcd, f);
		}
	}
	
	return kfe;
}

static void knife_start_cut(knifetool_opdata *kcd)
{
	kcd->prevedge = kcd->curedge;
	kcd->prevvert = kcd->curvert;
	kcd->prevbmface = kcd->curbmface;
	kcd->cutnr++;
	kcd->prev_is_space = kcd->is_space;
	kcd->is_space = 0;
	kcd->prevmval[0] = kcd->vc.mval[0];
	kcd->prevmval[1] = kcd->vc.mval[1];
	
	copy_v3_v3(kcd->prevco, kcd->vertco);
	copy_v3_v3(kcd->prevcage, kcd->vertcage);

	if (kcd->prevvert == NULL && kcd->prevedge == NULL && is_zero_v3(kcd->prevcage)) {
		/* Make prevcage a point on the view ray to mouse closest to a point on model: choose vertex 0 */
		float origin[3], ray[3], co[3];
		BMVert *v0;

		knife_input_ray_cast(kcd, kcd->vc.mval, origin, ray);
		add_v3_v3v3(co, origin, ray);
		v0 = BM_vert_at_index(kcd->em->bm, 0);
		if (v0) {
			closest_to_line_v3(kcd->prevcage, v0->co, co, origin);
			copy_v3_v3(kcd->prevco, kcd->prevcage);
			copy_v3_v3(kcd->vertcage, kcd->prevcage);
			copy_v3_v3(kcd->vertco, kcd->prevco);
		}
	}
}

static Ref *find_ref(ListBase *lb, void *ref)
{
	Ref *ref1;
	
	for (ref1 = lb->first; ref1; ref1 = ref1->next) {
		if (ref1->ref == ref)
			return ref1;
	}
	
	return NULL;
}

static ListBase *knife_get_face_kedges(knifetool_opdata *kcd, BMFace *f)
{
	ListBase *lst = BLI_ghash_lookup(kcd->kedgefacemap, f);
	
	if (!lst) {
		BMIter iter;
		BMEdge *e;
		
		lst = knife_empty_list(kcd);
		
		BM_ITER(e, &iter, kcd->em->bm, BM_EDGES_OF_FACE, f) {
			knife_append_list(kcd, lst, get_bm_knife_edge(kcd, e));
		}
		
		BLI_ghash_insert(kcd->kedgefacemap, f, lst);
	}
	
	return lst;
}

/* finds the proper face to restrict face fill to */
static void knife_find_basef(knifetool_opdata *kcd, KnifeEdge *kfe)
{
	if (!kfe->basef) {
		Ref *r1, *r2, *r3, *r4;
		
		if (kfe->v1->isface || kfe->v2->isface) {
			if (kfe->v2->isface)
				kfe->basef = kcd->curbmface;
			else 
				kfe->basef = kcd->prevbmface;
		}
		else {
			for (r1 = kfe->v1->edges.first; r1 && !kfe->basef; r1 = r1->next) {
				KnifeEdge *ke1 = r1->ref;
				for (r2 = ke1->faces.first; r2 && !kfe->basef; r2 = r2->next) {
					for (r3 = kfe->v2->edges.first; r3 && !kfe->basef; r3 = r3->next) {
						KnifeEdge *ke2 = r3->ref;
					
						for (r4 = ke2->faces.first; r4 && !kfe->basef; r4 = r4->next) {
							if (r2->ref == r4->ref) {
								kfe->basef = r2->ref;
							}
						}	
					}
				}
			}
		}
		/* ok, at this point kfe->basef should be set if any valid possibility exists */
	}
}

static void knife_edge_append_face(knifetool_opdata *kcd, KnifeEdge *kfe, BMFace *f)
{
	knife_append_list(kcd, knife_get_face_kedges(kcd, f), kfe);
	knife_append_list(kcd, &kfe->faces, f);
}

static KnifeVert *knife_split_edge(knifetool_opdata *kcd, KnifeEdge *kfe, float co[3], KnifeEdge **newkfe_out)
{
	KnifeEdge *newkfe = new_knife_edge(kcd);
	Ref *ref;
	float perc, cageco[3];
	
	perc = len_v3v3(co, kfe->v1->co) / len_v3v3(kfe->v1->co, kfe->v2->co);
	interp_v3_v3v3(cageco, kfe->v1->cageco, kfe->v2->cageco, perc);
	
	newkfe->v1 = kfe->v1;
	newkfe->v2 = new_knife_vert(kcd, co, cageco);
	newkfe->v2->draw = 1;
	newkfe->basef = kfe->basef;
	
	ref = find_ref(&kfe->v1->edges, kfe);
	BLI_remlink(&kfe->v1->edges, ref);
	
	kfe->v1 = newkfe->v2;
	BLI_addtail(&kfe->v1->edges, ref);
	
	for (ref = kfe->faces.first; ref; ref = ref->next)
		knife_edge_append_face(kcd, newkfe, ref->ref);

	knife_add_to_vert_edges(kcd, newkfe);
	
	newkfe->draw = kfe->draw;
	newkfe->e = kfe->e;
	
	*newkfe_out = newkfe;
			
	return newkfe->v2;
}

static void knife_add_single_cut(knifetool_opdata *kcd)
{
	KnifeEdge *kfe = new_knife_edge(kcd), *kfe2 = NULL, *kfe3 = NULL;
	
	if (kcd->prevvert && kcd->prevvert == kcd->curvert)
		return;
	if (kcd->prevedge && kcd->prevedge == kcd->curedge)
		return;
	
	kfe->draw = 1;

	if (kcd->prevvert) {
		kfe->v1 = kcd->prevvert;
	}
	else if (kcd->prevedge) {
		kfe->v1 = knife_split_edge(kcd, kcd->prevedge, kcd->prevco, &kfe2);
	}
	else {
		kfe->v1 = new_knife_vert(kcd, kcd->prevco, kcd->prevco);
		kfe->v1->draw = kfe->draw = !kcd->prev_is_space;
		kfe->v1->inspace = kcd->prev_is_space;
		kfe->draw = !kcd->prev_is_space;
		kfe->v1->isface = 1;
	}
	
	if (kcd->curvert) {
		kfe->v2 = kcd->curvert;
	}
	else if (kcd->curedge) {
		kfe->v2 = knife_split_edge(kcd, kcd->curedge, kcd->vertco, &kfe3);
	
		kcd->curvert = kfe->v2;
	}
	else {
		kfe->v2 = new_knife_vert(kcd, kcd->vertco, kcd->vertco);
		kfe->v2->draw = !kcd->is_space;
		kfe->v2->isface = 1;
		kfe->v2->inspace = kcd->is_space;
		
		if (kcd->is_space)
			kfe->draw = 0;

		kcd->curvert = kfe->v2;
	}
	
	knife_find_basef(kcd, kfe);
	
	knife_add_to_vert_edges(kcd, kfe);
	
	if (kfe->basef && !find_ref(&kfe->faces, kfe->basef))
		knife_edge_append_face(kcd, kfe, kfe->basef);

	/* sanity check to make sure we're in the right edge/face lists */
	if (kcd->curbmface) {
		if (!find_ref(&kfe->faces, kcd->curbmface)) {
			knife_edge_append_face(kcd, kfe, kcd->curbmface);
		}

		if (kcd->prevbmface && kcd->prevbmface != kcd->curbmface) {
			if (!find_ref(&kfe->faces, kcd->prevbmface)) {
				knife_edge_append_face(kcd, kfe, kcd->prevbmface);
			}
		}
	}
		
	/* set up for next cut */
	kcd->prevbmface = kcd->curbmface;
	kcd->prevvert = kcd->curvert;
	kcd->prevedge = kcd->curedge;
	copy_v3_v3(kcd->prevco, kcd->vertco);
	copy_v3_v3(kcd->prevcage, kcd->vertcage);
	kcd->prev_is_space = kcd->is_space;
	kcd->prevmval[0] = kcd->vc.mval[0];
	kcd->prevmval[1] = kcd->vc.mval[1];
}

static int verge_linehit(const void *vlh1, const void *vlh2)
{
	const BMEdgeHit *lh1 = vlh1, *lh2 = vlh2;

	if (lh1->l < lh2->l) return -1;
	else if (lh1->l > lh2->l) return 1;
	else return 0;
}

static void knife_add_single_cut_through(knifetool_opdata *kcd,
	KnifeVert *v1, KnifeVert *v2, BMFace *f)
{
	KnifeEdge *kfenew;

	kfenew = new_knife_edge(kcd);
	kfenew->draw = 1;
	kfenew->basef = f;
	kfenew->v1 = v1;
	kfenew->v2 = v2;
	kfenew->draw = 1;

	knife_add_to_vert_edges(kcd, kfenew);

	if (!find_ref(&kfenew->faces, f))
		knife_edge_append_face(kcd, kfenew, f);
}

static void knife_get_vert_faces(knifetool_opdata *kcd, KnifeVert* kfv, BMFace *facef, ListBase *lst)
{
	BMIter bmiter;
	BMFace *f;

	if (kfv->isface && facef) {
		knife_append_list(kcd, lst, facef);
	}
	else if (kfv->v) {
		BMesh *bm = kcd->em->bm;
		BM_ITER(f, &bmiter, bm, BM_FACES_OF_VERT, kfv->v) {
			knife_append_list(kcd, lst, f);
		}
	}
}

static void knife_get_edge_faces(knifetool_opdata *kcd, KnifeEdge* kfe, ListBase *lst)
{
	BMIter bmiter;
	BMFace *f;

	if (kfe->e) {
		BMesh *bm = kcd->em->bm;
		BM_ITER(f, &bmiter, bm, BM_FACES_OF_EDGE, kfe->e) {
			knife_append_list(kcd, lst, f);
		}
	}
}

/* BMESH_TODO: add more functionality to cut-through:
 *    - cutting "in face" (e.g., holes) should cut in all faces, not just visible one
 *    - perhaps improve O(n^2) algorithm used here */
static void knife_cut_through(knifetool_opdata *kcd)
{
	BMEdgeHit *lh, *lh2;
	BMFace *f;
	KnifeEdge *kfe, *kfe2, *kfe3;
	KnifeVert *v1, *v2, *firstv = NULL, *lastv = NULL;
	ListBase firstfaces = {NULL, NULL}, lastfaces = { NULL, NULL};
	Ref *r, *r2;
	KnifeEdge **splitkfe;
	int i, j, found;

	if (!kcd->totlinehit) {
		/* if no linehits then no interesting back face stuff to do */
		knife_add_single_cut(kcd);
		return;
	}

	qsort(kcd->linehits, kcd->totlinehit, sizeof(BMEdgeHit), verge_linehit);
	splitkfe = MEM_callocN(kcd->totlinehit * sizeof(KnifeEdge *), "knife_cut_through");

	if (kcd->prevvert) {
		if (kcd->prevvert == kcd->curvert)
			return;
		firstv = kcd->prevvert;
		knife_get_vert_faces(kcd, firstv, kcd->prevbmface, &firstfaces);
	}
	else if (kcd->prevedge) {
		if (kcd->prevedge == kcd->curedge)
			return;
		firstv = knife_split_edge(kcd, kcd->prevedge, kcd->prevco, &kfe3);
		knife_get_edge_faces(kcd, kcd->prevedge, &firstfaces);
	}

	if (kcd->curvert) {
		lastv = kcd->curvert;
		knife_get_vert_faces(kcd, lastv, kcd->curbmface, &lastfaces);
	}
	else if (kcd->curedge) {
		lastv = knife_split_edge(kcd, kcd->curedge, kcd->vertco, &kfe3);
		knife_get_edge_faces(kcd, kcd->curedge, &lastfaces);
	}

	if (firstv) {
		/* For each face incident to firstv,
		 * find the first following linehit (if any) sharing that face and connect */
		 for (r = firstfaces.first; r; r = r->next ) {
		 	f = r->ref;
		 	found = 0;
		 	for (j = 0, lh2 = kcd->linehits; j < kcd->totlinehit; j++, lh2++) {
		 		kfe2 = lh2->kfe;
		 		for (r2 = kfe2->faces.first; r2; r2 = r2->next) {
		 			if (r2->ref == f) {
		 				v2 = splitkfe[j] ? kfe2->v1 : knife_split_edge(kcd, kfe2, lh2->hit, &splitkfe[j]);
		 				knife_add_single_cut_through(kcd, firstv, v2, f);
		 				found = 1;
		 				break;
		 			}
		 		}
		 	}
		 	if (!found && lastv) {
				for (r2 = lastfaces.first; r2; r2 = r2->next) {
					if (r2->ref == f) {
						knife_add_single_cut_through(kcd, firstv, lastv, f);
						break;
					}
				}
		 	}
		 }
	}

	for (i = 0, lh = kcd->linehits; i < kcd->totlinehit; i++, lh++) {
		kfe = lh->kfe;

		/* For each face attached to edge for this linehit,
		 * find the first following linehit (if any) sharing that face and connect */
		for (r = kfe->faces.first; r; r = r->next) {
			f = r->ref;
			found = 0;
			for (j = i + 1, lh2 = lh + 1; j < kcd->totlinehit; j++, lh2++) {
				kfe2 = lh2->kfe;
				for (r2 = kfe2->faces.first; r2; r2 = r2->next) {
					if (r2->ref == f) {
						v1 = splitkfe[i]? kfe->v1 : knife_split_edge(kcd, kfe, lh->hit, &splitkfe[i]);
						v2 = splitkfe[j]? kfe2->v1 : knife_split_edge(kcd, kfe2, lh2->hit, &splitkfe[j]);
						knife_add_single_cut_through(kcd, v1, v2, f);
						found = 1;
						break;
					}
				}
			}
			if (!found && lastv) {
				for (r2 = lastfaces.first; r2; r2 = r2->next) {
					if (r2->ref == f) {
						v1 = splitkfe[i]? kfe->v1 : knife_split_edge(kcd, kfe, lh->hit, &splitkfe[i]);
						knife_add_single_cut_through(kcd, v1, lastv, f);
						break;
					}
				}
			}
		}
	}

	MEM_freeN(splitkfe);
	MEM_freeN(kcd->linehits);
	kcd->linehits = NULL;
	kcd->totlinehit = 0;

	/* set up for next cut */
	kcd->prevbmface = kcd->curbmface;
	kcd->prevvert = kcd->curvert;
	kcd->prevedge = kcd->curedge;
	copy_v3_v3(kcd->prevco, kcd->vertco);
	copy_v3_v3(kcd->prevcage, kcd->vertcage);
	kcd->prev_is_space = kcd->is_space;
	kcd->prevmval[0] = kcd->vc.mval[0];
	kcd->prevmval[1] = kcd->vc.mval[1];
}

static void knife_add_cut(knifetool_opdata *kcd)
{
	/* BMEditMesh *em = kcd->em;*/ /* UNUSED */
	knifetool_opdata oldkcd = *kcd;
	
	if (kcd->cut_through) {
		knife_cut_through(kcd);
	}
	else if (kcd->linehits) {
		BMEdgeHit *lh, *lastlh, *firstlh;
		int i;
		
		qsort(kcd->linehits, kcd->totlinehit, sizeof(BMEdgeHit), verge_linehit);
		
		lh = kcd->linehits;
		lastlh = firstlh = NULL;
		for (i = 0; i < kcd->totlinehit; i++, (lastlh = lh), lh++) {
			BMFace *f = lastlh ? lastlh->f : lh->f;
			
			if (lastlh && len_v3v3(lastlh->hit, lh->hit) == 0.0f) {
				if (!firstlh)
					firstlh = lastlh;
				continue;
			}
			else if (lastlh && firstlh) {
				if (firstlh->v || lastlh->v) {
					KnifeVert *kfv = firstlh->v ? firstlh->v : lastlh->v;
					
					kcd->prevvert = kfv;
					copy_v3_v3(kcd->prevco, firstlh->hit);
					copy_v3_v3(kcd->prevcage, firstlh->cagehit);
					kcd->prevedge = NULL;
					kcd->prevbmface = f;
				}
				lastlh = firstlh = NULL;
			}
			
			if (len_v3v3(kcd->prevcage, lh->realhit) < FLT_EPSILON * 80)
				continue;
			if (len_v3v3(kcd->vertcage, lh->realhit) < FLT_EPSILON * 80)
				continue;
			
			if (kcd->prev_is_space || kcd->is_space) {
				kcd->prev_is_space = kcd->is_space = 0;
				copy_v3_v3(kcd->prevco, lh->hit);
				copy_v3_v3(kcd->prevcage, lh->cagehit);
				kcd->prevedge = lh->kfe;
				kcd->curbmface = lh->f;
				continue;
			}			
			
			kcd->is_space = 0;
			kcd->curedge = lh->kfe;
			kcd->curbmface = lh->f;
			kcd->curvert = lh->v;
			copy_v3_v3(kcd->vertco, lh->hit);
			copy_v3_v3(kcd->vertcage, lh->cagehit);

			knife_add_single_cut(kcd);
		}
		
		kcd->curbmface = oldkcd.curbmface;
		kcd->curvert = oldkcd.curvert;
		kcd->curedge = oldkcd.curedge;
		kcd->is_space = oldkcd.is_space;
		copy_v3_v3(kcd->vertco, oldkcd.vertco);
		copy_v3_v3(kcd->vertcage, oldkcd.vertcage);
		
		knife_add_single_cut(kcd);
		
		MEM_freeN(kcd->linehits);
		kcd->linehits = NULL;
		kcd->totlinehit = 0;
	}
	else {
		knife_add_single_cut(kcd);
	}
}

static void knife_finish_cut(knifetool_opdata *UNUSED(kcd))
{

}

static void knifetool_draw_angle_snapping(knifetool_opdata *kcd)
{
	bglMats mats;
	double u[3], u1[2], u2[2], v1[3], v2[3], dx, dy;
	double wminx, wminy, wmaxx, wmaxy;

	/* make u the window coords of prevcage */
	view3d_get_transformation(kcd->ar, kcd->vc.rv3d, kcd->ob, &mats);
	gluProject(kcd->prevcage[0], kcd->prevcage[1], kcd->prevcage[2],
		mats.modelview, mats.projection, mats.viewport,
		&u[0], &u[1], &u[2]);

	/* make u1, u2 the points on window going through u at snap angle */
	wminx = kcd->ar->winrct.xmin;
	wmaxx = kcd->ar->winrct.xmin + kcd->ar->winx;
	wminy = kcd->ar->winrct.ymin;
	wmaxy = kcd->ar->winrct.ymin + kcd->ar->winy;

	switch (kcd->angle_snapping) {
		case ANGLE_0:
			u1[0] = wminx;
			u2[0] = wmaxx;
			u1[1] = u2[1] = u[1];
			break;
		case ANGLE_90:
			u1[0] = u2[0] = u[0];
			u1[1] = wminy;
			u2[1] = wmaxy;
			break;
		case ANGLE_45:
			/* clip against left or bottom */
			dx = u[0] - wminx;
			dy = u[1] - wminy;
			if (dy > dx) {
				u1[0] = wminx;
				u1[1] = u[1] - dx;
			}
			else {
				u1[0] = u[0] - dy;
				u1[1] = wminy;
			}
			/* clip against right or top */
			dx = wmaxx - u[0];
			dy = wmaxy - u[1];
			if (dy > dx) {
				u2[0] = wmaxx;
				u2[1] = u[1] + dx;
			}
			else {
				u2[0] = u[0] + dy;
				u2[1] = wmaxy;
			}
			break;
		case ANGLE_135:
			/* clip against right or bottom */
			dx = wmaxx - u[0];
			dy = u[1] - wminy;
			if (dy > dx) {
				u1[0] = wmaxx;
				u1[1] = u[1] - dx;
			}
			else {
				u1[0] = u[0] + dy;
				u1[1] = wminy;
			}
			/* clip against left or top */
			dx = u[0] - wminx;
			dy = wmaxy - u[1];
			if (dy > dx) {
				u2[0] = wminx;
				u2[1] = u[1] + dx;
			}
			else {
				u2[0] = u[0] - dy;
				u2[1] = wmaxy;
			}
			break;
		default:
			return;
	}

	/* unproject u1 and u2 back into object space */
	gluUnProject(u1[0], u1[1], 0.0,
		mats.modelview, mats.projection, mats.viewport,
		&v1[0], &v1[1], &v1[2]);
	gluUnProject(u2[0], u2[1], 0.0,
		mats.modelview, mats.projection, mats.viewport,
		&v2[0], &v2[1], &v2[2]);

	glColor3f(0.6, 0.6, 0.6);
	glLineWidth(2.0);
	glBegin(GL_LINES);
	glVertex3dv(v1);
	glVertex3dv(v2);
	glEnd();
}

/* modal loop selection drawing callback */
static void knifetool_draw(const bContext *UNUSED(C), ARegion *UNUSED(ar), void *arg)
{
	knifetool_opdata *kcd = arg;
	
	glDisable(GL_DEPTH_TEST);
	
	glPolygonOffset(1.0f, 1.0f);
	
	glPushMatrix();
	glMultMatrixf(kcd->ob->obmat);
	
	if (kcd->mode == MODE_DRAGGING) {
		if (kcd->angle_snapping != ANGLE_FREE)
			knifetool_draw_angle_snapping(kcd);

		glColor3f(0.1, 0.1, 0.1);
		glLineWidth(2.0);
		
		glBegin(GL_LINES);
		glVertex3fv(kcd->prevcage);
		glVertex3fv(kcd->vertcage);
		glEnd();
		
		glLineWidth(1.0);
	}
	
	if (kcd->curedge) {
		glColor3f(0.5, 0.3, 0.15);
		glLineWidth(2.0);
		
		glBegin(GL_LINES);
		glVertex3fv(kcd->curedge->v1->cageco);
		glVertex3fv(kcd->curedge->v2->cageco);
		glEnd();
		
		glLineWidth(1.0);
	}
	else if (kcd->curvert) {
		glColor3f(0.8, 0.2, 0.1);
		glPointSize(11);
		
		glBegin(GL_POINTS);
		glVertex3fv(kcd->vertcage);
		glEnd();
	}
	
	if (kcd->curbmface) {		
		glColor3f(0.1, 0.8, 0.05);
		glPointSize(9);
		
		glBegin(GL_POINTS);
		glVertex3fv(kcd->vertcage);
		glEnd();
	}
	
	if (kcd->totlinehit > 0) {
		BMEdgeHit *lh;
		int i;
		
		glEnable(GL_BLEND);
		glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
		
		/* draw any snapped verts first */
		glColor4f(0.8, 0.2, 0.1, 0.4);
		glPointSize(11);
		glBegin(GL_POINTS);
		lh = kcd->linehits;
		for (i = 0; i < kcd->totlinehit; i++, lh++) {
			float sv1[3], sv2[3];
			
			knife_project_v3(kcd, lh->kfe->v1->cageco, sv1);
			knife_project_v3(kcd, lh->kfe->v2->cageco, sv2);
			knife_project_v3(kcd, lh->cagehit, lh->schit);
			
			if (len_v2v2(lh->schit, sv1) < kcd->vthresh / 4.0f) {
				copy_v3_v3(lh->cagehit, lh->kfe->v1->cageco);
				glVertex3fv(lh->cagehit);
				lh->v = lh->kfe->v1;
			}
			else if (len_v2v2(lh->schit, sv2) < kcd->vthresh / 4.0f) {
				copy_v3_v3(lh->cagehit, lh->kfe->v2->cageco);
				glVertex3fv(lh->cagehit);
				lh->v = lh->kfe->v2;
			}
		}
		glEnd();
		
		/* now draw the rest */
		glColor4f(0.1, 0.8, 0.05, 0.4);
		glPointSize(7);
		glBegin(GL_POINTS);
		lh = kcd->linehits;
		for (i = 0; i < kcd->totlinehit; i++, lh++) {
			glVertex3fv(lh->cagehit);
		}
		glEnd();
		glDisable(GL_BLEND);
	}
	
	if (kcd->totkedge > 0) {
		BLI_mempool_iter iter;
		KnifeEdge *kfe;
		
		glLineWidth(1.0);
		glBegin(GL_LINES);

		BLI_mempool_iternew(kcd->kedges, &iter);
		for (kfe = BLI_mempool_iterstep(&iter); kfe; kfe = BLI_mempool_iterstep(&iter)) {
			if (!kfe->draw)
				continue;
				
			glColor3f(0.2, 0.2, 0.2);
			
			glVertex3fv(kfe->v1->cageco);
			glVertex3fv(kfe->v2->cageco);
		}
		
		glEnd();
		glLineWidth(1.0);
	}

	if (kcd->totkvert > 0) {
		BLI_mempool_iter iter;
		KnifeVert *kfv;
		
		glPointSize(5.0);
				
		glBegin(GL_POINTS);
		BLI_mempool_iternew(kcd->kverts, &iter);
		for (kfv = BLI_mempool_iterstep(&iter); kfv; kfv = BLI_mempool_iterstep(&iter)) {
			if (!kfv->draw)
				continue;
				
			glColor3f(0.6, 0.1, 0.2);
			
			glVertex3fv(kfv->cageco);
		}
		
		glEnd();
	}

	glPopMatrix();
	glEnable(GL_DEPTH_TEST);
}

/* do we need to keep these functions? - campbell */

static int UNUSED_FUNCTION(kfe_vert_in_edge)(KnifeEdge *e, KnifeVert *v)
{
	return e->v1 == v || e->v2 == v;
}

static int UNUSED_FUNCTION(point_on_line)(float p[3], float v1[3], float v2[3])
{
	float d = dist_to_line_segment_v3(p, v1, v2);
	if (d < 0.01) {
		d = len_v3v3(v1, v2);
		if (d == 0.0)
			return 0;
		
		d = len_v3v3(p, v1) / d;
		
		if (d >= -FLT_EPSILON * 10 || d <= 1.0 + FLT_EPSILON * 10)
			return 1;
	}
	
	return 0;
}

static float len_v3_tri_side_max(const float v1[3], const float v2[3], const float v3[3])
{
	const float s1 = len_v3v3(v1, v2);
	const float s2 = len_v3v3(v2, v3);
	const float s3 = len_v3v3(v3, v1);

	return MAX3(s1, s2, s3);
}

static BMEdgeHit *knife_edge_tri_isect(knifetool_opdata *kcd, BMBVHTree *bmtree,
                                       const float v1[3],  const float v2[3], const float v3[3],
                                       SmallHash *ehash, bglMats *mats, int *count)
{
	BVHTree *tree2 = BLI_bvhtree_new(3, FLT_EPSILON * 4, 8, 8), *tree = BMBVH_BVHTree(bmtree);
	BMEdgeHit *edges = NULL;
	BLI_array_declare(edges);
	BVHTreeOverlap *results, *result;
	BMLoop **ls;
	float cos[9], uv[3], lambda;
	unsigned int tot = 0;
	int i, j;
	
	/* for comparing distances, error of intersection depends on triangle scale.
	 * need to scale down before squaring for accurate comparison */
	const float depsilon = 50 * FLT_EPSILON * len_v3_tri_side_max(v1, v2, v3);
	const float depsilon_squared = depsilon * depsilon;

	copy_v3_v3(cos + 0, v1);
	copy_v3_v3(cos + 3, v2);
	copy_v3_v3(cos + 6, v3);

	BLI_bvhtree_insert(tree2, 0, cos, 3);
	BLI_bvhtree_balance(tree2);
	
	result = results = BLI_bvhtree_overlap(tree, tree2, &tot);
	
	for (i = 0; i < tot; i++, result++) {
		float p[3];
		
		ls = (BMLoop**)kcd->em->looptris[result->indexA];
		
		for (j = 0; j < 3; j++) {
			BMLoop *l1 = ls[j];
			BMFace *hitf;
			ListBase *lst = knife_get_face_kedges(kcd, l1->f);
			Ref *ref;
			
			for (ref = lst->first; ref; ref = ref->next) {
				KnifeEdge *kfe = ref->ref;
				
				//if (kfe == kcd->curedge || kfe == kcd->prevedge)
				//	continue;
				
				if (isect_line_tri_v3(kfe->v1->cageco, kfe->v2->cageco, v1, v2, v3, &lambda, uv)) {
					float no[3], view[3], sp[3];
					
					interp_v3_v3v3(p, kfe->v1->cageco, kfe->v2->cageco, lambda);
					
					if (kcd->curvert && len_squared_v3v3(kcd->curvert->cageco, p) < depsilon_squared)
						continue;
					if (kcd->prevvert && len_squared_v3v3(kcd->prevvert->cageco, p) < depsilon_squared)
						continue;
					if ( len_squared_v3v3(kcd->prevcage, p) < depsilon_squared ||
					     len_squared_v3v3(kcd->vertcage, p) < depsilon_squared)
					{
						continue;
					}

					knife_project_v3(kcd, p, sp);
					view3d_unproject(mats, view, sp[0], sp[1], 0.0f);
					mul_m4_v3(kcd->ob->imat, view);

					if (kcd->cut_through) {
						hitf = FALSE;
					}
					else {
						/* check if this point is visible in the viewport */
						sub_v3_v3(view, p);
						normalize_v3(view);

						copy_v3_v3(no, view);
						mul_v3_fl(no, 0.003);

						/* go towards view a bit */
						add_v3_v3(p, no);

						/* ray cast */
						hitf = BMBVH_RayCast(bmtree, p, no, NULL, NULL);
					}
					
					/* ok, if visible add the new point */
					if (!hitf && !BLI_smallhash_haskey(ehash, (intptr_t)kfe)) {
						BMEdgeHit hit;
						
						if ( len_squared_v3v3(p, kcd->vertco) < depsilon_squared ||
						     len_squared_v3v3(p, kcd->prevco) < depsilon_squared)
						{
							continue;
						}
						
						hit.kfe = kfe;
						hit.v = NULL;
						
						knife_find_basef(kcd, kfe);
						hit.f = kfe->basef;
						hit.perc = len_v3v3(p, kfe->v1->cageco) / len_v3v3(kfe->v1->cageco, kfe->v2->cageco);
						copy_v3_v3(hit.cagehit, p);
						
						interp_v3_v3v3(p, kfe->v1->co, kfe->v2->co, hit.perc);
						copy_v3_v3(hit.realhit, p);
						
						/* BMESH_TODO: should also snap to vertices */
						if (kcd->snap_midpoints) {
							float perc = hit.perc;

							/* select the closest from the edge endpoints or the midpoint */
							if (perc < 0.25f) {
								perc = 0.0f;
							}
							else if (perc < 0.75f) {
								perc = 0.5f;
							}
							else {
								perc = 1.0f;
							}
							
							interp_v3_v3v3(hit.hit, kfe->v1->co, kfe->v2->co, perc);
							interp_v3_v3v3(hit.cagehit, kfe->v1->cageco, kfe->v2->cageco, perc);
						}
						else {
							copy_v3_v3(hit.hit, p);
						}
						knife_project_v3(kcd, hit.cagehit, hit.schit);
						
						BLI_array_append(edges, hit);
						BLI_smallhash_insert(ehash, (intptr_t)kfe, NULL);
					}
				}
			}
		}
	}
	
	if (results)
		MEM_freeN(results);
	
	BLI_bvhtree_free(tree2);
	*count = BLI_array_count(edges);
	
	return edges;
}

static void knife_bgl_get_mats(knifetool_opdata *UNUSED(kcd), bglMats *mats)
{
	bgl_get_mats(mats);
	//copy_m4_m4(mats->modelview, kcd->vc.rv3d->viewmat);
	//copy_m4_m4(mats->projection, kcd->vc.rv3d->winmat);
}

/* Finds visible (or all, if cutting through) edges that intersects the current screen drag line */
static void knife_find_line_hits(knifetool_opdata *kcd)
{
	bglMats mats;
	BMEdgeHit *e1, *e2;
	SmallHash hash, *ehash = &hash;
	float v1[3], v2[3], v3[3], v4[4], s1[3], s2[3];
	int i, c1, c2;
		
	knife_bgl_get_mats(kcd, &mats);
	
	if (kcd->linehits) {
		MEM_freeN(kcd->linehits);
		kcd->linehits = NULL;
		kcd->totlinehit = 0;
	}
	
	copy_v3_v3(v1, kcd->prevcage);
	copy_v3_v3(v2, kcd->vertcage);
	
	/* project screen line's 3d coordinates back into 2d */
	knife_project_v3(kcd, v1, s1);
	knife_project_v3(kcd, v2, s2);
	
	if (len_v2v2(s1, s2) < 1)
		return;

	/* unproject screen line */
	ED_view3d_win_to_segment_clip(kcd->ar, kcd->vc.v3d, s1, v1, v3);
	ED_view3d_win_to_segment_clip(kcd->ar, kcd->vc.v3d, s2, v2, v4);
		
	mul_m4_v3(kcd->ob->imat, v1);
	mul_m4_v3(kcd->ob->imat, v2);
	mul_m4_v3(kcd->ob->imat, v3);
	mul_m4_v3(kcd->ob->imat, v4);
	
	BLI_smallhash_init(ehash);
	
	/* test two triangles of sceen line's plane */
	e1 = knife_edge_tri_isect(kcd, kcd->bmbvh, v1, v2, v3, ehash, &mats, &c1);
	e2 = knife_edge_tri_isect(kcd, kcd->bmbvh, v2, v3, v4, ehash, &mats, &c2);
	if (c1 && c2) {
		e1 = MEM_reallocN(e1, sizeof(BMEdgeHit) * (c1 + c2));
		memcpy(e1 + c1, e2, sizeof(BMEdgeHit) * c2);
		MEM_freeN(e2);
	}
	else if (c2) {
		e1 = e2;
	}
	
	kcd->linehits = e1;
	kcd->totlinehit = c1 + c2;

	/* find position along screen line, used for sorting */
	for (i = 0; i < kcd->totlinehit; i++) {
		BMEdgeHit *lh = e1 + i;
		
		lh->l = len_v2v2(lh->schit, s1) / len_v2v2(s2, s1);
	}
	
	BLI_smallhash_release(ehash);
}

static void knife_input_ray_cast(knifetool_opdata *kcd, const int mval_i[2],
                                 float r_origin[3], float r_ray[3])
{
	bglMats mats;
	float mval[2], imat[3][3];

	knife_bgl_get_mats(kcd, &mats);

	mval[0] = (float)mval_i[0];
	mval[1] = (float)mval_i[1];

	/* unproject to find view ray */
	view3d_unproject(&mats, r_origin, mval[0], mval[1], 0.0f);

	if (kcd->is_ortho) {
		negate_v3_v3(r_ray, kcd->vc.rv3d->viewinv[2]);
	}
	else {
		sub_v3_v3v3(r_ray, r_origin, kcd->vc.rv3d->viewinv[3]);
	}
	normalize_v3(r_ray);

	/* transform into object space */
	invert_m4_m4(kcd->ob->imat, kcd->ob->obmat);
	copy_m3_m4(imat, kcd->ob->obmat);
	invert_m3(imat);

	mul_m4_v3(kcd->ob->imat, r_origin);
	mul_m3_v3(imat, r_ray);
}


static BMFace *knife_find_closest_face(knifetool_opdata *kcd, float co[3], float cageco[3], int *is_space)
{
	BMFace *f;
	int dist = KMAXDIST;
	float origin[3];
	float ray[3];
	
	/* unproject to find view ray */
	knife_input_ray_cast(kcd, kcd->vc.mval, origin, ray);
	add_v3_v3v3(co, origin, ray);

	f = BMBVH_RayCast(kcd->bmbvh, origin, ray, co, cageco);
	
	if (is_space)
		*is_space = !f;
	
	if (!f) {
		/* try to use backbuffer selection method if ray casting failed */
		f = EDBM_findnearestface(&kcd->vc, &dist);
		
		/* cheat for now; just put in the origin instead
		 * of a true coordinate on the face.
		 * This just puts a point 1.0f infront of the view. */
		add_v3_v3v3(co, origin, ray);
	}
	
	return f;
}

/* find the 2d screen space density of vertices within a radius.  used to scale snapping
 * distance for picking edges/verts.*/
static int knife_sample_screen_density(knifetool_opdata *kcd, float radius)
{
	BMFace *f;
	int is_space;
	float co[3], cageco[3], sco[3];
	
	f = knife_find_closest_face(kcd, co, cageco, &is_space);
	
	if (f && !is_space) {
		ListBase *lst;
		Ref *ref;
		float dis;
		int c = 0;
		
		knife_project_v3(kcd, cageco, sco);
		
		lst = knife_get_face_kedges(kcd, f);
		for (ref = lst->first; ref; ref = ref->next) {
			KnifeEdge *kfe = ref->ref;
			int i;
			
			for (i = 0; i < 2; i++) {
				KnifeVert *kfv = i ? kfe->v2 : kfe->v1;
				
				knife_project_v3(kcd, kfv->cageco, kfv->sco);
				
				dis = len_v2v2(kfv->sco, sco);
				if (dis < radius) {
					if (kcd->vc.rv3d->rflag & RV3D_CLIPPING) {
						float vec[3];
						
						copy_v3_v3(vec, kfv->cageco);
						mul_m4_v3(kcd->vc.obedit->obmat, vec);
			
						if (ED_view3d_test_clipping(kcd->vc.rv3d, vec, 1) == 0) {
							c++;
						}
					}
					else {
						c++;
					}
				}
			}
		}
		
		return c;
	}
		
	return 0;
}

/* returns snapping distance for edges/verts, scaled by the density of the
 * surrounding mesh (in screen space)*/
static float knife_snap_size(knifetool_opdata *kcd, float maxsize)
{
	float density = (float)knife_sample_screen_density(kcd, maxsize * 2.0f);
	
	density = MAX2(density, 1);
	
	return MIN2(maxsize / (density * 0.5f), maxsize);
}

/* p is closest point on edge to the mouse cursor */
static KnifeEdge *knife_find_closest_edge(knifetool_opdata *kcd, float p[3], float cagep[3], BMFace **fptr, int *is_space)
{
	BMFace *f;
	float co[3], cageco[3], sco[3], maxdist = knife_snap_size(kcd, kcd->ethresh);
	
	if (kcd->ignore_vert_snapping)
		maxdist *= 0.5;

	f = knife_find_closest_face(kcd, co, cageco, NULL);
	*is_space = !f;
	
	/* set p to co, in case we don't find anything, means a face cut */
	copy_v3_v3(p, co);
	copy_v3_v3(cagep, cageco);
	
	kcd->curbmface = f;
	
	if (f) {
		KnifeEdge *cure = NULL;
		ListBase *lst;
		Ref *ref;
		float dis, curdis = FLT_MAX;
		
		knife_project_v3(kcd, cageco, sco);
		
		/* look through all edges associated with this face */
		lst = knife_get_face_kedges(kcd, f);
		for (ref = lst->first; ref; ref = ref->next) {
			KnifeEdge *kfe = ref->ref;
			
			/* project edge vertices into screen space */
			knife_project_v3(kcd, kfe->v1->cageco, kfe->v1->sco);
			knife_project_v3(kcd, kfe->v2->cageco, kfe->v2->sco);

			dis = dist_to_line_segment_v2(sco, kfe->v1->sco, kfe->v2->sco);
			if (dis < curdis && dis < maxdist) {
				if (kcd->vc.rv3d->rflag & RV3D_CLIPPING) {
					float labda = labda_PdistVL2Dfl(sco, kfe->v1->sco, kfe->v2->sco);
					float vec[3];
		
					vec[0] = kfe->v1->cageco[0] + labda*(kfe->v2->cageco[0] - kfe->v1->cageco[0]);
					vec[1] = kfe->v1->cageco[1] + labda*(kfe->v2->cageco[1] - kfe->v1->cageco[1]);
					vec[2] = kfe->v1->cageco[2] + labda*(kfe->v2->cageco[2] - kfe->v1->cageco[2]);
					mul_m4_v3(kcd->vc.obedit->obmat, vec);
		
					if (ED_view3d_test_clipping(kcd->vc.rv3d, vec, 1) == 0) {
						cure = kfe;
						curdis = dis;
					}
				}
				else {
					cure = kfe;
					curdis = dis;
				}
			}
		}
		
		if (fptr)
			*fptr = f;
		
		if (cure && p) {
			if (!kcd->ignore_edge_snapping || !(cure->e)) {
				if (kcd->snap_midpoints) {
					mid_v3_v3v3(p, cure->v1->co, cure->v2->co);
					mid_v3_v3v3(cagep, cure->v1->cageco, cure->v2->cageco);
				}
				else {
					float d;
					
					closest_to_line_segment_v3(cagep, cageco, cure->v1->cageco, cure->v2->cageco);
					d = len_v3v3(cagep, cure->v1->cageco) / len_v3v3(cure->v1->cageco, cure->v2->cageco);
					interp_v3_v3v3(p, cure->v1->co, cure->v2->co, d);
				}
			}
			else {
				return NULL;
			}
		}
		
		return cure;
	}
		
	if (fptr)
		*fptr = NULL;
	
	return NULL;
}

/* find a vertex near the mouse cursor, if it exists */
static KnifeVert *knife_find_closest_vert(knifetool_opdata *kcd, float p[3], float cagep[3], BMFace **fptr, int *is_space)
{
	BMFace *f;
	float co[3], cageco[3], sco[3], maxdist = knife_snap_size(kcd, kcd->vthresh);
	
	if (kcd->ignore_vert_snapping)
		maxdist *= 0.5;
	
	f = knife_find_closest_face(kcd, co, cageco, is_space);
	
	/* set p to co, in case we don't find anything, means a face cut */
	copy_v3_v3(p, co);
	copy_v3_v3(cagep, p);
	kcd->curbmface = f;
	
	if (f) {
		ListBase *lst;
		Ref *ref;
		KnifeVert *curv = NULL;
		float dis, curdis = FLT_MAX;
		
		knife_project_v3(kcd, cageco, sco);
		
		lst = knife_get_face_kedges(kcd, f);
		for (ref = lst->first; ref; ref = ref->next) {
			KnifeEdge *kfe = ref->ref;
			int i;
			
			for (i = 0; i < 2; i++) {
				KnifeVert *kfv = i ? kfe->v2 : kfe->v1;
				
				knife_project_v3(kcd, kfv->cageco, kfv->sco);
				
				dis = len_v2v2(kfv->sco, sco);
				if (dis < curdis && dis < maxdist) {
					if (kcd->vc.rv3d->rflag & RV3D_CLIPPING) {
						float vec[3];
						
						copy_v3_v3(vec, kfv->cageco);
						mul_m4_v3(kcd->vc.obedit->obmat, vec);
			
						if (ED_view3d_test_clipping(kcd->vc.rv3d, vec, 1) == 0) {
							curv = kfv;
							curdis = dis;
						}
					}
					else {
						curv = kfv;
						curdis = dis;
					}
				}
			}
		}
		
		if (!kcd->ignore_vert_snapping || !(curv && curv->v)) {
			if (fptr)
				*fptr = f;
		
			if (curv && p) {
				copy_v3_v3(p, curv->co);
				copy_v3_v3(cagep, curv->cageco);
			}
			
			return curv;
		}
		else {
			if (fptr)
				*fptr = f;
			
			return NULL;
		}
	}
		
	if (fptr)
		*fptr = NULL;
	
	return NULL;
}

static void knife_snap_angle(knifetool_opdata *kcd)
{
	int dx, dy;
	float w, abs_tan;

	dx = kcd->vc.mval[0] - kcd->prevmval[0];
	dy = kcd->vc.mval[1] - kcd->prevmval[1];
	if (dx == 0 || dy == 0)
		return;

	w = (float)dy / (float)dx;
	abs_tan = fabsf(w);
	if (abs_tan <= 0.4142f) { /* tan(22.5 degrees) = 0.4142 */	
		kcd->angle_snapping = ANGLE_0;
		kcd->vc.mval[1] = kcd->prevmval[1];
	}
	else if (abs_tan < 2.4142f) { /* tan(67.5 degrees) = 2.4142 */
		if (w > 0) {
			kcd->angle_snapping = ANGLE_45;
			kcd->vc.mval[1] = kcd->prevmval[1] + dx;
		}
		else {
			kcd->angle_snapping = ANGLE_135;
			kcd->vc.mval[1] = kcd->prevmval[1] - dx;
		}
	}
	else {
		kcd->angle_snapping = ANGLE_90;
		kcd->vc.mval[0] = kcd->prevmval[0];
	}
}

/* update active knife edge/vert pointers */
static int knife_update_active(knifetool_opdata *kcd)
{
	if (kcd->angle_snapping != ANGLE_FREE && kcd->mode == MODE_DRAGGING)
		knife_snap_angle(kcd);

	kcd->curvert = NULL; kcd->curedge = NULL; kcd->curbmface = NULL;
	
	kcd->curvert = knife_find_closest_vert(kcd, kcd->vertco, kcd->vertcage, &kcd->curbmface, &kcd->is_space);
	if (!kcd->curvert) {
		kcd->curedge = knife_find_closest_edge(kcd, kcd->vertco, kcd->vertcage, &kcd->curbmface, &kcd->is_space);
	}
	
	/* if no hits are found this would normally default to (0,0,0) so instead
	 * get a point at the mouse ray closest to the previous point.
	 * Note that drawing lines in `free-space` isn't properly supported
	 * but theres no guarantee (0,0,0) has any geometry either - campell */
	if (kcd->curvert == NULL && kcd->curedge == NULL) {
			float origin[3], ray[3], co[3];

			knife_input_ray_cast(kcd, kcd->vc.mval, origin, ray);
			add_v3_v3v3(co, origin, ray);

			closest_to_line_v3(kcd->vertcage, kcd->prevcage, co, origin);
	}

	if (kcd->mode == MODE_DRAGGING) {
		knife_find_line_hits(kcd);
	}
	return 1;
}

#define MARK			4
#define DEL				8	
#define VERT_ON_EDGE	16
#define VERT_ORIG		32
#define FACE_FLIP		64
#define BOUNDARY		128
#define FACE_NEW		256

typedef struct facenet_entry {
	struct facenet_entry *next, *prev;
	KnifeEdge *kfe;
} facenet_entry;

static void rnd_offset_co(float co[3], float scale)
{
	int i;
	
	for (i = 0; i < 3; i++) {
		co[i] += (BLI_drand()-0.5)*scale;
	}
}

static void remerge_faces(knifetool_opdata *kcd)
{
	BMesh *bm = kcd->em->bm;
	SmallHash svisit, *visit = &svisit;
	BMIter iter;
	BMFace *f;
	BMFace **stack = NULL;
	BLI_array_declare(stack);
	BMFace **faces = NULL;
	BLI_array_declare(faces);
	BMOperator bmop;
	int idx;
	
	BMO_op_initf(bm, &bmop, "beautify_fill faces=%ff constrain_edges=%fe", FACE_NEW, BOUNDARY);
	
	BMO_op_exec(bm, &bmop);
	BMO_slot_buffer_flag_enable(bm, &bmop, "geomout", FACE_NEW, BM_FACE);
	
	BMO_op_finish(bm, &bmop);
	
	BLI_smallhash_init(visit);
	BM_ITER(f, &iter, bm, BM_FACES_OF_MESH, NULL) {
		BMIter eiter;
		BMEdge *e;
		BMFace *f2;
		
		if (!BMO_elem_flag_test(bm, f, FACE_NEW))
			continue;
		
		if (BLI_smallhash_haskey(visit, (intptr_t)f))
			continue;
		
		BLI_array_empty(stack);
		BLI_array_empty(faces);
		BLI_array_append(stack, f);
		BLI_smallhash_insert(visit, (intptr_t)f, NULL);
		
		do {
			f2 = BLI_array_pop(stack);
			
			BLI_array_append(faces, f2);
			
			BM_ITER(e, &eiter, bm, BM_EDGES_OF_FACE, f2) {
				BMIter fiter;
				BMFace *f3;
				
				if (BMO_elem_flag_test(bm, e, BOUNDARY))
					continue;
				
				BM_ITER(f3, &fiter, bm, BM_FACES_OF_EDGE, e) {
					if (!BMO_elem_flag_test(bm, f3, FACE_NEW))
						continue;
					if (BLI_smallhash_haskey(visit, (intptr_t)f3))
						continue;
					
					BLI_smallhash_insert(visit, (intptr_t)f3, NULL);
					BLI_array_append(stack, f3);
				}
			}	
		} while (BLI_array_count(stack) > 0);
		
		if (BLI_array_count(faces) > 0) {
			idx = BM_elem_index_get(faces[0]);
			
			f2 = BM_faces_join(bm, faces, BLI_array_count(faces));
			if (f2)  {
				BMO_elem_flag_enable(bm, f2, FACE_NEW);
				BM_elem_index_set(f2, idx); /* set_dirty! */ /* BMESH_TODO, check if this is valid or not */
			}
		}
	}
	/* BMESH_TODO, check if the code above validates the indicies */
	/* bm->elem_index_dirty &= ~BM_FACE; */
	bm->elem_index_dirty |= BM_FACE;

	BLI_smallhash_release(visit);

	BLI_array_free(stack);
	BLI_array_free(faces);
}

/* use edgenet to fill faces.  this is a bit annoying and convoluted.*/
static void knifenet_fill_faces(knifetool_opdata *kcd)
{
	BMesh *bm = kcd->em->bm;
	BMIter bmiter;
	BLI_mempool_iter iter;
	BMFace *f;
	BMEdge *e;
	KnifeVert *kfv;
	KnifeEdge *kfe;
	facenet_entry *entry;
	ListBase *face_nets = MEM_callocN(sizeof(ListBase)*bm->totface, "face_nets");
	BMFace **faces = MEM_callocN(sizeof(BMFace *) * bm->totface, "faces knife");
	MemArena *arena = BLI_memarena_new(1 << 16, "knifenet_fill_faces");
	SmallHash shash;
	int i, j, k = 0, totface = bm->totface;
	
	BMO_push(bm, NULL);
	bmesh_begin_edit(bm, BMO_OP_FLAG_UNTAN_MULTIRES);

	/* BMESH_TODO this should be valid now, leaving here until we can ensure this - campbell */
	i = 0;
	BM_ITER(f, &bmiter, bm, BM_FACES_OF_MESH, NULL) {
		BM_elem_index_set(f, i); /* set_inline */
		faces[i] = f;
		i++;
	}
	bm->elem_index_dirty &= ~BM_FACE;
	
	BM_ITER(e, &bmiter, bm, BM_EDGES_OF_MESH, NULL) {
		BMO_elem_flag_enable(bm, e, BOUNDARY);
	}

	/* turn knife verts into real verts, as necassary */
	BLI_mempool_iternew(kcd->kverts, &iter);
	for (kfv = BLI_mempool_iterstep(&iter); kfv; kfv = BLI_mempool_iterstep(&iter)) {
		if (!kfv->v) {
			/* shouldn't we be at least copying the normal? - if not some comment here should explain why - campbell */
			kfv->v = BM_vert_create(bm, kfv->co, NULL);
			kfv->flag = 1;
			BMO_elem_flag_enable(bm, kfv->v, DEL);
		}
		else {
			kfv->flag = 0;
			BMO_elem_flag_enable(bm, kfv->v, VERT_ORIG);
		}

		BMO_elem_flag_enable(bm, kfv->v, MARK);
	}
	
	/* we want to only do changed faces.  first, go over new edges and add to
     * face net lists.*/
	i = j = k = 0;
	BLI_mempool_iternew(kcd->kedges, &iter);
	for (kfe = BLI_mempool_iterstep(&iter); kfe; kfe = BLI_mempool_iterstep(&iter)) {
		Ref *ref;
		if (!kfe->v1 || !kfe->v2 || kfe->v1->inspace || kfe->v2->inspace)
			continue;

		i++;

		if (kfe->e && kfe->v1->v == kfe->e->v1 && kfe->v2->v == kfe->e->v2) {
			kfe->oe = kfe->e;
			continue;
		}
		
		j++;
		
		if (kfe->e) {
			kfe->oe = kfe->e;

			BMO_elem_flag_enable(bm, kfe->e, DEL);
			BMO_elem_flag_disable(bm, kfe->e, BOUNDARY);
			kfe->e = NULL;
		}
		
		kfe->e = BM_edge_create(bm, kfe->v1->v, kfe->v2->v, NULL, TRUE);
		BMO_elem_flag_enable(bm, kfe->e, BOUNDARY);
		
		for (ref = kfe->faces.first; ref; ref = ref->next) {
			f = ref->ref;
			
			entry = BLI_memarena_alloc(arena, sizeof(*entry));
			entry->kfe = kfe;
			BLI_addtail(face_nets + BM_elem_index_get(f), entry);
		}
	}
	
	/* go over original edges, and add to faces with new geometry */
	BLI_mempool_iternew(kcd->kedges, &iter);
	for (kfe = BLI_mempool_iterstep(&iter); kfe; kfe = BLI_mempool_iterstep(&iter)) {
		Ref *ref;
		
		if (!kfe->v1 || !kfe->v2 || kfe->v1->inspace || kfe->v2->inspace)
			continue;
		if (!(kfe->oe && kfe->v1->v == kfe->oe->v1 && kfe->v2->v == kfe->oe->v2))
			continue;
		
		k++;
		
		BMO_elem_flag_enable(bm, kfe->e, BOUNDARY);
		kfe->oe = kfe->e;
		
		for (ref = kfe->faces.first; ref; ref = ref->next) {
			f = ref->ref;
			
			if (face_nets[BM_elem_index_get(f)].first) {
				entry = BLI_memarena_alloc(arena, sizeof(*entry));
				entry->kfe = kfe;
				BLI_addtail(face_nets + BM_elem_index_get(f), entry);
			}
		}
	}
	
	for (i = 0; i < totface; i++) {
		SmallHash *hash = &shash;
		ScanFillFace *efa;
		ScanFillVert *eve, *lasteve;
		int j;
		float rndscale = FLT_EPSILON * 25;
		
		f = faces[i];
		BLI_smallhash_init(hash);
		
		if (face_nets[i].first)
			BMO_elem_flag_enable(bm, f, DEL);
		
		BLI_begin_edgefill();
		
		for (entry = face_nets[i].first; entry; entry = entry->next) {
			if (!BLI_smallhash_haskey(hash, (intptr_t)entry->kfe->v1)) {
				eve = BLI_addfillvert(entry->kfe->v1->v->co);
				eve->poly_nr = 0;
				rnd_offset_co(eve->co, rndscale);
				eve->tmp.p = entry->kfe->v1->v;
				BLI_smallhash_insert(hash, (intptr_t)entry->kfe->v1, eve);
			}

			if (!BLI_smallhash_haskey(hash, (intptr_t)entry->kfe->v2)) {
				eve = BLI_addfillvert(entry->kfe->v2->v->co);
				eve->poly_nr = 0;
				rnd_offset_co(eve->co, rndscale);
				eve->tmp.p = entry->kfe->v2->v;
				BLI_smallhash_insert(hash, (intptr_t)entry->kfe->v2, eve);
			}						 
		}
		
		for (j = 0, entry = face_nets[i].first; entry; entry = entry->next, j++) {
			lasteve = BLI_smallhash_lookup(hash, (intptr_t)entry->kfe->v1);
			eve = BLI_smallhash_lookup(hash, (intptr_t)entry->kfe->v2);
			
			eve->poly_nr++;
			lasteve->poly_nr++;
		}
		
		for (j = 0, entry = face_nets[i].first; entry; entry = entry->next, j++) {
			lasteve = BLI_smallhash_lookup(hash, (intptr_t)entry->kfe->v1);
			eve = BLI_smallhash_lookup(hash, (intptr_t)entry->kfe->v2);
			
			if (eve->poly_nr > 1 && lasteve->poly_nr > 1) {
				ScanFillEdge *eed;
				eed = BLI_addfilledge(lasteve, eve);
				if (entry->kfe->oe)
					eed->f = FILLBOUNDARY;  /* mark as original boundary edge */
				
				BMO_elem_flag_disable(bm, entry->kfe->e->v1, DEL);
				BMO_elem_flag_disable(bm, entry->kfe->e->v2, DEL);
			}
			else {
				if (lasteve->poly_nr < 2)
					BLI_remlink(&fillvertbase, lasteve);
				if (eve->poly_nr < 2)
					BLI_remlink(&fillvertbase, eve);
			}
		}
		
		BLI_edgefill(0);
		
		for (efa = fillfacebase.first; efa; efa = efa->next) {
			BMVert *v1 = efa->v3->tmp.p, *v2 = efa->v2->tmp.p, *v3 = efa->v1->tmp.p;
			BMFace *f2;
			BMLoop *l_iter;
			BMVert *verts[3] = {v1, v2, v3};
			
			if (v1 == v2 || v2 == v3 || v1 == v3)
				continue;
			if (BM_face_exists(bm, verts, 3, &f2))
				continue;
		
			f2 = BM_face_create_quad_tri(bm,
			                          v1, v2, v3, NULL,
			                          NULL, FALSE);

			BMO_elem_flag_enable(bm, f2, FACE_NEW);
			
			l_iter = BM_FACE_FIRST_LOOP(f2);
			do {
				BMO_elem_flag_disable(bm, l_iter->e, DEL);
			} while ((l_iter = l_iter->next) != BM_FACE_FIRST_LOOP(f2));
	
			BMO_elem_flag_disable(bm, f2, DEL);
			BM_elem_index_set(f2, i); /* set_dirty! */ /* note, not 100% sure this is dirty? need to check */

			BM_face_normal_update(bm, f2);
			if (dot_v3v3(f->no, f2->no) < 0.0f) {
				BM_face_normal_flip(bm, f2);
			}
		}
		
		BLI_end_edgefill();
		BLI_smallhash_release(hash);
	}
	bm->elem_index_dirty |= BM_FACE;
	
	/* interpolate customdata */
	BM_ITER(f, &bmiter, bm, BM_FACES_OF_MESH, NULL) {
		BMLoop *l1;
		BMFace *f2;
		BMIter liter1;

		if (!BMO_elem_flag_test(bm, f, FACE_NEW))
			continue;
		
		f2 = faces[BM_elem_index_get(f)];
		if (BM_elem_index_get(f) < 0 || BM_elem_index_get(f) >= totface) {
			fprintf(stderr, "%s: face index out of range! (bmesh internal error)\n", __func__);
		}

		BM_elem_attrs_copy(bm, bm, f2, f);
		
		BM_ITER(l1, &liter1, bm, BM_LOOPS_OF_FACE, f) {
			BM_loop_interp_from_face(bm, l1, f2, TRUE, TRUE);
		}
	}

	/* merge triangles back into faces */
	remerge_faces(kcd);

	/* delete left over faces */
	BMO_op_callf(bm, "del geom=%ff context=%i", DEL, DEL_ONLYFACES);
	BMO_op_callf(bm, "del geom=%fe context=%i", DEL, DEL_EDGES);
	BMO_op_callf(bm, "del geom=%fv context=%i", DEL, DEL_VERTS);

	if (face_nets) 
		MEM_freeN(face_nets);
	if (faces)
		MEM_freeN(faces);
	BLI_memarena_free(arena);
	
	BMO_error_clear(bm); /* remerge_faces sometimes raises errors, so make sure to clear them */

	bmesh_end_edit(bm, BMO_OP_FLAG_UNTAN_MULTIRES);
	BMO_pop(bm);
}

/* called on tool confirmation */
static void knifetool_finish(bContext *C, wmOperator *op)
{
	knifetool_opdata *kcd = op->customdata;
	
	knifenet_fill_faces(kcd);
	
	DAG_id_tag_update(kcd->ob->data, OB_RECALC_DATA);
	WM_event_add_notifier(C, NC_GEOM|ND_DATA, kcd->ob->data);
}

/* copied from paint_image.c */
static int project_knife_view_clip(View3D *v3d, RegionView3D *rv3d, float *clipsta, float *clipend)
{
	int orth = ED_view3d_clip_range_get(v3d, rv3d, clipsta, clipend);

	if (orth) { /* only needed for ortho */
		float fac = 2.0f / ((*clipend) - (*clipsta));
		*clipsta *= fac;
		*clipend *= fac;
	}

	return orth;
}

static void knife_recalc_projmat(knifetool_opdata *kcd)
{
	ARegion *ar = CTX_wm_region(kcd->C);
	
	if (!ar)
		return;
	
	invert_m4_m4(kcd->ob->imat, kcd->ob->obmat);
	ED_view3d_ob_project_mat_get(ar->regiondata, kcd->ob, kcd->projmat);
	//mult_m4_m4m4(kcd->projmat, kcd->vc.rv3d->winmat, kcd->vc.rv3d->viewmat);
	
	kcd->is_ortho = project_knife_view_clip(kcd->vc.v3d, kcd->vc.rv3d, 
	                                        &kcd->clipsta, &kcd->clipend);
}

/* called when modal loop selection is done... */
static void knifetool_exit (bContext *UNUSED(C), wmOperator *op)
{
	knifetool_opdata *kcd = op->customdata;
	
	if (!kcd)
		return;
	
	/* deactivate the extra drawing stuff in 3D-View */
	ED_region_draw_cb_exit(kcd->ar->type, kcd->draw_handle);
	
	/* free the custom data */
	BLI_mempool_destroy(kcd->refs);
	BLI_mempool_destroy(kcd->kverts);
	BLI_mempool_destroy(kcd->kedges);

	BLI_ghash_free(kcd->origedgemap, NULL, NULL);
	BLI_ghash_free(kcd->origvertmap, NULL, NULL);
	BLI_ghash_free(kcd->kedgefacemap, NULL, NULL);
	
	BMBVH_FreeBVH(kcd->bmbvh);
	BLI_memarena_free(kcd->arena);
	
	/* tag for redraw */
	ED_region_tag_redraw(kcd->ar);

	if (kcd->cagecos)
		MEM_freeN(kcd->cagecos);

	/* destroy kcd itself */
	MEM_freeN(kcd);
	op->customdata = NULL;
}

static void cage_mapped_verts_callback(void *userData, int index, float *co, 
	float *UNUSED(no_f), short *UNUSED(no_s))
{
	void **data = userData;
	BMEditMesh *em = data[0];
	float (*cagecos)[3] = data[1];
	SmallHash *hash = data[2];
	
	if (index >= 0 && index < em->bm->totvert && !BLI_smallhash_haskey(hash, index)) {
		BLI_smallhash_insert(hash, index, NULL);
		copy_v3_v3(cagecos[index], co);
	}
}

/* called when modal loop selection gets set up... */
static int knifetool_init(bContext *C, wmOperator *op, int UNUSED(do_cut))
{
	knifetool_opdata *kcd;
	Scene *scene = CTX_data_scene(C);
	Object *obedit = CTX_data_edit_object(C);
	DerivedMesh *cage, *final;
	SmallHash shash;
	void *data[3];
	
	/* alloc new customdata */
	kcd = op->customdata = MEM_callocN(sizeof(knifetool_opdata), "knifetool Modal Op Data");
	
	/* assign the drawing handle for drawing preview line... */
	kcd->ob = obedit;
	kcd->ar = CTX_wm_region(C);
	kcd->C = C;
	kcd->draw_handle = ED_region_draw_cb_activate(kcd->ar->type, knifetool_draw, kcd, REGION_DRAW_POST_VIEW);
	em_setup_viewcontext(C, &kcd->vc);

	kcd->em = ((Mesh *)kcd->ob->data)->edit_btmesh;

	BM_mesh_elem_index_ensure(kcd->em->bm, BM_VERT);

	cage = editbmesh_get_derived_cage_and_final(scene, obedit, kcd->em, &final, CD_MASK_DERIVEDMESH);
	kcd->cagecos = MEM_callocN(sizeof(float) * 3 * kcd->em->bm->totvert, "knife cagecos");
	data[0] = kcd->em;
	data[1] = kcd->cagecos;
	data[2] = &shash;
	
	BLI_smallhash_init(&shash);
	cage->foreachMappedVert(cage, cage_mapped_verts_callback, data);
	BLI_smallhash_release(&shash);
	
	kcd->bmbvh = BMBVH_NewBVH(kcd->em, BMBVH_USE_CAGE|BMBVH_RETURN_ORIG, scene, obedit);
	kcd->arena = BLI_memarena_new(1 << 15, "knife");
	kcd->vthresh = KMAXDIST - 1;
	kcd->ethresh = KMAXDIST;
	
	kcd->extend = 1;
	
	knife_recalc_projmat(kcd);
	
	ED_region_tag_redraw(kcd->ar);
	
	kcd->refs = BLI_mempool_create(sizeof(Ref), 1, 2048, FALSE, FALSE);
	kcd->kverts = BLI_mempool_create(sizeof(KnifeVert), 1, 512, FALSE, TRUE);
	kcd->kedges = BLI_mempool_create(sizeof(KnifeEdge), 1, 512, FALSE, TRUE);
	
	kcd->origedgemap = BLI_ghash_new(BLI_ghashutil_ptrhash, BLI_ghashutil_ptrcmp, "knife origedgemap");
	kcd->origvertmap = BLI_ghash_new(BLI_ghashutil_ptrhash, BLI_ghashutil_ptrcmp, "knife origvertmap");
	kcd->kedgefacemap = BLI_ghash_new(BLI_ghashutil_ptrhash, BLI_ghashutil_ptrcmp, "knife origvertmap");

	/* cut all the way through the mesh if use_occlude_geometry button not pushed */
	kcd->cut_through = !(kcd->vc.v3d->flag & V3D_ZBUF_SELECT);

	return 1;
}

static int knifetool_cancel (bContext *C, wmOperator *op)
{
	/* this is just a wrapper around exit() */
	knifetool_exit(C, op);
	return OPERATOR_CANCELLED;
}

static int knifetool_invoke (bContext *C, wmOperator *op, wmEvent *evt)
{
	knifetool_opdata *kcd;

	view3d_operator_needs_opengl(C);

	if (!knifetool_init(C, op, 0))
		return OPERATOR_CANCELLED;
	
	/* add a modal handler for this operator - handles loop selection */
	WM_event_add_modal_handler(C, op);

	kcd = op->customdata;
	kcd->vc.mval[0] = evt->mval[0];
	kcd->vc.mval[1] = evt->mval[1];
	
	return OPERATOR_RUNNING_MODAL;
}

enum {
	KNF_MODAL_CANCEL = 1,
	KNF_MODAL_CONFIRM,
	KNF_MODAL_MIDPOINT_ON,
	KNF_MODAL_MIDPOINT_OFF,
	KNF_MODAL_NEW_CUT,
	KNF_MODEL_IGNORE_SNAP_ON,
	KNF_MODEL_IGNORE_SNAP_OFF,
	KNF_MODAL_ADD_CUT,
	KNF_MODAL_ANGLE_SNAP_TOGGLE
};

wmKeyMap* knifetool_modal_keymap(wmKeyConfig *keyconf)
{
	static EnumPropertyItem modal_items[] = {
	{KNF_MODAL_CANCEL, "CANCEL", 0, "Cancel", ""},
	{KNF_MODAL_CONFIRM, "CONFIRM", 0, "Confirm", ""},
	{KNF_MODAL_MIDPOINT_ON, "SNAP_MIDPOINTS_ON", 0, "Snap To Midpoints On", ""},
	{KNF_MODAL_MIDPOINT_OFF, "SNAP_MIDPOINTS_OFF", 0, "Snap To Midpoints Off", ""},
	{KNF_MODEL_IGNORE_SNAP_ON, "IGNORE_SNAP_ON", 0, "Ignore Snapping On", ""},
	{KNF_MODEL_IGNORE_SNAP_OFF, "IGNORE_SNAP_OFF", 0, "Ignore Snapping Off", ""},
	{KNF_MODAL_ANGLE_SNAP_TOGGLE, "ANGLE_SNAP_TOGGLE", 0, "Toggle Angle Snapping", ""},
	{KNF_MODAL_NEW_CUT, "NEW_CUT", 0, "End Current Cut", ""},
	{KNF_MODAL_ADD_CUT, "ADD_CUT", 0, "Add Cut", ""},

	{0, NULL, 0, NULL, NULL}};
	
	wmKeyMap *keymap = WM_modalkeymap_get(keyconf, "Knife Tool Modal Map");
	
	/* this function is called for each spacetype, only needs to add map once */
	if (keymap) return NULL;
	
	keymap = WM_modalkeymap_add(keyconf, "Knife Tool Modal Map", modal_items);
	
	/* items for modal map */
	WM_modalkeymap_add_item(keymap, ESCKEY,    KM_PRESS, KM_ANY, 0, KNF_MODAL_CONFIRM);
	WM_modalkeymap_add_item(keymap, LEFTMOUSE, KM_PRESS, KM_ANY, 0, KNF_MODAL_ADD_CUT);
	WM_modalkeymap_add_item(keymap, RIGHTMOUSE, KM_PRESS, KM_ANY, 0, KNF_MODAL_CONFIRM);
	WM_modalkeymap_add_item(keymap, RETKEY, KM_PRESS, KM_ANY, 0, KNF_MODAL_CONFIRM);
	WM_modalkeymap_add_item(keymap, PADENTER, KM_PRESS, KM_ANY, 0, KNF_MODAL_CONFIRM);
	WM_modalkeymap_add_item(keymap, EKEY, KM_PRESS, 0, 0, KNF_MODAL_NEW_CUT);

	WM_modalkeymap_add_item(keymap, LEFTCTRLKEY, KM_PRESS, KM_ANY, 0, KNF_MODAL_MIDPOINT_ON);
	WM_modalkeymap_add_item(keymap, LEFTCTRLKEY, KM_RELEASE, KM_ANY, 0, KNF_MODAL_MIDPOINT_OFF);
	WM_modalkeymap_add_item(keymap, RIGHTCTRLKEY, KM_PRESS, KM_ANY, 0, KNF_MODAL_MIDPOINT_ON);
	WM_modalkeymap_add_item(keymap, RIGHTCTRLKEY, KM_RELEASE, KM_ANY, 0, KNF_MODAL_MIDPOINT_OFF);

	WM_modalkeymap_add_item(keymap, LEFTSHIFTKEY, KM_PRESS, KM_ANY, 0, KNF_MODEL_IGNORE_SNAP_ON);
	WM_modalkeymap_add_item(keymap, LEFTSHIFTKEY, KM_RELEASE, KM_ANY, 0, KNF_MODEL_IGNORE_SNAP_OFF);
	WM_modalkeymap_add_item(keymap, RIGHTSHIFTKEY, KM_PRESS, KM_ANY, 0, KNF_MODEL_IGNORE_SNAP_ON);
	WM_modalkeymap_add_item(keymap, RIGHTSHIFTKEY, KM_RELEASE, KM_ANY, 0, KNF_MODEL_IGNORE_SNAP_OFF);
	
	WM_modalkeymap_add_item(keymap, CKEY, KM_PRESS, 0, 0, KNF_MODAL_ANGLE_SNAP_TOGGLE);

	WM_modalkeymap_assign(keymap, "MESH_OT_knifetool");
	
	return keymap;
}

static int knifetool_modal (bContext *C, wmOperator *op, wmEvent *event)
{
	Object *obedit;
	knifetool_opdata *kcd = op->customdata;
	
	if (!C) {
		return OPERATOR_FINISHED;
	}
	
	obedit = CTX_data_edit_object(C);
	if (!obedit || obedit->type != OB_MESH || ((Mesh *)obedit->data)->edit_btmesh != kcd->em) {
		knifetool_exit(C, op);
		return OPERATOR_FINISHED;
	}

	view3d_operator_needs_opengl(C);
	
	if (kcd->mode == MODE_PANNING)
		kcd->mode = kcd->prevmode;
	
	/* handle modal keymap */
	if (event->type == EVT_MODAL_MAP) {
		switch (event->val) {
			case KNF_MODAL_CANCEL:
				/* finish */
				ED_region_tag_redraw(kcd->ar);
				
				knifetool_exit(C, op);
				
				return OPERATOR_CANCELLED;
			case KNF_MODAL_CONFIRM:
				/* finish */
				ED_region_tag_redraw(kcd->ar);
				
				knifetool_finish(C, op);
				knifetool_exit(C, op);
				
				return OPERATOR_FINISHED;
			case KNF_MODAL_MIDPOINT_ON:
				kcd->snap_midpoints = 1;

				knife_recalc_projmat(kcd);
				knife_update_active(kcd);
				ED_region_tag_redraw(kcd->ar);
				break;
			case KNF_MODAL_MIDPOINT_OFF:
				kcd->snap_midpoints = 0;

				knife_recalc_projmat(kcd);
				knife_update_active(kcd);
				ED_region_tag_redraw(kcd->ar);
				break;
			case KNF_MODEL_IGNORE_SNAP_ON:
				ED_region_tag_redraw(kcd->ar);
				kcd->ignore_vert_snapping = kcd->ignore_edge_snapping = 1;
				break;
			case KNF_MODEL_IGNORE_SNAP_OFF:
				ED_region_tag_redraw(kcd->ar);
				kcd->ignore_vert_snapping = kcd->ignore_edge_snapping = 0;
				break;
			case KNF_MODAL_ANGLE_SNAP_TOGGLE:
				kcd->angle_snapping = !kcd->angle_snapping;
				break;
			case KNF_MODAL_NEW_CUT:
				ED_region_tag_redraw(kcd->ar);
				knife_finish_cut(kcd);
				kcd->mode = MODE_IDLE;
				break;
			case KNF_MODAL_ADD_CUT:
				knife_recalc_projmat(kcd);

				if (kcd->mode == MODE_DRAGGING) {
					knife_add_cut(kcd);
					if (!kcd->extend) {
						knife_finish_cut(kcd);
						kcd->mode = MODE_IDLE;
					}
				}
				else if (kcd->mode != MODE_PANNING) {
					knife_start_cut(kcd);
					kcd->mode = MODE_DRAGGING;
				}
		
				ED_region_tag_redraw(kcd->ar);
				break;
			}
	}
	else { /* non-modal-mapped events */
		switch (event->type) {
			case WHEELUPMOUSE:
			case WHEELDOWNMOUSE:
				return OPERATOR_PASS_THROUGH;
			case MIDDLEMOUSE:
				if (event->val != KM_RELEASE) {
					if (kcd->mode != MODE_PANNING)
						kcd->prevmode = kcd->mode;
					kcd->mode = MODE_PANNING;
				}
				else {
					kcd->mode = kcd->prevmode;
				}
				
				ED_region_tag_redraw(kcd->ar);
				return OPERATOR_PASS_THROUGH;
				
			case MOUSEMOVE:  /* mouse moved somewhere to select another loop */
				if (kcd->mode != MODE_PANNING) {
					knife_recalc_projmat(kcd);
					kcd->vc.mval[0] = event->mval[0];
					kcd->vc.mval[1] = event->mval[1];
					
					if (knife_update_active(kcd))					
						ED_region_tag_redraw(kcd->ar);
				}
	
				break;
		}
	}
	
	/* keep going until the user confirms */
	return OPERATOR_RUNNING_MODAL;
}

void MESH_OT_knifetool (wmOperatorType *ot)
{
	/* description */
	ot->name = "Knife Topology Tool";
	ot->idname = "MESH_OT_knifetool";
	ot->description = "Cut new topology";
	
	/* callbacks */
	ot->invoke = knifetool_invoke;
	ot->modal = knifetool_modal;
	ot->cancel = knifetool_cancel;
	ot->poll = ED_operator_editmesh_view3d;
	
	/* flags */
	ot->flag = OPTYPE_REGISTER|OPTYPE_UNDO|OPTYPE_BLOCKING;
}
