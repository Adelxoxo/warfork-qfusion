/*
Copyright (C) 2013 Victor Luchits

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.

See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.

*/

// r_surf.c: surface-related refresh code

#include "r_local.h"

#define WORLDSURF_DIST 1024.0f                  // hack the draw order for world surfaces

static vec3_t modelOrg;                         // relative to view point

#define DrawSurfLightsKey(ds) ((ds)->numRtLights ? (ds)->numRtLights >> 2 : (ds)->numLightmaps)

//==================================================================================

/*
* R_SurfNoDraw
*/
bool R_SurfNoDraw( const msurface_t *surf ) {
	const shader_t *shader = surf->shader;
	if( surf->flags & SURF_NODRAW ) {
		return true;
	}
	if( !surf->mesh.numVerts ) {
		return true;
	}
	if( !shader ) {
		return true;
	}
	return false;
}

/*
* R_SurfNoShadow
*/
bool R_SurfNoShadow( const msurface_t *surf ) {
	if( surf->flags & SURF_NODRAW ) {
		return true;
	}
	if( ( surf->flags & SURF_SKY ) && mapConfig.writeSkyDepth ) {
		return false;
	}
	if( !Shader_DepthWrite( surf->shader ) ) {
		return true;
	}
	if( ( surf->shader->sort >= SHADER_SORT_OPAQUE ) && ( surf->shader->sort <= SHADER_SORT_ALPHATEST ) ) {
		return false;
	}
	return true;
}

/*
* R_SurfNoDlight
*/
bool R_SurfNoDlight( const msurface_t *surf ) {
	if( surf->flags & ( SURF_NOLIGHTMAP | SURF_NODLIGHT | SURF_SKY | SURF_NODRAW ) ) {
		return true;
	}
	return false;
}

/*
* R_CullSurface
*/
static bool R_CullSurface( const entity_t *e, const msurface_t *surf, unsigned int clipflags ) {
	return ( clipflags && R_CullBox( surf->mins, surf->maxs, clipflags ) );
}

/*
* R_SurfaceClipRtLight
*/
bool R_SurfaceClipRtLight( rtlight_t *lt, unsigned s, bool useCompiledMask ) {
	int sideMask;

	if( useCompiledMask && lt->worldSurfMasks ) {
		sideMask = lt->worldSurfMasks[s];
	} else {
		msurface_t *surf = rsh.worldBrushModel->surfaces + s;

		if( R_SurfNoDlight( surf ) ) {
			return false;
		}

		sideMask = R_CalcRtLightSurfaceSidemask( lt, surf );
	}

	if( !sideMask ) {
		return false;
	}

	lt->receiveMask |= sideMask; 
	return true;
}

/*
* R_AddLightsToSurfaces
*/
static void R_AddLightsToSurfaces( void ) {
	unsigned i, j, k;
	mmodel_t *bmodel = &rsh.worldBrushModel->submodels[0];
	unsigned **lsi, *lc, *lm;
	unsigned **lsip, *lcp;

	if( rn.renderFlags & RF_SHADOWMAPVIEW ) {
		return;
	}

	lsi = alloca( sizeof( *lsi ) * rn.numRealtimeLights );
	lc = alloca( sizeof( *lc ) * rn.numRealtimeLights );
	lm = alloca( sizeof( *lm ) * rn.numRealtimeLights );

	lsip = lsi;
	lcp = lc;

	for( j = 0; j < rn.numRealtimeLights; j++ ) {
		lsi[j] = rn.rtlights[j]->surfaceInfo;
		lm[j] = 0;

		if( !lsi[j] ) {
			lc[j] = 0;
			continue;
		}

		lc[j] = lsi[j][0];
		lsi[j]++;
	}

	for( i = 0; i < bmodel->numModelDrawSurfaces; i++ ) {
		unsigned ds = bmodel->firstModelDrawSurface + i;
		drawSurfaceBSP_t *drawSurf = rsh.worldBrushModel->drawSurfaces + ds;

		if( drawSurf->visFrame != rf.frameCount ) {
			continue;
		}

		drawSurf->numRtLights = 0;
		memset( drawSurf->surfRtlightBits, 0, sizeof( *drawSurf->surfRtlightBits ) * drawSurf->numWorldSurfaces );

		for( j = 0; j < rn.numRealtimeLights; j++ ) {
			rtlight_t *l = rn.rtlights[j];

			if( drawSurf->numRtLights == MAX_DRAWSURF_RTLIGHTS ) {
				break;
			}

			while( lc[j] > 0 && lsi[j][0] < ds ) {
				lc[j]--;
				lsi[j] = lsi[j] + 2 + lsi[j][1] * 3;
				continue;
			}

			if( lc[j] == 0 ) {
				continue;
			}

			if( lsi[j][0] == ds ) {
				bool clipped;
				unsigned *p = lsi[j];
				p++;
				unsigned ns = *p++;			

				clipped = false;
				for( k = 0; k < ns; k++, p += 3 ) {
					if( rn.meshlist->worldSurfVis[p[0]] ) {
						clipped = true;
						break;
					}
				}

				if( clipped ) {
					unsigned bit = 1 << drawSurf->numRtLights;
					drawSurf->rtLights[drawSurf->numRtLights++] = l;

					for( ; k < ns; k++, p += 3 ) {
						unsigned s = p[0], si = p[1], mask = p[2];
						if( !rn.meshlist->worldSurfVis[s] ) {
							continue;
						}

						lm[j] |= mask;
						drawSurf->surfRtlightBits[si] |= bit;
					}
				}
			}
		}
	}

	for( j = 0; j < rn.numRealtimeLights; j++ ) {
		rtlight_t *l = rn.rtlights[j];
		l->receiveMask |= lm[j];
	}
}

