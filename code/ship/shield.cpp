/*
 * Copyright (C) Volition, Inc. 1999.  All rights reserved.
 *
 * All source code herein is the property of Volition, Inc. You may not sell 
 * or otherwise commercially exploit the source or things you created based on the 
 * source.
 *
*/


//	Detail level effects (Detail.shield_effects)
//		0		Nothing rendered
//		1		An animating bitmap rendered per hit, not shrink-wrapped.  Lasts half time.  One per ship.
//		2		Animating bitmap per hit, not shrink-wrapped.  Lasts full time.  Unlimited.
//		3		Shrink-wrapped texture.  Lasts half-time.
//		4		Shrink-wrapped texture.  Lasts full-time.

#include "render/3d.h"
#include "model/model.h"
#include "freespace.h"
#include "mission/missionparse.h"
#include "nebula/neb.h"
#include "network/multi.h"
#include "object/objectshield.h"
#include "ship/ship.h"
#include "species_defs/species_defs.h"
#include "tracing/Monitor.h"
#include "tracing/tracing.h"
#include "utils/Random.h"

int	Show_shield_mesh = 0;

//	One unit in 3d means this in the shield hit texture map.
#define	SHIELD_HIT_SCALE	0.15f			// Note, larger constant means smaller effect
#define	MAX_TRIS_PER_HIT	40					//	Number of triangles per shield hit, maximum.
#define	MAX_SHIELD_HITS	200					//	Maximum number of active shield hits.
#define	MAX_SHIELD_TRI_BUFFER	(MAX_SHIELD_HITS*MAX_TRIS_PER_HIT) //(MAX_SHIELD_HITS*20) //	Persistent buffer of triangle comprising all active shield hits.
#define	SHIELD_HIT_DURATION	(3*F1_0/4)	//	Duration, in milliseconds, of shield hit effect

#define	SH_UNUSED			-1					//	Indicates an unused record in Shield_hits
#define	SH_TYPE_1			1					//	Indicates Shield_hits record is of type 1.

#define	UV_MAX				(63.95f/64.0f)	//	max allowed value until tmapper bugs fixed, 1/24/97

float	Shield_scale = SHIELD_HIT_SCALE;

/**
 * Structure which mimics the ::shield_tri structure in model.h.  
 *
 * Since the global shield triangle array needs the vertex information, we will acutally 
 * store the information in this structure instead of the indices into the vertex list
 */
typedef struct gshield_tri {
	int				used;						//	Set if this triangle is currently in use.
	int				trinum;						//	a debug parameter
	fix				creation_time;				//	time at which created.
	shield_vertex	verts[4];					//	Triangles, but at lower detail level, a square.
} gshield_tri;

typedef struct shield_hit {
	fix	start_time;								//	start time of this object
	int	type;									//	type, probably the weapon type, to indicate the bitmap to use
	int	objnum;									//	Object index, needed to get current orientation, position.
	float radius_override;						//  the weapon which caused the hit may adjust the size of the effect
	int	num_tris;								//	Number of Shield_tris comprising this shield.
	int	tri_list[MAX_TRIS_PER_HIT];				//	Indices into Shield_tris, triangles for this shield hit.
	ubyte rgb[3];								//  rgb colors
	matrix hit_orient;							//	hit rotation
	vec3d hit_pos;								//	hit position
} shield_hit;

/**
 * Stores point at which shield was hit.
 * Gets processed in frame interval.
 */
typedef struct shield_point {
	int		objnum;								//	Object that was hit.
	int		shield_tri;							//	Triangle in shield mesh that took hit.
	vec3d	hit_point;							//	Point in global 3-space of hit.
	float	radius_override;
} shield_point;

#define	MAX_SHIELD_POINTS	100
shield_point	Shield_points[MAX_SHIELD_POINTS];
int		Num_shield_points;
int		Num_multi_shield_points;					// used by multiplayer clients

gshield_tri	Global_tris[MAX_SHIELD_TRI_BUFFER];	//	The persistent triangles, part of shield hits.
int	Num_tris;								//	Number of triangles in current shield.  Would be a local, but needed in numerous routines.

shield_hit	Shield_hits[MAX_SHIELD_HITS];

int Shield_bitmaps_loaded = 0;

//	This is a recursive function, so prototype it.
extern void create_shield_from_triangle(int trinum, matrix *orient, shield_info *shieldp, vec3d *tcp, vec3d *centerp, float radius, vec3d *rvec, vec3d *uvec);

