/*
 * Copyright (C) Volition, Inc. 1999.  All rights reserved.
 *
 * All source code herein is the property of Volition, Inc. You may not sell 
 * or otherwise commercially exploit the source or things you created based on the 
 * source.
 *
*/




#include <algorithm>

#include "asteroid/asteroid.h"
#include "cmdline/cmdline.h"
#include "debris/debris.h"
#include "debugconsole/console.h"
#include "freespace.h"
#include "gamesnd/gamesnd.h"
#include "globalincs/linklist.h"
#include "graphics/color.h"
#include "hud/hudets.h"
#include "hud/hudmessage.h"
#include "hud/hudshield.h"
#include "iff_defs/iff_defs.h"
#include "io/timer.h"
#include "lighting/lighting.h"
#include "lighting/lighting_profiles.h"
#include "math/fvi.h"
#include "math/curve.h"
#include "math/staticrand.h"
#include "nebula/neb.h"
#include "mod_table/mod_table.h"
#include "network/multi.h"
#include "network/multimsgs.h"
#include "object/objcollide.h"
#include "object/object.h"
#include "object/objectshield.h"
#include "parse/parselo.h"
#include "scripting/global_hooks.h"
#include "scripting/scripting.h"
#include "scripting/api/objs/model.h"
#include "scripting/api/objs/vecmath.h"
#include "particle/particle.h"
#include "playerman/player.h"
#include "render/3d.h"
#include "ship/ship.h"
#include "ship/shipfx.h"
#include "ship/shiphit.h"
#include "weapon/beam.h"
#include "weapon/weapon.h"
#include "globalincs/globals.h"
#include "globalincs/vmallocator.h"
#include "tracing/tracing.h"

// ------------------------------------------------------------------------------------------------
// BEAM WEAPON DEFINES/VARS
//

// this is the constant which defines when a beam is an "area" beam. meaning, when we switch on sphereline checking and when 
// a beam gets "stopped" by an object. It is a percentage of the object radius which the beam must be wider than
#define BEAM_AREA_PERCENT			0.4f

// randomness factor - all beam weapon aiming is adjusted by +/- some factor within this range
#define BEAM_RANDOM_FACTOR			0.4f

#define MAX_SHOT_POINTS				30
#define SHOT_POINT_TIME				200			// 5 arcs a second

#define TOOLTIME						1500.0f

std::array<beam, MAX_BEAMS> Beams;				// all beams
beam Beam_free_list;					// free beams
beam Beam_used_list;					// used beams
int Beam_count = 0;					// how many beams are in use

// debug stuff - keep track of how many collision tests we perform a second and how many we toss a second
#define BEAM_TEST_STAMP_TIME		4000	// every 4 seconds
int Beam_test_stamp = -1;
int Beam_test_ints = 0;
int Beam_test_ship = 0;
int Beam_test_ast = 0;
int Beam_test_framecount = 0;

// beam warmup completion %
#define BEAM_WARMUP_PCT(b)			( ((float)Weapon_info[b->weapon_info_index].b_info.beam_warmup - (float)timestamp_until(b->warmup_stamp)) / (float)Weapon_info[b->weapon_info_index].b_info.beam_warmup ) 

// beam warmdown completion %		
#define BEAM_WARMDOWN_PCT(b)		( ((float)Weapon_info[b->weapon_info_index].b_info.beam_warmdown - (float)timestamp_until(b->warmdown_stamp)) / (float)Weapon_info[b->weapon_info_index].b_info.beam_warmdown )

// link into the physics paused system
extern int physics_paused;

// beam lighting info
#define MAX_BEAM_LIGHT_INFO		100
typedef struct beam_light_info {
	beam *bm;					// beam casting the light
	int objnum;					// object getting light cast on it
	ubyte source;				// 0 to light the shooter, 1 for lighting any ship the beam passes, 2 to light the collision ship
	vec3d c_point;			// collision point for type 2 lights
} beam_light_info;

beam_light_info Beam_lights[MAX_BEAM_LIGHT_INFO];
int Beam_light_count = 0;

float b_whack_small = 2000.0f;	// used to be 500.0f with the retail whack bug
float b_whack_big = 10000.0f;	// used to be 1500.0f with the retail whack bug
float b_whack_damage = 150.0f;

DCF(b_whack_small, "Sets the whack factor for small whacks (Default is 2000f)")
{
	dc_stuff_float(&b_whack_small);
}
DCF(b_whack_big, "Sets the whack factor for big whacks (Default is 10000f)")
{
	dc_stuff_float(&b_whack_big);
}
DCF(b_whack_damage, "Sets the whack damage threshold (Default is 150f)")
{
	if (dc_optional_string_either("help", "--help")) {
		dc_printf("Sets the threshold to determine whether a big whack or a small whack should be applied. Values equal or greater than this threshold will trigger a big whack, while smaller values will trigger a small whack\n");
		return;
	}

	dc_stuff_float(&b_whack_damage);
}


// ------------------------------------------------------------------------------------------------
// BEAM WEAPON FORWARD DECLARATIONS
//

// delete a beam
void beam_delete(beam *b);

// handle a hit on a specific object
void beam_handle_collisions(beam *b);

// fills in binfo
void beam_get_binfo(beam* b, float accuracy, int num_shots, int burst_seed, float burst_shot_rotation, float per_burst_shot_rotation);

// aim the beam (setup last_start and last_shot - the endpoints). also recalculates object collision info
void beam_aim(beam *b);

// direct fire type functions
void beam_type_direct_fire_move(beam *b);

// slashing type functions
void beam_type_slashing_move(beam *b);

// targeting type functions
void beam_type_targeting_move(beam *b);

// antifighter type functions
void beam_type_antifighter_move(beam *b);
// stuffs the index of the current pulse in shot_index
// stuffs 0 in fire_wait if the beam is active, 1 if it is between pulses
void beam_type_antifighter_get_status(beam *b, int *shot_index, int *fire_wait);

// normal firing type functions
void beam_type_normal_move(beam *b);

// given a model #, and an object, stuff 2 good world coord points
void beam_get_octant_points(int modelnum, object *objp, int seed, vec3d *v1, vec3d *v2);

// given an object, return its model num
int beam_get_model(object *objp);

// for rendering the beam effect
// output top and bottom vectors
// fvec == forward vector (eye viewpoint basically. in world coords)
// pos == world coordinate of the point we're calculating "around"
// w == width of the diff between top and bottom around pos
void beam_calc_facing_pts(vec3d *top, vec3d *bot, vec3d *fvec, vec3d *pos, float w, float z_add);

// render the muzzle glow for a beam weapon
void beam_render_muzzle_glow(beam *b);

// generate particles for the muzzle glow
void beam_generate_muzzle_particles(beam *b);

// throw some jitter into the aim - based upon shot_aim
void beam_jitter_aim(beam *b, float aim);

// if it is legal for the beam to continue firing
// returns -1 if the beam should stop firing immediately
// returns 0 if the beam should go to warmdown
// returns 1 if the beam can continue along its way
int beam_ok_to_fire(beam *b);

// start the warmup phase for the beam
void beam_start_warmup(beam *b);

// start the firing phase for the beam, return 0 if the beam failed to start, and should be deleted altogether
int beam_start_firing(beam *b);

// start the warmdown phase for the beam
void beam_start_warmdown(beam *b);

// add a collision to the beam for this frame (to be evaluated later)
void beam_add_collision(beam *b, object *hit_object, mc_info *cinfo, int quad = -1, bool exit_flag = false);

// mark an object as being lit
void beam_add_light(beam *b, int objnum, int source, vec3d *c_point);

// apply lighting from any beams
void beam_apply_lighting();

// recalculate beam sounds (looping sounds relative to the player)
void beam_recalc_sounds(beam *b);

// apply a whack to a ship
void beam_apply_whack(beam *b, object *objp, vec3d *hit_point);

// if the beam is likely to tool a given target before its lifetime expires
int beam_will_tool_target(beam *b, object *objp);

// ------------------------------------------------------------------------------------------------
// BEAM WEAPON FUNCTIONS
//

// init at game startup
void beam_init()
{
	beam_level_close();
}

// initialize beam weapons for this level
void beam_level_init()
{
	// intialize beams
	int idx;

	Beam_count = 0;
	list_init( &Beam_free_list );
	list_init( &Beam_used_list );
	Beams.fill({});

	// Link all object slots into the free list
	for (idx=0; idx<MAX_BEAMS; idx++)	{
		Beams[idx].objnum = -1;
		list_append(&Beam_free_list, &Beams[idx] );
	}

	// reset muzzle particle spew timestamp
}

// shutdown beam weapons for this level
void beam_level_close()
{
	// clear the beams
	list_init( &Beam_free_list );
	list_init( &Beam_used_list );
}

// get the width of the widest section of the beam
float beam_get_widest(beam* b)
{
	int idx;
	float widest = -1.0f;

	// sanity
	Assert(b->weapon_info_index >= 0);
	if (b->weapon_info_index < 0) {
		return -1.0f;
	}

	// lookup
	for (idx = 0; idx < Weapon_info[b->weapon_info_index].b_info.beam_num_sections; idx++) {
		if (Weapon_info[b->weapon_info_index].b_info.sections[idx].width > widest) {
			widest = Weapon_info[b->weapon_info_index].b_info.sections[idx].width;
		}
	}

	// return	
	return widest;
}

static void beam_set_state(weapon_info* wip, beam* bm, WeaponState state)
{
	if (bm->weapon_state == state)
	{
		// No change
		return;
	}

	bm->weapon_state = state;

	auto map_entry = wip->state_effects.find(bm->weapon_state);

	if ((map_entry != wip->state_effects.end()) && map_entry->second.isValid())
	{
		auto source = particle::ParticleManager::get()->createSource(map_entry->second);
		source->setHost(make_unique<EffectHostBeam>(&Objects[bm->objnum]));
		source->finishCreation();
	}
}

// return false if the particular beam fire method doesn't have all the required, and specific to its fire method, info
bool beam_has_valid_params(beam_fire_info* fire_info) {
	switch (fire_info->fire_method) {
		case BFM_TURRET_FIRED:
			if (fire_info->shooter == nullptr || fire_info->turret == nullptr || fire_info->target == nullptr)
				return false;
			break;
		case BFM_TURRET_FORCE_FIRED:
			if (fire_info->shooter == nullptr || fire_info->turret == nullptr || (fire_info->target == nullptr && !(fire_info->bfi_flags & BFIF_TARGETING_COORDS)))
				return false;
			break;
		case BFM_FIGHTER_FIRED:
			if (fire_info->shooter == nullptr || fire_info->turret == nullptr)
				return false;
			break;
		case BFM_SPAWNED:
		case BFM_SEXP_FLOATING_FIRED:
			if (!(fire_info->bfi_flags & BFIF_FLOATING_BEAM) || (fire_info->target == nullptr && !(fire_info->bfi_flags & BFIF_TARGETING_COORDS)))
				return false;
			break;
		case BFM_SUBSPACE_STRIKE:
			if (!(fire_info->bfi_flags & BFIF_FLOATING_BEAM) || (fire_info->target == nullptr))
				return false;
			break;		
		default:
			Assertion(false, "Unrecognized beam fire method in beam_has_valid_params");
			return false;
	}

	// let's also validate the type of target, if applicable
	if (fire_info->target != nullptr) {
		if ((fire_info->target->type != OBJ_SHIP) && (fire_info->target->type != OBJ_ASTEROID) && (fire_info->target->type != OBJ_DEBRIS) && (fire_info->target->type != OBJ_WEAPON))
			return false;
	}

	return true;
}

// fire a beam, returns nonzero on success. the innards of the code handle all the rest, foo
int beam_fire(beam_fire_info *fire_info)
{
	beam *new_item;
	weapon_info *wip;
	ship *firing_ship = NULL;
	int objnum;		

	// sanity check
	if(fire_info == NULL){
		Int3();
		return -1;
	}

	// if we're out of beams, bail
	if(Beam_count >= MAX_BEAMS){
		return -1;
	}

	// make sure the beam_info_index is valid
	if ((fire_info->beam_info_index < 0) || (fire_info->beam_info_index >= weapon_info_size()) || !(Weapon_info[fire_info->beam_info_index].wi_flags[Weapon::Info_Flags::Beam])) {
		UNREACHABLE("beam_info_index (%d) invalid (either <0, >= %d, or not actually a beam)!\n", fire_info->beam_info_index, weapon_info_size());
		return -1;
	}

	wip = &Weapon_info[fire_info->beam_info_index];	

	// copied from weapon_create()
	if ((wip->num_substitution_patterns > 0) && (fire_info->shooter != nullptr)) {
		// using substitution

		// get to the instance of the gun
		Assertion(fire_info->shooter->type == OBJ_SHIP, "Expected type OBJ_SHIP, got %d", fire_info->shooter->type);
		Assertion((fire_info->shooter->instance < MAX_SHIPS) && (fire_info->shooter->instance >= 0),
			"Ship index is %d, which is out of range [%d,%d)", fire_info->shooter->instance, 0, MAX_SHIPS);
		ship* parent_shipp = &(Ships[fire_info->shooter->instance]);
		Assert(parent_shipp != nullptr);

		size_t* position = get_pointer_to_weapon_fire_pattern_index(fire_info->beam_info_index, fire_info->shooter->instance, fire_info->turret);
		Assertion(position != nullptr, "'%s' is trying to fire a weapon that is not selected", Ships[fire_info->shooter->instance].ship_name);

		size_t curr_pos = *position;
		if ((parent_shipp->flags[Ship::Ship_Flags::Primary_linked]) && curr_pos > 0) {
			curr_pos--;
		}
		++(*position);
		*position = (*position) % wip->num_substitution_patterns;

		if (wip->weapon_substitution_pattern[curr_pos] == -1) {
			// weapon doesn't want any sub
			return -1;
		}
		else if (wip->weapon_substitution_pattern[curr_pos] != fire_info->beam_info_index) {
			fire_info->beam_info_index = wip->weapon_substitution_pattern[curr_pos];
			// weapon wants to sub with weapon other than me
			return beam_fire(fire_info);
		}
	}

	if (!beam_has_valid_params(fire_info))
		return -1;

	if (fire_info->shooter != NULL) {
		firing_ship = &Ships[fire_info->shooter->instance];
	}

	// get a free beam
	new_item = GET_FIRST(&Beam_free_list);
	Assert( new_item != &Beam_free_list );		// shouldn't have the dummy element
	if(new_item == &Beam_free_list){
		return -1;
	}

	// make sure that our textures are loaded as well
	extern bool weapon_is_used(int weapon_index);
	extern void weapon_load_bitmaps(int weapon_index);
	if ( !weapon_is_used(fire_info->beam_info_index) ) {
		weapon_load_bitmaps(fire_info->beam_info_index);
	}

	// remove from the free list
	list_remove( &Beam_free_list, new_item );
	
	// insert onto the end of used list
	list_append( &Beam_used_list, new_item );

	// increment counter
	Beam_count++;	

	// fill in some values
	new_item->warmup_stamp = -1;
	new_item->warmdown_stamp = -1;
	new_item->weapon_info_index = fire_info->beam_info_index;	
	new_item->objp = fire_info->shooter;
	new_item->sig = (fire_info->shooter != NULL) ? fire_info->shooter->signature : 0;
	new_item->subsys = fire_info->turret;	
	new_item->life_left = wip->b_info.beam_life;	
	new_item->life_total = wip->b_info.beam_life;
	new_item->r_collision_count = 0;
	new_item->f_collision_count = 0;
	new_item->target = fire_info->target;
	new_item->target_subsys = fire_info->target_subsys;
	new_item->target_sig = (fire_info->target != NULL) ? fire_info->target->signature : 0;
	new_item->beam_sound_loop        = sound_handle::invalid();
	new_item->type = wip->b_info.beam_type;
	new_item->local_fire_postion = fire_info->local_fire_postion;
	new_item->framecount = 0;
	new_item->flags = 0;
	new_item->shot_index = 0;
	new_item->current_width_factor = wip->b_info.beam_initial_width < 0.1f ? 0.1f : wip->b_info.beam_initial_width;
	new_item->team = (firing_ship == NULL) ? fire_info->team : static_cast<char>(firing_ship->team);
	new_item->range = wip->b_info.range;
	new_item->damage_threshold = wip->b_info.damage_threshold;
	new_item->bank = fire_info->bank;
	new_item->beam_glow_frame = 0.0f;
	new_item->firingpoint = (fire_info->bfi_flags & BFIF_FLOATING_BEAM) ? -1 : fire_info->turret->turret_next_fire_pos;
	new_item->last_start = fire_info->starting_pos;
	new_item->type5_rot_speed = wip->b_info.t5info.continuous_rot;
	new_item->rotates = wip->b_info.beam_type == BeamType::OMNI && wip->b_info.t5info.continuous_rot_axis != Type5BeamRotAxis::UNSPECIFIED;
	new_item->modular_curves_instance = wip->beam_curves.create_instance();

	if (fire_info->bfi_flags & BFIF_FORCE_FIRING)
		new_item->flags |= BF_FORCE_FIRING;
	if (fire_info->bfi_flags & BFIF_IS_FIGHTER_BEAM)
		new_item->flags |= BF_IS_FIGHTER_BEAM;
	if (fire_info->bfi_flags & BFIF_FLOATING_BEAM)
		new_item->flags |= BF_FLOATING_BEAM;

	if (fire_info->bfi_flags & BFIF_TARGETING_COORDS) {
		new_item->flags |= BF_TARGETING_COORDS;
		new_item->target_pos1 = fire_info->target_pos1;
		new_item->target_pos2 = fire_info->target_pos2;
	} else {
		vm_vec_zero(&new_item->target_pos1);
		vm_vec_zero(&new_item->target_pos2);
	}

	for (float &frame : new_item->beam_section_frame)
		frame = 0.0f;

	// beam collision and light width
	if (wip->b_info.beam_width > 0.0f) {
		new_item->beam_collide_width = wip->b_info.beam_width;
		new_item->beam_light_width = wip->b_info.beam_width;
	} else {
		float widest = beam_get_widest(new_item);
		new_item->beam_collide_width = wip->collision_radius_override > 0.0f ? wip->collision_radius_override : widest;
		new_item->beam_light_width = widest;
	}
	
	if (fire_info->bfi_flags & BFIF_IS_FIGHTER_BEAM && new_item->type != BeamType::OMNI) {
		new_item->type = BeamType::TARGETING;
	}

	// if the targeted subsystem is not NULL, force it to be a direct fire beam
	if(new_item->target_subsys != nullptr && new_item->type != BeamType::TARGETING && new_item->type != BeamType::OMNI){
		new_item->type = BeamType::DIRECT_FIRE;
	}

	// antifighter beam weapons can only fire at small ships and missiles
	if(new_item->type == BeamType::ANTIFIGHTER){
		// if its a targeted ship, get the target ship
		if((fire_info->target != NULL) && (fire_info->target->type == OBJ_SHIP) && (fire_info->target->instance >= 0)){		
			ship *target_ship = &Ships[fire_info->target->instance];
			
			// maybe force to be direct fire
			if(Ship_info[target_ship->ship_info_index].class_type > -1 && (Ship_types[Ship_info[target_ship->ship_info_index].class_type].flags[Ship::Type_Info_Flags::Beams_easily_hit])){
				new_item->type = BeamType::DIRECT_FIRE;
			}
		}
	}
	
	// ----------------------------------------------------------------------
	// THIS IS THE CRITICAL POINT FOR MULTIPLAYER
	// beam_get_binfo(...) determines exactly how the beam will behave over the course of its life
	// it fills in binfo, which we can pass to clients in multiplayer	
	if(fire_info->beam_info_override != NULL){
		new_item->binfo = *fire_info->beam_info_override;
	} else {
		float burst_rot = 0.0f;
		if (new_item->type == BeamType::OMNI && !wip->b_info.t5info.burst_rot_pattern.empty()) {
			burst_rot = wip->b_info.t5info.burst_rot_pattern[fire_info->burst_index % wip->b_info.t5info.burst_rot_pattern.size()];
		}
		beam_get_binfo(new_item, fire_info->accuracy, wip->b_info.beam_shots,fire_info->burst_seed, burst_rot, fire_info->per_burst_rotation);			// to fill in b_info	- the set of directional aim vectors
	}	

    flagset<Object::Object_Flags> default_flags;
	if (!wip->wi_flags[Weapon::Info_Flags::No_collide])
		default_flags.set(Object::Object_Flags::Collides);
	if (wip->wi_flags[Weapon::Info_Flags::Can_damage_shooter])
		default_flags.set(Object::Object_Flags::Collides_with_parent);

	// create the associated object
	objnum = obj_create(OBJ_BEAM, ((fire_info->shooter != NULL) ? OBJ_INDEX(fire_info->shooter) : -1), BEAM_INDEX(new_item), &vmd_identity_matrix, &vmd_zero_vector, 1.0f, default_flags);
	if(objnum < 0){
		beam_delete(new_item);
		mprintf(("obj_create() failed for a beam weapon because you are running out of object slots!\n"));
		return -1;
	}
	new_item->objnum = objnum;

	if (new_item->objp != nullptr && Weapons_inherit_parent_collision_group) {
		Objects[objnum].collision_group_id = new_item->objp->collision_group_id;
	}

	// this sets up all info for the first frame the beam fires
	beam_aim(new_item);						// to fill in shot_point, etc.	

	// check to see if its legal to fire at this guy
	if (beam_ok_to_fire(new_item) != 1) {
		beam_delete(new_item);
		mprintf(("Killing beam at initial fire because of illegal targeting!!!\n"));
		return -1;
	}

	// if we're a multiplayer master - send a packet
	if (MULTIPLAYER_MASTER) {
		send_beam_fired_packet(fire_info, &new_item->binfo);
	}

	// start the warmup phase
	beam_start_warmup(new_item);

	//Do particles
	if (wip->b_info.beam_muzzle_effect.isValid()) {
		auto source = particle::ParticleManager::get()->createSource(wip->b_info.beam_muzzle_effect);

		std::unique_ptr<EffectHost> host;
		if (new_item->objp == nullptr) {
			vec3d beam_dir = new_item->last_shot - new_item->last_start;
			matrix orient;
			vm_vector_2_matrix(&orient, &beam_dir);

			host = std::make_unique<EffectHostVector>(new_item->last_start, orient, vmd_zero_vector);
		}
		else if (new_item->subsys == nullptr || new_item->firingpoint < 0) {
			vec3d beam_dir = new_item->last_shot - new_item->last_start;
			matrix orient;
			vm_vector_2_matrix(&orient, &beam_dir);

			vec3d local_pos = new_item->last_start - new_item->objp->pos;
			vm_vec_rotate(&local_pos, &local_pos, &new_item->objp->orient);
			orient = new_item->objp->orient * orient;

			host = std::make_unique<EffectHostObject>(new_item->objp, local_pos, orient);
		}
		else {
			host = std::make_unique<EffectHostTurret>(new_item->objp, new_item->subsys->system_info->turret_gun_sobj, new_item->firingpoint);
		}

		source->setHost(std::move(host));
		source->setTriggerRadius(wip->b_info.beam_muzzle_radius);
		source->finishCreation();
	}

	return objnum;
}