/*
* R_FlushBSPSurfBatch
*/
static void R_FlushBSPSurfBatch( void ) {
	drawListBatch_t *batch = &rn.meshlist->bspBatch;
	drawSurfaceBSP_t *drawSurf = batch->lastDrawSurf;
	int lightStyleNum = batch->lightStyleNum;
	const entity_t *e = batch->entity;
	const shader_t *shader = batch->shader;
	entSceneCache_t *cache = R_ENTCACHE( e );

	if( batch->count == 0 ) {
		return;
	}

	batch->count = 0;

	RB_BindShader( e, shader, batch->fog );

	RB_SetPortalSurface( batch->portalSurface );

	RB_SetLightstyle( lightStyleNum >= 0 ? rsh.worldBrushModel->superLightStyles + lightStyleNum : NULL );

	if( e != rsc.worldent ) {	
		RB_SetRtLightParams( cache->numRtLights, cache->rtLights, 0, NULL );
	} else {
		RB_SetRtLightParams( drawSurf->numRtLights, drawSurf->rtLights, drawSurf->numWorldSurfaces, drawSurf->surfRtlightBits );
	}

	RB_BindVBO( batch->vbo, GL_TRIANGLES );

	RB_DrawElements( batch->firstVert, batch->numVerts, batch->firstElem, batch->numElems );
}

/*
* R_BatchBSPSurf
*/
flushBatchDrawSurf_cb R_BatchBSPSurf( const entity_t *e, const shader_t *shader, const mfog_t *fog, 
	int lightStyleNum, const portalSurface_t *portalSurface, drawSurfaceBSP_t *drawSurf, bool mergable ) {
	unsigned i;
	drawListBatch_t *batch = &rn.meshlist->bspBatch;

	if( !mergable ) {
		R_FlushBSPSurfBatch();

		if( batch->entity != e ) {
			R_TransformForEntity( e );
		}
	}

	if( drawSurf->numInstances ) {
		batch->count = 0;

		RB_DrawElementsInstanced( drawSurf->firstVboVert, drawSurf->numVerts, 
			drawSurf->firstVboElem, drawSurf->numElems, drawSurf->numInstances, drawSurf->instances );
	} else {
		for( i = 0; i < drawSurf->numWorldSurfaces; i++ ) {
			unsigned si = drawSurf->worldSurfaces[i];
			msurface_t *surf = rsh.worldBrushModel->surfaces + si;
			unsigned surfFirstVert = drawSurf->firstVboVert + surf->firstDrawSurfVert;
			unsigned surfFirstElem = drawSurf->firstVboElem + surf->firstDrawSurfElem;

			if( batch->vbo != drawSurf->vbo->index || surfFirstElem != batch->firstElem+batch->numElems ) {
				R_FlushBSPSurfBatch();
			}

			if( rn.meshlist->worldSurfVis[si] ) {
				if( batch->count == 0 ) {
					batch->vbo = drawSurf->vbo->index;
					batch->shader = ( shader_t * )shader;
					batch->portalSurface = ( portalSurface_t *)portalSurface;
					batch->entity = ( entity_t * )e;
					batch->fog = ( mfog_t * )fog;
					batch->lightStyleNum = lightStyleNum;
					batch->lastDrawSurf = drawSurf;
					batch->firstVert = surfFirstVert;
					batch->firstElem = surfFirstElem;
					batch->numVerts = 0;
					batch->numElems = 0;
				}

				batch->count++;
				batch->numVerts += surf->mesh.numVerts;
				batch->numElems += surf->mesh.numElems;
			}
		}
		
		if( !( rn.renderFlags & RF_SHADOWMAPVIEW ) ) {
			R_FlushBSPSurfBatch();
		}
	}

	return &R_FlushBSPSurfBatch;
}