void load_shield_hit_bitmap()
{
	size_t i;
	// Check if we've already allocated the shield effect bitmaps
	if ( Shield_bitmaps_loaded )
		return;

	Shield_bitmaps_loaded = 1;

	for (i = 0; i < Species_info.size(); i++ )	
    {
        if (Species_info[i].shield_anim.filename[0] != '\0')
        {
		    Species_info[i].shield_anim.first_frame = bm_load_animation(Species_info[i].shield_anim.filename, &Species_info[i].shield_anim.num_frames, nullptr, nullptr, nullptr, true);
		    Assertion((Species_info[i].shield_anim.first_frame >= 0), "Error while loading shield hit ani: %s for species: %s\n", Species_info[i].shield_anim.filename, Species_info[i].species_name);
        }
	}
}

void shield_hit_page_in()
{
	size_t i;

	if ( !Shield_bitmaps_loaded )	{
		load_shield_hit_bitmap();
	}

	for (i = 0; i < Species_info.size(); i++) {
		generic_anim *sa = &Species_info[i].shield_anim;
		if ( sa->first_frame >= 0 ) {
			bm_page_in_xparent_texture(sa->first_frame, sa->num_frames );
		}
	}
}


/**
 * Initialize shield hit system.  
 *
 * Called from game_level_init()
 */
void shield_hit_init()
{
	int	i;

	for (i=0; i<MAX_SHIELD_HITS; i++) {
		Shield_hits[i].type = SH_UNUSED;
		Shield_hits[i].objnum = -1;
	}

	for (i=0; i<MAX_SHIELD_TRI_BUFFER; i++) {
		Global_tris[i].used = 0;
		Global_tris[i].creation_time = Missiontime;
	}

	Num_multi_shield_points = 0;

	load_shield_hit_bitmap();
}

/**
 * Release the storage allocated to store the shield effect.
 *
 * This doesn't need to do anything; the bitmap manager will release everything.
 */
void release_shield_hit_bitmap()
{
	if ( !Shield_bitmaps_loaded )
		return;
}

int	Poly_count = 0;

/**
 * De-initialize the shield hit system.  Called from game_level_close().
 *
 * @todo We should probably not bother releasing the shield hit bitmaps every level.
 */
void shield_hit_close()
{
	release_shield_hit_bitmap();
}

void shield_frame_init()
{
	Poly_count = 0;
	Num_shield_points = 0;
}

void create_low_detail_poly(int global_index, vec3d *tcp, vec3d *rightv, vec3d *upv)
{
	float		scale;
	gshield_tri	*trip;

	trip = &Global_tris[global_index];

	scale = vm_vec_mag(tcp) * 2.0f;

	vm_vec_scale_add(&trip->verts[0].pos, tcp, rightv, -scale/2.0f);
	vm_vec_scale_add2(&trip->verts[0].pos, upv, scale/2.0f);

	vm_vec_scale_add(&trip->verts[1].pos, &trip->verts[0].pos, rightv, scale);

	vm_vec_scale_add(&trip->verts[2].pos, &trip->verts[1].pos, upv, -scale);

	vm_vec_scale_add(&trip->verts[3].pos, &trip->verts[2].pos, rightv, -scale);

	//	Set u, v coordinates.
	//	Note, this need only be done once, as it's common for all explosions.
	trip->verts[0].u = 0.0f;
	trip->verts[0].v = 0.0f;

	trip->verts[1].u = 1.0f;
	trip->verts[1].v = 0.0f;

	trip->verts[2].u = 1.0f;
	trip->verts[2].v = 1.0f;

	trip->verts[3].u = 0.0f;
	trip->verts[3].v = 1.0f;

}

/**
 * Given a shield triangle, compute the uv coordinates at its vertices given
 * the center point of the explosion texture, distance to center of shield and
 * right and up vectors.
 *
 * For small distances (relative to radius), coordinates can be computed using
 * distance. For larger values, should compute angle.
 */
void rs_compute_uvs(shield_tri *stp, shield_vertex *verts, vec3d *tcp, float  /*radius*/, vec3d *rightv, vec3d *upv)
{
	int	i;
	shield_vertex *sv;

	for (i=0; i<3; i++) {
		vec3d	v2cp;

		sv = &verts[stp->verts[i]];

		vm_vec_sub(&v2cp, &sv->pos, tcp);
		sv->u = vm_vec_dot(&v2cp, rightv) * Shield_scale + 0.5f;
		sv->v = - vm_vec_dot(&v2cp, upv) * Shield_scale + 0.5f;

        CLAMP(sv->u, 0.0f, UV_MAX);
        CLAMP(sv->v, 0.0f, UV_MAX);
	}
}

/**
 * Free records in ::Global_tris previously used by Shield_hits[shnum].tri_list
 */
void free_global_tri_records(int shnum)
{
	int	i;

	Assert((shnum >= 0) && (shnum < MAX_SHIELD_HITS));

	for (i=0; i<Shield_hits[shnum].num_tris; i++){
		Global_tris[Shield_hits[shnum].tri_list[i]].used = 0;
	}
}