// fire a targeting beam, returns objnum on success. a much much simplified version of a beam weapon
// targeting lasers last _one_ frame. For a continuous stream - they must be created every frame.
// this allows it to work smoothly in multiplayer (detect "trigger down". every frame just create a targeting laser firing straight out of the
// object. this way you get all the advantages of nice rendering and collisions).
// NOTE : only references beam_info_index and shooter
int beam_fire_targeting(fighter_beam_fire_info *fire_info)
{
	beam *new_item;
	weapon_info *wip;
	int objnum;	
	ship *firing_ship;

	// sanity check
	if(fire_info == NULL){
		Int3();
		return -1;
	}

	// if we're out of beams, bail
	if(Beam_count >= MAX_BEAMS){
		return -1;
	}
	
	// make sure the beam_info_index is valid
	Assert((fire_info->beam_info_index >= 0) && (fire_info->beam_info_index < weapon_info_size()) && (Weapon_info[fire_info->beam_info_index].wi_flags[Weapon::Info_Flags::Beam]));
	if((fire_info->beam_info_index < 0) || (fire_info->beam_info_index >= weapon_info_size()) || !(Weapon_info[fire_info->beam_info_index].wi_flags[Weapon::Info_Flags::Beam])){
		return -1;
	}
	wip = &Weapon_info[fire_info->beam_info_index];	

	// make sure a ship is firing this
	Assert((fire_info->shooter->type == OBJ_SHIP) && (fire_info->shooter->instance >= 0) && (fire_info->shooter->instance < MAX_SHIPS));
	if ( (fire_info->shooter->type != OBJ_SHIP) || (fire_info->shooter->instance < 0) || (fire_info->shooter->instance >= MAX_SHIPS) ) {
		return -1;
	}
	firing_ship = &Ships[fire_info->shooter->instance];


	// get a free beam
	new_item = GET_FIRST(&Beam_free_list);
	Assert( new_item != &Beam_free_list );		// shouldn't have the dummy element

	// remove from the free list
	list_remove( &Beam_free_list, new_item );
	
	// insert onto the end of used list
	list_append( &Beam_used_list, new_item );

	// increment counter
	Beam_count++;

	// maybe allocate some extra data based on the beam type
	Assert(wip->b_info.beam_type == BeamType::TARGETING);
	if(wip->b_info.beam_type != BeamType::TARGETING){
		return -1;
	}

	// fill in some values
	new_item->warmup_stamp = fire_info->warmup_stamp;
	new_item->warmdown_stamp = fire_info->warmdown_stamp;
	new_item->weapon_info_index = fire_info->beam_info_index;	
	new_item->objp = fire_info->shooter;
	new_item->sig = fire_info->shooter->signature;
	new_item->subsys = NULL;
	new_item->life_left = fire_info->life_left;	
	new_item->life_total = fire_info->life_total;
	new_item->r_collision_count = 0;
	new_item->f_collision_count = 0;
	new_item->target = NULL;
	new_item->target_subsys = NULL;
	new_item->target_sig = 0;
	new_item->beam_sound_loop        = sound_handle::invalid();
	new_item->type = BeamType::TARGETING;
	new_item->local_fire_postion = fire_info->local_fire_postion;
	new_item->framecount = 0;
	new_item->flags = 0;
	new_item->shot_index = 0;
	new_item->current_width_factor = wip->b_info.beam_initial_width < 0.1f ? 0.1f : wip->b_info.beam_initial_width;
	new_item->team = (char)firing_ship->team;
	new_item->range = wip->b_info.range;
	new_item->damage_threshold = wip->b_info.damage_threshold;

	// beam collision and light width
	if (wip->b_info.beam_width > 0.0f) {
		new_item->beam_collide_width = wip->b_info.beam_width;
		new_item->beam_light_width = wip->b_info.beam_width;
	}
	else {
		float widest = beam_get_widest(new_item);
		new_item->beam_collide_width = wip->collision_radius_override > 0.0f ? wip->collision_radius_override : widest;
		new_item->beam_light_width = widest;
	}

	// targeting type beams are a very special weapon type - binfo has no meaning

	flagset<Object::Object_Flags> initial_flags;
	if (!wip->wi_flags[Weapon::Info_Flags::No_collide])
		initial_flags.set(Object::Object_Flags::Collides);

	// create the associated object
	objnum = obj_create(OBJ_BEAM, OBJ_INDEX(fire_info->shooter), BEAM_INDEX(new_item), &vmd_identity_matrix, &vmd_zero_vector, 1.0f, initial_flags);

	if(objnum < 0){
		beam_delete(new_item);
		nprintf(("General", "obj_create() failed for beam weapon! bah!\n"));
		Int3();
		return -1;
	}
	new_item->objnum = objnum;	

	// this sets up all info for the first frame the beam fires
	beam_aim(new_item);						// to fill in shot_point, etc.		

	if(Beams[Objects[objnum].instance].objnum != objnum){
		Int3();
		return -1;
	}
	
	return objnum;
}

// return an object index of the guy who's firing this beam
int beam_get_parent(const object *bm)
{
	beam *b;

	// get a handle to the beam
	Assert(bm->type == OBJ_BEAM);
	Assert(bm->instance >= 0);	
	if(bm->type != OBJ_BEAM){
		return -1;
	}
	if(bm->instance < 0){
		return -1;
	}
	b = &Beams[bm->instance];

	if(b->objp == NULL){
		return -1;
	}

	// if the object handle is invalid
	if(b->objp->signature != b->sig){
		return -1;
	}

	// return the handle
	return OBJ_INDEX(b->objp);
}

// return weapon_info_index of beam
int beam_get_weapon_info_index(const object *bm)
{
	Assert(bm->type == OBJ_BEAM);
	if (bm->type != OBJ_BEAM) {
		return -1;
	}

	Assert(bm->instance >= 0 && bm->instance < MAX_BEAMS);
	if (bm->instance < 0) {
		return -1;
	}
    //make sure it's returning a valid info index
	Assert((Beams[bm->instance].weapon_info_index > -1) && (Beams[bm->instance].weapon_info_index < weapon_info_size()));

	// return weapon_info_index
	return Beams[bm->instance].weapon_info_index;
}



// given a beam object, get the # of collisions which happened during the last collision check (typically, last frame)
int beam_get_num_collisions(int objnum)
{	
	// sanity checks
	if((objnum < 0) || (objnum >= MAX_OBJECTS)){
		Int3();
		return -1;
	}
	if((Objects[objnum].instance < 0) || (Objects[objnum].instance >= MAX_BEAMS)){
		Int3();
		return -1;
	}
	if(Beams[Objects[objnum].instance].objnum != objnum){
		Int3();
		return -1;
	}

	if(Beams[Objects[objnum].instance].objnum < 0){
		Int3();
		return -1;
	}

	// return the # of recent collisions
	return Beams[Objects[objnum].instance].r_collision_count;
}

// stuff collision info, returns 1 on success
int beam_get_collision(int objnum, int num, int *collision_objnum, mc_info **cinfo)
{
	// sanity checks
	if((objnum < 0) || (objnum >= MAX_OBJECTS)){
		Int3();
		return 0;
	}
	if((Objects[objnum].instance < 0) || (Objects[objnum].instance >= MAX_BEAMS)){
		Int3();
		return 0;
	}
	if((Beams[Objects[objnum].instance].objnum != objnum) || (Beams[Objects[objnum].instance].objnum < 0)){
		Int3();
		return 0;
	}
	if(num >= Beams[Objects[objnum].instance].r_collision_count){
		Int3();
		return 0;
	}

	// return - success
	*cinfo = &Beams[Objects[objnum].instance].r_collisions[num].cinfo;
	*collision_objnum = Beams[Objects[objnum].instance].r_collisions[num].c_objnum;
	return 1;
}

// pause all looping beam sounds
void beam_pause_sounds()
{
	beam *moveup = NULL;

	// set all beam volumes to 0	
	moveup = GET_FIRST(&Beam_used_list);
	if(moveup == NULL){
		return;
	}
	while(moveup != END_OF_LIST(&Beam_used_list)){				
		// set the volume to 0, if he has a looping beam sound
		if (moveup->beam_sound_loop.isValid()) {
			snd_set_volume(moveup->beam_sound_loop, 0.0f);
		}

		// next beam
		moveup = GET_NEXT(moveup);
	}
}

// unpause looping beam sounds
void beam_unpause_sounds()
{
	beam *moveup = NULL;

	// recalc all beam sounds
	moveup = GET_FIRST(&Beam_used_list);
	if(moveup == NULL){
		return;
	}
	while(moveup != END_OF_LIST(&Beam_used_list)){
		if (Cmdline_no_3d_sound) {
			beam_recalc_sounds(moveup);
		} else {
			if (moveup->beam_sound_loop.isValid()) {
				snd_set_volume(moveup->beam_sound_loop, 1.0f);
			}
		}

		// next beam
		moveup = GET_NEXT(moveup);
	}
}

void beam_get_global_turret_gun_info(const object *objp, const ship_subsys *ssp, vec3d *gpos, bool avg_origin, vec3d *gvec, bool use_angles, const vec3d *targetp, bool fighter_beam)
{
	if (fighter_beam)
	{
		ship_get_global_turret_gun_info(objp, ssp, gpos, avg_origin, nullptr, use_angles, targetp);
		*gvec = objp->orient.vec.fvec;
	}
	else
		ship_get_global_turret_gun_info(objp, ssp, gpos, avg_origin, gvec, use_angles, targetp);
}

// -----------------------------===========================------------------------------
// BEAM MOVEMENT FUNCTIONS
// -----------------------------===========================------------------------------

// move a direct fire type beam weapon
void beam_type_direct_fire_move(beam *b)
{
	vec3d dir;

	// LEAVE THIS HERE OTHERWISE MUZZLE GLOWS DRAW INCORRECTLY WHEN WARMING UP OR DOWN
	// get the "originating point" of the beam for this frame. essentially bashes last_start
	if (b->subsys != NULL)
		beam_get_global_turret_gun_info(b->objp, b->subsys, &b->last_start, false, nullptr, true, nullptr, (b->flags & BF_IS_FIGHTER_BEAM) != 0);

	// if the "warming up" timestamp has not expired
	if((b->warmup_stamp != -1) || (b->warmdown_stamp != -1)){
		return;
	}

	// put the "last_shot" point arbitrarily far away
	vm_vec_sub(&dir, &b->last_shot, &b->last_start);
	vm_vec_normalize_quick(&dir);
	vm_vec_scale_add(&b->last_shot, &b->last_start, &dir, b->range);
	Assert(is_valid_vec(&b->last_shot));
}

// move a slashing type beam weapon
#define BEAM_T(b)						((b->life_total - b->life_left) / b->life_total)
void beam_type_slashing_move(beam *b)
{		
	vec3d actual_dir;
	float dot_save;	

	// LEAVE THIS HERE OTHERWISE MUZZLE GLOWS DRAW INCORRECTLY WHEN WARMING UP OR DOWN
	// get the "originating point" of the beam for this frame. essentially bashes last_start
	if (b->subsys != NULL)
		beam_get_global_turret_gun_info(b->objp, b->subsys, &b->last_start, false, nullptr, true, nullptr, (b->flags & BF_IS_FIGHTER_BEAM) != 0);

	// if the "warming up" timestamp has not expired
	if((b->warmup_stamp != -1) || (b->warmdown_stamp != -1)){
		return;
	}

	// see if we are sharing a firing direction with another beam that is not this beam
	// (beam_aim took care of most of the bookkeeping)
	if ((b->subsys != nullptr) && (b->subsys->shared_fire_direction_beam_objnum >= 0) && (b->subsys->shared_fire_direction_beam_objnum != b->objnum)) {
		auto shared_candidate_objp = &Objects[b->subsys->shared_fire_direction_beam_objnum];
		auto shared_fire_reference = &Beams[shared_candidate_objp->instance];

		// use the same vector, then we're done
		vec3d beam_vector;
		vm_vec_sub(&beam_vector, &shared_fire_reference->last_shot, &shared_fire_reference->last_start);
		vm_vec_add(&b->last_shot, &b->last_start, &beam_vector);
		return;
	}

	// if the two direction vectors are _really_ close together, just use the original direction
	dot_save = vm_vec_dot(&b->binfo.dir_a, &b->binfo.dir_b);
	if((double)dot_save >= 0.999999999){
		actual_dir = b->binfo.dir_a;
	} 
	// otherwise move towards the dir we calculated when firing this beam
	else {
		vm_vec_interp_constant(&actual_dir, &b->binfo.dir_a, &b->binfo.dir_b, BEAM_T(b));
	}

	// now recalculate shot_point to be shooting through our new point
	vm_vec_scale_add(&b->last_shot, &b->last_start, &actual_dir, b->range);
	bool is_valid = is_valid_vec(&b->last_shot);
	Assert(is_valid);
	if(!is_valid){
		actual_dir = b->binfo.dir_a;
		vm_vec_scale_add(&b->last_shot, &b->last_start, &actual_dir, b->range);
	}
}

// targeting type beams functions
void beam_type_targeting_move(beam *b)
{	
	// If floating and targeting, synthesize orientation from known points
	// because the object could be validly nullptr as when fired in the lab
	if ((b->flags & BF_FLOATING_BEAM) && (b->flags & BF_TARGETING_COORDS)) {
		// Compute forward vector from start to target
		vec3d fvec;
		vm_vec_sub(&fvec, &b->target_pos1, &b->last_start);
		vm_vec_normalize_safe(&fvec);

		// Final beam endpoint
		vm_vec_scale_add(&b->last_shot, &b->last_start, &fvec, b->range);
	} else {
		Assertion(b->objp != nullptr, "Targeting beam does not have a valid parent object!");
		Assertion(b->objp->instance >= 0, "Targeting beam parent object instance is invalid!");

		// Standard case: beam fired from a real object
		// start point
		vm_vec_unrotate(&b->last_start, &b->local_fire_postion, &b->objp->orient);
		vm_vec_add2(&b->last_start, &b->objp->pos);
		// end point
		vm_vec_scale_add(&b->last_shot, &b->last_start, &b->objp->orient.vec.fvec, b->range);
	}
}

// antifighter type beam functions
void beam_type_antifighter_move(beam *b)
{
	int shot_index, fire_wait;
	vec3d dir;

	// LEAVE THIS HERE OTHERWISE MUZZLE GLOWS DRAW INCORRECTLY WHEN WARMING UP OR DOWN
	// get the "originating point" of the beam for this frame. essentially bashes last_start
	if (b->subsys != NULL)
		beam_get_global_turret_gun_info(b->objp, b->subsys, &b->last_start, false, nullptr, true, nullptr, (b->flags & BF_IS_FIGHTER_BEAM) != 0);

	// if the "warming up" timestamp has not expired
	if((b->warmup_stamp != -1) || (b->warmdown_stamp != -1)){
		return;
	}	

	// determine what stage of the beam we're in
	beam_type_antifighter_get_status(b, &shot_index, &fire_wait);

	// if we've changed shot index
	if(shot_index != b->shot_index){
		// set the new index
		b->shot_index = shot_index;

		// re-aim
		beam_aim(b);
	}

	// if we're in the fire wait stage
	b->flags &= ~BF_SAFETY;
	if(fire_wait){
		b->flags |= BF_SAFETY;
		beam_set_state(&Weapon_info[b->weapon_info_index], b, WeaponState::PAUSED);
	} else {
		beam_set_state(&Weapon_info[b->weapon_info_index], b, WeaponState::FIRING);
	}

	// put the "last_shot" point arbitrarily far away
	vm_vec_sub(&dir, &b->last_shot, &b->last_start);
	vm_vec_normalize_quick(&dir);
	vm_vec_scale_add(&b->last_shot, &b->last_start, &dir, b->range);
	Assert(is_valid_vec(&b->last_shot));
}
void beam_type_antifighter_get_status(beam *b, int *shot_index, int *fire_wait)
{	
	float shot_time = b->life_total / (float)b->binfo.shot_count;
	float beam_time = b->life_total - b->life_left;

	// determine what "shot" we're on	
	*shot_index = (int)(beam_time / shot_time);
	
	if(*shot_index >= b->binfo.shot_count){
		nprintf(("Beam","Shot of antifighter beam had bad shot_index value\n"));
		*shot_index = b->binfo.shot_count - 1;
	}	

	// determine if its the firing or waiting section of the shot (fire happens first, THEN wait)	
	*fire_wait = 0;
	if(beam_time > ((shot_time * (*shot_index)) + (shot_time * 0.5f))){
		*fire_wait = 1;
	} 	
}

// down-the-normal type beam functions
void beam_type_normal_move(beam *b)
{
	vec3d turret_norm;

	if (b->subsys == nullptr) {
		// If we're a floating beam and targeting coords then we don't have a subsystem to get a normal from, so we'll calculate it.
		if ((b->flags & BF_FLOATING_BEAM) && (b->flags & BF_TARGETING_COORDS)) {
			// Build normal from target_pos1 and last_start
			vm_vec_sub(&turret_norm, &b->target_pos1, &b->last_start);
			vm_vec_normalize_safe(&turret_norm);
		}
		// Otherwise, there's nothing to calculate here.
		else {
			return;
		}
	} else {
		// LEAVE THIS HERE OTHERWISE MUZZLE GLOWS DRAW INCORRECTLY WHEN WARMING UP OR DOWN
		// get the "originating point" of the beam for this frame. essentially bashes last_start
		beam_get_global_turret_gun_info(b->objp, b->subsys, &b->last_start, false, &turret_norm, true, nullptr, (b->flags & BF_IS_FIGHTER_BEAM) != 0);
	}

	// if the "warming up" timestamp has not expired
	if((b->warmup_stamp != -1) || (b->warmdown_stamp != -1)){
		return;
	}	

	// put the "last_shot" point arbitrarily far away
	vm_vec_scale_add(&b->last_shot, &b->last_start, &turret_norm, b->range);	
	Assert(is_valid_vec(&b->last_shot));
}

void beam_type_omni_move(beam* b)
{
	// keep this updated even if still warming up 
	if (b->flags & BF_IS_FIGHTER_BEAM) {
		vm_vec_unrotate(&b->last_start, &b->local_fire_postion, &b->objp->orient);
		vm_vec_add2(&b->last_start, &b->objp->pos);

		// compute the change in orientation the fighter went through
		matrix inv_new_orient, transform_matrix;
		vm_copy_transpose(&inv_new_orient, &b->objp->orient);
		vm_matrix_x_matrix(&transform_matrix, &b->objp->last_orient, &inv_new_orient);
		// and put the beam vectors through the same change
		vec3d old_dirA = b->binfo.dir_a;
		vec3d old_dirB = b->binfo.dir_b;
		vec3d old_rot_axis = b->binfo.rot_axis;
		vm_vec_rotate(&b->binfo.dir_a, &old_dirA, &transform_matrix);
		vm_vec_rotate(&b->binfo.dir_b, &old_dirB, &transform_matrix);
		vm_vec_rotate(&b->binfo.rot_axis, &old_rot_axis, &transform_matrix);
	}
	else if (b->subsys != nullptr) {
		beam_get_global_turret_gun_info(b->objp, b->subsys, &b->last_start, false, nullptr, true, nullptr, false);
	}

	// if the "warming up" timestamp has not expired
	if ((b->warmup_stamp != -1) || (b->warmdown_stamp != -1)) {
		return;
	}

	// see if we are sharing a firing direction with another beam that is not this beam
	// (beam_aim took care of most of the bookkeeping)
	if ((b->subsys != nullptr) && (b->subsys->shared_fire_direction_beam_objnum >= 0) && (b->subsys->shared_fire_direction_beam_objnum != b->objnum)) {
		auto shared_candidate_objp = &Objects[b->subsys->shared_fire_direction_beam_objnum];
		auto shared_fire_reference = &Beams[shared_candidate_objp->instance];

		// use the same vector, then we're done
		vec3d beam_vector;
		vm_vec_sub(&beam_vector, &shared_fire_reference->last_shot, &shared_fire_reference->last_start);
		vm_vec_add(&b->last_shot, &b->last_start, &beam_vector);
		return;
	}

	vec3d newdir_a = b->binfo.dir_a;
	vec3d newdir_b = b->binfo.dir_b;
	vec3d zero_vec = vmd_zero_vector;
	vec3d actual_dir;
	bool no_sweep = vm_vec_dot(&b->binfo.dir_a, &b->binfo.dir_b) > 0.9999f;

	float rotation_amount;
	if (Weapon_info[b->weapon_info_index].b_info.t5info.rot_curve_idx >= 0) {
		rotation_amount = Curves[Weapon_info[b->weapon_info_index].b_info.t5info.rot_curve_idx].GetValue(BEAM_T(b)) * PI2;
	} else {
		rotation_amount = (b->life_total - b->life_left) * b->type5_rot_speed;
	}

	if (b->rotates) {
		vm_rot_point_around_line(&newdir_a, &b->binfo.dir_a, rotation_amount, &zero_vec, &b->binfo.rot_axis); 
		if (no_sweep)
			actual_dir = newdir_a;
		else 
			vm_rot_point_around_line(&newdir_b, &b->binfo.dir_b, rotation_amount, &zero_vec, &b->binfo.rot_axis);
	}

	if (no_sweep)
		actual_dir = newdir_a;
	else {
		float slash_completion;
		if (Weapon_info[b->weapon_info_index].b_info.t5info.slash_pos_curve_idx >= 0) {
			slash_completion = Curves[Weapon_info[b->weapon_info_index].b_info.t5info.slash_pos_curve_idx].GetValue(BEAM_T(b));
		} else {
			slash_completion = BEAM_T(b);
		}
		vm_vec_interp_constant(&actual_dir, &newdir_a, &newdir_b, slash_completion);
	}

	// now recalculate shot_point to be shooting through our new point
	vm_vec_scale_add(&b->last_shot, &b->last_start, &actual_dir, b->range);
}