/*
* R_WalkBSPSurf
*/
void R_WalkBSPSurf( const entity_t *e, const shader_t *shader, int lightStyleNum,
	drawSurfaceBSP_t *drawSurf, walkDrawSurf_cb_cb cb, void *ptr ) {
	unsigned i;

	for( i = 0; i < drawSurf->numWorldSurfaces; i++ ) {
		int s = drawSurf->worldSurfaces[i];
		msurface_t *surf = rsh.worldBrushModel->surfaces + s;

		assert( rn.meshlist->worldDrawSurfVis[surf->drawSurf - 1] );

		if( rn.meshlist->worldSurfVis[s] ) {
			cb( ptr, e, shader, lightStyleNum, drawSurf, surf );
		}
	}
}

/*
* R_AddSurfaceToDrawList
*/
static bool R_AddSurfaceToDrawList( const entity_t *e, unsigned ds ) {
	drawSurfaceBSP_t *drawSurf = rsh.worldBrushModel->drawSurfaces + ds;
	const mfog_t *fog = drawSurf->fog;
	const shader_t *shader = drawSurf->shader;
	int lightStyleNum = drawSurf->superLightStyle;
	portalSurface_t *portalSurface = NULL;
	bool sky, portal;
	unsigned drawOrder = 0;

	if( drawSurf->visFrame == rf.frameCount ) {
		return true;
	}

	sky = ( shader->flags & SHADER_SKY ) != 0;
	portal = ( shader->flags & SHADER_PORTAL ) != 0;

	if( rn.renderFlags & RF_SHADOWMAPVIEW ) {
		fog = NULL;
		lightStyleNum = -1;
		shader = R_OpaqueShadowShader( shader );
		if( !shader ) {
			return false;
		}
	}

	if( r_lighting_realtime_world_shadows->integer ) {
		lightStyleNum = -1;
	}

	if( sky ) {
		if( R_FASTSKY() ) {
			return false;
		}

		if( rn.refdef.rdflags & RDF_SKYPORTALINVIEW ) {
			// this will later trigger the skyportal view to be rendered in R_DrawPortals
			portalSurface = R_AddSkyportalSurface( e, shader, drawSurf );
		} else if( mapConfig.writeSkyDepth ) {
			// add the sky surface to depth mask
			R_AddSurfToDrawList( rn.portalmasklist, e, rsh.depthOnlyShader, NULL, -1, 0, 0, NULL, drawSurf );
		}

		// we still need to add draw surface to the draw list to perform sky clipping in R_UpdateSurfaceInDrawList
		drawSurf->visFrame = rf.frameCount;
		drawSurf->listSurf = R_AddSurfToDrawList( rn.meshlist, e, mapConfig.writeSkyDepth ? rsh.depthOnlyShader : shader, 
			fog, lightStyleNum, 0, drawOrder, portalSurface, drawSurf );

		// the actual skydome surface
		R_AddSkySurfToDrawList( rn.meshlist, shader, portalSurface, &rn.skyDrawSurface );

		rf.stats.c_world_draw_surfs++;
		return true;
	}

	if( portal ) {
		portalSurface = R_AddPortalSurface( e, shader, drawSurf );
	}

	drawOrder = R_PackOpaqueOrder( fog, shader, DrawSurfLightsKey( drawSurf ), false );

	drawSurf->visFrame = rf.frameCount;
	drawSurf->listSurf = R_AddSurfToDrawList( rn.meshlist, e, shader, fog, lightStyleNum, 
		WORLDSURF_DIST, drawOrder, portalSurface, drawSurf );

	if( !drawSurf->listSurf ) {
		return false;
	}

	if( portalSurface && !( shader->flags & ( SHADER_PORTAL_CAPTURE | SHADER_PORTAL_CAPTURE2 ) ) ) {
		R_AddSurfToDrawList( rn.portalmasklist, e, rsh.depthOnlyShader, NULL, -1, 0, 0, NULL, drawSurf );
	}

	rf.stats.c_world_draw_surfs++;
	return true;
}