void shield_render_low_detail_bitmap(int texture, float alpha, gshield_tri *trip, matrix *orient, vec3d *pos, ubyte r, ubyte g, ubyte b)
{
	int		j;
	vec3d	pnt;
	vertex	verts[4];
    
    memset(verts, 0, sizeof(verts));

	for (j=0; j<4; j++ )	{
		// Rotate point into world coordinates
		vm_vec_unrotate(&pnt, &trip->verts[j].pos, orient);
		vm_vec_add2(&pnt, pos);

		// Pnt is now the x,y,z world coordinates of this vert.
		g3_transfer_vertex(&verts[j], &pnt);
		verts[j].texture_position.u = trip->verts[j].u;
		verts[j].texture_position.v = trip->verts[j].v;
	}	

	verts[0].r = r;
	verts[0].g = g;
	verts[0].b = b;
	verts[1].r = r;
	verts[1].g = g;
	verts[1].b = b;
	verts[2].r = r;
	verts[2].g = g;
	verts[2].b = b;
	verts[3].r = r;
	verts[3].g = g;
	verts[3].b = b;

	vec3d	norm;
	vm_vec_perp(&norm, &trip->verts[0].pos, &trip->verts[1].pos, &trip->verts[2].pos);
	//vertex	*vertlist[4];
	vertex	vertlist[4];
	if ( vm_vec_dot(&norm, &trip->verts[1].pos ) < 0.0 )	{
		vertlist[0] = verts[3]; 
		vertlist[1] = verts[2];
		vertlist[2] = verts[1]; 
		vertlist[3] = verts[0];
		//vertlist[0] = &verts[3]; 
		//vertlist[1] = &verts[2];
		//vertlist[2] = &verts[1]; 
		//vertlist[3] = &verts[0]; 
		//g3_draw_poly( 4, vertlist, TMAP_FLAG_TEXTURED | TMAP_FLAG_RGB | TMAP_FLAG_GOURAUD | TMAP_HTL_3D_UNLIT);
	} else {
		vertlist[0] = verts[0]; 
		vertlist[1] = verts[1];
		vertlist[2] = verts[2]; 
		vertlist[3] = verts[3]; 
		//vertlist[0] = &verts[0]; 
		//vertlist[1] = &verts[1];
		//vertlist[2] = &verts[2]; 
		//vertlist[3] = &verts[3]; 
		//g3_draw_poly( 4, vertlist, TMAP_FLAG_TEXTURED | TMAP_FLAG_RGB | TMAP_FLAG_GOURAUD | TMAP_HTL_3D_UNLIT);
	}

	material material_params;
	material_set_unlit_emissive(&material_params, texture, alpha, 2.0f);
	g3_render_primitives_colored_textured(&material_params, vertlist, 4, PRIM_TYPE_TRIFAN, false);
}

/**
 * Render one triangle of a shield hit effect on one ship.
 * Each frame, the triangle needs to be rotated into global coords.
 *
 * @param texture	handle to desired bitmap to render with
 * @param alpha		alpha value for color blending
 * @param trip		pointer to triangle in global array
 * @param orient	orientation of object shield is associated with
 * @param pos		center point of object
 * @param r			Red colour
 * @param g			Green colour
 * @param b			Blue colour
 */
void shield_render_triangle(int texture, float alpha, gshield_tri *trip, matrix *orient, vec3d *pos, ubyte r, ubyte g, ubyte b)
{
	int		j;
	vec3d	pnt;
	vertex	verts[3];
    
    memset(&verts, 0, sizeof(verts));

	if (trip->trinum == -1)
		return;	//	Means this is a quad, must have switched detail_level.

	for (j=0; j<3; j++ )	{
		// Rotate point into world coordinates
		vm_vec_unrotate(&pnt, &trip->verts[j].pos, orient);
		vm_vec_add2(&pnt, pos);

		// Pnt is now the x,y,z world coordinates of this vert.
		// For this example, I am just drawing a sphere at that point.

	 	g3_transfer_vertex(&verts[j],&pnt);
			
		verts[j].texture_position.u = trip->verts[j].u;
		verts[j].texture_position.v = trip->verts[j].v;
		Assert((trip->verts[j].u >= 0.0f) && (trip->verts[j].u <= UV_MAX));
		Assert((trip->verts[j].v >= 0.0f) && (trip->verts[j].v <= UV_MAX));
	}

	verts[0].r = r;
	verts[0].g = g;
	verts[0].b = b;
	verts[1].r = r;
	verts[1].g = g;
	verts[1].b = b;
	verts[2].r = r;
	verts[2].g = g;
	verts[2].b = b;

	vec3d	norm;
	Poly_count++;
	vm_vec_perp(&norm, &verts[0].world, &verts[1].world, &verts[2].world);

	material material_params;
	material_set_unlit(&material_params, texture, alpha, true, true);
	material_params.set_color_scale(2.0f);

	if ( vm_vec_dot(&norm, &verts[1].world) >= 0.0 ) {
		vertex	vertlist[3];
		vertlist[0] = verts[2]; 
		vertlist[1] = verts[1]; 
		vertlist[2] = verts[0];

		g3_render_primitives_colored_textured(&material_params, vertlist, 3, PRIM_TYPE_TRIFAN, false);
	} else {
		g3_render_primitives_colored_textured(&material_params, verts, 3, PRIM_TYPE_TRIFAN, false);
	}
}