// pre-move (before collision checking - but AFTER ALL OTHER OBJECTS HAVE BEEN MOVED)
void beam_move_all_pre()
{	
	beam *b;	
	beam *moveup;		

	// zero lights for this frame yet
	Beam_light_count = 0;

	// traverse through all active beams
	moveup = GET_FIRST(&Beam_used_list);
	while (moveup != END_OF_LIST(&Beam_used_list)) {				
		// get the beam
		b = moveup;

		// check if parent object has died, if so then delete beam
		if (b->objp != NULL && b->objp->type == OBJ_NONE) {
			// set next beam
			moveup = GET_NEXT(moveup);
			// delete current beam
			beam_delete(b);

			continue;
		}

		// unset collision info
		b->f_collision_count = 0;

		if ( !physics_paused ) {
			// make sure to check that firingpoint is still properly set
			int temp = -1;
			if (b->subsys != NULL) {
				temp = b->subsys->turret_next_fire_pos;

				if (!(b->flags & BF_IS_FIGHTER_BEAM))
					b->subsys->turret_next_fire_pos = b->firingpoint;
			}

			// move the beam
			switch (b->type)
			{
				// direct fire type beam weapons don't move
				case BeamType::DIRECT_FIRE :			
					beam_type_direct_fire_move(b);
					break;

				// slashing type beam weapons move across the target somewhat randomly
				case BeamType::SLASHING:
					beam_type_slashing_move(b);
					break;				

				// targeting type beam weapons are attached to a fighter - pointing forward
				case BeamType::TARGETING:
					beam_type_targeting_move(b);
					break;

				// antifighter type
				case BeamType::ANTIFIGHTER:
					beam_type_antifighter_move(b);
					break;

				// down-the-normal type beams
				case BeamType::NORMAL_FIRE:
					beam_type_normal_move(b);
					break;

				case BeamType::OMNI:
					beam_type_omni_move(b);
					break;

				// illegal beam type
				default :
					Int3();
			}
			if (b->subsys != NULL) {
				b->subsys->turret_next_fire_pos = temp;
			}
		}

		// next
		moveup = GET_NEXT(moveup);
	}
}

// post-collision time processing for beams
void beam_move_all_post()
{
	beam *moveup;	
	beam *next_one;	
	int bf_status;	
	beam_weapon_info *bwi;

	// traverse through all active beams
	moveup = GET_FIRST(&Beam_used_list);
	while(moveup != END_OF_LIST(&Beam_used_list)){				
        bwi = &Weapon_info[moveup->weapon_info_index].b_info;

        // check the status of the beam
		bf_status = beam_ok_to_fire(moveup);

		// if we're warming up
		if(moveup->warmup_stamp != -1){
			next_one = GET_NEXT(moveup);			

			// should we be stopping?
			if(bf_status < 0){
				beam_delete(moveup);
			} else {
				if (moveup->objp != NULL) {
					// add a muzzle light for the shooter
					beam_add_light(moveup, OBJ_INDEX(moveup->objp), 0, NULL);
				}

				// if the warming up timestamp has expired, start firing
				if(timestamp_elapsed(moveup->warmup_stamp)){							
					// start firing
					if(!beam_start_firing(moveup)){
						beam_delete(moveup);												
					} 			
				} 
			}

			// next
			moveup = next_one;
			continue;
		} 
		// if we're warming down
		else if(moveup->warmdown_stamp != -1){			
			next_one = GET_NEXT(moveup);

			// should we be stopping?
			if(bf_status < 0){
				beam_delete(moveup);
			} else {
				if (moveup->objp != NULL) {
					// add a muzzle light for the shooter
					beam_add_light(moveup, OBJ_INDEX(moveup->objp), 0, NULL);
				}

				// if we're done warming down, the beam is finished
				if(timestamp_elapsed(moveup->warmdown_stamp)){
					beam_delete(moveup);				
				}			
			}

			// next
			moveup = next_one;
			continue;
		}
		// otherwise, we're firing away.........		

		if (moveup->objp != NULL) {
			// add a muzzle light for the shooter
			beam_add_light(moveup, OBJ_INDEX(moveup->objp), 0, NULL);
		}

		// subtract out the life left for the beam
		if(!physics_paused){
			moveup->life_left -= flFrametime;						
		}

		// if we're past the shrink point, start shrinking the beam
		if(moveup->life_left <= (moveup->life_total * bwi->beam_shrink_factor)){
			moveup->flags |= BF_SHRINK;
		}

		// if we're shrinking the beam
		if(moveup->flags & BF_SHRINK){
			moveup->current_width_factor -= bwi->beam_shrink_pct * flFrametime;
			if(moveup->current_width_factor < 0.1f){
				moveup->current_width_factor = 0.1f;
			}
		}

		// if we're past the grow point and haven't already finished growing, start growing the beam
		if (moveup->life_left <= (moveup->life_total * bwi->beam_grow_factor) && !(moveup->flags & BF_FINISHED_GROWING)) {
			moveup->flags |= BF_GROW;
		}

		// if we're growing the beam (but not shrinking yet)
		if ((moveup->flags & BF_GROW) && !(moveup->flags & BF_SHRINK)) {
			moveup->current_width_factor += bwi->beam_grow_pct * flFrametime;
			if (moveup->current_width_factor > 1.0f) { // We've finished growing!
				moveup->current_width_factor = 1.0f;
				moveup->flags &= ~BF_GROW;
				moveup->flags |= BF_FINISHED_GROWING;
			}
		}

		// add tube light for the beam
		if (moveup->objp != nullptr) {
			if (moveup->type == BeamType::ANTIFIGHTER) {

				//we only use the second variable but we need two pointers to pass.
				int beam_index, beam_waiting = 0;

				beam_type_antifighter_get_status(moveup, &beam_index, &beam_waiting);

				//create a tube light only if we are not waiting between shots
				if (beam_waiting == 0) {
					beam_add_light(moveup, OBJ_INDEX(moveup->objp), 1, nullptr);
				}
			}
			else
			{
				beam_add_light(moveup, OBJ_INDEX(moveup->objp), 1, nullptr);
			}
		}

		// deal with ammo/energy for fighter beams
		bool multi_ai = MULTIPLAYER_CLIENT && (moveup->objp != Player_obj);
		bool cheating_player = Weapon_energy_cheat && (moveup->objp == Player_obj);

		if (moveup->flags & BF_IS_FIGHTER_BEAM && !multi_ai && !cheating_player) {
			ship* shipp = &Ships[moveup->objp->instance];
			weapon_info* wip = &Weapon_info[moveup->weapon_info_index];

			shipp->weapon_energy -= wip->energy_consumed * flFrametime;

			if (shipp->weapon_energy < 0.0f)
				shipp->weapon_energy = 0.0f;
		}

		// stop shooting?
		if(bf_status <= 0){
			next_one = GET_NEXT(moveup);

			// if beam should abruptly stop
			if(bf_status == -1){
				beam_delete(moveup);							
			}
			// if the beam should just power down
			else {			
				beam_start_warmdown(moveup);
			}
			
			// next beam
			moveup = next_one;
			continue;
		}				

		// increment framecount
		moveup->framecount++;
		// targetin type weapons live for one frame only
		// done firing, so go into the warmdown phase
		{
			if((moveup->life_left <= 0.0f) &&
               (moveup->warmdown_stamp == -1) &&
               (moveup->framecount > 1))
            {
				beam_start_warmdown(moveup);
				
				moveup = GET_NEXT(moveup);
				continue;
			}				
		}

		// handle any collisions which occured collision (will take care of applying damage to all objects which got hit)
		beam_handle_collisions(moveup);						

		// recalculate beam sounds
		beam_recalc_sounds(moveup);

		// next item
		moveup = GET_NEXT(moveup);
	}

	// apply all beam lighting
	beam_apply_lighting();
}

// -----------------------------===========================------------------------------
// BEAM RENDERING FUNCTIONS
// -----------------------------===========================------------------------------

// render a beam weapon
#define STUFF_VERTICES()	do {\
	verts[0]->texture_position.u = 0.0f;\
	verts[0]->texture_position.v = 0.0f;\
	verts[1]->texture_position.u = 1.0f;\
	verts[1]->texture_position.v = 0.0f;\
	verts[2]->texture_position.u = 1.0f;\
	verts[2]->texture_position.v = 1.0f;\
	verts[3]->texture_position.u = 0.0f;\
	verts[3]->texture_position.v = 1.0f;\
} while(false);

#define P_VERTICES()		do {\
	for(idx=0; idx<4; idx++){\
		g3_project_vertex(verts[idx]);\
	}\
} while(false);

void beam_render(beam *b, float u_offset)
{	
	int idx, s_idx;
	vertex h1[4];				// halves of a beam section
	vertex *verts[4] = { &h1[0], &h1[1], &h1[2], &h1[3] };
	vec3d fvec, top1, bottom1, top2, bottom2;
	float scale;
	float u_scale;	// beam tileing -Bobboau
	beam_weapon_section_info *bwsi;
	beam_weapon_info *bwi;
	weapon_info *wip;

	memset( h1, 0, sizeof(vertex) * 4 );

	// bogus weapon info index
	if ( (b == NULL) || (b->weapon_info_index < 0) )
		return;

	// if the beam start and endpoints are the same
	if ( vm_vec_same(&b->last_start, &b->last_shot) )
		return;

	// get beam direction
	vm_vec_sub(&fvec, &b->last_shot, &b->last_start);
	vm_vec_normalize_quick(&fvec);		

	// turn off backface culling
	//int cull = gr_set_cull(0);


	wip = &Weapon_info[b->weapon_info_index];
	bwi = &wip->b_info;

	// if this beam tracks its own u_offset, use that instead
	if (bwi->flags[Weapon::Beam_Info_Flags::Track_own_texture_tiling]) {
		u_offset = b->u_offset_local;			// the parameter is passed by value so this won't interfere with the u_offset in the calling function
		b->u_offset_local += flFrametime;		// increment *after* we grab the offset so that the first frame will always be at offset=0
	}

	float width_mult = wip->beam_curves.get_output(weapon_info::BeamCurveOutputs::BEAM_WIDTH_MULT, *b, &b->modular_curves_instance);
	float alpha_mult = wip->beam_curves.get_output(weapon_info::BeamCurveOutputs::BEAM_ALPHA_MULT, *b, &b->modular_curves_instance);
	bool anim_has_curve = wip->beam_curves.has_curve(weapon_info::BeamCurveOutputs::BEAM_ANIM_STATE);
	float anim_state = wip->beam_curves.get_output(weapon_info::BeamCurveOutputs::BEAM_ANIM_STATE, *b, &b->modular_curves_instance);

	float length = vm_vec_dist(&b->last_start, &b->last_shot);					// beam tileing -Bobboau
	float per = 1.0f;
	if (bwi->range)
		per -= length / bwi->range;

	float end_alphaf = 255.0f * alpha_mult * per;
	float start_alphaf = 255.0f * alpha_mult;

	// just to be safe
	CLAMP(end_alphaf, 0.0f, 255.0f);
	CLAMP(start_alphaf, 0.0f, 255.0f);

	ubyte end_alpha = static_cast<ubyte>(end_alphaf);
	ubyte start_alpha = static_cast<ubyte>(start_alphaf);

	// draw all sections	
	for (s_idx = 0; s_idx < bwi->beam_num_sections; s_idx++) {
		bwsi = &bwi->sections[s_idx];

		if ( (bwsi->texture.first_frame < 0) || (bwsi->width <= 0.0f) )
			continue;

		// calculate the beam points
		scale = frand_range(1.0f - bwsi->flicker, 1.0f + bwsi->flicker) * width_mult;
		beam_calc_facing_pts(&top1, &bottom1, &fvec, &b->last_start, bwsi->width * scale * b->current_width_factor, bwsi->z_add);	
		beam_calc_facing_pts(&top2, &bottom2, &fvec, &b->last_shot, bwsi->width * scale * scale * b->current_width_factor, bwsi->z_add);

		g3_transfer_vertex(verts[0], &bottom1); 
		g3_transfer_vertex(verts[1], &bottom2);	
		g3_transfer_vertex(verts[2], &top2); 
		g3_transfer_vertex(verts[3], &top1);

		P_VERTICES();						
		STUFF_VERTICES();		// stuff the beam with creamy goodness (texture coords)

		if (bwsi->tile_type == 1)
			u_scale = length / (bwsi->width * 0.5f) / bwsi->tile_factor;	// beam tileing, might make a tileing factor in beam index later -Bobboau
		else
			u_scale = bwsi->tile_factor;

		verts[1]->texture_position.u = (u_scale + (u_offset * bwsi->translation));	// beam tileing -Bobboau
		verts[2]->texture_position.u = (u_scale + (u_offset * bwsi->translation));	// beam tileing -Bobboau
		verts[3]->texture_position.u = (0 + (u_offset * bwsi->translation));
		verts[0]->texture_position.u = (0 + (u_offset * bwsi->translation));


		verts[1]->r = end_alpha;
		verts[2]->r = end_alpha;
		verts[1]->g = end_alpha;
		verts[2]->g = end_alpha;
		verts[1]->b = end_alpha;
		verts[2]->b = end_alpha;
		verts[1]->a = end_alpha;
		verts[2]->a = end_alpha;

		verts[0]->r = start_alpha;
		verts[3]->r = start_alpha;
		verts[0]->g = start_alpha;
		verts[3]->g = start_alpha;
		verts[0]->b = start_alpha;
		verts[3]->b = start_alpha;
		verts[0]->a = start_alpha;
		verts[3]->a = start_alpha;

		// set the right texture with additive alpha, and draw the poly
		int framenum = 0;

		if (bwsi->texture.num_frames > 1) {
			if (anim_has_curve) {
				framenum = fl2i(i2fl(bwsi->texture.num_frames - 1) * anim_state);
			} else {
				b->beam_section_frame[s_idx] += flFrametime;
				framenum = bm_get_anim_frame(bwsi->texture.first_frame, b->beam_section_frame[s_idx], bwsi->texture.total_time, true);
			}
		}

		CLAMP(framenum, 0, bwsi->texture.num_frames - 1);

		float fade = 0.9999f;

		if (Neb_affects_beams) {
			vec3d nearest;
			int result = vm_vec_dist_to_line(&Eye_position, &b->last_start, &b->last_shot, &nearest, nullptr);
			if (result == 1)
				nearest = b->last_shot;
			if (result == -1)
				nearest = b->last_start;

			nebula_handle_alpha(fade, &nearest, 
				Neb2_fog_visibility_beam_const + (b->beam_light_width * Neb2_fog_visibility_beam_scaled_factor));
		}

		material material_params;
		material_set_unlit_emissive(&material_params, bwsi->texture.first_frame + framenum, fade, 2.0f);
		g3_render_primitives_colored_textured(&material_params, h1, 4, PRIM_TYPE_TRIFAN, false);
	}

	// turn backface culling back on
	//gr_set_cull(cull);
}

static float get_muzzle_glow_alpha(beam* b)
{
	float dist;
	float alpha = 0.8f;

	const float inner_radius = 15.0f;
	const float magic_num = 2.75f;

	// determine what alpha to draw this bitmap with
	// higher alpha the closer the bitmap gets to the eye
	dist = vm_vec_dist_quick(&Eye_position, &b->last_start);

	// if the point is inside the inner radius, alpha is based on distance to the player's eye,
	// becoming more transparent as it gets close
	if (dist <= inner_radius) {
		// alpha per meter between the magic # and the inner radius
		alpha /= (inner_radius - magic_num);

		// above value times the # of meters away we are
		alpha *= (dist - magic_num);
		if (alpha < 0.005f)
			return 0.0f;
	}

	if (Neb_affects_beams) {
		nebula_handle_alpha(alpha, &b->last_start, 
			Neb2_fog_visibility_beam_const + (b->beam_light_width * Neb2_fog_visibility_beam_scaled_factor));
	}

	return alpha;
}