/*
* R_ClipSpecialWorldSurf
*/
static bool R_ClipSpecialWorldSurf( drawSurfaceBSP_t *drawSurf, const msurface_t *surf, const vec3_t origin, float *pdist ) {
	bool sky, portal;
	portalSurface_t *portalSurface = NULL;
	const shader_t *shader = drawSurf->shader;

	sky = ( shader->flags & SHADER_SKY ) != 0;
	portal = ( shader->flags & SHADER_PORTAL ) != 0;

	if( sky ) {
		if( R_ClipSkySurface( &rn.skyDrawSurface, surf ) ) {
			return true;
		}
		return false;
	}

	if( portal ) {
		portalSurface = R_GetDrawListSurfPortal( drawSurf->listSurf );
	}

	if( portalSurface != NULL ) {
		vec3_t centre;
		float dist = 0;

		if( origin ) {
			VectorCopy( origin, centre );
		} else {
			VectorAdd( surf->mins, surf->maxs, centre );
			VectorScale( centre, 0.5, centre );
		}
		dist = Distance( rn.refdef.vieworg, centre );

		// draw portals in front-to-back order
		dist = 1024 - dist / 100.0f;
		if( dist < 1 ) {
			dist = 1;
		}

		R_UpdatePortalSurface( portalSurface, &surf->mesh, surf->mins, surf->maxs, shader, drawSurf );

		*pdist = dist;
	}

	return true;
}

/*
* R_UpdateSurfaceInDrawList
*
* Walk the list of visible world surfaces and draw order bits.
* For sky surfaces, skybox clipping is also performed.
*/
static void R_UpdateSurfaceInDrawList( const entity_t *e, unsigned ds, const vec3_t origin ) {
	unsigned i;
	float dist = 0;
	bool special;
	bool rtlight;
	drawSurfaceBSP_t *drawSurf = rsh.worldBrushModel->drawSurfaces + ds;
	unsigned numVisSurfaces;
	unsigned visSurfaces[MAX_DRAWSURF_SURFS];

	if( !drawSurf->listSurf ) {
		return;
	}

	numVisSurfaces = 0;
	for( i = 0; i < drawSurf->numWorldSurfaces; i++ ) {
		int s = drawSurf->worldSurfaces[i];
		if( !rn.meshlist->worldSurfVis[s] ) {
			continue;			
		}
		visSurfaces[numVisSurfaces++] = i;
	}

	special = ( drawSurf->shader->flags & (SHADER_SKY|SHADER_PORTAL) ) != 0;

	for( i = 0; i < numVisSurfaces; i++ ) {
		unsigned si = visSurfaces[i];
		unsigned s = drawSurf->worldSurfaces[si];
		msurface_t *surf = rsh.worldBrushModel->surfaces + s;
		float sdist = 0;

		if( special && !R_ClipSpecialWorldSurf( drawSurf, surf, origin, &sdist ) ) {
			// clipped away
			continue;
		}

		if( sdist > sdist )
			dist = sdist;
	}

	rtlight = drawSurf->numRtLights != 0;

	// update the distance sorting key if it's a portal surface or a normal dlit surface
	if( dist != 0 || rtlight ) {
		int drawOrder = R_PackOpaqueOrder( drawSurf->fog, drawSurf->shader, DrawSurfLightsKey( drawSurf ), rtlight );
		if( dist == 0 )
			dist = WORLDSURF_DIST;
		R_UpdateDrawSurfDistKey( drawSurf->listSurf, 0, drawSurf->shader, dist, drawOrder );
	}
}

/*
=============================================================

BRUSH MODELS

=============================================================
*/

/*
* R_BrushModelBBox
*/
float R_BrushModelBBox( const entity_t *e, vec3_t mins, vec3_t maxs, bool *rotated ) {
	int i;
	const model_t   *model = e->model;

	if( !Matrix3_Compare( e->axis, axis_identity ) ) {
		if( rotated ) {
			*rotated = true;
		}
		for( i = 0; i < 3; i++ ) {
			mins[i] = e->origin[i] - model->radius * e->scale;
			maxs[i] = e->origin[i] + model->radius * e->scale;
		}
	} else {
		if( rotated ) {
			*rotated = false;
		}
		VectorMA( e->origin, e->scale, model->mins, mins );
		VectorMA( e->origin, e->scale, model->maxs, maxs );
	}
	return model->radius * e->scale;
}