void shield_render_decal(polymodel *pm, matrix *orient, vec3d *pos, matrix* hit_orient, vec3d *hit_pos, float hit_radius, int bitmap_id, color *clr)
{
	if (!pm->shield.buffer_id.isValid() || pm->shield.buffer_n_verts < 3) {
		return;
	}

	g3_start_instance_matrix(pos, orient, true);

	shield_material material_info;

	material_info.set_texture_map(TM_BASE_TYPE, bitmap_id);
	material_info.set_color(*clr);
	material_info.set_blend_mode(bm_has_alpha_channel(bitmap_id) ? ALPHA_BLEND_ALPHA_BLEND_ALPHA : ALPHA_BLEND_ADDITIVE);
	material_info.set_depth_mode(ZBUFFER_TYPE_READ);
	material_info.set_impact_radius(hit_radius);
	material_info.set_impact_transform(*hit_orient, *hit_pos);
	material_info.set_cull_mode(false);

	gr_render_shield_impact(&material_info, PRIM_TYPE_TRIS, &pm->shield.layout, pm->shield.buffer_id, pm->shield.buffer_n_verts);

	g3_done_instance(true);
}

MONITOR(NumShieldRend)

/**
 * Render a shield mesh in the global array ::Shield_hits[]
 */
void render_shield(int shield_num)
{
	vec3d	*centerp;
	matrix	*orient;
	object	*objp;
	ship		*shipp;
	ship_info	*si;
	shield_hit* sh = &Shield_hits[shield_num];

	if (sh->type == SH_UNUSED)	{
		return;
	}

	Assert(sh->objnum >= 0);

	objp = &Objects[sh->objnum];

	if (objp->flags[Object::Object_Flags::No_shields])	{
		return;
	}

	//	If this object didn't get rendered, don't render its shields.  In fact, make the shield hit go away.
	if (!(objp->flags[Object::Object_Flags::Was_rendered])) {
		sh->type = SH_UNUSED;
		return;
	}

	//	At detail levels 1, 3, animations play at double speed to reduce load.
	if ( (Detail.shield_effects == 1) || (Detail.shield_effects == 3) ) {
		sh->start_time -= Frametime;
	}

	MONITOR_INC(NumShieldRend,1);

	shipp = &Ships[objp->instance];
	si = &Ship_info[shipp->ship_info_index];
	// objp, shipp, and si are now setup correctly

	//	If this ship is in its deathroll, make the shield hit effects go away faster.
	if (shipp->flags[Ship::Ship_Flags::Dying])	{
		sh->start_time -= fl2f(2*flFrametime);
	}

	//	Detail level stuff.  When lots of shield hits, maybe make them go away faster.
	if (Poly_count > 50) {
		if (sh->start_time + (SHIELD_HIT_DURATION*50)/Poly_count < Missiontime) {
			sh->type = SH_UNUSED;
			free_global_tri_records(shield_num);
			return;
		}
	} else if ((sh->start_time + SHIELD_HIT_DURATION) < Missiontime) {
		sh->type = SH_UNUSED;
		free_global_tri_records(shield_num);
		return;
	}

	orient = &objp->orient;
	centerp = &objp->pos;

	int bitmap_id, frame_num;

	// Do some sanity checking
	Assert( (si->species >= 0) && (si->species < (int)Species_info.size()) );

	generic_anim *sa = &Species_info[si->species].shield_anim;
	polymodel *pm = model_get(si->model_num);

	// don't try to draw if we don't have an ani
	if ( sa->first_frame >= 0 )
	{
		frame_num = bm_get_anim_frame(sa->first_frame, f2fl(Missiontime - sh->start_time), f2fl(SHIELD_HIT_DURATION));
		bitmap_id = sa->first_frame + frame_num;

		float alpha = 0.9999f;
		nebula_handle_alpha(alpha, centerp, Neb2_fog_visibility_shield);

		ubyte r, g, b;
		r = (ubyte)(sh->rgb[0] * alpha);
		g = (ubyte)(sh->rgb[1] * alpha);
		b = (ubyte)(sh->rgb[2] * alpha);

		if ( bitmap_id <= -1 ) {
			return;
		}

		if ( (Detail.shield_effects == 1) || (Detail.shield_effects == 2) ) {
			shield_render_low_detail_bitmap(bitmap_id, alpha, &Global_tris[sh->tri_list[0]], orient, centerp, r, g, b);
		} else if ( Detail.shield_effects < 4 ) {
			for ( int i = 0; i < sh->num_tris; i++ ) {
				shield_render_triangle(bitmap_id, alpha, &Global_tris[sh->tri_list[i]], orient, centerp, r, g, b);
			}
		} else {
			float hit_radius = pm->core_radius;
			if ( si->is_big_or_huge() ) {
				hit_radius = pm->core_radius * 0.5f;
			}

			if (sh->radius_override >= 0.0f)
				hit_radius = sh->radius_override;

			if (si->max_shield_impact_effect_radius >= 0.0f && hit_radius > si->max_shield_impact_effect_radius) {
				hit_radius = si->max_shield_impact_effect_radius;
			}

			if (hit_radius > 0.0) {
				color clr;
				gr_init_alphacolor(&clr, r, g, b, fl2i(alpha * 255.0f));
				shield_render_decal(pm, orient, centerp, &sh->hit_orient, &sh->hit_pos, hit_radius, bitmap_id, &clr);
			}
		}
	}
}