// render the muzzle glow for a beam weapon
void beam_render_muzzle_glow(beam *b)
{
	vertex pt;
	weapon_info *wip = &Weapon_info[b->weapon_info_index];
	beam_weapon_info *bwi = &wip->b_info;
	float rad, pct, rand_val;
	pt.flags = 0;    // avoid potential read of uninit var

	// if we don't have a glow bitmap
	if (bwi->beam_glow.first_frame < 0)
		return;

	// don't show the muzzle glow for players in the cockpit unless show_ship_model is on, provided Render_player_mflash isn't on
	bool in_cockpit_view = (Viewer_mode & (VM_EXTERNAL | VM_CHASE | VM_OTHER_SHIP | VM_WARP_CHASE)) == 0;
	bool player_show_ship_model = (
		b->objp != nullptr // Can validly be nullptr for floating beams!
		&& b->objp == Player_obj  
		&& Ship_info[Ships[b->objp->instance].ship_info_index].flags[Ship::Info_Flags::Show_ship_model] 
		&& (!Show_ship_only_if_cockpits_enabled || Cockpit_active));
	if ((b->flags & BF_IS_FIGHTER_BEAM) && (b->objp == Player_obj && !Render_player_mflash && in_cockpit_view && !player_show_ship_model)) {
		return;
	}

	bool radius_has_curve = wip->beam_curves.has_curve(weapon_info::BeamCurveOutputs::GLOW_RADIUS_MULT);
	float radius_mult = wip->beam_curves.get_output(weapon_info::BeamCurveOutputs::GLOW_RADIUS_MULT, *b, &b->modular_curves_instance);
	bool alpha_has_curve = wip->beam_curves.has_curve(weapon_info::BeamCurveOutputs::GLOW_ALPHA_MULT);
	float alpha_mult = wip->beam_curves.get_output(weapon_info::BeamCurveOutputs::GLOW_ALPHA_MULT, *b, &b->modular_curves_instance);
	bool anim_has_curve = wip->beam_curves.has_curve(weapon_info::BeamCurveOutputs::GLOW_ANIM_STATE);
	float anim_state = wip->beam_curves.get_output(weapon_info::BeamCurveOutputs::GLOW_ANIM_STATE, *b, &b->modular_curves_instance);

	// if the beam is warming up, scale the glow
	if (b->warmup_stamp != -1) {		
		// get warmup pct
		pct = BEAM_WARMUP_PCT(b);
		rand_val = 1.0f;
	}
	// if the beam is warming down
	else if (b->warmdown_stamp != -1)
	{
		// get warmup pct
		pct = 1.0f - BEAM_WARMDOWN_PCT(b);
		rand_val = 1.0f;
	} 
	// otherwise the beam is really firing
	else {
		pct = 1.0f;
		rand_val = frand_range(0.90f, 1.0f);
	}

	if (radius_has_curve) {
		rad = wip->b_info.beam_muzzle_radius * radius_mult;
	} else {
		rad = wip->b_info.beam_muzzle_radius * pct * rand_val;
	}

	// don't bother trying to draw if there is no radius
	if (rad <= 0.0f)
		return;

	float alpha;
	if (alpha_has_curve) {
		alpha = alpha_mult;
	} else {
		alpha = get_muzzle_glow_alpha(b);
	}

	if (alpha <= 0.0f)
		return;

	alpha = MIN(alpha, 1.f);

	if (bwi->directional_glow == true){
		vertex h1[4];
		vertex *verts[4] = { &h1[0], &h1[1], &h1[2], &h1[3] };
		vec3d fvec, top1, top2, bottom1, bottom2, sub1, sub2, start, end;
		int idx;
		float g_length = bwi->glow_length * pct * rand_val;

		vm_vec_sub(&fvec, &b->last_shot, &b->last_start);
		vm_vec_normalize_quick(&fvec);
		
		/* (DahBlount)
		If the glow_length is less than the diameter of the muzzle glow
		we need to account for that by placing the start and end distances
		such that the glow is centered on the firing point of the turret.
		There was actually some oversight here when developing the directional glow feature
		and any refactoring of it will require some more complex parameters for glow placement.
		*/
		if (bwi->glow_length >= 2.0f*rad) {
			vm_vec_copy_scale(&sub1, &fvec, rad);
			vm_vec_sub(&start, &b->last_start, &sub1);
			vm_vec_copy_scale(&sub2, &fvec, g_length);
			vm_vec_add(&end, &start, &sub2);
		} else {
			vm_vec_copy_scale(&sub1, &fvec, 0.5f*g_length);
			vm_vec_sub(&start, &b->last_start, &sub1);
			vm_vec_add(&end, &b->last_start, &sub1);
		}

		beam_calc_facing_pts(&top1, &bottom1, &fvec, &start, rad, 1.0f);
		beam_calc_facing_pts(&top2, &bottom2, &fvec, &end, rad, 1.0f);

		g3_transfer_vertex(verts[0], &bottom1);
		g3_transfer_vertex(verts[1], &bottom2);
		g3_transfer_vertex(verts[2], &top2);
		g3_transfer_vertex(verts[3], &top1);

		P_VERTICES();
		STUFF_VERTICES();

		verts[0]->r = 255;
		verts[1]->r = 255;
		verts[2]->r = 255;
		verts[3]->r = 255;
		verts[0]->g = 255;
		verts[1]->g = 255;
		verts[2]->g = 255;
		verts[3]->g = 255;
		verts[0]->b = 255;
		verts[1]->b = 255;
		verts[2]->b = 255;
		verts[3]->b = 255;
		verts[0]->a = 255;
		verts[1]->a = 255;
		verts[2]->a = 255;
		verts[3]->a = 255;

		int framenum = 0;

		if ( bwi->beam_glow.num_frames > 1 ) {
			if (anim_has_curve) {
				framenum = fl2i(i2fl(bwi->beam_glow.num_frames - 1) * anim_state);
			} else {
				b->beam_glow_frame += flFrametime;
				framenum = bm_get_anim_frame(bwi->beam_glow.first_frame, b->beam_glow_frame, bwi->beam_glow.total_time, true);
			}
		}

		CLAMP(framenum, 0, bwi->beam_glow.num_frames - 1);

		//gr_set_bitmap(bwi->beam_glow.first_frame + framenum, GR_ALPHABLEND_FILTER, GR_BITBLT_MODE_NORMAL, alpha * pct);

		// draw a poly
		//g3_draw_poly(4, verts, TMAP_FLAG_TEXTURED | TMAP_FLAG_CORRECT | TMAP_HTL_3D_UNLIT);

		if (!alpha_has_curve) {
			alpha = alpha * pct;
		}
		material material_info;
		material_set_unlit_emissive(&material_info, bwi->beam_glow.first_frame + framenum, alpha, 2.0f);
		g3_render_primitives_textured(&material_info, h1, 4, PRIM_TYPE_TRIFAN, false);

	} else {

		// draw the bitmap
		g3_transfer_vertex(&pt, &b->last_start);

		int framenum = 0;

		if (bwi->beam_glow.num_frames > 1) {
			if (anim_has_curve) {
				framenum = fl2i(i2fl(bwi->beam_glow.num_frames - 1) * anim_state);
			} else {
				b->beam_glow_frame += flFrametime;
				framenum = bm_get_anim_frame(bwi->beam_glow.first_frame, b->beam_glow_frame, bwi->beam_glow.total_time, true);
			}
		}

		CLAMP(framenum, 0, bwi->beam_glow.num_frames - 1);

		//gr_set_bitmap(bwi->beam_glow.first_frame + framenum, GR_ALPHABLEND_FILTER, GR_BITBLT_MODE_NORMAL, alpha * pct);

		if (!alpha_has_curve) {
			alpha = alpha * pct;
		}
		// draw 1 bitmap
		//g3_draw_bitmap(&pt, 0, rad, tmap_flags);
		material mat_params;
		material_set_unlit_emissive(&mat_params, bwi->beam_glow.first_frame + framenum, alpha, 2.0f);
		g3_render_rect_screen_aligned(&mat_params, &pt, 0, rad, 0.0f);

		// maybe draw more
		if ( pct > 0.3f ) {
			//g3_draw_bitmap(&pt, 0, rad * 0.75f, tmap_flags, rad * 0.25f);
			g3_render_rect_screen_aligned(&mat_params, &pt, 0, rad * 0.75f, rad * 0.25f);
		}

		if ( pct > 0.5f ) {
			//g3_draw_bitmap(&pt, 0, rad * 0.45f, tmap_flags, rad * 0.55f);
			g3_render_rect_screen_aligned(&mat_params, &pt, 0, rad * 0.45f, rad * 0.55f);
		}

		if ( pct > 0.7f ) {
			//g3_draw_bitmap(&pt, 0, rad * 0.25f, tmap_flags, rad * 0.75f);
			g3_render_rect_screen_aligned(&mat_params, &pt, 0, rad * 0.25f, rad * 0.75f);
		}
	}
}

// render all beam weapons
void beam_render_all()
{
	GR_DEBUG_SCOPE("Render Beams");
	TRACE_SCOPE(tracing::DrawBeams);

	beam *moveup;	

	// moves the U value of texture coods in beams if desired-Bobboau
	static float u_offset = 0.0f;
	u_offset += flFrametime;

	// traverse through all active beams
	moveup = GET_FIRST(&Beam_used_list);
	while ( moveup != END_OF_LIST(&Beam_used_list) ) {				
		// each beam type renders a little bit differently
		if ( (moveup->warmup_stamp == -1) && (moveup->warmdown_stamp == -1) && !(moveup->flags & BF_SAFETY) ) {
			// HACK -  if this is the first frame the beam is firing, don't render it
            if (moveup->framecount <= 0) {
				moveup->u_offset_local = 0;
				moveup = GET_NEXT(moveup);
				continue;
			}			

			// render the beam itself
			Assert(moveup->weapon_info_index >= 0);

			if (moveup->weapon_info_index < 0) {
				moveup = GET_NEXT(moveup);
				continue;
			}

			beam_render(moveup, u_offset);
		}

		// render the muzzle glow
		beam_render_muzzle_glow(moveup);

		// next item
		moveup = GET_NEXT(moveup);
	}	
}

// Delete all active beams
void beam_delete_all()
{
	beam* b = GET_FIRST(&Beam_used_list);
	while (b != END_OF_LIST(&Beam_used_list)) {
		beam* next = GET_NEXT(b);
		beam_delete(b);
		b = next;
	}
}

// output top and bottom vectors
// fvec == forward vector (eye viewpoint basically. in world coords)
// pos == world coordinate of the point we're calculating "around"
// w == width of the diff between top and bottom around pos
void beam_calc_facing_pts( vec3d *top, vec3d *bot, vec3d *fvec, vec3d *pos, float w, float  /*z_add*/ )
{
	vec3d uvec, rvec;
	vec3d temp;

	temp = *pos;

	vm_vec_sub( &rvec, &Eye_position, &temp );
	vm_vec_normalize( &rvec );

	vm_vec_cross(&uvec,fvec,&rvec);
	// VECMAT-ERROR: NULL VEC3D (value of, fvec == rvec)
	vm_vec_normalize_safe(&uvec);

	// Scale the beam width so that they always appear at least some configured amount of pixels wide.
	float scaled_w = model_render_get_diameter_clamped_to_min_pixel_size(pos, w, Min_pixel_size_beam);

	vm_vec_scale_add( top, &temp, &uvec, scaled_w * 0.5f );
	vm_vec_scale_add( bot, &temp, &uvec, -scaled_w * 0.5f );
}

// light scale factor
float blight = 25.5f;
DCF(blight, "Sets the beam light scale factor (Default is 25.5f)")
{
	dc_stuff_float(&blight);
}
namespace ltp = lighting_profiles;
float beam_current_light_radius(beam *bm, weapon_info *wip, beam_weapon_info *bwi, float noise)
{
	auto lp = ltp::current();
	float width = lp->beam_light_radius.handle(wip->light_radius);

	if(bwi->beam_light_as_multiplier)
		width *= bm->beam_light_width;

	if(bwi->beam_light_flicker)
		width *= noise;

	return width;
}

/**
 * @brief Validates a safe state for beam light creation functions
 *
 * @return true if state is safe, false otherwise
 */
bool beam_light_sanity_and_setup(beam const* bm, object const* objp, weapon_info** wip, beam_weapon_info** bwi)
{
	// no lighting
	if (Detail.lighting < 2) {
		return false;
	}

	// sanity
	Assert(bm != nullptr);
	Assert(objp != nullptr);
	if (objp == nullptr || bm == nullptr) {
		return false;
	}
	Assert(bm->weapon_info_index >= 0);
	*wip = &Weapon_info[bm->weapon_info_index];
	*bwi = &(*wip)->b_info;

	return true;
}

float beam_light_noise(beam const* bm, beam_weapon_info const* bwi)
{
	if ((bm->warmup_stamp < 0) && (bm->warmdown_stamp < 0)) // disable noise when warming up or down
		return frand_range(1.0f - bwi->sections[0].flicker, 1.0f + bwi->sections[0].flicker);
	else
		return 1.0f;
}

void beam_light_color(weapon_info *wip,hdr_color *to_fill )
{
	//If a custom light wasn't set we only base off of laser_color_1,
	//  and so we might as well calculate once and store it.

	if(!wip->light_color_set){
		wip->light_color.set_rgb(wip->laser_color_1.red, wip->laser_color_1.green,wip->laser_color_1.blue);
		wip->light_color_set=true;
	}
	SCP_vector<float> colors;
	wip->light_color.get_v5f(colors);
	to_fill->set_vecf(colors);
}

// call to add a light source to a small object
void beam_add_light_small(beam *bm, object *objp, vec3d *pt)
{
	weapon_info *wip = nullptr;
	beam_weapon_info *bwi = nullptr;

	if (!beam_light_sanity_and_setup(bm, objp, &wip, &bwi))
		return;

	Assert(pt != nullptr);

	float noise = beam_light_noise(bm, bwi);

	// get the width of the beam
	float light_rad = beam_current_light_radius(bm, wip, bwi, noise);

	float pct = 0.0f;

	if (bm->warmup_stamp != -1) {	// calculate muzzle light intensity
		// get warmup pct
		pct = BEAM_WARMUP_PCT(bm)*0.5f;
	} else if (bm->warmdown_stamp != -1) {	// if the beam is warming down
		// get warmup pct
		pct = MAX(1.0f - BEAM_WARMDOWN_PCT(bm)*1.3f,0.0f)*0.5f;
	}
	// otherwise the beam is really firing
	else {
		pct = 1.0f;
	}

	// Color is a copy so that we can modify it the brigthness without side-effect
	hdr_color light_color;

	beam_light_color(wip, &light_color);
	light_color.i(light_color.i() * pct);

	auto lp = ltp::current();
	light_color.i(lp->beam_light_brightness.handle(light_color.i()));

	if (light_color.i() <= 0.0f || light_rad <= 0.0f)
		return;
	light_add_point(pt, light_rad * 0.0001f, light_rad, &light_color,bm->beam_light_width*pct*noise/3.0f);
}

// call to add a light source to a large object
void beam_add_light_large(beam *bm, object *objp, vec3d *pt0, vec3d *pt1)
{
	weapon_info *wip;
	beam_weapon_info *bwi;

	if (!beam_light_sanity_and_setup(bm, objp, &wip, &bwi))
		return;

	float noise = beam_light_noise(bm, bwi);

	// width of the beam
	float light_rad = beam_current_light_radius(bm, wip, bwi, noise);

	// Color is a copy so that we can modify it the brigthness without side-effect
	hdr_color light_color;
	beam_light_color(wip, &light_color);

	if (bwi->beam_light_flicker)
		light_color.i(light_color.i() * noise);
	auto lp = ltp::current();
	light_color.i(lp->beam_light_brightness.handle(light_color.i()));
	light_rad = lp->beam_light_radius.handle(light_rad);
	if (light_color.i() <= 0.0f || light_rad <= 0.0f)
		return;
	light_add_tube(pt0, pt1, 1.0f, light_rad, &light_color,bm->beam_light_width*noise/3.0f);
}

// mark an object as being lit
void beam_add_light(beam *b, int objnum, int source, vec3d *c_point)
{
	beam_light_info *l;

	// if we're out of light slots!
	if(Beam_light_count >= MAX_BEAM_LIGHT_INFO){
		return;
	}

	// otherwise add it
	l = &Beam_lights[Beam_light_count++];
	l->bm = b;
	l->objnum = objnum;
	l->source = (ubyte)source;
	
	// only type 2 lights (from collisions) need a collision point
	if(c_point != NULL){
		l->c_point = *c_point;
	} else {
		Assert(source != 2);
		if(source == 2){
			Beam_light_count--;
		}
	}
}

// apply lighting from any beams
void beam_apply_lighting()
{
	int idx;
	beam_light_info *l;
	vec3d pt, dir;
	beam_weapon_info *bwi;

	// convert all beam lights into real lights
	for(idx=0; idx<Beam_light_count; idx++){
		// get the light
		l = &Beam_lights[idx];		

		// bad object
		if((l->objnum < 0) || (l->objnum >= MAX_OBJECTS) || (l->bm == NULL)){
			continue;
		}

		bwi = &Weapon_info[l->bm->weapon_info_index].b_info;

		// different light types
		switch(l->source){
		// from the muzzle of the gun
		case 0:
			// a few meters in from the of muzzle			
			vm_vec_sub(&dir, &l->bm->last_start, &l->bm->last_shot);
			vm_vec_normalize_quick(&dir);
			vm_vec_scale(&dir, -0.8f);  // TODO: This probably needs to *not* be stupid. -taylor
			vm_vec_scale_add(&pt, &l->bm->last_start, &dir, bwi->beam_muzzle_radius * 5.0f);

			beam_add_light_small(l->bm, &Objects[l->objnum], &pt);
			break;

		// from the beam passing by
		case 1:
			Assert( Objects[l->objnum].instance >= 0 );
			// Valathil: Everyone gets tube lights now
			beam_add_light_large(l->bm, &Objects[l->objnum], &l->bm->last_start, &l->bm->last_shot);
			break;

		// from a collision
		case 2:
			// Valathil: Dont render impact lights for shaders, handled by tube lighting
			break;
		}
	}	
}

// -----------------------------===========================------------------------------
// BEAM BOOKKEEPING FUNCTIONS
// -----------------------------===========================------------------------------

// delete a beam
void beam_delete(beam *b)
{
	// there isn't much distinction between death and death-started for beams
	if (scripting::hooks::OnBeamDeath->isActive()) {
		scripting::hooks::OnBeamDeath->run(scripting::hook_param_list(
			scripting::hook_param("Beam", 'o', &Objects[b->objnum])
		));
	}

	// remove from active list and put on free list
	list_remove(&Beam_used_list, b);
	list_append(&Beam_free_list, b);

	// delete our associated object
	if(b->objnum >= 0){
		obj_delete(b->objnum);
	}
	b->objnum = -1;

	// kill the beam looping sound
	if (b->beam_sound_loop.isValid()) {
		snd_stop(b->beam_sound_loop);
		b->beam_sound_loop = sound_handle::invalid();
	}	

	// handle model animation reversal (closing)
	// (beam animations should end pretty much immediately - taylor)
    if ((b->subsys) &&
        (b->subsys->turret_animation_position == MA_POS_READY))
    {
        b->subsys->turret_animation_done_time = timestamp(50);
    }

	// subtract one
	Beam_count--;
	Assert(Beam_count >= 0);
	nprintf(("Beam", "Recycled beam (%d beams remaining)\n", Beam_count));
}

// given an object, return its model num
int beam_get_model(object *objp)
{
	int pof;

	if (objp == NULL) {
		return -1;
	}

	Assert(objp->instance >= 0);
	if(objp->instance < 0){
		return -1;
	}

	switch(objp->type){
	case OBJ_SHIP:		
		return Ship_info[Ships[objp->instance].ship_info_index].model_num;

	case OBJ_WEAPON:
		Assert(Weapons[objp->instance].weapon_info_index >= 0);
		if(Weapons[objp->instance].weapon_info_index < 0){
			return -1;
		}
		return Weapon_info[Weapons[objp->instance].weapon_info_index].model_num;

	case OBJ_DEBRIS:
		Assert(Debris[objp->instance].is_hull);
		if(!Debris[objp->instance].is_hull){
			return -1;
		}
		return Debris[objp->instance].model_num;		

	case OBJ_ASTEROID:
		pof = Asteroids[objp->instance].asteroid_subtype;
		Assert(Asteroids[objp->instance].asteroid_type >= 0);
		if(Asteroids[objp->instance].asteroid_type < 0){
			return -1;
		}
		return Asteroid_info[Asteroids[objp->instance].asteroid_type].subtypes[pof].model_number;

	default:
		// this shouldn't happen too often
		mprintf(("Beam couldn't find a good object model/type!! (%d)\n", objp->type));
		return -1;
	}
}

// start the warmup phase for the beam
void beam_start_warmup(beam *b)
{
	auto wip = &Weapon_info[b->weapon_info_index];

	// set the warmup stamp
	b->warmup_stamp = timestamp(wip->b_info.beam_warmup);

	// start playing warmup sound
	if(!(Game_mode & GM_STANDALONE_SERVER) && (wip->b_info.beam_warmup_sound.isValid())){
		snd_play_3d(gamesnd_get_game_sound(wip->b_info.beam_warmup_sound), &b->last_start, &View_position);
	}

	beam_set_state(wip, b, WeaponState::WARMUP);

	if (scripting::hooks::OnBeamWarmup->isActive()) {
		scripting::hooks::OnBeamWarmup->run(scripting::hooks::WeaponUsedConditions{ b->objp == nullptr ? nullptr : &Ships[b->objp->instance], b->target, SCP_vector<int>{ b->weapon_info_index }, true },
			scripting::hook_param_list(
				scripting::hook_param("Beam", 'o', &Objects[b->objnum]),
				scripting::hook_param("User", 'o', b->objp),
				scripting::hook_param("Target", 'o', b->target)
			));
	}
}

// start the firing phase for the beam, return 0 if the beam failed to start, and should be deleted altogether
int beam_start_firing(beam *b)
{
	// kill the warmup stamp so the rest of the code knows its firing
	b->warmup_stamp = -1;	

	// any special stuff for each weapon type
	switch(b->type){
	// re-aim direct fire and antifighter beam weapons here, otherwise they tend to miss		
	case BeamType::DIRECT_FIRE:
	case BeamType::ANTIFIGHTER:
		beam_aim(b);
		break;
	
	case BeamType::SLASHING:
		break;

	case BeamType::TARGETING:
		break;	

	case BeamType::NORMAL_FIRE:
		break;

	case BeamType::OMNI:
		break;

	default:
		Int3();
	}

	// determine if we can legitimately start firing, or if we need to take other action
	switch(beam_ok_to_fire(b)){
	case -1 :
		return 0;

	case 0 :			
		beam_start_warmdown(b);
		return 1;
	}

	weapon_info* wip = &Weapon_info[b->weapon_info_index];

	// start the beam firing sound now, if we haven't already
	if ((!b->beam_sound_loop.isValid()) && (Weapon_info[b->weapon_info_index].b_info.beam_loop_sound.isValid())) {
		b->beam_sound_loop = snd_play_3d(gamesnd_get_game_sound(Weapon_info[b->weapon_info_index].b_info.beam_loop_sound), &b->last_start, &View_position, 0.0f, NULL, 1, 1.0, SND_PRIORITY_SINGLE_INSTANCE, NULL, 1.0f, 1);
	}	

	// "shot" sound
	if (b->objp == Player_obj && b->flags & BF_IS_FIGHTER_BEAM && Weapon_info[b->weapon_info_index].cockpit_launch_snd.isValid())
		snd_play(gamesnd_get_game_sound(Weapon_info[b->weapon_info_index].cockpit_launch_snd), 0.0f, 1.0f, SND_PRIORITY_MUST_PLAY);
	else if (Weapon_info[b->weapon_info_index].launch_snd.isValid())
		snd_play_3d(gamesnd_get_game_sound(Weapon_info[b->weapon_info_index].launch_snd), &b->last_start, &View_position);

	// if this is a fighter ballistic beam, always take at least one ammo to start with
	if (b->flags & BF_IS_FIGHTER_BEAM && wip->wi_flags[Weapon::Info_Flags::Ballistic])
		Ships[b->objp->instance].weapons.primary_bank_ammo[b->bank]--;

	beam_set_state(wip, b, WeaponState::FIRING);

	if (scripting::hooks::OnBeamFired->isActive()) {
		scripting::hooks::OnBeamFired->run(scripting::hooks::WeaponUsedConditions{ b->objp == nullptr ? nullptr : &Ships[b->objp->instance], b->target, SCP_vector<int>{ b->weapon_info_index }, true },
			scripting::hook_param_list(
				scripting::hook_param("Beam", 'o', &Objects[b->objnum]),
				scripting::hook_param("User", 'o', b->objp),
				scripting::hook_param("Target", 'o', b->target)
			));
	}

	// success
	return 1;
}