#define R_TransformPointToModelSpace( e,rotated,in,out ) \
	VectorSubtract( in, ( e )->origin, out ); \
	if( rotated ) { \
		vec3_t temp; \
		VectorCopy( out, temp ); \
		Matrix3_TransformVector( ( e )->axis, temp, out ); \
	}

/*
* R_CacheBrushModelEntity
*/
void R_CacheBrushModelEntity( const entity_t *e ) {
	const model_t *mod;
	entSceneCache_t *cache = R_ENTCACHE( e );

	mod = e->model;
	if( mod->type != mod_brush ) {
		assert( mod->type == mod_brush );
		return;
	}

	cache->radius = R_BrushModelBBox( e, cache->mins, cache->maxs, &cache->rotated );
	cache->fog = R_FogForBounds( cache->mins, cache->maxs );
	VectorCopy( cache->mins, cache->absmins );
	VectorCopy( cache->maxs, cache->absmaxs );
}

/*
* R_AddBrushModelToDrawList
*/
bool R_AddBrushModelToDrawList( const entity_t *e ) {
	unsigned int i;
	vec3_t origin;
	model_t *model = e->model;
	mbrushmodel_t *bmodel = ( mbrushmodel_t * )model->extradata;
	mfog_t *fog;
	unsigned numVisSurfaces;
	const entSceneCache_t *cache = R_ENTCACHE( e );

	if( cache->mod_type != mod_brush ) {
		return false;
	}

	if( bmodel->numModelDrawSurfaces == 0 ) {
		return false;
	}

	VectorAdd( e->model->mins, e->model->maxs, origin );
	VectorMA( e->origin, 0.5, origin, origin );

	fog = cache->fog;

	R_TransformPointToModelSpace( e, cache->rotated, rn.refdef.vieworg, modelOrg );

	numVisSurfaces = 0;

	for( i = 0; i < bmodel->numModelSurfaces; i++ ) {
		unsigned s = bmodel->firstModelSurface + i;
		msurface_t *surf = rsh.worldBrushModel->surfaces + s;

		if( !surf->drawSurf ) {
			continue;
		}

		rn.meshlist->worldSurfVis[s] = 1;
		rn.meshlist->worldDrawSurfVis[surf->drawSurf - 1] = 1;

		numVisSurfaces++;
	}

	if( !numVisSurfaces ) {
		return false;
	}

	for( i = 0; i < bmodel->numModelDrawSurfaces; i++ ) {
		unsigned ds = bmodel->firstModelDrawSurface + i;

		if( rn.meshlist->worldDrawSurfVis[ds] ) {
			R_AddSurfaceToDrawList( e, ds );

			R_UpdateSurfaceInDrawList( e, ds, origin );
		}
	}

	return true;
}

/*
=============================================================

WORLD MODEL

=============================================================
*/