/**
 * Render all the shield hits  in the global array Shield_hits[]
 *
 * This is a temporary function.  Shield hit rendering will at least have to
 * occur with the ship, perhaps even internal to the ship.
 */
void render_shields()
{
	GR_DEBUG_SCOPE("Render Shields");
	TRACE_SCOPE(tracing::DrawShields);

	int	i;

	if (Detail.shield_effects == 0 || Disable_shield_effects) {
		return;	//	No shield effect rendered at lowest detail level.
	}

	for (i=0; i<MAX_SHIELD_HITS; i++){
		if (Shield_hits[i].type != SH_UNUSED){
			render_shield(i);
		}
	}

	gr_clear_states();
}

void create_tris_containing(vec3d *vp, matrix *orient, shield_info *shieldp, vec3d *tcp, vec3d *centerp, float radius, vec3d *rvec, vec3d *uvec)
{
	int	i, j;
	shield_vertex *verts;

	verts = shieldp->verts;

	for (i=0; i<Num_tris; i++) {
		if ( !shieldp->tris[i].used ) {
			for (j=0; j<3; j++) {
				vec3d v;

				v = verts[shieldp->tris[i].verts[j]].pos;
				if ((vp->xyz.x == v.xyz.x) && (vp->xyz.y == v.xyz.y) && (vp->xyz.z == v.xyz.z))
					create_shield_from_triangle(i, orient, shieldp, tcp, centerp, radius, rvec, uvec);
			}
		}
	}
}

void visit_children(int trinum, int vertex_index, matrix *orient, shield_info *shieldp, vec3d *tcp, vec3d *centerp, float radius, vec3d *rvec, vec3d *uvec)
{
	shield_vertex *sv;

	sv = &(shieldp->verts[shieldp->tris[trinum].verts[vertex_index]]);

	if ( (sv->u > 0.0f) && (sv->u < UV_MAX) && (sv->v > 0.0f) && (sv->v < UV_MAX))
			create_tris_containing(&sv->pos, orient, shieldp, tcp, centerp, radius, rvec, uvec);
}

int get_free_global_shield_index()
{
	int	gi = 0;

	while ((gi < MAX_SHIELD_TRI_BUFFER) && (Global_tris[gi].used) && (Global_tris[gi].creation_time + SHIELD_HIT_DURATION > Missiontime)) {
		gi++;
	}

	//	If couldn't find one, choose a random one.
	if (gi == MAX_SHIELD_TRI_BUFFER)
		gi = (int) (frand() * MAX_SHIELD_TRI_BUFFER);

	return gi;
}

int get_global_shield_tri()
{
	int	shnum;

	//	Find unused shield hit buffer
	for (shnum=0; shnum<MAX_SHIELD_HITS; shnum++)
		if (Shield_hits[shnum].type == SH_UNUSED)
			break;

	if (shnum == MAX_SHIELD_HITS) {
		shnum = Random::next(MAX_SHIELD_HITS);
	}

	Assert((shnum >= 0) && (shnum < MAX_SHIELD_HITS));

	return shnum;
}