// start the warmdown phase for the beam
void beam_start_warmdown(beam *b)
{
	// timestamp
	b->warmdown_stamp = timestamp(Weapon_info[b->weapon_info_index].b_info.beam_warmdown);			

	// start the warmdown sound
	if(Weapon_info[b->weapon_info_index].b_info.beam_warmdown_sound.isValid()){
		snd_play_3d(gamesnd_get_game_sound(Weapon_info[b->weapon_info_index].b_info.beam_warmdown_sound), &b->last_start, &View_position);
	}

	// kill the beam looping sound
	if (b->beam_sound_loop.isValid()) {
		snd_stop(b->beam_sound_loop);
		b->beam_sound_loop = sound_handle::invalid();
	}

	if (b->subsys != nullptr) {
		// Starts the warmdown program if it exists
		b->subsys->system_info->beam_warmdown_program.start(b->objp,
			&vmd_zero_vector,
			&vmd_identity_matrix,
			b->subsys->system_info->subobj_num);
	}

	beam_set_state(&Weapon_info[b->weapon_info_index], b, WeaponState::WARMDOWN);

	if (scripting::hooks::OnBeamWarmdown->isActive()) {
		scripting::hooks::OnBeamWarmdown->run(scripting::hooks::WeaponUsedConditions{ b->objp == nullptr ? nullptr : &Ships[b->objp->instance], b->target, SCP_vector<int>{ b->weapon_info_index }, true },
			scripting::hook_param_list(
				scripting::hook_param("Beam", 'o', &Objects[b->objnum]),
				scripting::hook_param("User", 'o', b->objp),
				scripting::hook_param("Target", 'o', b->target)
			));
	}
}

// recalculate beam sounds (looping sounds relative to the player)
void beam_recalc_sounds(beam *b)
{
	beam_weapon_info *bwi;
	vec3d pos;	

	Assert(b->weapon_info_index >= 0);
	if(b->weapon_info_index < 0){
		return;
	}
	bwi = &Weapon_info[b->weapon_info_index].b_info;

	// update the sound position relative to the player
	if (b->beam_sound_loop.isValid()) {
		// get the point closest to the player's viewing position
		switch(vm_vec_dist_to_line(&View_position, &b->last_start, &b->last_shot, &pos, NULL)){
		// behind the beam, so use the start pos
		case -1:
			pos = b->last_start;
			break;

		// use the closest point
		case 0:
			// already calculated in vm_vec_dist_to_line(...)
			break;

		// past the beam, so use the shot pos
		case 1:
			pos = b->last_shot;
			break;
		}

		snd_update_3d_pos(b->beam_sound_loop, gamesnd_get_game_sound(bwi->beam_loop_sound), &pos);
	}
}


// -----------------------------===========================------------------------------
// BEAM AIMING FUNCTIONS
// -----------------------------===========================------------------------------

// fills in binfo
void beam_get_binfo(beam *b, float accuracy, int num_shots, int burst_seed, float burst_shot_rotation, float per_burst_shot_rotation)
{
	vec3d p2;
	int model_num, idx;
	vec3d pos1, pos2;
	vec3d turret_point, turret_norm;
	beam_weapon_info *bwi;
	float miss_factor;

	if (b->flags & BF_IS_FIGHTER_BEAM) {
		vm_vec_unrotate(&turret_point, &b->local_fire_postion, &b->objp->orient);
		turret_point += b->objp->pos;
		turret_norm = b->objp->orient.vec.fvec;
	} else if (b->subsys != nullptr) {
		int temp = b->subsys->turret_next_fire_pos;

		b->subsys->turret_next_fire_pos = b->firingpoint;

		// where the shot is originating from (turret_point gets filled in)
		// (we'll use the average origin if this turret is sharing fire direction)
		beam_get_global_turret_gun_info(b->objp, b->subsys, &turret_point, b->subsys->system_info->flags[Model::Subsystem_Flags::Share_fire_direction], &turret_norm, true, nullptr, (b->flags & BF_IS_FIGHTER_BEAM) != 0);

		b->subsys->turret_next_fire_pos = temp;
	} else {
		turret_point = b->last_start;
		if (b->flags & BF_TARGETING_COORDS) {
			p2 = b->target_pos1;
		} else {
			p2 = b->target->pos;
		}
		vm_vec_normalized_dir(&turret_norm, &p2, &turret_point);
	}

	// get a model # to work with
	model_num = beam_get_model(b->target);
	if ((model_num < 0) && !(b->flags & BF_TARGETING_COORDS) && b->type != BeamType::OMNI) {
		return;
	}

	// get beam weapon info
	Assert(b->weapon_info_index >= 0);
	if(b->weapon_info_index < 0){
		return;
	}

	auto& wi = Weapon_info[b->weapon_info_index];
	bwi = &wi.b_info;

	// stuff num shots even though its only used for antifighter beam weapons
	b->binfo.shot_count = (ubyte)num_shots;
	if(b->binfo.shot_count > MAX_BEAM_SHOTS){
		b->binfo.shot_count = MAX_BEAM_SHOTS;
	}

	int seed = bwi->flags[Weapon::Beam_Info_Flags::Burst_share_random] ? burst_seed : Random::next();

	// generate the proper amount of directional vectors
	switch(b->type){
	// pick an accuracy. beam will be properly aimed at actual fire time
	case BeamType::DIRECT_FIRE:
		// determine the miss factor
		Assert(Game_skill_level >= 0 && Game_skill_level < NUM_SKILL_LEVELS);
		Assert(b->team >= 0 && b->team < (int)Iff_info.size());
		miss_factor = bwi->beam_iff_miss_factor[b->team][Game_skill_level];

		// all we will do is decide whether or not we will hit - direct fire beam weapons are re-aimed immediately before firing
		b->binfo.shot_aim[0] = frand_range(0.0f, 1.0f + miss_factor * accuracy);
		b->binfo.shot_count = 1;

		if (b->flags & BF_TARGETING_COORDS) {
			// these aren't used for direct fire beams, so zero them out
			vm_vec_zero(&b->binfo.dir_a);
			vm_vec_zero(&b->binfo.dir_b);
		} else {
			// get random model points, this is useful for big ships, because we never miss when shooting at them
			b->binfo.dir_a = submodel_get_random_point(model_num, 0, seed);
			b->binfo.dir_b = submodel_get_random_point(model_num, 0, seed);
		}
		break;

	// just 2 points in the "slash"
	case BeamType::SLASHING:
		if (b->flags & BF_TARGETING_COORDS) {
			// slash between the two
			pos1 = b->target_pos1;
			pos2 = b->target_pos2;
		} else {
			beam_get_octant_points(model_num, b->target, seed, &pos1, &pos2);
		}

		// point 1
		vm_vec_sub(&b->binfo.dir_a, &pos1, &turret_point);
		vm_vec_normalize(&b->binfo.dir_a);

		if (b->target && wi.beam_curves.has_curve(weapon_info::BeamCurveOutputs::END_POSITION_BY_VELOCITY)) {
			float velocity_mult = wi.beam_curves.get_output(weapon_info::BeamCurveOutputs::END_POSITION_BY_VELOCITY, *b, &b->modular_curves_instance);
			pos2 += b->target->phys_info.vel * velocity_mult;
		}

		// point 2
		vm_vec_sub(&b->binfo.dir_b, &pos2, &turret_point);
		vm_vec_normalize(&b->binfo.dir_b);

		break;

	// nothing for this beam - its very special case
	case BeamType::TARGETING:
		break;

	// antifighter beams fire at small ship multiple times
	case BeamType::ANTIFIGHTER:
		// determine the miss factor
		Assert(Game_skill_level >= 0 && Game_skill_level < NUM_SKILL_LEVELS);
		Assert(b->team >= 0 && b->team < (int)Iff_info.size());
		miss_factor = bwi->beam_iff_miss_factor[b->team][Game_skill_level];

		// get a bunch of shot aims
		for(idx=0; idx<b->binfo.shot_count; idx++){
			//	MK, 9/3/99: Added pow() function to make increasingly likely to miss with subsequent shots.  30% more likely with each shot.
			float r = ((float) pow(1.3f, (float) idx)) * miss_factor * accuracy;
			b->binfo.shot_aim[idx] = frand_range(0.0f, 1.0f + r);
		}
		break;

	// normal-fire beams just fire straight
	case BeamType::NORMAL_FIRE:
		b->binfo.shot_aim[0] = 0.0000001f;
		b->binfo.shot_count = 1;
		b->binfo.dir_a = turret_norm;
		b->binfo.dir_b = turret_norm;
		break;

	case BeamType::OMNI:
	{
		vm_vec_zero(&pos1);
		vm_vec_zero(&pos2);
		vec3d rot_axis, burst_rot_axis, per_burst_rot_axis;

		object* usable_target = nullptr;
		// don't use the target if this is a fighter beam
		if (!(b->flags & BF_IS_FIGHTER_BEAM) && b->target)
			usable_target = b->target;

		// set up shooter orient now
		matrix orient = vmd_identity_matrix;
		if (b->flags & BF_IS_FIGHTER_BEAM) {
			orient = b->objp->orient;
		} else if (b->subsys) {
			vec3d fvec, uvec, target_pos;
			if (b->target)
				target_pos = b->target->pos;
			else if (b->flags & BF_TARGETING_COORDS)
				target_pos = b->target_pos1;
			else
				UNREACHABLE("Turret beam fired without a target or target coordinates?");
			vm_vec_normalized_dir(&fvec, &target_pos, &turret_point);
			vm_vec_unrotate(&uvec, &b->subsys->system_info->turret_norm, &b->objp->orient);
			vm_vector_2_matrix_norm(&orient, &fvec, &uvec);
		} else if (b->flags & BF_TARGETING_COORDS) {
			// targeting coords already set up turret_norm with target_pos above
			vm_vector_2_matrix_norm(&orient, &turret_norm);
		} 

		vec3d rand1_on = vm_vec_new(0.f, 0.f, 0.f); 
		vec3d rand2_on = vm_vec_new(0.f, 0.f, 0.f);
		vec3d rand1_off = vm_vec_new(0.f, 0.f, 0.f);
		vec3d rand2_off = vm_vec_new(0.f, 0.f, 0.f);

		// Get our two starting points
		if (usable_target) {
			// set up our two kinds of random points if needed
			if (bwi->t5info.start_pos == Type5BeamPos::RANDOM_INSIDE || bwi->t5info.end_pos == Type5BeamPos::RANDOM_INSIDE) {
				vec3d temp1 = submodel_get_random_point(model_num, 0, seed);
				vec3d temp2 = submodel_get_random_point(model_num, 0, seed);
				vm_vec_rotate(&rand1_on, &temp1, &b->target->orient);
				vm_vec_rotate(&rand2_on, &temp2, &b->target->orient);
				rand1_on += b->target->pos;
				rand2_on += b->target->pos;
			}
			if (bwi->t5info.start_pos == Type5BeamPos::RANDOM_OUTSIDE || bwi->t5info.end_pos == Type5BeamPos::RANDOM_OUTSIDE)
				beam_get_octant_points(model_num, usable_target, seed, &rand1_off, &rand2_off);

			// get start and end points
			switch (bwi->t5info.start_pos) {
				case Type5BeamPos::CENTER:
					pos1 = b->target->pos;
					break;
				case Type5BeamPos::RANDOM_INSIDE:
					pos1 = rand1_on;
					break;
				case Type5BeamPos::RANDOM_OUTSIDE:
					pos1 = rand1_off;
					break;
				default:;
					// the other cases dont matter
			}


			if (bwi->t5info.no_translate || bwi->t5info.end_pos == Type5BeamPos::SAME_RANDOM)
				pos2 = pos1;
			else {
				switch (bwi->t5info.end_pos) {
					case Type5BeamPos::CENTER:
						pos2 = b->target->pos;
						break;
					case Type5BeamPos::RANDOM_INSIDE:
						pos2 = rand2_on;
						break;
					case Type5BeamPos::RANDOM_OUTSIDE:
						pos2 = rand2_off;
						break;
					default:;
						// the other cases dont matter
				}
			}

			// set rot_axis if its center
			if (bwi->t5info.continuous_rot_axis == Type5BeamRotAxis::CENTER)
				rot_axis = b->target->pos;
			if (bwi->t5info.per_burst_rot_axis == Type5BeamRotAxis::CENTER)
				per_burst_rot_axis = b->target->pos;
			if (bwi->t5info.burst_rot_axis == Type5BeamRotAxis::CENTER)
				burst_rot_axis = b->target->pos;

			if (wi.beam_curves.has_curve(weapon_info::BeamCurveOutputs::END_POSITION_BY_VELOCITY)) {
				float velocity_mult = wi.beam_curves.get_output(weapon_info::BeamCurveOutputs::END_POSITION_BY_VELOCITY, *b, &b->modular_curves_instance);
				pos2 += b->target->phys_info.vel * velocity_mult;
			}
			
		} else { // No usable target
			vec3d center = vm_vec_new(0.f, 0.f, 0.f);
			// if we have no target let's act as though we're shooting at something with a 300m radius 300m away

			// randomize the start and end points if not center aiming
			// aim on the edge for random outside 
			if (bwi->t5info.start_pos != Type5BeamPos::CENTER)
				vm_vec_random_in_circle(&pos1, &center, &orient, 1.f, bwi->t5info.start_pos == Type5BeamPos::RANDOM_OUTSIDE);

			if (bwi->t5info.end_pos != Type5BeamPos::CENTER)
				vm_vec_random_in_circle(&pos2, &center, &orient, 1.f, bwi->t5info.end_pos == Type5BeamPos::RANDOM_OUTSIDE);

			if (bwi->t5info.no_translate || bwi->t5info.end_pos == Type5BeamPos::SAME_RANDOM)
				pos2 = pos1;

			pos1 *= 300.f;
			pos2 *= 300.f;
			vec3d move_forward = vm_vec_new(0.f, 0.f, 300.f);
			center += move_forward;
			pos1 += move_forward;
			pos2 += move_forward;

			// unrotate the points to get world positions
			vec3d temp = pos1; vm_vec_unrotate(&pos1, &temp, &orient);
			temp = pos2;       vm_vec_unrotate(&pos2, &temp, &orient);
			temp = center;     vm_vec_unrotate(&center, &temp, &orient);
			pos1 += turret_point;
			pos2 += turret_point;
			center += turret_point;

			// set rot_axis if its center
			if (bwi->t5info.continuous_rot_axis == Type5BeamRotAxis::CENTER)
				rot_axis = center;
			if (bwi->t5info.per_burst_rot_axis == Type5BeamRotAxis::CENTER)
				per_burst_rot_axis = center;
			if (bwi->t5info.burst_rot_axis == Type5BeamRotAxis::CENTER)
				burst_rot_axis = center;

		}
		// OKAY DONE WITH THE INITIAL SET UP

		// set rot_axis if its one of the before offset points
		if (bwi->t5info.continuous_rot_axis == Type5BeamRotAxis::STARTPOS_NO_OFFSET || bwi->t5info.continuous_rot_axis == Type5BeamRotAxis::ENDPOS_NO_OFFSET)
			rot_axis = bwi->t5info.continuous_rot_axis == Type5BeamRotAxis::STARTPOS_NO_OFFSET ? pos1 : pos2;
		if (bwi->t5info.per_burst_rot_axis == Type5BeamRotAxis::STARTPOS_NO_OFFSET || bwi->t5info.per_burst_rot_axis == Type5BeamRotAxis::ENDPOS_NO_OFFSET)
			per_burst_rot_axis = bwi->t5info.per_burst_rot_axis == Type5BeamRotAxis::STARTPOS_NO_OFFSET ? pos1 : pos2;
		if (bwi->t5info.burst_rot_axis == Type5BeamRotAxis::STARTPOS_NO_OFFSET || bwi->t5info.burst_rot_axis == Type5BeamRotAxis::ENDPOS_NO_OFFSET)
			burst_rot_axis = bwi->t5info.burst_rot_axis == Type5BeamRotAxis::STARTPOS_NO_OFFSET ? pos1 : pos2;

		// now the offsets
		float scale_factor;
		if (usable_target && b->target != nullptr) {
			if (bwi->t5info.target_scale_positions)
				scale_factor = b->target->radius;
			else
				scale_factor = vm_vec_dist(&b->target->pos, &turret_point); // using dist here means we have a constant angular width
		} else
			scale_factor = 300.f; // no target, just use 300m like the notarget scenario above

		vec3d offset = bwi->t5info.start_pos_offset;
		offset *= scale_factor;

		// switch to the target's orient if applicable
		if (bwi->t5info.target_orient_positions && b->target != nullptr)
			orient = b->target->orient;

		// maybe add some random
		vec3d random_offset;
		vm_vec_random_in_sphere(&random_offset, &vmd_zero_vector, 1.f, false, true);
		random_offset *= scale_factor;
		random_offset.xyz.x *= bwi->t5info.start_pos_rand.xyz.x;
		random_offset.xyz.y *= bwi->t5info.start_pos_rand.xyz.y;
		random_offset.xyz.z *= bwi->t5info.start_pos_rand.xyz.z;
		offset += random_offset;

		// then unrotate by it to get the world orientation
		vec3d rotated_offset;
		vm_vec_unrotate(&rotated_offset, &offset, &orient);
		pos1 += rotated_offset;

		// end pos offset
		if (bwi->t5info.no_translate)
			pos2 = pos1;
		else {
			offset = bwi->t5info.end_pos_offset;
			offset *= scale_factor;

			// randomness
			vm_vec_random_in_sphere(&random_offset, &vmd_zero_vector, 1.f, false, true);
			random_offset *= scale_factor;
			random_offset.xyz.x *= bwi->t5info.end_pos_rand.xyz.x;
			random_offset.xyz.y *= bwi->t5info.end_pos_rand.xyz.y;
			random_offset.xyz.z *= bwi->t5info.end_pos_rand.xyz.z;
			offset += random_offset;

			// rotate
			vm_vec_unrotate(&rotated_offset, &offset, &orient);
			pos2 += rotated_offset;
		}

		// finally grab the last cases for rot_axis
		if (bwi->t5info.continuous_rot_axis == Type5BeamRotAxis::STARTPOS_OFFSET || bwi->t5info.continuous_rot_axis == Type5BeamRotAxis::ENDPOS_OFFSET)
			rot_axis = bwi->t5info.continuous_rot_axis == Type5BeamRotAxis::STARTPOS_OFFSET ? pos1 : pos2;
		if (bwi->t5info.per_burst_rot_axis == Type5BeamRotAxis::STARTPOS_OFFSET || bwi->t5info.per_burst_rot_axis == Type5BeamRotAxis::ENDPOS_OFFSET)
			per_burst_rot_axis = bwi->t5info.per_burst_rot_axis == Type5BeamRotAxis::STARTPOS_OFFSET ? pos1 : pos2;
		if (bwi->t5info.burst_rot_axis == Type5BeamRotAxis::STARTPOS_OFFSET || bwi->t5info.burst_rot_axis == Type5BeamRotAxis::ENDPOS_OFFSET)
			burst_rot_axis = bwi->t5info.burst_rot_axis == Type5BeamRotAxis::STARTPOS_OFFSET ? pos1 : pos2;

		// normalize the vectors
		vec3d per_burst_rot_axis_direction, burst_rot_axis_direction;

		vm_vec_sub(&per_burst_rot_axis_direction, &per_burst_rot_axis, &turret_point);
		vm_vec_normalize(&per_burst_rot_axis_direction);

		vm_vec_sub(&burst_rot_axis_direction, &burst_rot_axis, &turret_point);
		vm_vec_normalize(&burst_rot_axis_direction);

		if (bwi->t5info.continuous_rot_axis != Type5BeamRotAxis::UNSPECIFIED) {
			vm_vec_sub(&b->binfo.rot_axis, &rot_axis, &turret_point);
			vm_vec_normalize(&b->binfo.rot_axis);
		}

		vm_vec_sub(&b->binfo.dir_a, &pos1, &turret_point);
		vm_vec_normalize(&b->binfo.dir_a);

		vm_vec_sub(&b->binfo.dir_b, &pos2, &turret_point);
		vm_vec_normalize(&b->binfo.dir_b);

		vec3d zero_vec = vmd_zero_vector;
		// and finally rotate around the per_burst and burst rot_axes
		if (bwi->t5info.per_burst_rot_axis != Type5BeamRotAxis::UNSPECIFIED) {
			// negative means random
			float per_burst_rot = per_burst_shot_rotation;
			if (per_burst_rot < 0.0f)
				per_burst_rot = static_randf_range(seed, 0.f, PI2);

			vm_rot_point_around_line(&b->binfo.dir_a,    &b->binfo.dir_a,    per_burst_rot, &zero_vec, &per_burst_rot_axis_direction);
			vm_rot_point_around_line(&b->binfo.dir_b,    &b->binfo.dir_b,    per_burst_rot, &zero_vec, &per_burst_rot_axis_direction);
			vm_rot_point_around_line(&b->binfo.rot_axis, &b->binfo.rot_axis, per_burst_rot, &zero_vec, &per_burst_rot_axis_direction);
		}

		if (bwi->t5info.burst_rot_axis != Type5BeamRotAxis::UNSPECIFIED) {
			// negative means random
			float burst_rot = burst_shot_rotation;
			if (burst_rot < 0.0f)
				burst_rot = frand_range(0.f, PI2);

			vm_rot_point_around_line(&b->binfo.dir_a,    &b->binfo.dir_a,    burst_rot, &zero_vec, &burst_rot_axis_direction);
			vm_rot_point_around_line(&b->binfo.dir_b,    &b->binfo.dir_b,    burst_rot, &zero_vec, &burst_rot_axis_direction);
			vm_rot_point_around_line(&b->binfo.rot_axis, &b->binfo.rot_axis, burst_rot, &zero_vec, &burst_rot_axis_direction);
		}

		break;
	}
	default:
		break;
	}
}