/*
* R_CullVisLeaves
*/
static void R_CullVisLeaves( unsigned firstLeaf, unsigned numLeaves, unsigned clipFlags ) {
	unsigned i, j;
	mleaf_t *leaf;
	const uint8_t *pvs = rn.pvs;
	const uint8_t *areabits = rn.areabits;

	if( rn.renderFlags & RF_NOVIS ) {
		pvs = NULL;
		areabits = NULL;
	}

	for( i = 0; i < numLeaves; i++ ) {
		int clipped;
		unsigned bit, testFlags;
		cplane_t *clipplane;
		unsigned l = firstLeaf + i;

		leaf = rsh.worldBrushModel->visleafs[l];
		if( leaf->cluster < 0 ) {
			// we shouldn't really be here...
			continue;
		}

		// check for door connected areas
		if( areabits ) {
			if( leaf->area < 0 || !( areabits[leaf->area >> 3] & ( 1 << ( leaf->area & 7 ) ) ) ) {
				continue; // not visible
			}
		}

		if( pvs ) {
			if( !( pvs[leaf->cluster >> 3] & ( 1 << ( leaf->cluster & 7 ) ) ) ) {
				continue; // not visible
			}
		}

		// track leaves, which are entirely inside the frustum
		clipped = 0;
		testFlags = clipFlags;
		for( j = sizeof( rn.frustum ) / sizeof( rn.frustum[0] ), bit = 1, clipplane = rn.frustum; j > 0; j--, bit <<= 1, clipplane++ ) {
			if( testFlags & bit ) {
				clipped = BoxOnPlaneSide( leaf->mins, leaf->maxs, clipplane );
				if( clipped == 2 ) {
					break;
				} else if( clipped == 1 ) {
					testFlags &= ~bit; // node is entirely on screen
				}
			}
		}

		if( clipped == 2 ) {
			continue; // fully clipped
		}

		if( testFlags == 0 ) {
			// fully visible
			for( j = 0; j < leaf->numVisSurfaces; j++ ) {
				assert( leaf->visSurfaces[j] < rn.meshlist->numWorldSurfVis );
				rn.meshlist->worldSurfFullVis[leaf->visSurfaces[j]] = 1;
			}
		} else {
			// partly visible
			for( j = 0; j < leaf->numVisSurfaces; j++ ) {
				assert( leaf->visSurfaces[j] < rn.meshlist->numWorldSurfVis );
				rn.meshlist->worldSurfVis[leaf->visSurfaces[j]] = 1;
			}
		}

		assert( l < rn.meshlist->numWorldLeafVis );
		rn.meshlist->worldLeafVis[l] = 1;
	}
}

/*
* R_CullVisSurfaces
*/
static void R_CullVisSurfaces( unsigned firstSurf, unsigned numSurfs, unsigned clipFlags ) {
	unsigned i;
	unsigned end;

	end = firstSurf + numSurfs;

	for( i = firstSurf; i < end; i++ ) {
		msurface_t *surf = rsh.worldBrushModel->surfaces + i;

		if( !surf->drawSurf ) {
			rn.meshlist->worldSurfVis[i] = 0;
			rn.meshlist->worldSurfFullVis[i] = 0;
			continue;
		}

		if( ( rn.renderFlags & RF_SHADOWMAPVIEW ) && R_SurfNoShadow( surf ) ) {
			rn.meshlist->worldSurfVis[i] = 0;
			rn.meshlist->worldSurfFullVis[i] = 0;
			continue;
		}

		if( rn.meshlist->worldSurfVis[i] ) {
			// the surface is at partly visible in at least one leaf, frustum cull it
			if( R_CullSurface( rsc.worldent, surf, clipFlags ) ) {
				rn.meshlist->worldSurfVis[i] = 0;
			}
			rn.meshlist->worldSurfFullVis[i] = 0;
		}
		else {
			if( rn.meshlist->worldSurfFullVis[i] ) {
				// a fully visible surface, mark as visible
				rn.meshlist->worldSurfVis[i] = 1;
			}
		}

		if( rn.meshlist->worldSurfVis[i] ) {
			rn.meshlist->worldDrawSurfVis[surf->drawSurf - 1] = 1;
		}
	}
}

/*
* R_AddVisSurfaces
*/
static void R_AddVisSurfaces( void ) {
	unsigned i;

	for( i = 0; i < rsh.worldBrushModel->numModelDrawSurfaces; i++ ) {
		if( !rn.meshlist->worldDrawSurfVis[i] ) {
			continue;
		}
		R_AddSurfaceToDrawList( rsc.worldent, i );
	}
}

/*
* R_AddWorldDrawSurfaces
*/
static void R_AddWorldDrawSurfaces( unsigned firstDrawSurf, unsigned numDrawSurfs ) {
	unsigned i;

	for( i = 0; i < numDrawSurfs; i++ ) {
		unsigned ds = firstDrawSurf + i;

		if( !rn.meshlist->worldDrawSurfVis[ds] ) {
			continue;
		}
		R_UpdateSurfaceInDrawList( rsc.worldent, ds, NULL );
	}
}

/*
* R_AddWorldDrawSurfacesJob
*/
static void R_AddWorldDrawSurfacesJob( unsigned first, unsigned items, jobarg_t *j ) {
	R_AddWorldDrawSurfaces( first, items );
}

/*
* R_CullVisLeavesJob
*/
static void R_CullVisLeavesJob( unsigned first, unsigned items, jobarg_t *j ) {
	R_CullVisLeaves( first, items, j->uarg );
}