void create_shield_from_triangle(int trinum, matrix *orient, shield_info *shieldp, vec3d *tcp, vec3d *centerp, float radius, vec3d *rvec, vec3d *uvec)
{
	rs_compute_uvs( &shieldp->tris[trinum], shieldp->verts, tcp, radius, rvec, uvec);

	shieldp->tris[trinum].used = 1;

	visit_children(trinum, 0, orient, shieldp, tcp, centerp, radius, rvec, uvec);
	visit_children(trinum, 1, orient, shieldp, tcp, centerp, radius, rvec, uvec);
	visit_children(trinum, 2, orient, shieldp, tcp, centerp, radius, rvec, uvec);
}

/**
 * Copy information from Current_tris to ::Global_tris, stuffing information
 * in a slot in ::Shield_hits.  
 *
 * The Global_tris array is not a shield_tri structure.
 * We need to store vertex information in the global array since the vertex list
 * will not be available to us when we actually use the array.
 */
void copy_shield_to_globals( int objnum, shield_info *shieldp, matrix *hit_orient, vec3d *hit_pos, float radius_override)
{
	int	i, j;
	int	gi = 0;
	int	count = 0;			//	Number of triangles in this shield hit.
	int	shnum;				//	shield hit number, index in Shield_hits.

	shnum = get_global_shield_tri();
	
	Shield_hits[shnum].type = SH_TYPE_1;

	for (i = 0; i < shieldp->ntris; i++ ) {
		if ( shieldp->tris[i].used ) {
			while ( (gi < MAX_SHIELD_TRI_BUFFER) && (Global_tris[gi].used) && (Global_tris[gi].creation_time + SHIELD_HIT_DURATION > Missiontime)) {
				gi++;
			}
			
			//	If couldn't find one, choose a random one.
			if (gi == MAX_SHIELD_TRI_BUFFER)
				gi = (int) (frand() * MAX_SHIELD_TRI_BUFFER);

			Global_tris[gi].used = shieldp->tris[i].used;
			Global_tris[gi].trinum = i;
			Global_tris[gi].creation_time = Missiontime;

			// copy the pos/u/v elements of the shield_vertex structure into the shield vertex structure for this global triangle.
			for (j = 0; j < 3; j++)
				Global_tris[gi].verts[j] = shieldp->verts[shieldp->tris[i].verts[j]];
			Shield_hits[shnum].tri_list[count++] = gi;

			if (count >= MAX_TRIS_PER_HIT) {
				if (Detail.shield_effects < 4) {
					mprintf(("Warning: Too many triangles in shield hit.\n"));
				}
				break;
			}
		}
	}

	Shield_hits[shnum].num_tris = count;
	Shield_hits[shnum].start_time = Missiontime;
	Shield_hits[shnum].objnum = objnum;
	Shield_hits[shnum].hit_orient = *hit_orient;
	Shield_hits[shnum].hit_pos = *hit_pos;
	Shield_hits[shnum].radius_override = radius_override;

	Shield_hits[shnum].rgb[0] = 255;
	Shield_hits[shnum].rgb[1] = 255;
	Shield_hits[shnum].rgb[2] = 255;

	if((objnum >= 0) && (objnum < MAX_OBJECTS) && (Objects[objnum].type == OBJ_SHIP) && (Objects[objnum].instance >= 0) && (Objects[objnum].instance < MAX_SHIPS) && (Ships[Objects[objnum].instance].ship_info_index >= 0) && (Ships[Objects[objnum].instance].ship_info_index < ship_info_size())){
		ship_info *sip = &Ship_info[Ships[Objects[objnum].instance].ship_info_index];
		
		Shield_hits[shnum].rgb[0] = sip->shield_color[0];
		Shield_hits[shnum].rgb[1] = sip->shield_color[1];
		Shield_hits[shnum].rgb[2] = sip->shield_color[2];
	}
}


/**
 * This function needs to be called by big ships which have shields. It should be able to be modified to deal with
 * the large polygons we use for their shield meshes - unknownplayer
 *
 * At lower detail levels, shield hit effects are a single texture, applied to one enlarged triangle.
 */