// aim the beam (setup last_start and last_shot - the endpoints). also recalculates collision pairs
void beam_aim(beam *b)
{
	// figure out if we are sharing a fire direction, and if so, the reference to share
	// incidentally, sharing fire direction isn't applicable to fighter beams, targeting beams, or normal beams, so check for those too
	bool shared_fire_direction = false;
	beam *shared_fire_reference = nullptr;
	if ((b->subsys != nullptr) && (b->subsys->system_info->flags[Model::Subsystem_Flags::Share_fire_direction])
		&& !(b->flags & BF_IS_FIGHTER_BEAM) && (b->type != BeamType::TARGETING) && (b->type != BeamType::NORMAL_FIRE))
	{
		shared_fire_direction = true;

		if (b->subsys->shared_fire_direction_beam_objnum >= 0) {
			auto shared_candidate_objp = &Objects[b->subsys->shared_fire_direction_beam_objnum];

			// if beam reference is no longer valid, clear the objnum
			if (shared_candidate_objp->type != OBJ_BEAM || shared_candidate_objp->flags[Object::Object_Flags::Should_be_dead]) {
				b->subsys->shared_fire_direction_beam_objnum = -1;
			}
			// reference is probably valid...
			else {
				shared_fire_reference = &Beams[shared_candidate_objp->instance];
				// ...but clear it if we have cycled back to the first point
				if (shared_fire_reference->firingpoint == 0) {
					shared_fire_reference = nullptr;
					b->subsys->shared_fire_direction_beam_objnum = -1;
				}
			}
		}
	}
	
	if (!(b->flags & BF_TARGETING_COORDS)) {
		// targeting type beam weapons have no target
		if (b->target == NULL) {
			Assert(b->type == BeamType::TARGETING || b->type == BeamType::OMNI);
			if(b->type != BeamType::TARGETING && b->type != BeamType::OMNI){
				return;
			}
		}
		// get a model # to work with
		else {
			// this can happen if we fire at a target that was just destroyed
			if (beam_get_model(b->target) < 0) {
				return;
			}	
		}
	}

	vec3d turret_norm = vmd_zero_vector;
	vec3d shared_last_start = vmd_zero_vector;
	if (b->subsys != nullptr && b->type != BeamType::TARGETING) {	// targeting type beams don't use this information.
		int temp_int = b->subsys->turret_next_fire_pos;

		if (!(b->flags & BF_IS_FIGHTER_BEAM))
			b->subsys->turret_next_fire_pos = b->firingpoint;

		// where the shot is originating from (b->last_start gets filled in)
		beam_get_global_turret_gun_info(b->objp, b->subsys, &b->last_start, false, &turret_norm, true, nullptr, (b->flags & BF_IS_FIGHTER_BEAM) != 0);

		// for shared beams that don't currently have a reference, we'll need a virtual origin
		if (shared_fire_direction && !shared_fire_reference)
			beam_get_global_turret_gun_info(b->objp, b->subsys, &shared_last_start, true, nullptr, true, nullptr, false);

		b->subsys->turret_next_fire_pos = temp_int;
	}

	// setup our initial shot point and aim direction
	vec3d beam_vector;
	switch(b->type){
	case BeamType::DIRECT_FIRE:
		// if we're targeting a subsystem - shoot directly at it
		if(b->target_subsys != nullptr){
			if (shared_fire_reference) {
				vm_vec_sub(&beam_vector, &shared_fire_reference->last_shot, &shared_fire_reference->last_start);
			} else {
				// find the location of the subsystem
				vm_vec_unrotate(&b->last_shot, &b->target_subsys->system_info->pnt, &b->target->orient);
				vm_vec_add2(&b->last_shot, &b->target->pos);

				// if this beam will be the reference for shared firing
				if (shared_fire_direction) {
					// find the vector from the virtual origin
					vm_vec_sub(&beam_vector, &b->last_shot, &shared_last_start);
					// update the reference
					shared_fire_reference = b;
					b->subsys->shared_fire_direction_beam_objnum = b->objnum;
				}
				// otherwise find the vector from the regular origin
				else
					vm_vec_sub(&beam_vector, &b->last_shot, &b->last_start);
			}
			vm_vec_scale_add(&b->last_shot, &b->last_start, &beam_vector, 2.0f);
			break;
		}

		// if we're shooting at a big ship - shoot directly at the model
		if((b->target != nullptr) && (b->target->type == OBJ_SHIP) && (Ship_info[Ships[b->target->instance].ship_info_index].is_big_or_huge())){
			if (shared_fire_reference) {
				vm_vec_sub(&beam_vector, &shared_fire_reference->last_shot, &shared_fire_reference->last_start);
			} else {
				// rotate the random model point into world coords
				vm_vec_unrotate(&b->last_shot, &b->binfo.dir_a, &b->target->orient);
				vm_vec_add2(&b->last_shot, &b->target->pos);

				// if this beam will be the reference for shared firing
				if (shared_fire_direction) {
					// find the vector from the virtual origin
					vm_vec_sub(&beam_vector, &b->last_shot, &shared_last_start);
					// update the reference
					shared_fire_reference = b;
					b->subsys->shared_fire_direction_beam_objnum = b->objnum;
				}
				// otherwise find the vector from the regular origin
				else
					vm_vec_sub(&beam_vector, &b->last_shot, &b->last_start);
			}
			vm_vec_scale_add(&b->last_shot, &b->last_start, &beam_vector, 2.0f);
			break;
		}

		// all other direct-fire cases
		FALLTHROUGH;

	// antifighter beams are aimed the same way as direct fire beams in the general case 
	case BeamType::ANTIFIGHTER:
		if (shared_fire_reference) {
			vm_vec_sub(&beam_vector, &shared_fire_reference->last_shot, &shared_fire_reference->last_start);
			vm_vec_add(&b->last_shot, &b->last_start, &beam_vector);
		} else {
			// point at the center of the target
			if (b->flags & BF_TARGETING_COORDS) {
				b->last_shot = b->target_pos1;
			} else {
				b->last_shot = b->target->pos;
			}

			// if this beam will be the reference for shared firing
			if (shared_fire_direction) {
				// find the vector from the virtual origin
				vm_vec_sub(&beam_vector, &b->last_shot, &shared_last_start);
				// adjust the beam based on the true origin
				vm_vec_add(&b->last_shot, &b->last_start, &beam_vector);
				// update the reference
				shared_fire_reference = b;
				b->subsys->shared_fire_direction_beam_objnum = b->objnum;
			}

			// after pointing, jitter based on shot_aim (if we have a target object)
			if (!(b->flags & BF_TARGETING_COORDS)) {
				beam_jitter_aim(b, b->binfo.shot_aim[b->shot_index]);
			}
		}
		break;

	// slashing and omni beams are aimed the same way here, since everything is based on dir_a
	case BeamType::SLASHING:
	case BeamType::OMNI:
		// if we share directions, copy them
		if (shared_fire_reference) {
			b->binfo.dir_a = shared_fire_reference->binfo.dir_a;
			b->binfo.dir_b = shared_fire_reference->binfo.dir_b;
		}

		// set the shot point (no need to adjust based on virtual origin since dir_a controls everything)
		vm_vec_scale_add(&b->last_shot, &b->last_start, &b->binfo.dir_a, b->range);
		Assert(is_valid_vec(&b->last_shot));

		// if this beam will be the reference for shared firing
		// (the beam vector update will happen in the relevant _move function)
		if (!shared_fire_reference && shared_fire_direction) {
			// update the reference
			shared_fire_reference = b;
			b->subsys->shared_fire_direction_beam_objnum = b->objnum;
		}
		break;

	case BeamType::TARGETING:
		beam_type_targeting_move(b);
		break;

	case BeamType::NORMAL_FIRE:
		// point directly in the direction of the turret
		vm_vec_scale_add(&b->last_shot, &b->last_start, &turret_norm, b->range);
		break;

	default:
		UNREACHABLE("Impossible beam type (%d); get a coder!\n", (int)b->type);
	}

	if (!Weapon_info[b->weapon_info_index].wi_flags[Weapon::Info_Flags::No_collide]) {
		// recalculate object pairs
		OBJ_RECALC_PAIRS((&Objects[b->objnum]));
	}
}

// given a model #, and an object, stuff 2 good world coord points
void beam_get_octant_points(int modelnum, object *objp, int seed, vec3d *v1, vec3d *v2)
{	
	polymodel *pm = model_get(modelnum);

	// bad bad bad bad bad bad
	if(pm == nullptr){
		Int3();
		return;
	}

	vec3d t1 = vmd_zero_vector;
	vec3d t2 = vmd_zero_vector;

	// make the seed go from [1-26] (0 would make it choose the center, which we dont want
	seed %= 26;
	seed += 1;

	// randomly pick octants	
	for (int i = 0; i < 3; i++) {
		// seed % 3
		// seed / 3 % 3
		// seed / 9 % 3
		// 3 numbers, each is 2, 1 or 0
		// max->min, min->max, or no movement  on each axis
		int num = (seed / (int)pow(3, i)) % 3;
		if (num == 2) {
			t1.a1d[i] = pm->maxs.a1d[i];
			t2.a1d[i] = pm->mins.a1d[i];
		} else if (num == 1) {
			t1.a1d[i] = pm->mins.a1d[i];
			t2.a1d[i] = pm->maxs.a1d[i];
		}
	}
	Assert(!vm_vec_same(&t1, &t2));

	// get them in world coords
	vec3d  temp;
	vm_vec_unrotate(&temp, &t1, &objp->orient);
	vm_vec_add(v1, &temp, &objp->pos);
	vm_vec_unrotate(&temp, &t2, &objp->orient);
	vm_vec_add(v2, &temp, &objp->pos);
}

// throw some jitter into the aim - based upon shot_aim
void beam_jitter_aim(beam *b, float aim)
{
	Assert(b->target != NULL);
	vec3d forward, circle;
	matrix m;
	float subsys_strength;

	// if the weapons subsystem is damaged or destroyed
	if((b->objp != NULL) && (b->objp->signature == b->sig) && (b->objp->type == OBJ_SHIP) && (b->objp->instance >= 0) && (b->objp->instance < MAX_SHIPS)){
		// get subsytem strength
		subsys_strength = ship_get_subsystem_strength(&Ships[b->objp->instance], SUBSYSTEM_WEAPONS);
		
		// when subsytem strength is 0, double the aim error factor
		aim += aim * (1.0f - subsys_strength);
	}

	// shot aim is a direct linear factor of the target model's radius.
	// so, pick a random point on the circle
	vm_vec_normalized_dir(&forward, &b->last_shot, &b->last_start);
	
	// vector
	vm_vector_2_matrix_norm(&m, &forward, nullptr, nullptr);

	// get a random vector on the circle, but somewhat biased towards the center
	vm_vec_random_in_circle(&circle, &b->last_shot, &m, aim * b->target->radius, false, true);
	
	// get the vector pointing to the circle point
	vm_vec_sub(&forward, &circle, &b->last_start);	
	vm_vec_scale_add(&b->last_shot, &b->last_start, &forward, 2.0f);
}


// -----------------------------===========================------------------------------
// BEAM COLLISION FUNCTIONS
// -----------------------------===========================------------------------------

// collide a beam with a ship, returns 1 if we can ignore all future collisions between the 2 objects
int beam_collide_ship(obj_pair *pair)
{
	beam * a_beam;
	object *weapon_objp;
	object *ship_objp;
	ship *shipp;
	ship_info *sip;
	weapon_info *bwi;
	mc_info mc_hull_enter, mc_hull_exit, mc_shield, *mc;
	int model_num;
	float width;

	// bogus
	if (pair == NULL) {
		return 0;
	}

	if (reject_due_collision_groups(pair->a, pair->b))
		return 0;

	// get the beam
	Assert(pair->a->instance >= 0);
	Assert(pair->a->type == OBJ_BEAM);
	Assert(Beams[pair->a->instance].objnum == OBJ_INDEX(pair->a));
	weapon_objp = pair->a;
	a_beam = &Beams[pair->a->instance];

	// Don't check collisions for warping out player if past stage 1.
	if (Player->control_mode >= PCM_WARPOUT_STAGE1) {
		if ( pair->a == Player_obj ) return 0;
		if ( pair->b == Player_obj ) return 0;
	}

	// if the "warming up" timestamp has not expired
	if ((a_beam->warmup_stamp != -1) || (a_beam->warmdown_stamp != -1)) {
		return 0;
	}

	// if the beam is on "safety", don't collide with anything
	if (a_beam->flags & BF_SAFETY) {
		return 0;
	}
	
	// if the colliding object is the shooting object, return 1 so this is culled
	if (!pair->a->flags[Object::Object_Flags::Collides_with_parent] && pair->b == a_beam->objp) {
		return 1;
	}	

	// try and get a model
	model_num = beam_get_model(pair->b);
	if (model_num < 0) {
		return 1;
	}
	
#ifndef NDEBUG
	Beam_test_ints++;
	Beam_test_ship++;
#endif

	// get the ship
	Assert(pair->b->instance >= 0);
	Assert(pair->b->type == OBJ_SHIP);
	Assert(Ships[pair->b->instance].objnum == OBJ_INDEX(pair->b));
	if ((pair->b->type != OBJ_SHIP) || (pair->b->instance < 0))
		return 1;
	ship_objp = pair->b;
	shipp = &Ships[ship_objp->instance];

	if (shipp->flags[Ship::Ship_Flags::Arriving_stage_1])
		return 0;

	int quadrant_num = -1;
	bool valid_hit_occurred = false;
	sip = &Ship_info[shipp->ship_info_index];
	bwi = &Weapon_info[a_beam->weapon_info_index];

	polymodel *pm = model_get(model_num);

	// get the width of the beam
	width = a_beam->beam_collide_width * a_beam->current_width_factor;


	// Goober5000 - I tried to make collision code much saner... here begin the (major) changes

	// set up collision struct
	mc_hull_enter.model_instance_num = shipp->model_instance_num;
	mc_hull_enter.model_num = model_num;
	mc_hull_enter.submodel_num = -1;
	mc_hull_enter.orient = &ship_objp->orient;
	mc_hull_enter.pos = &ship_objp->pos;
	mc_hull_enter.p0 = &a_beam->last_start;
	mc_hull_enter.p1 = &a_beam->last_shot;

	// maybe do a sphereline
	if (width > ship_objp->radius * BEAM_AREA_PERCENT) {
		mc_hull_enter.radius = width * 0.5f;
		mc_hull_enter.flags = MC_CHECK_SPHERELINE;
	} else {
		mc_hull_enter.flags = MC_CHECK_RAY;
	}

	// check all three kinds of collisions ---
	int shield_collision, hull_enter_collision, hull_exit_collision;

	if (pm->shield.ntris > 0) {
		mc_shield = mc_hull_enter;
		mc_shield.flags |= MC_CHECK_SHIELD;

		shield_collision = model_collide(&mc_shield);
	} else {
		shield_collision = 0;
	}

	if (beam_will_tool_target(a_beam, ship_objp)) {
		mc_hull_exit = mc_hull_enter;
		mc_hull_exit.flags |= MC_CHECK_MODEL;

		// reverse this vector so that we check for exit holes as opposed to entrance holes
		std::swap(mc_hull_exit.p0, mc_hull_exit.p1);
		hull_exit_collision = model_collide(&mc_hull_exit);
	} else {
		hull_exit_collision = 0;
	}

	mc_hull_enter.flags |= MC_CHECK_MODEL;
	hull_enter_collision = model_collide(&mc_hull_enter);
	// ---

    // If we have a range less than the "far" range, check if the ray actually hit within the range
    if (a_beam->range < BEAM_FAR_LENGTH
        && (shield_collision || hull_enter_collision || hull_exit_collision))
    {
        // We can't use hit_dist as "1" is the distance between p0 and p1
        float rangeSq = a_beam->range * a_beam->range;

        // actually make sure that the collision points are within range of our beam
        if (shield_collision && vm_vec_dist_squared(&a_beam->last_start, &mc_shield.hit_point_world) > rangeSq)
        {
            shield_collision = 0;
        }

        if (hull_enter_collision && vm_vec_dist_squared(&a_beam->last_start, &mc_hull_enter.hit_point_world) > rangeSq)
        {
            hull_enter_collision = 0;
        }

        if (hull_exit_collision && vm_vec_dist_squared(&mc_hull_exit.hit_point_world, &a_beam->last_start) > rangeSq)
        {
            hull_exit_collision = 0;
        }
    }

	
	if (hull_enter_collision || hull_exit_collision || shield_collision) {
		WarpEffect* warp_effect = nullptr;

		if (shipp->flags[Ship::Ship_Flags::Depart_warp] && shipp->warpout_effect != nullptr)
			warp_effect = shipp->warpout_effect;
		else if (shipp->flags[Ship::Ship_Flags::Arriving_stage_2] && shipp->warpin_effect != nullptr)
			warp_effect = shipp->warpin_effect;


		bool hull_no_collide, shield_no_collide;
		hull_no_collide = shield_no_collide = false;
		if (warp_effect != nullptr) {
			hull_no_collide = point_is_clipped_by_warp(&mc_hull_enter.hit_point_world, warp_effect);
			shield_no_collide = point_is_clipped_by_warp(&mc_shield.hit_point_world, warp_effect);
		}

		if (hull_no_collide)
			hull_enter_collision = hull_exit_collision = 0;
		if (shield_no_collide)
			shield_collision = 0;
	}

	// check shields for impact
	// (tooled ships are probably not going to be maintaining a shield over their exit hole,
	// therefore we need only check the entrance, just as with conventional weapons)
	if (!(ship_objp->flags[Object::Object_Flags::No_shields]))
	{
		// pick out the shield quadrant
		if (shield_collision)
			quadrant_num = get_quadrant(&mc_shield.hit_point, ship_objp);
		else if (hull_enter_collision && (sip->flags[Ship::Info_Flags::Surface_shields]))
			quadrant_num = get_quadrant(&mc_hull_enter.hit_point, ship_objp);

		// make sure that the shield is active in that quadrant
		if ((quadrant_num >= 0) && ((shipp->flags[Ship::Ship_Flags::Dying]) || !ship_is_shield_up(ship_objp, quadrant_num)))
			quadrant_num = -1;

		// see if we hit the shield
		if (quadrant_num >= 0)
		{
			// do the hit effect
			if (shield_collision) {
				if (mc_shield.shield_hit_tri != -1) {
					add_shield_point(OBJ_INDEX(ship_objp), mc_shield.shield_hit_tri, &mc_shield.hit_point, bwi->shield_impact_effect_radius);
				}
			} else {
				/* TODO */;
			}

			// if this weapon pierces the shield, then do the hit effect, but act like a shield collision never occurred;
			// otherwise, we have a valid hit on this shield
			if (bwi->wi_flags[Weapon::Info_Flags::Pierce_shields])
				quadrant_num = -1;
			else
				valid_hit_occurred = 1;
		}
	}

	// see which impact we use
	if (shield_collision && valid_hit_occurred)
	{
		mc = &mc_shield;
		Assert(quadrant_num >= 0);
	}
	else if (hull_enter_collision)
	{
		mc = &mc_hull_enter;
		valid_hit_occurred = 1;
	}
	else
		mc = nullptr;

	// if we got a hit
	if (valid_hit_occurred)
	{
		// since we might have two collisions handled the same way, let's loop over both of them
		mc_info *mc_array[2];
		int mc_size = 1;
		mc_array[0] = mc;
		if (hull_exit_collision)
		{
			mc_array[1] = &mc_hull_exit;
			++mc_size;
		}

		for (int i = 0; i < mc_size; ++i)
		{
			bool ship_override = false, weapon_override = false;

			// get submodel handle if scripting needs it
			bool has_submodel = (mc_array[i]->hit_submodel >= 0);
			scripting::api::submodel_h smh(mc_array[i]->model_num, mc_array[i]->hit_submodel);

			if (scripting::hooks::OnBeamCollision->isActive()) {
				ship_override = scripting::hooks::OnBeamCollision->isOverride(scripting::hooks::CollisionConditions{ {ship_objp, weapon_objp} },
					scripting::hook_param_list(scripting::hook_param("Self", 'o', ship_objp),
						scripting::hook_param("Object", 'o', weapon_objp),
						scripting::hook_param("Ship", 'o', ship_objp),
						scripting::hook_param("Beam", 'o', weapon_objp),
						scripting::hook_param("Hitpos", 'o', mc_array[i]->hit_point_world)));
			}

			if (scripting::hooks::OnShipCollision->isActive()) {
				weapon_override = scripting::hooks::OnShipCollision->isOverride(scripting::hooks::CollisionConditions{{ship_objp, weapon_objp}},
					scripting::hook_param_list(scripting::hook_param("Self", 'o', weapon_objp),
						scripting::hook_param("Object", 'o', ship_objp),
						scripting::hook_param("Ship", 'o', ship_objp),
						scripting::hook_param("Beam", 'o', weapon_objp),
						scripting::hook_param("Hitpos", 'o', mc_array[i]->hit_point_world),
						scripting::hook_param("ShipSubmodel", 'o', scripting::api::l_Submodel.Set(smh), has_submodel)));
			}

			if (!ship_override && !weapon_override)
			{
				// add to the collision_list
				// if we got "tooled", add an exit hole too
				beam_add_collision(a_beam, ship_objp, mc_array[i], quadrant_num, i != 0);
			}

			if (scripting::hooks::OnBeamCollision->isActive() && !(weapon_override && !ship_override)) {
				scripting::hooks::OnBeamCollision->run(scripting::hooks::CollisionConditions{ {ship_objp, weapon_objp} },
					scripting::hook_param_list(scripting::hook_param("Self", 'o', ship_objp),
						scripting::hook_param("Object", 'o', weapon_objp),
						scripting::hook_param("Ship", 'o', ship_objp),
						scripting::hook_param("Beam", 'o', weapon_objp),
						scripting::hook_param("Hitpos", 'o', mc_array[i]->hit_point_world)));
			}
			if (scripting::hooks::OnShipCollision->isActive() && ((weapon_override && !ship_override) || (!weapon_override && !ship_override)))
			{
				scripting::hooks::OnShipCollision->run(scripting::hooks::CollisionConditions{{ship_objp, weapon_objp}},
					scripting::hook_param_list(scripting::hook_param("Self", 'o', weapon_objp),
						scripting::hook_param("Object", 'o', ship_objp),
						scripting::hook_param("Ship", 'o', ship_objp),
						scripting::hook_param("Beam", 'o', weapon_objp),
						scripting::hook_param("Hitpos", 'o', mc_array[i]->hit_point_world),
						scripting::hook_param("ShipSubmodel", 'o', scripting::api::l_Submodel.Set(smh), has_submodel)));
			}
		}
	}

	// reset timestamp to timeout immediately
	pair->next_check_time = timestamp(0);
		
	return 0;
}