/*
* R_CullVisSurfacesJob
*/
static void R_CullVisSurfacesJob( unsigned first, unsigned items, jobarg_t *j ) {
	R_CullVisSurfaces( first, items, j->uarg );
}

/*
* R_GetVisFarClip
*/
static float R_GetVisFarClip( void ) {
	int i;
	float dist;
	vec3_t tmp;
	float farclip_dist;

	farclip_dist = 0;
	for( i = 0; i < 8; i++ ) {
		tmp[0] = ( ( i & 1 ) ? rn.visMins[0] : rn.visMaxs[0] );
		tmp[1] = ( ( i & 2 ) ? rn.visMins[1] : rn.visMaxs[1] );
		tmp[2] = ( ( i & 4 ) ? rn.visMins[2] : rn.visMaxs[2] );

		dist = DistanceSquared( tmp, rn.viewOrigin );
		farclip_dist = max( farclip_dist, dist );
	}

	return sqrt( farclip_dist );
}

/*
* R_PostCullVisLeaves
*/
static void R_PostCullVisLeaves( void ) {
	unsigned i, j;
	mleaf_t *leaf;
	float farclip;

	for( i = 0; i < rsh.worldBrushModel->numvisleafs; i++ ) {
		if( !rn.meshlist->worldLeafVis[i] ) {
			continue;
		}

		rf.stats.c_world_leafs++;

		if( rn.renderFlags & RF_SHADOWMAPVIEW ) {
			continue;
		}

		leaf = rsh.worldBrushModel->visleafs[i];
		if( r_leafvis->integer && !( rn.renderFlags & RF_NONVIEWERREF ) ) {
			R_AddDebugBounds( leaf->mins, leaf->maxs, colorRed );
		}

		// add leaf bounds to view bounds
		for( j = 0; j < 3; j++ ) {
			rn.visMins[j] = min( rn.visMins[j], leaf->mins[j] );
			rn.visMaxs[j] = max( rn.visMaxs[j], leaf->maxs[j] );
		}
	}

	if( rn.renderFlags & RF_SHADOWMAPVIEW ) {
		return;
	}

	// now set  the real far clip value and reload view matrices
	farclip = R_GetVisFarClip();

	if( rsh.worldBrushModel->globalfog ) {
		float fogdist = rsh.worldBrushModel->globalfog->shader->fog_dist;
		if( farclip > fogdist ) {
			farclip = fogdist;
		}
	}

	rn.farClip = max( Z_NEAR, farclip ) + Z_BIAS;

	R_SetupViewMatrices( &rn.refdef );
}