void create_shield_low_detail(int objnum, int  /*model_num*/, matrix * /*orient*/, vec3d * /*centerp*/, vec3d *tcp, int tr0, shield_info *shieldp)
{
	matrix	tom;
	int		gi;
	int		shnum;

	shnum = get_global_shield_tri();
	Shield_hits[shnum].type = SH_TYPE_1;

	gi = get_free_global_shield_index();

	Global_tris[gi].used = 1;
	Global_tris[gi].trinum = -1;		//	This tells triangle renderer to not render in case detail_level was switched.
	Global_tris[gi].creation_time = Missiontime;

	Shield_hits[shnum].tri_list[0] = gi;
	Shield_hits[shnum].num_tris = 1;
	Shield_hits[shnum].start_time = Missiontime;
	Shield_hits[shnum].objnum = objnum;

	Shield_hits[shnum].rgb[0] = 255;
	Shield_hits[shnum].rgb[1] = 255;
	Shield_hits[shnum].rgb[2] = 255;
	if((objnum >= 0) && (objnum < MAX_OBJECTS) && (Objects[objnum].type == OBJ_SHIP) && (Objects[objnum].instance >= 0) && (Objects[objnum].instance < MAX_SHIPS) && (Ships[Objects[objnum].instance].ship_info_index >= 0) && (Ships[Objects[objnum].instance].ship_info_index < ship_info_size())){
		ship_info *sip = &Ship_info[Ships[Objects[objnum].instance].ship_info_index];
		
		Shield_hits[shnum].rgb[0] = sip->shield_color[0];
		Shield_hits[shnum].rgb[1] = sip->shield_color[1];
		Shield_hits[shnum].rgb[2] = sip->shield_color[2];
	}

	vm_vector_2_matrix_norm(&tom, &shieldp->tris[tr0].norm, nullptr, nullptr);

	create_low_detail_poly(gi, tcp, &tom.vec.rvec, &tom.vec.uvec);
}

// Algorithm for shrink-wrapping a texture across a triangular mesh.
// 
// - Given a point of intersection, tcp (local to objnum)
// - Vector to center of shield from tcp is v2c.
// - Using v2c, compute right and down vectors.  These are the vectors of
//   increasing u and v, respectively.
// - Triangle of intersection of tcp is tr0.
// - For 3 points in tr0, compute u,v coordinates using up and down vectors
//   from center point, tcp.  Need to know size of explosion texture.  N units
//   along right vector corresponds to O units in explosion texture space.
// - For each edge, if either endpoint was outside texture bounds, recursively
//   apply previous and current step.
// 
// Output of above is a list of triangles with u,v coordinates.  These u,v
// coordinates will have to be clipped against the explosion texture bounds.

void create_shield_explosion(int objnum, int model_num, vec3d *tcp, int tr0, float radius_override)
{
	matrix	tom;		//	Texture Orientation Matrix
	shield_info	*shieldp;
	polymodel	*pm;
	int		i;
	object* objp = &Objects[objnum];

	if (objp->flags[Object::Object_Flags::No_shields])
		return;

	pm = model_get(model_num);
	Num_tris = pm->shield.ntris;
	shieldp = &pm->shield;

	if (Num_tris == 0)
		return;

	if ( (Detail.shield_effects == 1) || (Detail.shield_effects == 2) ) {
		create_shield_low_detail(objnum, model_num, &objp->orient, &objp->pos, tcp, tr0, shieldp);
		return;
	}

	for (i=0; i<Num_tris; i++)
		shieldp->tris[i].used = 0;

	//	Compute orientation matrix from normal of surface hit.
	//	Note, this will cause the shape of the bitmap to change abruptly
	//	as the impact point moves to another triangle.  To prevent this,
	//	you could average the normals at the vertices, then interpolate the
	//	normals from the vertices to get a smoothly changing normal across the face.
	//	I had tried using the vector from the impact point to the center, which
	//	changes smoothly, but this looked surprisingly bad.
	vm_vector_2_matrix_norm(&tom, &shieldp->tris[tr0].norm, nullptr, nullptr);

	//	Create the shield from the current triangle, as well as its neighbors.
	create_shield_from_triangle(tr0, &objp->orient, shieldp, tcp, &objp->pos, objp->radius, &tom.vec.rvec, &tom.vec.uvec);

	for (i=0; i<3; i++)
		create_shield_from_triangle(shieldp->tris[tr0].neighbors[i], &objp->orient, shieldp, tcp, &objp->pos, objp->radius, &tom.vec.rvec, &tom.vec.uvec);
	
	copy_shield_to_globals(objnum, shieldp, &tom, tcp, radius_override);
}

MONITOR(NumShieldHits)

/**
 * Add data for a shield hit.
 */
void add_shield_point(int objnum, int tri_num, const vec3d *hit_pos, float radius_override)
{
	if (Num_shield_points >= MAX_SHIELD_POINTS)
		return;

	Verify(objnum < MAX_OBJECTS);

	MONITOR_INC(NumShieldHits,1);

	Shield_points[Num_shield_points].objnum = objnum;
	Shield_points[Num_shield_points].shield_tri = tri_num;
	Shield_points[Num_shield_points].hit_point = *hit_pos;
	Shield_points[Num_shield_points].radius_override = radius_override;

	Num_shield_points++;

	Ships[Objects[objnum].instance].shield_hits++;
}