// collide a beam with an asteroid, returns 1 if we can ignore all future collisions between the 2 objects
int beam_collide_asteroid(obj_pair *pair)
{
	beam * a_beam;
	int model_num;

	// bogus
	if(pair == NULL){
		return 0;
	}

	// get the beam
	Assert(pair->a->instance >= 0);
	Assert(pair->a->type == OBJ_BEAM);
	Assert(Beams[pair->a->instance].objnum == OBJ_INDEX(pair->a));
	a_beam = &Beams[pair->a->instance];

	// if the "warming up" timestamp has not expired
	if((a_beam->warmup_stamp != -1) || (a_beam->warmdown_stamp != -1)){
		return 0;
	}

	// if the beam is on "safety", don't collide with anything
	if(a_beam->flags & BF_SAFETY){
		return 0;
	}
	
	// if the colliding object is the shooting object, return 1 so this is culled
	if(pair->b == a_beam->objp){
		return 1;
	}	

	// try and get a model
	model_num = beam_get_model(pair->b);
	if(model_num < 0){
		Int3();
		return 1;
	}	

#ifndef NDEBUG
	Beam_test_ints++;
	Beam_test_ast++;
#endif

	// do the collision
	mc_info test_collide;
	test_collide.model_instance_num = -1;
	test_collide.model_num = model_num;
	test_collide.submodel_num = -1;
	test_collide.orient = &pair->b->orient;
	test_collide.pos = &pair->b->pos;
	test_collide.p0 = &a_beam->last_start;
	test_collide.p1 = &a_beam->last_shot;
	test_collide.flags = MC_CHECK_MODEL | MC_CHECK_RAY;
	model_collide(&test_collide);

	// if we got a hit
	if (test_collide.num_hits)
	{
		// add to the collision list
		bool weapon_override = false, asteroid_override = false;

		if (scripting::hooks::OnAsteroidCollision->isActive()) {
			weapon_override = scripting::hooks::OnAsteroidCollision->isOverride(scripting::hooks::CollisionConditions{ {pair->a, pair->b} },
				scripting::hook_param_list(scripting::hook_param("Self", 'o', pair->a),
					scripting::hook_param("Object", 'o', pair->b),
					scripting::hook_param("Asteroid", 'o', pair->b),
					scripting::hook_param("Beam", 'o', pair->a),
					scripting::hook_param("Hitpos", 'o', test_collide.hit_point_world)));
		}
		if (scripting::hooks::OnBeamCollision->isActive()) {
			asteroid_override = scripting::hooks::OnBeamCollision->isOverride(scripting::hooks::CollisionConditions{ {pair->a, pair->b} },
				scripting::hook_param_list(scripting::hook_param("Self", 'o', pair->b),
					scripting::hook_param("Object", 'o', pair->a),
					scripting::hook_param("Asteroid", 'o', pair->b),
					scripting::hook_param("Beam", 'o', pair->a),
					scripting::hook_param("Hitpos", 'o', test_collide.hit_point_world)));
		}

		if (!weapon_override && !asteroid_override)
		{
			beam_add_collision(a_beam, pair->b, &test_collide);
		}

		if (scripting::hooks::OnAsteroidCollision->isActive() && !(asteroid_override && !weapon_override)) {
			scripting::hooks::OnAsteroidCollision->run(scripting::hooks::CollisionConditions{ {pair->a, pair->b} },
				scripting::hook_param_list(scripting::hook_param("Self", 'o', pair->a),
					scripting::hook_param("Object", 'o', pair->b),
					scripting::hook_param("Asteroid", 'o', pair->b),
					scripting::hook_param("Beam", 'o', pair->a),
					scripting::hook_param("Hitpos", 'o', test_collide.hit_point_world)));
		}
		if (scripting::hooks::OnBeamCollision->isActive() && ((asteroid_override && !weapon_override) || (!asteroid_override && !weapon_override))) {
			scripting::hooks::OnBeamCollision->run(scripting::hooks::CollisionConditions{ {pair->a, pair->b} },
				scripting::hook_param_list(scripting::hook_param("Self", 'o', pair->b),
					scripting::hook_param("Object", 'o', pair->a),
					scripting::hook_param("Asteroid", 'o', pair->b),
					scripting::hook_param("Beam", 'o', pair->a),
					scripting::hook_param("Hitpos", 'o', test_collide.hit_point_world)));
		}

		return 0;
	}

	// reset timestamp to timeout immediately
	pair->next_check_time = timestamp(0);
		
	return 0;	
}

// collide a beam with a missile, returns 1 if we can ignore all future collisions between the 2 objects
int beam_collide_missile(obj_pair *pair)
{
	beam *a_beam;	
	int model_num;

	// bogus
	if(pair == NULL){
		return 0;
	}

	// get the beam
	Assert(pair->a->instance >= 0);
	Assert(pair->a->type == OBJ_BEAM);
	Assert(Beams[pair->a->instance].objnum == OBJ_INDEX(pair->a));
	a_beam = &Beams[pair->a->instance];

	// if the "warming up" timestamp has not expired
	if((a_beam->warmup_stamp != -1) || (a_beam->warmdown_stamp != -1)){
		return 0;
	}

	// if the beam is on "safety", don't collide with anything
	if(a_beam->flags & BF_SAFETY){
		return 0;
	}
	
	// don't collide if the beam and missile share their parent
	if (pair->b->parent_sig >= 0 && a_beam->objp && pair->b->parent_sig == a_beam->objp->signature) {
		return 1;
	}

	// try and get a model
	model_num = beam_get_model(pair->b);
	if(model_num < 0){
		return 1;
	}

#ifndef NDEBUG
	Beam_test_ints++;
#endif

	// do the collision
	mc_info test_collide;
	test_collide.model_instance_num = -1;
	test_collide.model_num = model_num;
	test_collide.submodel_num = -1;
	test_collide.orient = &pair->b->orient;
	test_collide.pos = &pair->b->pos;
	test_collide.p0 = &a_beam->last_start;
	test_collide.p1 = &a_beam->last_shot;
	test_collide.flags = MC_CHECK_MODEL | MC_CHECK_RAY;
	model_collide(&test_collide);

	// if we got a hit
	if(test_collide.num_hits)
	{
		// add to the collision list
		bool a_override = false, b_override = false;

		if (scripting::hooks::OnWeaponCollision->isActive()) {
			a_override = scripting::hooks::OnWeaponCollision->isOverride(scripting::hooks::CollisionConditions{ {pair->a, pair->b} },
				scripting::hook_param_list(scripting::hook_param("Self", 'o', pair->a),
					scripting::hook_param("Object", 'o', pair->b),
					scripting::hook_param("Weapon", 'o', pair->b),
					scripting::hook_param("Beam", 'o', pair->a),
					scripting::hook_param("Hitpos", 'o', test_collide.hit_point_world)));
		}
		if (scripting::hooks::OnBeamCollision->isActive()) {
			b_override = scripting::hooks::OnBeamCollision->isOverride(scripting::hooks::CollisionConditions{ {pair->a, pair->b} },
				scripting::hook_param_list(scripting::hook_param("Self", 'o', pair->b),
					scripting::hook_param("Object", 'o', pair->a),
					scripting::hook_param("Weapon", 'o', pair->b),
					scripting::hook_param("Beam", 'o', pair->a),
					scripting::hook_param("Hitpos", 'o', test_collide.hit_point_world)));
		}

		if(!a_override && !b_override)
		{
			beam_add_collision(a_beam, pair->b, &test_collide);
		}

		if (scripting::hooks::OnWeaponCollision->isActive() && !(b_override && !a_override)) {
			scripting::hooks::OnWeaponCollision->run(scripting::hooks::CollisionConditions{ {pair->a, pair->b} },
				scripting::hook_param_list(scripting::hook_param("Self", 'o', pair->a),
					scripting::hook_param("Object", 'o', pair->b),
					scripting::hook_param("Weapon", 'o', pair->b),
					scripting::hook_param("Beam", 'o', pair->a),
					scripting::hook_param("Hitpos", 'o', test_collide.hit_point_world)));
		}
		if (scripting::hooks::OnBeamCollision->isActive() && ((b_override && !a_override) || (!b_override && !a_override))) {
			scripting::hooks::OnBeamCollision->run(scripting::hooks::CollisionConditions{ {pair->a, pair->b} },
				scripting::hook_param_list(scripting::hook_param("Self", 'o', pair->b),
					scripting::hook_param("Object", 'o', pair->a),
					scripting::hook_param("Weapon", 'o', pair->b),
					scripting::hook_param("Beam", 'o', pair->a),
					scripting::hook_param("Hitpos", 'o', test_collide.hit_point_world)));
		}
	}

	// reset timestamp to timeout immediately
	pair->next_check_time = timestamp(0);

	return 0;
}

// collide a beam with debris, returns 1 if we can ignore all future collisions between the 2 objects
int beam_collide_debris(obj_pair *pair)
{	
	beam * a_beam;
	int model_num;

	// bogus
	if(pair == NULL){
		return 0;
	}

	if (reject_due_collision_groups(pair->a, pair->b))
		return 0;

	// get the beam
	Assert(pair->a->instance >= 0);
	Assert(pair->a->type == OBJ_BEAM);
	Assert(Beams[pair->a->instance].objnum == OBJ_INDEX(pair->a));
	a_beam = &Beams[pair->a->instance];

	// if the "warming up" timestamp has not expired
	if((a_beam->warmup_stamp != -1) || (a_beam->warmdown_stamp != -1)){
		return 0;
	}

	// if the beam is on "safety", don't collide with anything
	if(a_beam->flags & BF_SAFETY){
		return 0;
	}
	
	// if the colliding object is the shooting object, return 1 so this is culled
	if(pair->b == a_beam->objp){
		return 1;
	}	

	// try and get a model
	model_num = beam_get_model(pair->b);
	if(model_num < 0){
		return 1;
	}	

#ifndef NDEBUG
	Beam_test_ints++;
#endif

	// do the collision
	mc_info test_collide;
	test_collide.model_instance_num = -1;
	test_collide.model_num = model_num;
	test_collide.submodel_num = -1;
	test_collide.orient = &pair->b->orient;
	test_collide.pos = &pair->b->pos;
	test_collide.p0 = &a_beam->last_start;
	test_collide.p1 = &a_beam->last_shot;
	test_collide.flags = MC_CHECK_MODEL | MC_CHECK_RAY;
	model_collide(&test_collide);

	// if we got a hit
	if(test_collide.num_hits)
	{
		bool weapon_override = false, debris_override = false;

		if (scripting::hooks::OnDebrisCollision->isActive()) {
			weapon_override = scripting::hooks::OnWeaponCollision->isOverride(scripting::hooks::CollisionConditions{ {pair->a, pair->b} },
				scripting::hook_param_list(scripting::hook_param("Self", 'o', pair->a),
					scripting::hook_param("Object", 'o', pair->b),
					scripting::hook_param("Debris", 'o', pair->b),
					scripting::hook_param("Beam", 'o', pair->a),
					scripting::hook_param("Hitpos", 'o', test_collide.hit_point_world)));
		}
		if (scripting::hooks::OnBeamCollision->isActive()) {
			debris_override = scripting::hooks::OnBeamCollision->isOverride(scripting::hooks::CollisionConditions{ {pair->a, pair->b} },
				scripting::hook_param_list(scripting::hook_param("Self", 'o', pair->b),
					scripting::hook_param("Object", 'o', pair->a),
					scripting::hook_param("Debris", 'o', pair->b),
					scripting::hook_param("Beam", 'o', pair->a),
					scripting::hook_param("Hitpos", 'o', test_collide.hit_point_world)));
		}

		if(!weapon_override && !debris_override)
		{
			// add to the collision list
			beam_add_collision(a_beam, pair->b, &test_collide);
		}

		if (scripting::hooks::OnDebrisCollision->isActive() && !(debris_override && !weapon_override)) {
			scripting::hooks::OnWeaponCollision->run(scripting::hooks::CollisionConditions{ {pair->a, pair->b} },
				scripting::hook_param_list(scripting::hook_param("Self", 'o', pair->a),
					scripting::hook_param("Object", 'o', pair->b),
					scripting::hook_param("Debris", 'o', pair->b),
					scripting::hook_param("Beam", 'o', pair->a),
					scripting::hook_param("Hitpos", 'o', test_collide.hit_point_world)));
		}
		if (scripting::hooks::OnBeamCollision->isActive() && ((debris_override && !weapon_override) || (!debris_override && !weapon_override))) {
			scripting::hooks::OnBeamCollision->run(scripting::hooks::CollisionConditions{ {pair->a, pair->b} },
				scripting::hook_param_list(scripting::hook_param("Self", 'o', pair->b),
					scripting::hook_param("Object", 'o', pair->a),
					scripting::hook_param("Debris", 'o', pair->b),
					scripting::hook_param("Beam", 'o', pair->a),
					scripting::hook_param("Hitpos", 'o', test_collide.hit_point_world)));
		}
	}

	// reset timestamp to timeout immediately
	pair->next_check_time = timestamp(0);

	return 0;
}

// early-out function for when adding object collision pairs, return 1 if the pair should be ignored
int beam_collide_early_out(object *a, object *b)
{
	beam *bm;
	weapon_info *bwi;
		
	// get the beam
	Assert(a->instance >= 0);
	if(a->instance < 0){
		return 1;
	}
	Assert(a->type == OBJ_BEAM);
	if(a->type != OBJ_BEAM){
		return 1;
	}
	Assert(Beams[a->instance].objnum == OBJ_INDEX(a));
	if(Beams[a->instance].objnum != OBJ_INDEX(a)){
		return 1;
	}	
	bm = &Beams[a->instance];
	Assert(bm->weapon_info_index >= 0);
	if(bm->weapon_info_index < 0){
		return 1;
	}
	bwi = &Weapon_info[bm->weapon_info_index];

	// if the second object has an invalid instance, bail
	if(b->instance < 0){
		return 1;
	}

	if((vm_vec_dist(&bm->last_start, &b->pos)-b->radius) > bwi->b_info.range){
		return 1;
	}//if the object is too far away, don't bother trying to colide with it-Bobboau

	// baseline bails
	switch(b->type){
	case OBJ_SHIP:
		break;
	case OBJ_ASTEROID:
		// targeting lasers only hit ships
/*		if(bwi->b_info.beam_type == BEAM_TYPE_C){
			return 1;
		}*/
		break;
	case OBJ_DEBRIS:
		// targeting lasers only hit ships
/*		if(bwi->b_info.beam_type == BEAM_TYPE_C){
			return 1;
		}*/
		// don't ever collide with non hull pieces
		if(!Debris[b->instance].is_hull){
			return 1;
		}
		break;
	case OBJ_WEAPON:
		// targeting lasers only hit ships
/*		if(bwi->b_info.beam_type == BEAM_TYPE_C){
			return 1;
		}*/
		if(The_mission.ai_profile->flags[AI::Profile_Flags::Beams_damage_weapons]) {
			if((Weapon_info[Weapons[b->instance].weapon_info_index].weapon_hitpoints <= 0) && (Weapon_info[Weapons[b->instance].weapon_info_index].subtype == WP_LASER)) {
				return 1;
			}
		} else {
			// don't ever collide against laser weapons - duh
			if(Weapon_info[Weapons[b->instance].weapon_info_index].subtype == WP_LASER){
				return 1;
			}
		}
		break;
	}

	float beam_radius = bm->beam_collide_width * bm->current_width_factor * 0.5f;
	// do a cylinder-sphere collision test
	if (!fvi_cylinder_sphere_may_collide(&bm->last_start, &bm->last_shot,
		beam_radius, &b->pos, b->radius * 1.2f)) {
		return 1;
	}
	
	// don't cull
	return 0;
}

// add a collision to the beam for this frame (to be evaluated later)
// Goober5000 - erg.  Rearranged for clarity, and also to fix a bug that caused is_exit_collision to hardly ever be assigned,
// resulting in "tooled" ships taking twice as much damage (in a later function) as they should.
void beam_add_collision(beam *b, object *hit_object, mc_info *cinfo, int quadrant_num, bool exit_flag)
{
	beam_collision *bc = nullptr;
	int idx;

	// if we haven't reached the limit for beam collisions, just add it
	if (b->f_collision_count < MAX_FRAME_COLLISIONS) {
		bc = &b->f_collisions[b->f_collision_count++];
	}
	// otherwise, we've got to do some checking, ick. 
	// I guess we can always just remove the farthest item
	else {
		for (idx = 0; idx < MAX_FRAME_COLLISIONS; idx++) {
			if ((bc == nullptr) || (b->f_collisions[idx].cinfo.hit_dist > bc->cinfo.hit_dist))
				bc = &b->f_collisions[idx];
		}
	}

	if (bc == nullptr) {
		Int3();
		return;
	}

	// copy in
	bc->c_objnum = OBJ_INDEX(hit_object);
	bc->cinfo = *cinfo;
	bc->quadrant = quadrant_num;
	bc->is_exit_collision = exit_flag;

	// let the hud shield gauge know when Player or Player target is hit
	if (quadrant_num >= 0)
		hud_shield_quadrant_hit(hit_object, quadrant_num);
}

// sort collisions for the frame
bool beam_sort_collisions_func(const beam_collision &b1, const beam_collision &b2)
{
	return (b1.cinfo.hit_dist < b2.cinfo.hit_dist);
}

static std::unique_ptr<EffectHost> beam_hit_make_effect_host(const beam* b, const object* impacted_obj, int impacted_submodel, const vec3d* hitpos, const vec3d* local_hitpos) {
	vec3d beam_dir;
	vm_vec_normalized_dir(&beam_dir, &b->last_shot, &b->last_start);

	if (impacted_obj->type != OBJ_SHIP) {
		//Fall back to Vector. Since we don't have a ship, it's quite likely whatever we're hitting will immediately die, so don't try to attach a particle source.
		matrix beamOrientation;
		vm_vector_2_matrix_norm(&beamOrientation, &beam_dir);

		auto vector_host = std::make_unique<EffectHostVector>(*hitpos, beamOrientation, vmd_zero_vector);
		vector_host->setRadius(impacted_obj->radius);
		return vector_host;
	}
	else {
		vec3d local_norm;
		model_instance_global_to_local_dir(&local_norm, &beam_dir, object_get_model(impacted_obj), object_get_model_instance(impacted_obj), impacted_submodel, &impacted_obj->orient);

		matrix orient;
		vm_vector_2_matrix_norm(&orient, &local_norm);

		if (impacted_submodel < 0) {
			//Fall back to object
			return std::make_unique<EffectHostObject>(impacted_obj, *local_hitpos, orient);
		}
		else {
			//Full subobject
			return std::make_unique<EffectHostSubmodel>(impacted_obj, impacted_submodel, *local_hitpos, orient);
		}
	}
}