/*
* R_DrawWorld
*/
void R_DrawWorld( void ) {
	unsigned i;
	int clipFlags;
	int64_t msec = 0, msec2 = 0;
	bool worldOutlines;
	jobarg_t ja = { 0 };
	bool speeds = r_speeds->integer != 0;

	if( !r_drawworld->integer ) {
		return;
	}
	if( !rsh.worldModel ) {
		return;
	}

	VectorCopy( rsh.worldModel->mins, rn.visMins );
	VectorCopy( rsh.worldModel->maxs, rn.visMaxs );

	VectorCopy( rn.refdef.vieworg, modelOrg );

	worldOutlines = mapConfig.forceWorldOutlines || ( rn.refdef.rdflags & RDF_WORLDOUTLINES );

	if( worldOutlines && ( rn.viewcluster != -1 ) && r_outlines_scale->value > 0 ) {
		rsc.worldent->outlineHeight = max( 0.0f, r_outlines_world->value );
	} else {
		rsc.worldent->outlineHeight = 0;
	}
	Vector4Copy( mapConfig.outlineColor, rsc.worldent->outlineColor );

	clipFlags = rn.clipFlags;
	if( r_nocull->integer ) {
		clipFlags = 0;
	}

	// BEGIN t_world_node
	if( speeds ) {
		msec = ri.Sys_Milliseconds();
	}

	ja.uarg = clipFlags;

	R_ReserveDrawListWorldSurfaces( rn.meshlist );

	if( rsh.worldBrushModel->numvisleafs > rsh.worldBrushModel->numsurfaces ) {
		memset( (void *)rn.meshlist->worldSurfVis, 1, rsh.worldBrushModel->numsurfaces * sizeof( *rn.meshlist->worldSurfVis ) );
		memset( (void *)rn.meshlist->worldSurfFullVis, 0, rsh.worldBrushModel->numsurfaces * sizeof( *rn.meshlist->worldSurfVis ) );
		memset( (void *)rn.meshlist->worldLeafVis, 1, rsh.worldBrushModel->numvisleafs * sizeof( *rn.meshlist->worldLeafVis ) );
		memset( (void *)rn.meshlist->worldDrawSurfVis, 0, rsh.worldBrushModel->numDrawSurfaces * sizeof( *rn.meshlist->worldDrawSurfVis ) );
	} else {
		memset( (void *)rn.meshlist->worldSurfVis, 0, rsh.worldBrushModel->numsurfaces * sizeof( *rn.meshlist->worldSurfVis ) );
		memset( (void *)rn.meshlist->worldSurfFullVis, 0, rsh.worldBrushModel->numsurfaces * sizeof( *rn.meshlist->worldSurfVis ) );
		memset( (void *)rn.meshlist->worldLeafVis, 0, rsh.worldBrushModel->numvisleafs * sizeof( *rn.meshlist->worldLeafVis ) );
		memset( (void *)rn.meshlist->worldDrawSurfVis, 0, rsh.worldBrushModel->numDrawSurfaces * sizeof( *rn.meshlist->worldDrawSurfVis ) );
	}

	//
	// cull leafs
	//
	if( rsh.worldBrushModel->numvisleafs <= rsh.worldBrushModel->numsurfaces ) {
		if( r_speeds->integer ) {
			msec2 = ri.Sys_Milliseconds();
		}

		//RJ_ScheduleJob( &R_CullVisLeavesJob, &ja, rsh.worldBrushModel->numvisleafs );
		R_CullVisLeaves( 0, rsh.worldBrushModel->numvisleafs, clipFlags );

		//RJ_FinishJobs();

		if( speeds ) {
			rf.stats.t_cull_world_nodes += ri.Sys_Milliseconds() - msec2;
		}
	}

	//
	// cull surfaces and do some background work on computed vis leafs
	// 
	if( speeds ) {
		msec2 = ri.Sys_Milliseconds();
	}

	//RJ_ScheduleJob( &R_CullVisSurfacesJob, &ja, rsh.worldBrushModel->numModelSurfaces );
	R_CullVisSurfaces( 0, rsh.worldBrushModel->numModelSurfaces, clipFlags );

	R_PostCullVisLeaves();

	//RJ_FinishJobs();

	if( speeds ) {
		rf.stats.t_cull_world_surfs += ri.Sys_Milliseconds() - msec2;
	}

	//
	// cull rtlights
	//
	if( !(rn.renderFlags & RF_SHADOWMAPVIEW) && !r_fullbright->integer ) {
		if( speeds ) {
			msec2 = ri.Sys_Milliseconds();
		}

		if( r_lighting_realtime_world->integer != 0 ) {
			rf.stats.c_world_lights += R_DrawRtLights( rsh.worldBrushModel->numRtLights, 
				rsh.worldBrushModel->rtLights, clipFlags, r_lighting_realtime_world_shadows->integer != 0 );
		}

		if( r_lighting_realtime_dlight->integer != 0 ) {
			if( !( rn.renderFlags & RF_ENVVIEW ) && r_dynamiclight->integer == 1 ) {
				rf.stats.c_dynamic_lights += R_DrawRtLights( rsc.numDlights, 
					rsc.dlights, clipFlags, r_lighting_realtime_dlight_shadows->integer != 0 );
			}
		}

		if( speeds ) {
			rf.stats.t_cull_rtlights += ri.Sys_Milliseconds() - msec;
		}
	}

	//
	// add visible surfaces to draw list
	//
	if( speeds ) {
		msec2 = ri.Sys_Milliseconds();
	}

	R_AddVisSurfaces();

	//RJ_ScheduleJob( &R_AddWorldDrawSurfacesJob, &ja, rsh.worldBrushModel->numModelDrawSurfaces );
	R_AddWorldDrawSurfaces( 0, rsh.worldBrushModel->numModelDrawSurfaces );

	R_AddLightsToSurfaces();

	if( speeds ) {
		for( i = 0; i < rsh.worldBrushModel->numsurfaces; i++ ) {
			if( rn.meshlist->worldSurfVis[i] ) {
				rf.stats.c_brush_polys++;
			}
		}
	}

	// END t_world_node
	if( speeds ) {
		rf.stats.t_world_node += ri.Sys_Milliseconds() - msec;
	}
}