// ugh!  I wrote a special routine to store shield points for clients in multiplayer
// games.  Problem is initilization and flow control of normal gameplay make this problem
// more than trivial to solve.  Turns out that I think I can just keep track of the
// shield_points for multiplayer in a separate count -- then assign the multi count to
// the normal count at the correct time.
void add_shield_point_multi(int objnum, int tri_num, vec3d *hit_pos)
{
	if (Num_multi_shield_points >= MAX_SHIELD_POINTS)
		return;

	Shield_points[Num_shield_points].objnum = objnum;
	Shield_points[Num_shield_points].shield_tri = tri_num;
	Shield_points[Num_shield_points].hit_point = *hit_pos;

	Num_multi_shield_points++;
}

/**
 * Sets up the shield point hit information for multiplayer clients
 */
void shield_point_multi_setup()
{
	int i;

	Assert( MULTIPLAYER_CLIENT );

	if ( Num_multi_shield_points == 0 )
		return;

	Num_shield_points = Num_multi_shield_points;
	for (i = 0; i < Num_shield_points; i++ ){
		Ships[Objects[Shield_points[i].objnum].instance].shield_hits++;
	}

	Num_multi_shield_points = 0;
}


/**
 * Create all the shield explosions that occurred on object *objp this frame.
 */
void create_shield_explosion_all(object *objp)
{
	if (Detail.shield_effects == 0 || Disable_shield_effects) {
		return;	
	}

	int	i;
	int	num;
	int	count;
	int	objnum;
	ship	*shipp;

	num = objp->instance;
	shipp = &Ships[num];

	count = shipp->shield_hits;
	objnum = OBJ_INDEX(objp);

	for (i=0; i<Num_shield_points; i++) {
		if (Shield_points[i].objnum == objnum) {
			create_shield_explosion(objnum, Ship_info[shipp->ship_info_index].model_num, &Shield_points[i].hit_point, Shield_points[i].shield_tri, Shield_points[i].radius_override);
			count--;
			if (count <= 0){
				break;
			}
		}
	}

	// some some reason, clients seem to have a bogus count valud on occation.  I"ll chalk it up
	// to missed packets :-)  MWA 2/6/98
	if ( !MULTIPLAYER_CLIENT ) {
		Assert(count == 0);	//	Couldn't find all the alleged shield hits.  Bogus!
	}
}

/**
 * Returns the lowest threshold of shield hitpoints that triggers a shield hit
 *
 * @return If all_quadrants is true, looks at entire shield, otherwise just one quadrant
 */
float ship_shield_hitpoint_threshold(const object* obj, bool all_quadrants) 
{
	if (all_quadrants) {
		// All quadrants
		auto num_quads = static_cast<float>(obj->shield_quadrant.size());
		return MAX(2.0f * num_quads, Shield_percent_skips_damage * shield_get_max_strength(obj));
	} else {
		// Just one quadrant
		return MAX(2.0f, Shield_percent_skips_damage * shield_get_max_quad(obj));
	}
}

/**
 * Returns true if the shield presents any opposition to something trying to force through it.
 *
 * @return If quadrant is -1, looks at entire shield, otherwise just one quadrant
 */
bool ship_is_shield_up(const object *obj, int quadrant)
{
	if ( (quadrant >= 0) && (quadrant < static_cast<int>(obj->shield_quadrant.size())))	{
		// Just check one quadrant
		return ( shield_get_quad(obj, quadrant) > ship_shield_hitpoint_threshold(obj, false) );
	} else {
		// Check all quadrants
		return ( shield_get_strength(obj) > ship_shield_hitpoint_threshold(obj, true) );
	}
}

//	return quadrant containing hit_pnt.
//	\  1  /.
//	3 \ / 0
//	  / \.
//	/  2  \.
//	Note: This is in the object's local reference frame.  Do _not_ pass a vector in the world frame.
int get_quadrant(const vec3d *hit_pnt, const object *shipobjp)
{
	if (shipobjp != NULL && Ship_info[Ships[shipobjp->instance].ship_info_index].flags[Ship::Info_Flags::Model_point_shields]) {
		int closest = -1;
		float closest_dist = FLT_MAX;

		for (unsigned int i=0; i<Ships[shipobjp->instance].shield_points.size(); i++) {
			float dist = vm_vec_dist(hit_pnt, &Ships[shipobjp->instance].shield_points.at(i));

			if (dist < closest_dist) {
				closest = i;
				closest_dist = dist;
			}
		}

		return closest;
	} else {
		int	result = 0;

		if (hit_pnt->xyz.x < hit_pnt->xyz.z)
			result |= 1;

		if (hit_pnt->xyz.x < -hit_pnt->xyz.z)
			result |= 2;

		return result;
	}
}