// handle a hit on a specific object
void beam_handle_collisions(beam *b)
{	
	int idx, s_idx;
	beam_collision r_coll[MAX_FRAME_COLLISIONS];
	int r_coll_count = 0;
	weapon_info *wi;
	float width;	

	// early out if we had no collisions
	if(b->f_collision_count <= 0){
		return;
	}

	// get beam weapon info
	if((b->weapon_info_index < 0) || (b->weapon_info_index >= weapon_info_size())){
		Int3();
		return;
	}
	wi = &Weapon_info[b->weapon_info_index];

	// get the width of the beam
	width = b->beam_collide_width * b->current_width_factor;

	// the first thing we need to do is sort the collisions, from closest to farthest
	std::sort(b->f_collisions, b->f_collisions + b->f_collision_count, beam_sort_collisions_func);

	float damage_time_mod = (flFrametime * 1000.0f) / i2fl(BEAM_DAMAGE_TIME);
	float real_damage = wi->damage * damage_time_mod;

	// now apply all collisions until we reach a ship which "stops" the beam or we reach the end of the list
	for(idx=0; idx<b->f_collision_count; idx++){	
		int model_num = -1;
		int apply_beam_physics = 0;
		int draw_effects = 1;
		int first_hit = 1;
		int target = b->f_collisions[idx].c_objnum;

		// if we have an invalid object
		if((target < 0) || (target >= MAX_OBJECTS)){
			continue;
		}

		// try and get a model to deal with		
		model_num = beam_get_model(&Objects[target]);
		if(model_num < 0){
			continue;
		}

		if (wi->wi_flags[Weapon::Info_Flags::Huge]) {
			if (Objects[target].type == OBJ_SHIP) {
				ship_type_info *sti;
				sti = ship_get_type_info(&Objects[target]);
				if (!sti || sti->flags[Ship::Type_Info_Flags::No_huge_impact_eff])
					draw_effects = 0;
			}
		}

		//Don't draw effects if we're in the cockpit of the hit ship
		if (Viewer_obj == &Objects[target])
			draw_effects = 0;

		// add to the recent collision list
		r_coll[r_coll_count].c_objnum = target;
		r_coll[r_coll_count].c_sig = Objects[target].signature;
		r_coll[r_coll_count].c_stamp = -1;
		r_coll[r_coll_count].cinfo = b->f_collisions[idx].cinfo;
		r_coll[r_coll_count].quadrant = -1;
		r_coll[r_coll_count].is_exit_collision = false;
		
		// if he was already on the recent collision list, copy his timestamp
		// also, be sure not to play the impact sound again.
		for(s_idx=0; s_idx<b->r_collision_count; s_idx++){
			if((r_coll[r_coll_count].c_objnum == b->r_collisions[s_idx].c_objnum) && (r_coll[r_coll_count].c_sig == b->r_collisions[s_idx].c_sig)){
				// timestamp
				r_coll[r_coll_count].c_stamp = b->r_collisions[s_idx].c_stamp;

				// don't play the impact sound again
				first_hit = 0;
			}
		}

		// if the physics timestamp has expired or is not set yet, apply physics
		if((r_coll[r_coll_count].c_stamp == -1) || timestamp_elapsed(r_coll[r_coll_count].c_stamp))
        {
            float time_compression = f2fl(Game_time_compression);
            float delay_time = i2fl(BEAM_DAMAGE_TIME) / time_compression;
            apply_beam_physics = 1;
            r_coll[r_coll_count].c_stamp = timestamp(fl2i(delay_time));
		}

		// increment collision count
		r_coll_count++;		

		// play the impact sound
		if ( first_hit && (wi->impact_snd.isValid()) ) {
			snd_play_3d( gamesnd_get_game_sound(wi->impact_snd), &b->f_collisions[idx].cinfo.hit_point_world, &Eye_position );
		}

		// KOMET_EXT -->

		// draw flash, explosion
		if (draw_effects &&
		    ((wi->piercing_impact_effect.isValid()) || (wi->flash_impact_weapon_expl_effect.isValid()))) {
			float rnd = frand();
			int do_expl = 0;
			if ((rnd < 0.2f || apply_beam_physics) && wi->impact_weapon_expl_effect.isValid()) {
				do_expl = 1;
			}
			vec3d temp_pos, temp_local_pos;
				
			vm_vec_sub(&temp_pos, &b->f_collisions[idx].cinfo.hit_point_world, &Objects[target].pos);
			vm_vec_rotate(&temp_local_pos, &temp_pos, &Objects[target].orient);

			vec3d worldNormal;
			if (Objects[target].type == OBJ_SHIP) {
				auto shipp = &Ships[Objects[target].instance];
				model_instance_local_to_global_dir(&worldNormal,
											  &b->f_collisions[idx].cinfo.hit_normal,
											  shipp->model_instance_num,
											  b->f_collisions[idx].cinfo.hit_submodel,
											  &Objects[target].orient);
			} else {
				// Just assume that we don't need to handle model subobjects here
				vm_vec_unrotate(&worldNormal, &b->f_collisions[idx].cinfo.hit_normal, &Objects[target].orient);
			}

			if (wi->flash_impact_weapon_expl_effect.isValid()) {
				auto particleSource = particle::ParticleManager::get()->createSource(wi->flash_impact_weapon_expl_effect);
				particleSource->setHost(beam_hit_make_effect_host(b, &Objects[target], b->f_collisions[idx].cinfo.hit_submodel, &b->f_collisions[idx].cinfo.hit_point_world, &b->f_collisions[idx].cinfo.hit_point));
				particleSource->setNormal(worldNormal);
				particleSource->setTriggerRadius(width);
				particleSource->finishCreation();
			}

			if(do_expl){
				auto particleSource = particle::ParticleManager::get()->createSource(wi->impact_weapon_expl_effect);
				particleSource->setHost(beam_hit_make_effect_host(b, &Objects[target], b->f_collisions[idx].cinfo.hit_submodel, &b->f_collisions[idx].cinfo.hit_point_world, &b->f_collisions[idx].cinfo.hit_point));
				particleSource->setNormal(worldNormal);
				particleSource->setTriggerRadius(width);
				particleSource->finishCreation();
			}

			if (wi->piercing_impact_effect.isValid()) {

				// get beam direction

				int ok_to_draw = 0;

				if (beam_will_tool_target(b, &Objects[target])) {
					ok_to_draw = 1;

					if (Objects[target].type == OBJ_SHIP) {
						ship *shipp = &Ships[Objects[target].instance];

						if (shipp->armor_type_idx != -1) {
							if (Armor_types[shipp->armor_type_idx].GetPiercingType(wi->damage_type_idx) == SADTF_PIERCING_RETAIL) {
								ok_to_draw = 0;
							}
						}
					}
				} else {
					ok_to_draw = 0;

					if (Objects[target].type == OBJ_SHIP) {
						float draw_limit, hull_pct;
						int dmg_type_idx, piercing_type;

						ship *shipp = &Ships[Objects[target].instance];

						hull_pct = Objects[target].hull_strength / shipp->ship_max_hull_strength;
						dmg_type_idx = wi->damage_type_idx;
						draw_limit = Ship_info[shipp->ship_info_index].piercing_damage_draw_limit;

						if (shipp->armor_type_idx != -1) {
							piercing_type = Armor_types[shipp->armor_type_idx].GetPiercingType(dmg_type_idx);
							if (piercing_type == SADTF_PIERCING_DEFAULT) {
								draw_limit = Armor_types[shipp->armor_type_idx].GetPiercingLimit(dmg_type_idx);
							} else if ((piercing_type == SADTF_PIERCING_NONE) || (piercing_type == SADTF_PIERCING_RETAIL)) {
								draw_limit = -1.0f;
							}
						}

						if ((draw_limit != -1.0f) && (hull_pct <= draw_limit))
							ok_to_draw = 1;
					}
				}

				if (ok_to_draw){
					// stream of fire for big ships
					if (width <= Objects[target].radius * BEAM_AREA_PERCENT) {
						auto particleSource = particle::ParticleManager::get()->createSource(wi->piercing_impact_effect);

						particleSource->setHost(beam_hit_make_effect_host(b, &Objects[target], b->f_collisions[idx].cinfo.hit_submodel, &b->f_collisions[idx].cinfo.hit_point_world, &b->f_collisions[idx].cinfo.hit_point));
						particleSource->setNormal(worldNormal);
						particleSource->setTriggerRadius(width);
						particleSource->finishCreation();
					}
				}
			}
			// <-- KOMET_EXT
		}

		if(!physics_paused){

			switch(Objects[target].type){
			case OBJ_DEBRIS:
			{
				vec3d force = b->last_shot - b->last_start; // Valathil - use the beam direction as the force direction (like a high pressure water jet)
				vm_vec_normalize(&force);
				force *= wi->mass * wi->beam_hit_curves.get_output(weapon_info::BeamHitCurveOutputs::MASS_MULT, std::forward_as_tuple(*b, Objects[target]), &b->modular_curves_instance);

				// hit the debris - the debris hit code takes care of checking for MULTIPLAYER_CLIENT, etc
				debris_hit(&Objects[target], &Objects[b->objnum], &b->f_collisions[idx].cinfo.hit_point_world, wi->damage, &force);
				break;
			}

			case OBJ_WEAPON:
				if (The_mission.ai_profile->flags[AI::Profile_Flags::Beams_damage_weapons]) {
					if (!(Game_mode & GM_MULTIPLAYER) || MULTIPLAYER_MASTER) {
						object *trgt = &Objects[target];

						if (trgt->hull_strength > 0) {
							float attenuation = 1.0f;
							if ((b->damage_threshold >= 0.0f) && (b->damage_threshold < 1.0f)) {
								float dist = vm_vec_dist(&b->f_collisions[idx].cinfo.hit_point_world, &b->last_start);
								float range = b->range;
								float atten_dist = range * b->damage_threshold;
								if ((range > dist) && (atten_dist < dist)) {
									attenuation = 1 - ((dist - atten_dist) / (range - atten_dist));
								}
							}

							float damage = real_damage * attenuation;

							int dmg_type_idx = wi->damage_type_idx;
							
							weapon_info* trgt_wip = &Weapon_info[Weapons[trgt->instance].weapon_info_index];
							if (trgt_wip->armor_type_idx != -1)
								damage = Armor_types[trgt_wip->armor_type_idx].GetDamage(damage, dmg_type_idx, 1.0f, true);

							trgt->hull_strength -= damage;

							if (trgt->hull_strength < 0) {
								Weapons[trgt->instance].weapon_flags.set(Weapon::Weapon_Flags::Destroyed_by_weapon);
								weapon_hit(trgt, NULL, &trgt->pos);
							}
						} else {
							if (!(Game_mode & GM_MULTIPLAYER) || MULTIPLAYER_MASTER) {
								Weapons[trgt->instance].weapon_flags.set(Weapon::Weapon_Flags::Destroyed_by_weapon);
								weapon_hit(&Objects[target], NULL, &Objects[target].pos);
							}
						}
						

					}
				} else {
					// detonate the missile
					Assert(Weapon_info[Weapons[Objects[target].instance].weapon_info_index].subtype == WP_MISSILE);

					if (!(Game_mode & GM_MULTIPLAYER) || MULTIPLAYER_MASTER) {
						Weapons[Objects[target].instance].weapon_flags.set(Weapon::Weapon_Flags::Destroyed_by_weapon);
						weapon_hit(&Objects[target], NULL, &Objects[target].pos);
					}
				}
				break;

			case OBJ_ASTEROID:
				// hit the asteroid
				if (!(Game_mode & GM_MULTIPLAYER) || MULTIPLAYER_MASTER) {
					vec3d force = b->last_shot - b->last_start; // Valathil - use the beam direction as the force direction (like a high pressure water jet)
					vm_vec_normalize(&force);
					force *= wi->mass * wi->beam_hit_curves.get_output(weapon_info::BeamHitCurveOutputs::MASS_MULT, std::forward_as_tuple(*b, Objects[target]), &b->modular_curves_instance);

					asteroid_hit(&Objects[target], &Objects[b->objnum], &b->f_collisions[idx].cinfo.hit_point_world, wi->damage, &force);
				}
				break;
			case OBJ_SHIP:	
				// hit the ship - again, the innards of this code handle multiplayer cases
				// maybe vaporize ship.
				//only apply damage if the collision is not an exit collision.  this prevents twice the damage from being done, although it probably be more realistic since two holes are being punched in the ship instead of one.
				if (!b->f_collisions[idx].is_exit_collision) {
					real_damage = beam_get_ship_damage(b, &Objects[target], &b->f_collisions[idx].cinfo.hit_point_world) * damage_time_mod;
					ship_apply_local_damage(&Objects[target], &Objects[b->objnum], &b->f_collisions[idx].cinfo.hit_point_world, real_damage, wi->damage_type_idx, b->f_collisions[idx].quadrant);
				}
				// if this is the first hit on the player ship. whack him
				if(apply_beam_physics)
				{
					beam_apply_whack(b, &Objects[target], &b->f_collisions[idx].cinfo.hit_point_world);
				}
				break;
			}		
		}				

		// if the radius of the target is somewhat close to the radius of the beam, "stop" the beam here
		// for now : if its smaller than about 1/3 the radius of the ship
		if(width <= (Objects[target].radius * BEAM_AREA_PERCENT) && !beam_will_tool_target(b, &Objects[target])){
			// set last_shot so we know where to properly draw the beam		
			b->last_shot = b->f_collisions[idx].cinfo.hit_point_world;
			Assert(is_valid_vec(&b->last_shot));		

			// done wif the beam
			break;
		}
	}

	// store the new recent collisions
	for(idx=0; idx<r_coll_count; idx++){
		b->r_collisions[idx] = r_coll[idx];
	}
	b->r_collision_count = r_coll_count;
}

// if it is legal for the beam to fire, or continue firing
int beam_ok_to_fire(beam *b)
{
	if (b->objp == NULL) {	// If we don't have a firing object, none of these checks make sense.
		return 1;
	}
	// if my own object is invalid, stop firing
	if (b->objp->signature != b->sig) {
		mprintf(("BEAM : killing beam because of invalid parent object SIGNATURE!\n"));
		return -1;
	}

	// if my own object is a ghost
	if (b->objp->type != OBJ_SHIP) {
		mprintf(("BEAM : killing beam because of invalid parent object TYPE!\n"));
		return -1;
	}	

	ship* shipp = &Ships[b->objp->instance];
	// targeting type beams are ok to fire all the time
	if (b->type == BeamType::TARGETING) {

		if (shipp->weapon_energy <= 0.0f ) {

			if ( OBJ_INDEX(Player_obj) == shipp->objnum && !(b->life_left>0.0f)) {
				extern void ship_maybe_do_primary_fail_sound_hud(bool depleted_energy);
				ship_maybe_do_primary_fail_sound_hud(true);
			}

			return 0;
		} else {
			return 1;
		}
	}

	if (b->subsys == NULL) {	// IF we don't have a firing turret, none of these checks make sense.
		return 1;
	}

	if (!(b->flags & BF_FORCE_FIRING)) {
		// if the shooting turret is destroyed	
		if (b->subsys->current_hits <= 0.0f) {
			mprintf(("BEAM : killing beam because turret has been destroyed!\n"));
			return -1;
		}
		
		// kill it if its disrupted
		if (ship_subsys_disrupted(b->subsys)) {
			return -1;
		}

		// if the beam will be firing out of its FOV, power it down
		vec3d aim_dir;
		vm_vec_sub(&aim_dir, &b->last_shot, &b->last_start);
		vm_vec_normalize(&aim_dir);

		if (!(b->flags & BF_IS_FIGHTER_BEAM)) {
			if (The_mission.ai_profile->flags[AI::Profile_Flags::Force_beam_turret_fov]) {
				vec3d turret_normal;
				model_instance_local_to_global_dir(&turret_normal, &b->subsys->system_info->turret_norm, Ships[b->objp->instance].model_instance_num, b->subsys->system_info->subobj_num, &b->objp->orient, true);

				if (!(turret_fov_test(b->subsys, &turret_normal, &aim_dir))) {
					nprintf(("BEAM", "BEAM : powering beam down because of FOV condition!\n"));
					return 0;
				}
			}
			else {
				vec3d turret_dir, turret_pos;
				beam_get_global_turret_gun_info(b->objp, b->subsys, &turret_pos, false, &turret_dir, true, nullptr, (b->flags & BF_IS_FIGHTER_BEAM) != 0);
				if (vm_vec_dot(&aim_dir, &turret_dir) < b->subsys->system_info->turret_fov) {
					nprintf(("BEAM", "BEAM : powering beam down because of FOV condition!\n"));
					return 0;
				}
			}
		}

		if (b->subsys->system_info->flags[Model::Subsystem_Flags::Turret_hull_check]) {
			int model_num = Ship_info[shipp->ship_info_index].model_num;
			mc_info hull_check;
			hull_check.model_instance_num = shipp->model_instance_num;
			hull_check.model_num = model_num;
			hull_check.orient = &b->objp->orient;
			hull_check.pos = &b->objp->pos;
			hull_check.p0 = &b->last_start;
			hull_check.p1 = &b->last_shot;
			hull_check.flags = MC_CHECK_MODEL | MC_CHECK_RAY;

			if (model_collide(&hull_check)) {
				nprintf(("BEAM", "BEAM : powering beam down because of hull check condition!\n"));
				return 0;
			}
		}
	}

	// ok to fire/continue firing
	return 1;
}

// apply a whack to a ship
void beam_apply_whack(beam *b, object *objp, vec3d *hit_point)
{
	weapon_info *wip;	
	ship *shipp;

	// sanity
	Assert((b != NULL) && (objp != NULL) && (hit_point != NULL));
	if((b == NULL) || (objp == NULL) || (hit_point == NULL)){
		return;
	}	
	Assert(b->weapon_info_index >= 0);
	wip = &Weapon_info[b->weapon_info_index];	
	Assert((objp != NULL) && (objp->type == OBJ_SHIP) && (objp->instance >= 0) && (objp->instance < MAX_SHIPS));
	if((objp == NULL) || (objp->type != OBJ_SHIP) || (objp->instance < 0) || (objp->instance >= MAX_SHIPS)){
		return;
	}
	shipp = &Ships[objp->instance];
	if((shipp->ai_index < 0) || (shipp->ai_index >= MAX_AI_INFO)){
		return;
	}

	// don't whack docked ships
	// Goober5000 - whacking docked ships should work now, so whack them
	// Goober5000 - weapons with no mass don't whack (bypass the calculations)
	if(wip->mass == 0.0f) {
		return;
	}

	// determine how big of a whack to apply
	float whack;

	// this if block was added by Bobboau to make beams whack properly while preserving reverse compatibility
	if(wip->mass == 100.0f){
		if(wip->damage < b_whack_damage){
			whack = b_whack_small;
		} else {
			whack = b_whack_big;
		}
	}else{
		whack = wip->mass * wip->beam_hit_curves.get_output(weapon_info::BeamHitCurveOutputs::MASS_MULT, std::forward_as_tuple(*b, *objp), &b->modular_curves_instance);
	}

	// whack direction
	vec3d whack_dir;
	vm_vec_sub(&whack_dir, &b->last_shot, &b->last_start); // Valathil - use the beam direction as the force direction (like a high pressure water jet)
	vm_vec_normalize(&whack_dir);
	vm_vec_scale(&whack_dir, whack);

	// apply the whack
	ship_apply_whack(&whack_dir, hit_point, objp);
}

// return the amount of damage which should be applied to a ship. basically, filters friendly fire damage 
float beam_get_ship_damage(beam *b, object *objp, vec3d* hitpos)
{	
	// if the beam is on the same team as the object
	if ( (objp == NULL) || (b == NULL) ) {
		Int3();
		return 0.0f;
	}

	if ( (objp->type != OBJ_SHIP) || (objp->instance < 0) || (objp->instance >= MAX_SHIPS) ) {
		Int3();
		return 0.0f;
	}

	weapon_info *wip = &Weapon_info[b->weapon_info_index];

	if (wip->damage <= 0)
		return 0.0f;	// Not much point in calculating the attenuation if the beam doesn't hurt in the first place.

	float attenuation = 1.0f;

	if ((b->damage_threshold >= 0.0f) && (b->damage_threshold < 1.0f)) {
		float dist = hitpos ? vm_vec_dist(hitpos, &b->last_start) : 0.0f;
		float range = b->range;
		float atten_dist = range * b->damage_threshold;
		if ((range > dist) && (atten_dist < dist)) {
			attenuation =  1 - ((dist - atten_dist) / (range - atten_dist));
		}
	}

	float damage = wip->damage;
	damage *= wip->beam_hit_curves.get_output(weapon_info::BeamHitCurveOutputs::DAMAGE_MULT, std::forward_as_tuple(*b, *objp), &b->modular_curves_instance);

	// same team. yikes
	if ( (b->team == Ships[objp->instance].team)
			&& (The_mission.ai_profile->beam_friendly_damage_cap[Game_skill_level] >= 0.f)
			&& (damage > The_mission.ai_profile->beam_friendly_damage_cap[Game_skill_level]) ) {
		damage = The_mission.ai_profile->beam_friendly_damage_cap[Game_skill_level] * attenuation;
	} else {
		// normal damage
		damage *= attenuation;
	}

	return damage;
}

// if the beam is likely to tool a given target before its lifetime expires
int beam_will_tool_target(beam *b, object *objp)
{
	weapon_info *wip = &Weapon_info[b->weapon_info_index];
	float total_strength, damage_in_a_few_seconds, hp_limit, hp_pct;
	
	// sanity
	if(objp == NULL){
		return 0;
	}

	// if the object is not a ship, bail
	if(objp->type != OBJ_SHIP){
		return 0;
	}
	if((objp->instance < 0) || (objp->instance >= MAX_SHIPS)){
		return 0;
	}
	
	ship *shipp = &Ships[objp->instance];
	total_strength = objp->hull_strength;

	if (shipp->armor_type_idx != -1) {
		if (Armor_types[shipp->armor_type_idx].GetPiercingType(wip->damage_type_idx) == SADTF_PIERCING_NONE) {
			return 0;
		}
		hp_limit = Armor_types[shipp->armor_type_idx].GetPiercingLimit(wip->damage_type_idx);
		if (hp_limit > 0.0f) {
			hp_pct = total_strength / shipp->ship_max_hull_strength;
			if (hp_limit >= hp_pct)
				return 1;
		}
	}

	// calculate total strength, factoring in shield
	if (!(wip->wi_flags[Weapon::Info_Flags::Pierce_shields]))
		total_strength += shield_get_strength(objp);

	// if the beam is going to apply more damage in about 1 and a half than the ship can take
	damage_in_a_few_seconds = (TOOLTIME / (float)BEAM_DAMAGE_TIME) * wip->damage;
	return (damage_in_a_few_seconds > total_strength);
}

float beam_get_warmup_lifetime_pct(const beam& b) {
	return BEAM_WARMUP_PCT((&b));
}

float beam_get_warmdown_lifetime_pct(const beam& b) {
	return BEAM_WARMDOWN_PCT((&b));
}

float beam_get_warmdown_age(const beam& b) {
	return ((float)timestamp_since(b.warmdown_stamp)) / MILLISECONDS_PER_SECOND;
}

float beam_accuracy = 1.0f;
DCF(b_aim, "Adjusts the beam accuracy factor (Default is 1.0f)")
{
	dc_stuff_float(&beam_accuracy);
}
DCF(beam_list, "Lists all beams")
{
	int b_count = 0;

	for (auto &wi : Weapon_info) {
		if (wi.wi_flags[Weapon::Info_Flags::Beam]) {
			++b_count;
			dc_printf("Beam %d : %s\n", b_count, wi.name);
		}
	}
}
