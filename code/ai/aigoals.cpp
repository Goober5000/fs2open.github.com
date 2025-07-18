/*
 * Copyright (C) Volition, Inc. 1999.  All rights reserved.
 *
 * All source code herein is the property of Volition, Inc. You may not sell 
 * or otherwise commercially exploit the source or things you created based on the 
 * source.
 *
*/




#include "ai/aigoals.h"
#include "ai/ailua.h"
#include "globalincs/linklist.h"
#include "globalincs/utility.h"
#include "mission/missionlog.h"
#include "mission/missionparse.h"
#include "network/multi.h"
#include "network/multimsgs.h"
#include "object/object.h"
#include "object/objectdock.h"
#include "object/waypoint.h"
#include "parse/sexp.h"
#include "parse/sexp/LuaAISEXP.h"
#include "playerman/player.h"
#include "scripting/global_hooks.h"
#include "scripting/scripting.h"
#include "ship/ship.h"
#include "ship/awacs.h"
#include "weapon/weapon.h"

// Just a reference to this struct being for looking up a ship's team in TVT
struct ship_registry_entry;

// all ai goals dealt with in this code are goals that are specified through
// sexpressions in the mission file.  They are either specified as part of a
// ships goals in the #Object section of the mission file, or are created (and
// removed) dynamically using the #Events section.  Default goal behaviour and
// dynamic goals are not handled here.

// defines for player issued goal priorities
#define PLAYER_PRIORITY_MIN				90
#define PLAYER_PRIORITY_SHIP				100
#define PLAYER_PRIORITY_WING				95
#define PLAYER_PRIORITY_SUPPORT_LOW		10

#define MAX_GOAL_PRIORITY				200

// define for which goals cause other goals to get purged
// Goober5000 - okay, this seems really stupid.  If any ship in the mission is assigned a goal
// in PURGE_GOALS_ALL_SHIPS, *every* other ship will have certain goals purged.  So I added
// PURGE_GOALS_ONE_SHIP for goals which should only purge other goals in the one ship.
// Goober5000 - note that the new disable and disarm goals (AI_GOAL_DISABLE_SHIP_TACTICAL and
// AI_GOAL_DISARM_SHIP_TACTICAL) do not purge ANY goals, not even the ones in the one ship
inline bool purge_goals_all_ships(ai_goal_mode ai_mode)
{
	return ai_mode == AI_GOAL_IGNORE || ai_mode == AI_GOAL_DISABLE_SHIP || ai_mode == AI_GOAL_DISARM_SHIP;
}
inline bool purge_goals_one_ship(ai_goal_mode ai_mode)
{
	return ai_mode == AI_GOAL_IGNORE_NEW;
}

// goals given from the player to other ships in the game are also handled in this
// code

int	Ai_goal_signature = 0;
int	Num_ai_dock_names = 0;
char	Ai_dock_names[MAX_AI_DOCK_NAMES][NAME_LENGTH];

// Used in objecttypes.tbl to define custom ship types
ai_goal_list Ai_goal_names[] =
{
	{ "Attack ship",			AI_GOAL_CHASE,					0 },
	{ "Dock",					AI_GOAL_DOCK,					0 },
	{ "Waypoints",				AI_GOAL_WAYPOINTS,				0 },
	{ "Waypoints once",			AI_GOAL_WAYPOINTS_ONCE,			0 },
	{ "Depart",					AI_GOAL_WARP,					0 },
	{ "Attack subsys",			AI_GOAL_DESTROY_SUBSYSTEM,		0 },
	{ "Form on wing",			AI_GOAL_FORM_ON_WING,			0 },
	{ "Undock",					AI_GOAL_UNDOCK,					0 },
	{ "Attack wing",			AI_GOAL_CHASE_WING,				0 },
	{ "Guard ship",				AI_GOAL_GUARD,					0 },
	{ "Disable ship",			AI_GOAL_DISABLE_SHIP,			0 },
	{ "Disable ship (tactical)",AI_GOAL_DISABLE_SHIP_TACTICAL,	0 },
	{ "Disarm ship",			AI_GOAL_DISARM_SHIP,			0 },
	{ "Disarm ship (tactical)",	AI_GOAL_DISARM_SHIP_TACTICAL,	0 },
	{ "Attack any",				AI_GOAL_CHASE_ANY,				0 },
	{ "Ignore ship",			AI_GOAL_IGNORE,					0 },
	{ "Ignore ship (new)",		AI_GOAL_IGNORE_NEW,				0 },
	{ "Guard wing",				AI_GOAL_GUARD_WING,				0 },
	{ "Evade ship",				AI_GOAL_EVADE_SHIP,				0 },
	{ "Stay near ship",			AI_GOAL_STAY_NEAR_SHIP,			0 },
	{ "Keep safe dist",			AI_GOAL_KEEP_SAFE_DISTANCE,		0 },
	{ "Rearm ship",				AI_GOAL_REARM_REPAIR,			0 },
	{ "Stay still",				AI_GOAL_STAY_STILL,				0 },
	{ "Play dead",				AI_GOAL_PLAY_DEAD,				0 },
	{ "Play dead (persistent)",	AI_GOAL_PLAY_DEAD_PERSISTENT,	0 },
	{ "Attack weapon",			AI_GOAL_CHASE_WEAPON,			0 },
	{ "Fly to ship",			AI_GOAL_FLY_TO_SHIP,			0 },
	{ "Attack ship class",		AI_GOAL_CHASE_SHIP_CLASS,		0 },
};

int Num_ai_goals = sizeof(Ai_goal_names) / sizeof(ai_goal_list);

// AL 11-17-97: A text description of the AI goals.  This is used for printing out on the
// HUD what a ship's current orders are.  If the AI goal doesn't correspond to something that
// ought to be printable, then NULL is used.
// JAS: Converted to a function in order to externalize the strings
const char *Ai_goal_text(ai_goal_mode goal, int submode)
{
	switch(goal)	{
	case AI_GOAL_CHASE:
	case AI_GOAL_CHASE_WING:
	case AI_GOAL_CHASE_SHIP_CLASS:
		return XSTR( "attack ", 474);
	case AI_GOAL_DOCK:
		return XSTR( "dock ", 475);
	case AI_GOAL_WAYPOINTS:
	case AI_GOAL_WAYPOINTS_ONCE:
		return XSTR( "waypoints", 476);
	case AI_GOAL_DESTROY_SUBSYSTEM:
		return XSTR( "destroy ", 477);
	case AI_GOAL_FORM_ON_WING:
		return XSTR( "form on ", 478);
	case AI_GOAL_UNDOCK:
		return XSTR( "undock ", 479);
	case AI_GOAL_GUARD:
	case AI_GOAL_GUARD_WING:
		return XSTR( "guard ", 480);
	case AI_GOAL_DISABLE_SHIP:
	case AI_GOAL_DISABLE_SHIP_TACTICAL:
		return XSTR( "disable ", 481);
	case AI_GOAL_DISARM_SHIP:
	case AI_GOAL_DISARM_SHIP_TACTICAL:
		return XSTR( "disarm ", 482);
	case AI_GOAL_EVADE_SHIP:
		return XSTR( "evade ", 483);
	case AI_GOAL_REARM_REPAIR:
		return XSTR( "rearm ", 484);
	case AI_GOAL_FLY_TO_SHIP:
		return XSTR( "rendezvous with ", 1597);
	case AI_GOAL_LUA:	{
		auto mode = ai_lua_find_mode(submode);
		if (mode == nullptr)
			return nullptr;
		return mode->hudText;
	}
	default:
		return nullptr;
	}
}

/**
 * Reset all fields to their uninitialized defaults.  But if this is being called before adding a new goal, the function will set the correct signature and time.
 * Similarly the added mode, submode, and type can be assigned.
 */
void ai_goal_reset(ai_goal *aigp, bool adding_goal, ai_goal_mode ai_mode, int ai_submode, ai_goal_type type)
{
	if (ai_mode != AI_GOAL_NONE)
		Assertion(adding_goal, "If a goal mode is being assigned, the adding_goal parameter must be true so that the signature and mission time can be set");

	aigp->signature = adding_goal ? Ai_goal_signature++ : -1;
	aigp->ai_mode = ai_mode;
	aigp->ai_submode = ai_submode;
	aigp->type = type;
	aigp->flags.reset();		// must reset the flags since not doing so will screw up goal sorting.
	aigp->time = adding_goal ? Missiontime : 0;
	aigp->priority = -1;

	aigp->target_name = nullptr;
	aigp->target_name_index = -1;

	aigp->wp_list_index = -1;

	aigp->target_instance = -1;
	aigp->target_signature = -1;

	aigp->int_data = 0;
	aigp->float_data = 0.0f;

	aigp->docker.name = nullptr;
	aigp->dockee.name = nullptr;

	aigp->lua_ai_target.target.clear();
	aigp->lua_ai_target.arguments.clear();
}

void ai_maybe_add_form_goal(wing* wingp)
{
	// Cyborg17 - Changes from the client would just get overridden by the server anyway
	// might as well keep them more closely in sync.
	if (MULTIPLAYER_CLIENT) {
		return;
	}

	int j;

	// iterate through the ship_index list of this wing and check for orders.  We will do
	// this for all ships in the wing instead of on a wing only basis in case some ships
	// in the wing actually have different orders than others
	for (j = 0; j < wingp->current_count; j++) {
		ai_info* aip;

		Assert(wingp->ship_index[j] != -1);						// get Allender

		aip = &Ai_info[Ships[wingp->ship_index[j]].ai_index];
		// don't process a Player_ship
		if (Objects[Ships[aip->shipnum].objnum].flags[Object::Object_Flags::Player_ship]) {
			continue;
		}

		// need to add a form on my wing goal here.  Ships are always forming on the player's wing.
		// it is sufficient enough to check the first goal entry to see if it has a valid goal
		if (aip->goals[0].ai_mode == AI_GOAL_NONE) {
			// Need to have a more specific target in multi, or they may end up trying to target standalone placeholder.
			// So form on their team leader.  In dogfight, all player-slot ai die, so just exclude.
			if (MULTIPLAYER_MASTER && !(Netgame.type_flags & NG_TYPE_DOGFIGHT)) {
				int wingnum;
				if (Netgame.type_flags & NG_TYPE_TEAM) {
					const ship_registry_entry* ship_regp = ship_registry_get(Ships[wingp->ship_index[j]].ship_name);
					wingnum = TVT_wings[ship_regp->p_objp()->team];
					ai_add_ship_goal_player(ai_goal_type::PLAYER_SHIP, AI_GOAL_FORM_ON_WING, -1, Ships[Wings[wingnum].ship_index[Wings[wingnum].special_ship]].ship_name, aip);
				} else {
					wingnum = Starting_wings[0];
					ai_add_ship_goal_player(ai_goal_type::PLAYER_SHIP, AI_GOAL_FORM_ON_WING, -1, Ships[Wings[wingnum].ship_index[Wings[wingnum].special_ship]].ship_name, aip);
				}
			} else if (!(Game_mode & GM_MULTIPLAYER)) {
				ai_add_ship_goal_player(ai_goal_type::PLAYER_SHIP, AI_GOAL_FORM_ON_WING, -1, Player_ship->ship_name, aip);
			}
		}
	}
}

void ai_post_process_mission()
{
	object *objp;
	int i;

	// make sure team visibility is updated first
	if ( !Fred_running ) {
		awacs_process();
	}

	// Check ships in player starting wings.  Those ships should follow these rules:
	// (1) if they have no orders, they should get a form on my wing order
	// (2) if they have an order, they are free to act on it.
	//
	// So basically, we are checking for (1)
	if ( !Fred_running ) {
		//	MK, 5/9/98: Used to iterate through MAX_STARTING_WINGS, but this was too many ships forming on player.
		// Goober5000 - MK originally iterated on only the first wing; now we iterate on only the player wing
		// because the player wing may not be first
		for ( i = 0; i < MAX_STARTING_WINGS; i++ ) {	
			if (Starting_wings[i] >= 0 && Starting_wings[i] == Player_ship->wingnum) {
				wing *wingp;

				wingp = &Wings[Starting_wings[i]];

				ai_maybe_add_form_goal( wingp );
			}
		}
	}

	// for every valid ship object, call process_mission_orders to be sure that ships start the
	// mission following the orders in the mission file right away instead of waiting N seconds
	// before following them.  Do both the created list and the object list for safety
	for ( objp = GET_FIRST(&obj_used_list); objp != END_OF_LIST(&obj_used_list); objp = GET_NEXT(objp) ) {
		if (objp->flags[Object::Object_Flags::Should_be_dead])
			continue;
		if ( objp->type != OBJ_SHIP )
			continue;
		ai_process_mission_orders( OBJ_INDEX(objp), &Ai_info[Ships[objp->instance].ai_index] );
	}
	for ( objp = GET_FIRST(&obj_create_list); objp != END_OF_LIST(&obj_create_list); objp = GET_NEXT(objp) ) {
		if ( (objp->type != OBJ_SHIP) || Fred_running )
			continue;
		ai_process_mission_orders( OBJ_INDEX(objp), &Ai_info[Ships[objp->instance].ai_index] );
	}

	return;
}

/**
 * Determines if a goal is valid for a particular type of ship
 *
 * @param ship Ship type to test
 * @param ai_mode Goal type to test
 */
int ai_query_goal_valid( int ship, ai_goal_mode ai_mode )
{
	int accepted;

	if (ai_mode == AI_GOAL_NONE || ai_mode == AI_GOAL_LUA)
		return 1;  // anything can have no orders or Lua'd orders.

	accepted = 0;

	//WMC - This is much simpler with shiptypes.tbl
	//Except you have to add the orders into it by hand.
	int ship_type = Ship_info[Ships[ship].ship_info_index].class_type;
	if(ship_type > -1)
	{
		const auto &set = Ship_types[ship_type].ai_valid_goals;
		if (set.find(ai_mode) != set.end()) {
			accepted = 1;
		}
	}

	return accepted;
}

// remove an ai goal from it's list.  Uses the active_goal member as the goal to remove
void ai_remove_ship_goal( ai_info *aip, int index )
{
	// only need to set the ai_mode for the particular goal to AI_GOAL_NONE
	// reset ai mode to default behavior.  Might get changed next time through
	// ai goal code look
	Assert ( index >= 0 );			// must have a valid goal

	if (index == aip->active_goal)
	{
		auto shipp = &Ships[aip->shipnum];

		// rearm/repair needs a bit of extra cleanup
		if (aip->goals[index].ai_mode == AI_GOAL_REARM_REPAIR)
			ai_abort_rearm_request(&Objects[shipp->objnum]);

		// play dead needs some extra cleanup, too
		if (aip->goals[index].ai_mode == AI_GOAL_PLAY_DEAD || aip->goals[index].ai_mode == AI_GOAL_PLAY_DEAD_PERSISTENT)
		{
			// wookieejedi - there is an early return for the mode AIM_PLAY_DEAD in AI frame, so it needs to be set back to AIM_NONE
			if (aip->ai_profile_flags[AI::Profile_Flags::Fixed_removing_play_dead_order])
			{
				aip->mode = AIM_NONE;
				aip->submode_start_time = Missiontime;
			}

			// Goober5000 - any ship subsystems will start moving now, so their initial velocity should be 0 to match original behavior
			for (auto pss = GET_FIRST(&shipp->subsys_list); pss != END_OF_LIST(&shipp->subsys_list); pss = GET_NEXT(pss))
			{
				if (pss->submodel_instance_1)
				{
					pss->submodel_instance_1->current_turn_rate = 0.0f;
					pss->submodel_instance_1->current_shift_rate = 0.0f;
				}
			}
		}

		aip->active_goal = AI_ACTIVE_GOAL_NONE;
	}

	ai_goal_reset(&aip->goals[index]);

	// mwa -- removed this line 8/5/97.  Just because we remove a goal doesn't mean to do the default
	// behavior.  We will make the call commented out below in a more reasonable location
	//ai_do_default_behavior( &Objects[Ships[aip->shipnum].objnum] );
}

void ai_clear_ship_goals( ai_info *aip )
{
	int i;

	for (i = 0; i < MAX_AI_GOALS; i++)
		ai_remove_ship_goal( aip, i );			// resets active_goal and default behavior

	aip->active_goal = AI_ACTIVE_GOAL_NONE;					// for good measure

	// next line moved here on 8/5/97 by MWA
	// Don't reset player ai (and hence target)
	// Goober5000 - account for player ai
	//if ( !((Player_ship != NULL) && (&Ships[aip->shipnum] == Player_ship)) || Player_use_ai ) {
	if ( (Player_ship == NULL) || (&Ships[aip->shipnum] != Player_ship) || Player_use_ai )
	{
		ai_do_default_behavior( &Objects[Ships[aip->shipnum].objnum] );
	}

	// add scripting hook for 'On Goals Cleared' --wookieejedi
	if (scripting::hooks::OnGoalsCleared->isActive()) {
		scripting::hooks::OnGoalsCleared->run(scripting::hooks::ShipSourceConditions{ &Ships[aip->shipnum] },
			scripting::hook_param_list(scripting::hook_param("Ship", 'o', &Objects[Ships[aip->shipnum].objnum])));
	}
}

void ai_clear_wing_goals( wing *wingp )
{
	int i;

	// clear the goals for all ships in the wing
	for (i = 0; i < wingp->current_count; i++) {
		int num = wingp->ship_index[i];

		if ( num > -1 )
			ai_clear_ship_goals( &Ai_info[Ships[num].ai_index] );

	}

	// clear out the goals for the wing now
	for (i = 0; i < MAX_AI_GOALS; i++) {
		ai_goal_reset(&wingp->ai_goals[i]);
	}
}

// routine which marks a wing goal as being complete.  We get the wingnum and a pointer to the goal
// structure of the goal to be removed.  This process is slightly tricky since some member of the wing
// might be pursuing a different goal.  We will have to compare based on mode, submode, priority,
// and name.. This routine is only currently called from waypoint code!!!
void ai_mission_wing_goal_complete( int wingnum, ai_goal *remove_goalp )
{
	int mode, submode, priority, i;
	const char *name;
	ai_goal *aigp;
	wing *wingp;

	wingp = &Wings[wingnum];

	// set up locals for faster access.
	mode = remove_goalp->ai_mode;
	submode = remove_goalp->ai_submode;
	priority = remove_goalp->priority;
	name = remove_goalp->target_name;

	Assert ( name );			// should not be NULL!!!!

	// remove the goal from all the ships currently in the wing
	for (i = 0; i < wingp->current_count; i++ ) {
		int num, j;
		ai_info *aip;

		num = wingp->ship_index[i];
		Assert ( num >= 0 );
		aip = &Ai_info[Ships[num].ai_index];
		for ( j = 0; j < MAX_AI_GOALS; j++ ) {
			aigp = &(aip->goals[j]);

			// don't need to worry about these types of goals since they can't possibly be a goal we are looking for.
			if ( (aigp->ai_mode == AI_GOAL_NONE) || !aigp->target_name )
				continue;

			if ( (aigp->ai_mode == mode) && (aigp->ai_submode == submode) && (aigp->priority == priority) && !stricmp(name, aigp->target_name) ) {
				ai_remove_ship_goal( aip, j );
				ai_do_default_behavior( &Objects[Ships[aip->shipnum].objnum] );		// do the default behavior
				break;			// we are all done
			}
		}
	}

	// now remove the goal from the wing
	for (i = 0; i < MAX_AI_GOALS; i++ ) {
		aigp = &(wingp->ai_goals[i]);
		if ( (aigp->ai_mode == AI_GOAL_NONE) || !aigp->target_name )
			continue;

		if ( (aigp->ai_mode == mode) && (aigp->ai_submode == submode) && (aigp->priority == priority) && !stricmp(name, aigp->target_name) ) {
			ai_goal_reset(aigp);
			break;
		}
	}
			
}

// routine which is called with an ai object complete it's goal.  Do some action
// based on the goal what was just completed

void ai_mission_goal_complete( ai_info *aip )
{
	// if the active goal is dynamic or none, just return.
	if ( (aip->active_goal == AI_ACTIVE_GOAL_NONE) || (aip->active_goal == AI_ACTIVE_GOAL_DYNAMIC) )
		return;

	ai_remove_ship_goal( aip, aip->active_goal );
	ai_do_default_behavior( &Objects[Ships[aip->shipnum].objnum] );		// do the default behavior

}

// function to prune out goals which are no longer valid, based on a goal pointer passed in.
// for instance, if we get passed a goal of "disable X", then any goals in the given goal array
// which are destroy, etc, should get removed.  goal list is the list of goals to purge.  It is
// always MAX_AI_GOALS in length.  This function will only get called when the goal which causes
// purging becomes valid.
void ai_goal_purge_invalid_goals( ai_goal *aigp, ai_goal *goal_list, ai_info *aip, int ai_wingnum )
{
	int i, j;
	ai_goal *purge_goal;
	const char *name;
	int mode, ship_index, wingnum;

	// get locals for easer access
	name = aigp->target_name;
	mode = aigp->ai_mode;

	// these goals cannot be associated to wings, but can to a ship in a wing.  So, we should find out
	// if the ship is in a wing so we can purge goals which might operate on that wing
	ship_index = ship_name_lookup(name);

	Assertion(ship_index > -1, "Found a bad ship_index of %d in ai_goal_purge_invalid_goals, please report to the SCP!", ship_index); // get allender -- this is sort of odd
	if ( ship_index < 0 ) {					
		return; 
	}

	wingnum = Ships[ship_index].wingnum;

	purge_goal = goal_list;
	for ( i = 0; i < MAX_AI_GOALS; purge_goal++, i++ ) {
		auto purge_ai_mode = purge_goal->ai_mode;

		// don't need to process AI_GOAL_NONE
		if ( purge_ai_mode == AI_GOAL_NONE )
			continue;

		// goals must operate on something to be purged.
		if ( purge_goal->target_name == NULL )
			continue;

		// goals operating on ship classes are handled slightly differently
		if ( purge_ai_mode == AI_GOAL_CHASE_SHIP_CLASS ) {
			// if the target of the purge goal is the same class of ship we are concerned about, then we have a match;
			// if it is not, then we can continue (see standard ship check below)
			if ( stricmp(purge_goal->target_name, Ship_info[Ships[ship_index].ship_info_index].name) != 0 )
				continue;
		}
		// standard goals operating on either wings or ships
		else {
			// determine if the purge goal is acting either on the ship or the ship's wing.
			int purge_wing = wing_name_lookup( purge_goal->target_name, 1 );

			// if the target of the purge goal is a ship (purge_wing will be -1), then if the names
			// don't match, we can continue;  if the wing is valid, don't process if the wing numbers
			// are different.
			if ( purge_wing == -1 ) {
				if ( stricmp(purge_goal->target_name, name ) != 0 )
					continue;
			} else if ( purge_wing != wingnum )
				continue;
		}

		switch (mode)
		{
			// ignore goals should get rid of any kind of attack goal
			case AI_GOAL_IGNORE:
			case AI_GOAL_IGNORE_NEW:
				if (ai_goal_is_disable_or_disarm(purge_ai_mode) || ai_goal_is_specific_chase(purge_ai_mode) || (purge_ai_mode == AI_GOAL_DESTROY_SUBSYSTEM))
					purge_goal->flags.set(AI::Goal_Flags::Purge);
				break;

			// disarm/disable goals should remove attacks from certain ships types
			// (but not tactical disarm/disable)
			case AI_GOAL_DISARM_SHIP:
			case AI_GOAL_DISABLE_SHIP:
				if (ai_goal_is_specific_chase(purge_ai_mode)) {
					int ai_ship_type;

					// for wings we grab the ship type of the wing leader
					if (ai_wingnum >= 0) {
						ai_ship_type = Ship_info[Wings[ai_wingnum].special_ship_ship_info_index].class_type;
					}
					// otherwise we simply grab it from the ship itself
					else {
						Assert(aip != NULL);
						ai_ship_type = Ship_info[Ships[aip->shipnum].ship_info_index].class_type;
					}

					// grab the ship type of the ship that is being disarmed/disabled
					int crippled_ship_type = Ship_info[Ships[ship_index].ship_info_index].class_type;

					if (ai_ship_type < 0 || crippled_ship_type < 0)
						break;

					ship_type_info *crippled_ship_type_info = &Ship_types[crippled_ship_type];

					// work through all the ship types to see if the class matching our ai ship must ignore the ship 
					// being disarmed/disabled
					for ( j=0 ; j < (int)crippled_ship_type_info->ai_cripple_ignores.size(); j++) {
						if (crippled_ship_type_info->ai_cripple_ignores[j] == ai_ship_type) {
							purge_goal->flags.set(AI::Goal_Flags::Purge);
						}
					}
				}	
				break;
		}
	}
}

// function to purge the goals of *all* ships in the game based on the incoming goal structure
void ai_goal_purge_all_invalid_goals(ai_goal *aigp)
{
	int i;
	ship_obj *sop;

	// only purge goals if a new goal is one of the types in next statement
	if (!purge_goals_all_ships(aigp->ai_mode))
		return;
	
	for (sop = GET_FIRST(&Ship_obj_list); sop != END_OF_LIST(&Ship_obj_list); sop = GET_NEXT(sop))
	{
		if (Objects[sop->objnum].flags[Object::Object_Flags::Should_be_dead])
			continue;
		ship *shipp = &Ships[Objects[sop->objnum].instance];
		ai_goal_purge_invalid_goals(aigp, Ai_info[shipp->ai_index].goals, &Ai_info[shipp->ai_index], -1);
	}

	// we must do the same for the wing goals
	for (i = 0; i < Num_wings; i++)
	{
		ai_goal_purge_invalid_goals(aigp, Wings[i].ai_goals, NULL, i);
	}
}

// Goober5000
int ai_goal_find_dockpoint(int shipnum, int dock_type)
{
	int dock_index = -1;
	int loop_count = 0;

	ship *shipp = &Ships[shipnum];
	object *objp = &Objects[shipp->objnum];

	// only check 100 points for sanity's sake
	while (loop_count < 100)
	{
		dock_index = model_find_dock_index(Ship_info[shipp->ship_info_index].model_num, dock_type, dock_index+1);

		// not found?
		if (dock_index == -1)
		{
			if (loop_count == 0)
			{
				// first time around... there are no slots fitting this description
				return -1;
			}
			else
			{
				// every slot is full
				break;
			}
		}

		// we've found something... check if it's occupied
		if (dock_find_object_at_dockpoint(objp, dock_index) == NULL)
		{
			// not occupied... yay, we've found an index
			return dock_index;
		}

		// keep track
		loop_count++;
	}

	// insanity?
	if (loop_count >= 100)
		Warning(LOCATION, "Too many iterations while looking for a dockpoint on %s.\n", shipp->ship_name);

	// if we're here, just return the first dockpoint
	return model_find_dock_index(Ship_info[shipp->ship_info_index].model_num, dock_type);
}

// function to fix up dock point references for objects.
// passed are the pointer to goal we are working with.  aip is the ai_info pointer
// of the ship with the order.  aigp is a pointer to the goal (of aip) of which we are
// fixing up the docking points
void ai_goal_fixup_dockpoints(ai_info *aip, ai_goal *aigp)
{
	int shipnum, docker_index, dockee_index;

	Assert ( aip->shipnum != -1 );
	shipnum = ship_name_lookup( aigp->target_name );
	Assertion ( shipnum != -1, "Couldn't find ai goal's target_name (%s); get a coder!\n", aigp->target_name );
	docker_index = -1;
	dockee_index = -1;

	//WMC - This gets a bit complex with shiptypes.tbl
	//Basically this finds the common dockpoint.
	//For this, the common flags for aip's active point (ie point it wants to dock with)
	//and aigp's passive point (point it wants to be docked with) are combined
	//and the common ones are used, in order of precedence.
	//Yes, it does sound vaguely like a double-entree.

	int aip_type_dock = Ship_info[Ships[aip->shipnum].ship_info_index].class_type;
	int aigp_type_dock = Ship_info[Ships[shipnum].ship_info_index].class_type;

	int common_docks = 0;

	if(aip_type_dock > -1) {
		aip_type_dock = Ship_types[aip_type_dock].ai_active_dock;
	} else {
		aip_type_dock = 0;
	}

	if(aigp_type_dock > -1) {
		aigp_type_dock = Ship_types[aigp_type_dock].ai_passive_dock;
	} else {
		aigp_type_dock = 0;
	}

	common_docks = aip_type_dock & aigp_type_dock;

	//Now iterate through types.
	for(int i = 0; i < Num_dock_type_names; i++)
	{
		if(common_docks & Dock_type_names[i].def) {
			docker_index = ai_goal_find_dockpoint(aip->shipnum, Dock_type_names[i].def);
			dockee_index = ai_goal_find_dockpoint(shipnum, Dock_type_names[i].def);
			break;
		}
	}

	// look for docking points of the appropriate type.  Use cargo docks for cargo ships.
	/*
	if (Ship_info[Ships[shipnum].ship_info_index].flags[Ship::Info_Flags::Cargo]) {
		docker_index = ai_goal_find_dockpoint(aip->shipnum, DOCK_TYPE_CARGO);
		dockee_index = ai_goal_find_dockpoint(shipnum, DOCK_TYPE_CARGO);
	} else if (Ship_info[Ships[aip->shipnum].ship_info_index].flags[Ship::Info_Flags::Support]) {
		docker_index = ai_goal_find_dockpoint(aip->shipnum, DOCK_TYPE_REARM);
		dockee_index = ai_goal_find_dockpoint(shipnum, DOCK_TYPE_REARM);
	}
	*/

	// if we didn't find dockpoints above, then we should just look for generic docking points
	if ( docker_index == -1 )
		docker_index = ai_goal_find_dockpoint(aip->shipnum, DOCK_TYPE_GENERIC);
	if ( dockee_index == -1 )
		dockee_index = ai_goal_find_dockpoint(shipnum, DOCK_TYPE_GENERIC);
		
	aigp->docker.index = docker_index;
	aigp->dockee.index = dockee_index;
	aigp->flags.set(AI::Goal_Flags::Dockee_index_valid);
	aigp->flags.set(AI::Goal_Flags::Docker_index_valid);
}

// these functions deal with adding goals sent from the player.  They are slightly different
// from the mission goals (i.e. those goals which come from events) in that we don't
// use sexpressions for goals from the player...so we enumerate all the parameters

void ai_add_goal_sub_player(ai_goal_type type, ai_goal_mode mode, int submode, const char *target_name, ai_goal *aigp, int int_data, float float_data, const ai_lua_parameters& lua_target )
{
	Assert ( (type == ai_goal_type::PLAYER_WING) || (type == ai_goal_type::PLAYER_SHIP) );

	ai_goal_reset(aigp, true, mode, submode, type);

	aigp->int_data = int_data;
	aigp->float_data = float_data;
	aigp->lua_ai_target = lua_target;

	if ( mode == AI_GOAL_WARP ) {
		if (submode >= 0) {
			aigp->wp_list_index = submode;
			Assert(find_waypoint_list_at_index(aigp->wp_list_index) != nullptr);
		}
	}

	if ( mode == AI_GOAL_CHASE_WEAPON ) {
		aigp->target_instance = submode;				// submode contains the instance of the weapon
		aigp->target_signature = Objects[Weapons[submode].objnum].signature;
	}

	if ( target_name != NULL )
		aigp->target_name = ai_get_goal_target_name( target_name, &aigp->target_name_index );

	if (The_mission.ai_profile->flags[AI::Profile_Flags::Player_orders_afterburn_hard])
		aigp->flags.set(AI::Goal_Flags::Afterburn_hard);


	// special case certain orders from player so that ships continue to do the right thing

	// make priority for these two support ship orders low so that they will prefer repairing
	// a ship over staying near a ship.
	if ( (mode == AI_GOAL_STAY_NEAR_SHIP) || (mode == AI_GOAL_KEEP_SAFE_DISTANCE) )
		aigp->priority = PLAYER_PRIORITY_SUPPORT_LOW;

	// Goober5000 - same with form-on-wing, since it's a type of staying near
	else if ( mode == AI_GOAL_FORM_ON_WING )
		aigp->priority = PLAYER_PRIORITY_SUPPORT_LOW;

	else if ( aigp->type == ai_goal_type::PLAYER_WING )	// NOLINT(readability-braces-around-statements)
		aigp->priority = PLAYER_PRIORITY_WING;			// player wing goals not as high as ship goals
	else
		aigp->priority = PLAYER_PRIORITY_SHIP;
}

// Goober5000 - Modified this function for clarity and to avoid returning the active goal's index
// as the empty slot.  This avoids overwriting the active goal while it's being executed.  So far
// the only time I've noticed it being a problem is during a rare situation where more than five
// friendlies want to rearm at the same time.  The support ship forgets what it's doing and flies
// off to repair somebody while still docked.  I reproduced this with retail, so it's not a bug in
// my new docking code. :)
int ai_goal_find_empty_slot( ai_goal *goals, int active_goal )
{
	int gindex, oldest_index;

	oldest_index = -1;
	for ( gindex = 0; gindex < MAX_AI_GOALS; gindex++ )
	{
		// get the index for the first unused goal
		if (goals[gindex].ai_mode == AI_GOAL_NONE)
			return gindex;

		// if this is the active goal, don't consider it for pre-emption!!
		if (gindex == active_goal)
			continue;

		// store the index of the oldest goal
		if (oldest_index < 0)
			oldest_index = gindex;
		else if (goals[gindex].time < goals[oldest_index].time)
			oldest_index = gindex;
	}

	// if we didn't find an empty slot, use the oldest goal's slot
	return oldest_index;
}

int ai_goal_num(ai_goal *goals)
{
	int gindex = 0;
	int num_goals = 0;
	for(gindex = 0; gindex < MAX_AI_GOALS; gindex++)
	{
		if(goals[gindex].ai_mode != AI_GOAL_NONE)
			num_goals++;
	}

	return num_goals;
}


void ai_add_goal_sub_scripting(ai_goal_type type, ai_goal_mode mode, int submode, int priority, const char *target_name, ai_goal *aigp, int int_data, float float_data)
{
	Assert ( (type == ai_goal_type::PLAYER_WING) || (type == ai_goal_type::PLAYER_SHIP) );

	ai_goal_reset(aigp, true, mode, submode, type);

	if ( mode == AI_GOAL_CHASE_WEAPON ) {
		aigp->target_instance = submode;								// submode contains the instance of the weapon
		aigp->target_signature = Objects[Weapons[submode].objnum].signature;
	}

	if ( target_name != NULL )
		aigp->target_name = ai_get_goal_target_name( target_name, &aigp->target_name_index );

	aigp->priority = priority;
	aigp->int_data = int_data;
	aigp->float_data = float_data;
}

void ai_add_ship_goal_scripting(ai_goal_mode mode, int submode, int priority, const char *shipname, ai_info *aip, int int_data, float float_data)
{
	int empty_index;
	ai_goal *aigp;

	empty_index = ai_goal_find_empty_slot(aip->goals, aip->active_goal);
	aigp = &aip->goals[empty_index];
	ai_add_goal_sub_scripting(ai_goal_type::PLAYER_SHIP, mode, submode, priority, shipname, aigp, int_data, float_data);

	//WMC - hack to get docking setup correctly
	if ( mode == AI_GOAL_DOCK ) {
		aigp->docker.name = Ships[aip->shipnum].ship_name;
		aigp->dockee.name = shipname;
	}

	if ( (mode == AI_GOAL_REARM_REPAIR) || (mode == AI_GOAL_DOCK) ) {
		ai_goal_fixup_dockpoints( aip, aigp );
	}
}

// adds a goal from a player to the given ship's ai_info structure.  'type' tells us if goal
// is issued to ship or wing (from player),  mode is AI_GOAL_*. submode is the submode the
// ship should go into.  shipname is the object of the action.  aip is the ai_info pointer
// of the ship receiving the order
void ai_add_ship_goal_player(ai_goal_type type, ai_goal_mode mode, int submode, const char *shipname, ai_info *aip, int int_data, float float_data, const ai_lua_parameters& lua_target)
{
	int empty_index;
	ai_goal *aigp;

	empty_index = ai_goal_find_empty_slot( aip->goals, aip->active_goal );
	aigp = &aip->goals[empty_index];
	ai_add_goal_sub_player( type, mode, submode, shipname, aigp, int_data, float_data, lua_target );

	// if the goal is to dock, then we must determine which dock points on the two ships to use.
	// If the target of the dock is a cargo type container, then we should use DOCK_TYPE_CARGO
	// on both ships.  Code is here instead of in ai_add_goal_sub_player() since a dock goal
	// should only occur to a specific ship.

	if ( (mode == AI_GOAL_REARM_REPAIR) || (mode == AI_GOAL_DOCK) ) {
		ai_goal_fixup_dockpoints( aip, aigp );
	}
}

// adds a goal from the player to the given wing (which in turn will add it to the proper
// ships in the wing
void ai_add_wing_goal_player(ai_goal_type type, ai_goal_mode mode, int submode, const char *shipname, int wingnum, int int_data, float float_data, const ai_lua_parameters& lua_target)
{
	int i, empty_index;
	wing *wingp = &Wings[wingnum];

	// add the ai goal for any ship that is currently arrived in the game.
	if ( !Fred_running ) {										// only add goals to ships if fred isn't running
		for (i = 0; i < wingp->current_count; i++) {
			int num = wingp->ship_index[i];
			if ( num == -1 )			// ship must have been destroyed or departed
				continue;
			ai_add_ship_goal_player( type, mode, submode, shipname, &Ai_info[Ships[num].ai_index], int_data, float_data, lua_target );
		}
	}

	// add the sexpression index into the wing's list of goal sexpressions if
	// there are more waves to come.  We use the same method here as when adding a goal to
	// a ship -- find the first empty entry.  If none exists, take the oldest entry and replace it.
	empty_index = ai_goal_find_empty_slot( wingp->ai_goals, -1 );
	ai_add_goal_sub_player( type, mode, submode, shipname, &wingp->ai_goals[empty_index], int_data, float_data, lua_target );
}


// common routine to add a sexpression mission goal to the appropriate goal structure.
void ai_add_goal_sub_sexp( int sexp, ai_goal_type type, ai_info *aip, ai_goal *aigp, const char *actor_name )
{
	int node, dummy, op;
	bool priority_is_nan = false, priority_is_nan_forever = false;

	Assert ( Sexp_nodes[sexp].first != -1 );
	node = Sexp_nodes[sexp].first;
	op = get_operator_const( node );

	ai_goal_reset(aigp, true);
	aigp->type = type;

	switch (op) {

	case OP_AI_WAYPOINTS_ONCE:
	case OP_AI_WAYPOINTS:
	{
		int ref_type;
		bool is_nan, is_nan_forever;

		ref_type = Sexp_nodes[CDR(node)].subtype;
		Assertion(ref_type == SEXP_ATOM_STRING || ref_type == SEXP_ATOM_CONTAINER_DATA, "Found a bad ref_type in ai_add_goal_sub_sexp of %d. Please report to the SCP!", ref_type);
		
		// referenced by name
		// save the waypoint path name -- the list will get resolved when the goal is checked
		// for achievability.
		aigp->target_name = ai_get_goal_target_name(CTEXT(CDR(node)), &aigp->target_name_index);  // waypoint path name;


		aigp->priority = eval_num(CDDR(node), priority_is_nan, priority_is_nan_forever);
		aigp->ai_mode = AI_GOAL_WAYPOINTS;
		if ( op == OP_AI_WAYPOINTS_ONCE )
			aigp->ai_mode = AI_GOAL_WAYPOINTS_ONCE;
		if (CDDDDR(node) < 0)
			aigp->int_data = 0;	// handle optional node separately because we don't subtract 1 here
		else
			aigp->int_data = eval_num(CDDDDR(node), is_nan, is_nan_forever) - 1;
		if (is_sexp_true(CDDDDDR(node)))
			aigp->flags.set(AI::Goal_Flags::Waypoints_in_reverse);
		break;
	}

	case OP_AI_DESTROY_SUBSYS:
		aigp->ai_mode = AI_GOAL_DESTROY_SUBSYSTEM;
		aigp->target_name = ai_get_goal_target_name( CTEXT(CDR(node)), &aigp->target_name_index );
		// store the name of the subsystem in the docker.name field for now -- this field must get
		// fixed up when the goal is valid since we need to locate the subsystem on the ship's model
		aigp->docker.name = ai_get_goal_target_name(CTEXT(CDDR(node)), &dummy);
		aigp->flags.set(AI::Goal_Flags::Subsys_needs_fixup);
		aigp->priority = eval_num(CDDDR(node), priority_is_nan, priority_is_nan_forever);
		break;

	case OP_AI_DISABLE_SHIP:
	case OP_AI_DISABLE_SHIP_TACTICAL:
		aigp->ai_mode = (op == OP_AI_DISABLE_SHIP) ? AI_GOAL_DISABLE_SHIP : AI_GOAL_DISABLE_SHIP_TACTICAL;
		aigp->target_name = ai_get_goal_target_name( CTEXT(CDR(node)), &aigp->target_name_index );
		aigp->ai_submode = -SUBSYSTEM_ENGINE;
		aigp->priority = eval_num(CDDR(node), priority_is_nan, priority_is_nan_forever);
		break;

	case OP_AI_DISARM_SHIP:
	case OP_AI_DISARM_SHIP_TACTICAL:
		aigp->ai_mode = (op == OP_AI_DISARM_SHIP) ? AI_GOAL_DISARM_SHIP : AI_GOAL_DISARM_SHIP_TACTICAL;
		aigp->target_name = ai_get_goal_target_name( CTEXT(CDR(node)), &aigp->target_name_index );
		aigp->ai_submode = -SUBSYSTEM_TURRET;
		aigp->priority = eval_num(CDDR(node), priority_is_nan, priority_is_nan_forever);
		break;

	case OP_AI_WARP_OUT:
		aigp->ai_mode = AI_GOAL_WARP;
		aigp->priority = eval_num(CDR(node), priority_is_nan, priority_is_nan_forever);
		break;

		// the following goal is obsolete, but here for compatibility
	case OP_AI_WARP:
		aigp->ai_mode = AI_GOAL_WARP;
		aigp->target_name = ai_get_goal_target_name(CTEXT(CDR(node)), &aigp->target_name_index);  // waypoint path name;
		aigp->priority = eval_num(CDDR(node), priority_is_nan, priority_is_nan_forever);
		break;

	case OP_AI_UNDOCK:
		aigp->priority = eval_num(CDR(node), priority_is_nan, priority_is_nan_forever);

		// Goober5000 - optional undock with something
		if (CDDR(node) != -1)
			aigp->target_name = ai_get_goal_target_name( CTEXT(CDDR(node)), &aigp->target_name_index );

		aigp->ai_mode = AI_GOAL_UNDOCK;
		aigp->ai_submode = AIS_UNDOCK_0;
		break;

	case OP_AI_REARM_REPAIR:
	{
		aigp->ai_mode = AI_GOAL_REARM_REPAIR;
		aigp->target_name = ai_get_goal_target_name(CTEXT(CDR(node)), &aigp->target_name_index);
		aigp->priority = eval_num(CDDR(node), priority_is_nan, priority_is_nan_forever);

		// this goal needs some extra setup
		// if this doesn't work, the goal will be immediately removed
		auto ship_entry = ship_registry_get(aigp->target_name);
		if (ship_entry && ship_entry->has_shipp())
		{
			auto target_aip = &Ai_info[ship_entry->shipp()->ai_index];
			target_aip->ai_flags.set(AI::AI_Flags::Awaiting_repair);
			Assertion(aip != nullptr, "Must provide aip if assigning a rearm goal!");
			ai_goal_fixup_dockpoints( aip, aigp );
		}
		break;
	}

	case OP_AI_STAY_STILL:
		aigp->ai_mode = AI_GOAL_STAY_STILL;
		aigp->target_name = ai_get_goal_target_name(CTEXT(CDR(node)), &aigp->target_name_index);  // waypoint path name;
		aigp->priority = eval_num(CDDR(node), priority_is_nan, priority_is_nan_forever);
		break;

	case OP_AI_DOCK:
		aigp->target_name = ai_get_goal_target_name( CTEXT(CDR(node)), &aigp->target_name_index );
		aigp->docker.name = ai_add_dock_name(CTEXT(CDDR(node)));
		aigp->dockee.name = ai_add_dock_name(CTEXT(CDDDR(node)));
		aigp->priority = eval_num(CDDDDR(node), priority_is_nan, priority_is_nan_forever);

		aigp->ai_mode = AI_GOAL_DOCK;
		aigp->ai_submode = AIS_DOCK_0;		// be sure to set the submode
		break;

	case OP_AI_CHASE_ANY:
		aigp->priority = eval_num(CDR(node), priority_is_nan, priority_is_nan_forever);
		aigp->ai_mode = AI_GOAL_CHASE_ANY;
		break;

	case OP_AI_PLAY_DEAD:
		aigp->priority = eval_num(CDR(node), priority_is_nan, priority_is_nan_forever);
		aigp->ai_mode = AI_GOAL_PLAY_DEAD;
		break;

	case OP_AI_PLAY_DEAD_PERSISTENT:
		aigp->priority = eval_num(CDR(node), priority_is_nan, priority_is_nan_forever);
		aigp->ai_mode = AI_GOAL_PLAY_DEAD_PERSISTENT;
		break;

	case OP_AI_KEEP_SAFE_DISTANCE:
		aigp->priority = eval_num(CDR(node), priority_is_nan, priority_is_nan_forever);
		aigp->ai_mode = AI_GOAL_KEEP_SAFE_DISTANCE;
		break;

	case OP_AI_FLY_TO_SHIP:
	case OP_AI_STAY_NEAR_SHIP:
	{
		bool is_nan, is_nan_forever;

		aigp->target_name = ai_get_goal_target_name( CTEXT(CDR(node)), &aigp->target_name_index );
		aigp->priority = eval_num(CDDR(node), priority_is_nan, priority_is_nan_forever);

		// distance from ship
		if ( CDDDR(node) < 0 )
			aigp->float_data = 300.0f;
		else
			aigp->float_data = i2fl(eval_num(CDDDR(node), is_nan, is_nan_forever));

		// (the CDDDDR argument is whether to afterburn, and is handled at the end of the function, so don't handle it here)

		// whether to "escort" the ship
		if ( CDDDDDR(node) >= 0 )
			aigp->int_data = is_sexp_true(CDDDDDR(node)) ? 1 : 0;

		if (op == OP_AI_FLY_TO_SHIP)
			aigp->ai_mode = AI_GOAL_FLY_TO_SHIP;
		else
			aigp->ai_mode = AI_GOAL_STAY_NEAR_SHIP;
		break;
	}

	case OP_AI_FORM_ON_WING:
		aigp->priority = 99;
		aigp->target_name = ai_get_goal_target_name(CTEXT(CDR(node)), &aigp->target_name_index);
		aigp->ai_mode = AI_GOAL_FORM_ON_WING;
		break;

	case OP_AI_CHASE:
	case OP_AI_CHASE_WING:
	case OP_AI_CHASE_SHIP_CLASS:
	case OP_AI_GUARD:
	case OP_AI_GUARD_WING:
	case OP_AI_EVADE_SHIP:
	case OP_AI_IGNORE:
	case OP_AI_IGNORE_NEW:
		aigp->target_name = ai_get_goal_target_name( CTEXT(CDR(node)), &aigp->target_name_index );
		aigp->priority = eval_num(CDDR(node), priority_is_nan, priority_is_nan_forever);

		if ( op == OP_AI_CHASE ) {
			aigp->ai_mode = AI_GOAL_CHASE;

			// in the case of ai_chase (and ai_guard) we must do a wing_name_lookup on the name
			// passed here to see if we could be chasing a wing.  Hoffoss and I have consolidated
			// sexpression operators which makes this step necessary
			if ( wing_name_lookup(aigp->target_name, 1) != -1 )
				aigp->ai_mode = AI_GOAL_CHASE_WING;

		} else if ( op == OP_AI_GUARD ) {
			aigp->ai_mode = AI_GOAL_GUARD;
			if ( wing_name_lookup(aigp->target_name, 1) != -1 )
				aigp->ai_mode = AI_GOAL_GUARD_WING;

		} else if ( op == OP_AI_EVADE_SHIP ) {
			aigp->ai_mode = AI_GOAL_EVADE_SHIP;

		} else if ( op == OP_AI_GUARD_WING ) {
			aigp->ai_mode = AI_GOAL_GUARD_WING;
		} else if ( op == OP_AI_CHASE_WING ) {
			aigp->ai_mode = AI_GOAL_CHASE_WING;
		} else if (op == OP_AI_CHASE_SHIP_CLASS) {
			aigp->ai_mode = AI_GOAL_CHASE_SHIP_CLASS;
		} else if ( op == OP_AI_IGNORE ) {
			aigp->ai_mode = AI_GOAL_IGNORE;
		} else if ( op == OP_AI_IGNORE_NEW ) {
			aigp->ai_mode = AI_GOAL_IGNORE_NEW;
		} else
			UNREACHABLE("Coding error: unhandled AI goal in ai_add_goal_sub_sexp!");

		break;

	default:
		const ai_mode_lua* luaAIMode = ai_lua_find_mode(op);
		if(luaAIMode != nullptr){
			//Found a LuaAI mode sexp for this
			int localnode = CDR(node);
			
			aigp->ai_mode = AI_GOAL_LUA;
			aigp->ai_submode = op;
			

			object_ship_wing_point_team target;
			if(luaAIMode->needsTarget) {
				eval_object_ship_wing_point_team(&target, localnode);
				localnode = CDR(localnode);
			}

			aigp->priority = eval_num(localnode, priority_is_nan, priority_is_nan_forever);

			aigp->lua_ai_target = { std::move(target), luaAIMode->sexp.getSEXPArgumentList(CDR(localnode)) };
		}
		else {
			UNREACHABLE("Invalid SEXP-OP number %d for an AI goal!", op);
		}
	}

	if ( priority_is_nan || priority_is_nan_forever ) {
		Warning(LOCATION, "add-goal tried to add %s with a NaN priority; aborting...", Sexp_nodes[CAR(sexp)].text);
		ai_goal_reset(aigp);
		return;
	} else if ( aigp->priority > MAX_GOAL_PRIORITY ) {
		nprintf (("AI", "bashing add-goal sexpression priority of goal %s from %d to %d.\n", Sexp_nodes[CAR(sexp)].text, aigp->priority, MAX_GOAL_PRIORITY));
		aigp->priority = MAX_GOAL_PRIORITY;
	}

	// Goober5000 - we now have an extra optional chase argument to allow chasing our own team
	if ( op == OP_AI_CHASE || op == OP_AI_CHASE_WING || op == OP_AI_CHASE_SHIP_CLASS
		|| op == OP_AI_DISABLE_SHIP || op == OP_AI_DISABLE_SHIP_TACTICAL || op == OP_AI_DISARM_SHIP || op == OP_AI_DISARM_SHIP_TACTICAL ) {
		if (is_sexp_true(CDDDR(node)))
			aigp->flags.set(AI::Goal_Flags::Target_own_team);
	}
	if ( op == OP_AI_DESTROY_SUBSYS ) {
		if (is_sexp_true(CDDDDR(node)))
			aigp->flags.set(AI::Goal_Flags::Target_own_team);
	}

	// maybe get the afterburn hard flag
	if (op == OP_AI_CHASE_ANY) {
		if (is_sexp_true(CDDR(node)))
			aigp->flags.set(AI::Goal_Flags::Afterburn_hard);
	}
	if (op == OP_AI_GUARD || 
		op == OP_AI_GUARD_WING || 
		op == OP_AI_WAYPOINTS || 
		op == OP_AI_WAYPOINTS_ONCE) {
		if (is_sexp_true(CDDDR(node)))
			aigp->flags.set(AI::Goal_Flags::Afterburn_hard);
	}
	if (op == OP_AI_CHASE || 
		op == OP_AI_CHASE_WING || 
		op == OP_AI_CHASE_SHIP_CLASS || 
		op == OP_AI_DISABLE_SHIP || 
		op == OP_AI_DISABLE_SHIP_TACTICAL || 
		op == OP_AI_DISARM_SHIP || 
		op == OP_AI_DISARM_SHIP_TACTICAL || 
		op == OP_AI_STAY_NEAR_SHIP ||
		op == OP_AI_FLY_TO_SHIP) {
		if (is_sexp_true(CDDDDR(node)))
			aigp->flags.set(AI::Goal_Flags::Afterburn_hard);
	}	
	if (op == OP_AI_DESTROY_SUBSYS || op == OP_AI_DOCK) {
		if (is_sexp_true(CDDDDDR(node)))
			aigp->flags.set(AI::Goal_Flags::Afterburn_hard);
	}

	// Goober5000 - since none of the goals act on the actor,
	// don't assign the goal if the actor's goal target is itself
	if (aigp->target_name != NULL && !strcmp(aigp->target_name, actor_name))
	{
		ai_goal_reset(aigp);
	}
}

/* Find the index of the goal in the passed ai_goal array
 * Call something like ai_find_goal_index( aiip->goals, AIM_* );
 * Pass -1 in submode to ignore ai_submode when searching
 * Pass -1 in priority to ignore priority when searching
 * Returns -1 if not found, or [0, MAX_AI_GOALS)
 */
int ai_find_goal_index( ai_goal* aigp, int mode, int submode, int priority )
{
	Assert( aigp != NULL );
	for ( int i = 0; i < MAX_AI_GOALS; i++ )
	{
		if ( aigp[ i ].ai_mode == mode &&
			 ( submode == -1 || aigp[ i ].ai_submode == submode ) &&
			 ( priority == -1 || aigp[ i ].priority == priority ) )
		{
			return i;
		}
	}

	return -1;
}

/* Remove a goal from the given goals structure
 * Returns the index of the goal that it clears out.
 * This is important so that if active_goal == index you can set AI_ACTIVE_GOAL_NONE.
 * NOTE: Callers should check the value of remove_more.  If it is true, the function should be called again.
 */
int ai_remove_goal_sexp_sub( int sexp, ai_goal* aigp, bool &remove_more )
{
	/* Sanity check */
	Assert( Sexp_nodes[ sexp ].first != -1 );

	/* The bits we're searching for in the goals list */
	int priority = -1;

	int goalmode = -1;
	int goalsubmode = -1;

	/* Sexp node */
	int node = Sexp_nodes[sexp].first;
	/* The operator to use */
	int op = get_operator_const( node );

	// since this logic is common to all goals removed by the remove-goal sexp
	auto eval_priority_et_seq = [sexp, &remove_more](int n, int priority_if_no_n = -1)->int
	{
		bool _priority_is_nan = false, _priority_is_nan_forever = false;

		int _priority = (n >= 0) ? eval_num(n, _priority_is_nan, _priority_is_nan_forever) : priority_if_no_n;
		n = CDR(sexp);	// we want the first node after the goal sub-tree

		if (_priority_is_nan || _priority_is_nan_forever)
		{
			Warning(LOCATION, "remove-goal tried to remove %s with a NaN priority; the priority will not be used for goal comparison", Sexp_nodes[CAR(sexp)].text);
			_priority = -1;
		}
		else if (_priority > MAX_GOAL_PRIORITY)
		{
			nprintf(("AI", "bashing remove-goal sexpression priority of goal %s from %d to %d.\n", Sexp_nodes[CAR(sexp)].text, _priority, MAX_GOAL_PRIORITY));
			_priority = MAX_GOAL_PRIORITY;
		}

		if (n >= 0)
		{
			remove_more = is_sexp_true(n);
			n = CDR(n);
		}

		if (n >= 0)
		{
			if (is_sexp_true(n))
				_priority = -1;
			n = CDR(n);
		}

		return _priority;
	};

	/* We now need to determine what the mode and submode values are*/
	switch( op )
	{
	case OP_AI_WAYPOINTS_ONCE:
		goalmode = AI_GOAL_WAYPOINTS_ONCE;
		priority = eval_priority_et_seq(CDDR(node));
		break;
	case OP_AI_WAYPOINTS:
		goalmode = AI_GOAL_WAYPOINTS;
		priority = eval_priority_et_seq(CDDR(node));
		break;
	case OP_AI_DESTROY_SUBSYS:
		goalmode = AI_GOAL_DESTROY_SUBSYSTEM;
		priority = eval_priority_et_seq(CDDDR(node));
		break;
	case OP_AI_DISABLE_SHIP:
	case OP_AI_DISABLE_SHIP_TACTICAL:
		goalmode = (op == OP_AI_DISABLE_SHIP) ? AI_GOAL_DISABLE_SHIP : AI_GOAL_DISABLE_SHIP_TACTICAL;
		priority = eval_priority_et_seq(CDDR(node));
		break;
	case OP_AI_DISARM_SHIP:
	case OP_AI_DISARM_SHIP_TACTICAL:
		goalmode = (op == OP_AI_DISARM_SHIP) ? AI_GOAL_DISARM_SHIP : AI_GOAL_DISARM_SHIP_TACTICAL;
		priority = eval_priority_et_seq(CDDR(node));
		break;
	case OP_AI_WARP_OUT:
		goalmode = AI_GOAL_WARP;
		priority = eval_priority_et_seq(CDR(node));
		break;
	case OP_AI_WARP:
		goalmode = AI_GOAL_WARP;
		priority = eval_priority_et_seq(CDDR(node));
		break;
	case OP_AI_UNDOCK:
		goalmode = AI_GOAL_UNDOCK;
		goalsubmode = AIS_UNDOCK_0;
		priority = eval_priority_et_seq(CDR(node));
		break;
	case OP_AI_STAY_STILL:
		goalmode = AI_GOAL_STAY_STILL;
		priority = eval_priority_et_seq(CDDR(node));
		break;
	case OP_AI_DOCK:
		goalmode = AI_GOAL_DOCK;
		goalsubmode = AIS_DOCK_0;
		priority = eval_priority_et_seq(CDDDDR(node));
		break;
	case OP_AI_CHASE_ANY:
		goalmode = AI_GOAL_CHASE_ANY;
		priority = eval_priority_et_seq(CDR(node));
		break;
	case OP_AI_PLAY_DEAD:
	case OP_AI_PLAY_DEAD_PERSISTENT:
		goalmode = (op == OP_AI_PLAY_DEAD) ? AI_GOAL_PLAY_DEAD : AI_GOAL_PLAY_DEAD_PERSISTENT;
		priority = eval_priority_et_seq(CDR(node));
		break;
	case OP_AI_KEEP_SAFE_DISTANCE:
		priority = eval_priority_et_seq(CDR(node));
		goalmode = AI_GOAL_KEEP_SAFE_DISTANCE;
		break;
	case OP_AI_CHASE:
		priority = eval_priority_et_seq(CDDR(node));
		if ( wing_name_lookup( CTEXT( CDR( node ) ), 1 ) != -1 )
			goalmode = AI_GOAL_CHASE_WING;
		else
			goalmode = AI_GOAL_CHASE;
		break;
	case OP_AI_GUARD:
		priority = eval_priority_et_seq(CDDR(node));
		if ( wing_name_lookup( CTEXT( CDR( node ) ), 1 ) != -1 )
			goalmode = AI_GOAL_GUARD_WING;
		else
			goalmode = AI_GOAL_GUARD;
		break;
	case OP_AI_GUARD_WING:
		priority = eval_priority_et_seq(CDDR(node));
		goalmode = AI_GOAL_GUARD_WING;
		break;
	case OP_AI_CHASE_WING:
		priority = eval_priority_et_seq(CDDR(node));
		goalmode = AI_GOAL_CHASE_WING;
		break;
	case OP_AI_CHASE_SHIP_CLASS:
		priority = eval_priority_et_seq(CDDR(node));
		goalmode = AI_GOAL_CHASE_SHIP_CLASS;
		break;
	case OP_AI_EVADE_SHIP:
		priority = eval_priority_et_seq(CDDR(node));
		goalmode = AI_GOAL_EVADE_SHIP;
		break;
	case OP_AI_STAY_NEAR_SHIP:
		priority = eval_priority_et_seq(CDDR(node));
		goalmode = AI_GOAL_STAY_NEAR_SHIP;
		break;
	case OP_AI_IGNORE:
	case OP_AI_IGNORE_NEW:
		priority = eval_priority_et_seq(CDDR(node));
		goalmode = (op == OP_AI_IGNORE) ? AI_GOAL_IGNORE : AI_GOAL_IGNORE_NEW;
		break;
	case OP_AI_FORM_ON_WING:
		priority = eval_priority_et_seq(-1, 99);
		goalmode = AI_GOAL_FORM_ON_WING;
		break;
	case OP_AI_FLY_TO_SHIP:
		priority = eval_priority_et_seq(CDDR(node));
		goalmode = AI_GOAL_FLY_TO_SHIP;
		break;
	case OP_AI_REARM_REPAIR:
		priority = eval_priority_et_seq(CDDR(node));
		goalmode = AI_GOAL_REARM_REPAIR;
		break;
	default:
		const ai_mode_lua* luaAIMode = ai_lua_find_mode(op);
		if(luaAIMode != nullptr){
			//Found a LuaAI mode sexp for this
			int localnode = CDR(node);

			goalmode = AI_GOAL_LUA;
			goalsubmode = op;

			if(luaAIMode->needsTarget) {
				localnode = CDR(localnode);
			}

			priority = eval_priority_et_seq(localnode);
		}
		else {
			UNREACHABLE("Invalid SEXP-OP %s (number %d) for an AI goal!", Sexp_nodes[node].text, op);
		}
		break;
	};
	
	/* Attempt to find the goal */
	int goalindex = ai_find_goal_index( aigp, goalmode, goalsubmode, priority );

	if ( goalindex == -1 )
	{
		remove_more = false;
		return -1; /* no more to do; */
	}

	/* Clear out the contents of the goal. We can't use ai_remove_ship_goal since it needs ai_info and
	 * we've only got ai_goals */
	ai_goal_reset(&aigp[goalindex]);

	return goalindex;
}

// code to remove ai goals from wings.
void ai_remove_wing_goal_sexp(int sexp, wing *wingp)
{
	int i;
	int goalindex = -1;
	bool remove_more = false;

	// remove the ai goal for any ship that is currently arrived in the game (only if fred isn't running)
	if ( !Fred_running ) {
		for (i = 0; i < wingp->current_count; i++) {
			int num = wingp->ship_index[i];
			if ( num == -1 )			// ship must have been destroyed or departed
				continue;
			auto aip = &Ai_info[Ships[num].ai_index];

			do {
				goalindex = ai_remove_goal_sexp_sub(sexp, aip->goals, remove_more);
				if (aip->active_goal == goalindex)
					aip->active_goal = AI_ACTIVE_GOAL_NONE;
			} while (remove_more);
		}
	}

	// remove the sexpression index from the wing's list of goal sexpressions if
	// there are more waves to come
	if ((wingp->num_waves - wingp->current_wave > 0) || Fred_running) 
	{
		do {
			ai_remove_goal_sexp_sub(sexp, wingp->ai_goals, remove_more);
		} while (remove_more);
	}
}

// adds an ai goal for an individual ship
// type determines who has issues this ship a goal (i.e. the player/mission event/etc)
void ai_add_ship_goal_sexp( int sexp, ai_goal_type type, ai_info *aip )
{
	int gindex;

	gindex = ai_goal_find_empty_slot( aip->goals, aip->active_goal );
	ai_add_goal_sub_sexp( sexp, type, aip, &aip->goals[gindex], Ships[aip->shipnum].ship_name );
}

// code to add ai goals to wings.
void ai_add_wing_goal_sexp(int sexp, ai_goal_type type, wing *wingp)
{
	int i;

	// add the ai goal for any ship that is currently arrived in the game (only if fred isn't running)
	if ( !Fred_running ) {
		for (i = 0; i < wingp->current_count; i++) {
			int num = wingp->ship_index[i];
			if ( num == -1 )			// ship must have been destroyed or departed
				continue;
			ai_add_ship_goal_sexp( sexp, type, &Ai_info[Ships[num].ai_index] );
		}
	}

	// add the sexpression index into the wing's list of goal sexpressions if
	// there are more waves to come
	if ((wingp->num_waves - wingp->current_wave > 0) || Fred_running) {
		int gindex;

		gindex = ai_goal_find_empty_slot( wingp->ai_goals, -1 );
		ai_add_goal_sub_sexp( sexp, type, nullptr, &wingp->ai_goals[gindex], wingp->name );
	}
}

// function for internal code to add a goal to a ship.  Needed when the AI finds itself in a situation
// that it must get out of by issuing itself an order.
//
// objp is the object getting the goal
// goal_type is one of AI_GOAL_*
// other_name is a character string objp might act on (for docking, this is a shipname, for guarding
// this name can be a shipname or a wingname)
// docker_point and dockee_point are used for the AI_GOAL_DOCK command to tell two ships where to dock
// immediate means to process this order right away
void ai_add_goal_ship_internal( ai_info *aip, int goal_type, char *name, int  /*docker_point*/, int  /*dockee_point*/, int immediate )
{
	int gindex;
	ai_goal *aigp;

	// Goober5000 - none of the goals act on the actor, as in ai_add_goal_sub_sexp
	Assertion(strcmp(name, Ships[aip->shipnum].ship_name) != 0, "The goals apply to the actor in ai_add_goal_ship_internal for ship %s, please report to the SCP!", name);

	// find an empty slot to put this goal in.
	gindex = ai_goal_find_empty_slot( aip->goals, aip->active_goal );
	aigp = &(aip->goals[gindex]);
	ai_goal_reset(aigp, true);

	aigp->type = ai_goal_type::DYNAMIC;
	aigp->flags.set(AI::Goal_Flags::Goal_override);

	switch ( goal_type ) {

/* Goober5000 - this seems to not be used
	case AI_GOAL_DOCK:
		aigp->ship_name = name;
		aigp->docker.index = docker_point;
		aigp->dockee.index = dockee_point;
		aigp->priority = 100;

		aigp->ai_mode = AI_GOAL_DOCK;
		aigp->ai_submode = AIS_DOCK_0;		// be sure to set the submode
		break;
*/

	case AI_GOAL_UNDOCK:
		aigp->target_name = name;
		aigp->priority = MAX_GOAL_PRIORITY;
		aigp->ai_mode = AI_GOAL_UNDOCK;
		aigp->ai_submode = AIS_UNDOCK_0;
		break;

/* Goober5000 - this seems to not be used
	case AI_GOAL_GUARD:
		if ( wing_name_lookup(name, 1) != -1 )
			aigp->ai_mode = AI_GOAL_GUARD_WING;
		else
			aigp->ai_mode = AI_GOAL_GUARD;
		aigp->priority = PLAYER_PRIORITY_MIN-1;		// make the priority always less than what the player's is
		break;
*/

	case AI_GOAL_REARM_REPAIR:
		aigp->ai_mode = AI_GOAL_REARM_REPAIR;
		aigp->ai_submode = 0;
		aigp->target_name = name;
		aigp->priority = PLAYER_PRIORITY_MIN-1;		// make the priority always less than what the player's is
		aigp->flags.remove(AI::Goal_Flags::Goal_override);	// don't override this goal.  rearm repair requests should happen in order
		ai_goal_fixup_dockpoints( aip, aigp );
		break;

	default:
		UNREACHABLE("unsupported internal goal of %d found in ai_add_goal_ship_internal. Please report to the SCP", goal_type); // see Mike K or Mark A.
		return;
	}


	// process the orders immediately so that these goals take effect right away
	if ( immediate )
		ai_process_mission_orders( Ships[aip->shipnum].objnum, aip );
}

// this function copies goals from a wing to an ai_info * from a ship.
void ai_copy_mission_wing_goal( ai_goal *aigp, ai_info *aip )
{
	int j;

	for ( j = 0; j < MAX_AI_GOALS; j++ ) {
		if ( aip->goals[j].ai_mode == AI_GOAL_NONE ) {
			aip->goals[j] = *aigp;
			break;
		}
	}

	if (j >= MAX_AI_GOALS) {
		mprintf(("Unable to assign wing goal to ship %s; the ship goals are already filled to capacity\n", Ships[aip->shipnum].ship_name));
	}
}


#define SHIP_STATUS_GONE				1
#define SHIP_STATUS_NOT_ARRIVED		2
#define SHIP_STATUS_ARRIVED			3
#define SHIP_STATUS_UNKNOWN			4

// function to determine if an ai goal is achieveable or not.  Will return
// one of the AI_GOAL_* values.  Also determines is a goal was successful.

ai_achievability ai_mission_goal_achievable( int objnum, ai_goal *aigp )
{
	int status;
	ai_achievability return_val;

	auto objp = &Objects[objnum];
	Assert( objp->instance != -1 );
	auto shipp = &Ships[objp->instance];
	auto aip = &Ai_info[shipp->ai_index];

	//  these orders are always achievable.
	if ( (aigp->ai_mode == AI_GOAL_KEEP_SAFE_DISTANCE)
		|| (aigp->ai_mode == AI_GOAL_CHASE_ANY) || (aigp->ai_mode == AI_GOAL_STAY_STILL)
		|| (aigp->ai_mode == AI_GOAL_PLAY_DEAD) || (aigp->ai_mode == AI_GOAL_PLAY_DEAD_PERSISTENT))
		return ai_achievability::ACHIEVABLE;

	if (aigp->ai_mode == AI_GOAL_LUA)
		return ai_lua_is_achievable(aigp, objnum);

	auto target_ship_entry = aigp->target_name == nullptr ? nullptr : ship_registry_get(aigp->target_name);

	// warp (depart) only achievable if there's somewhere to depart to
	if (aigp->ai_mode == AI_GOAL_WARP)
	{
		// always valid if has working subspace drive, not disabled, and not limited by subsystem strength
		if ( ship_can_warp_full_check(shipp) )
			return ai_achievability::ACHIEVABLE;

		// if we can't warp, we can only depart if the ship (or its wing) departs to a fighter bay and the mothership is present
		if (ship_can_bay_depart(shipp)) {
			return ai_achievability::ACHIEVABLE;
		}
		else {
			return ai_achievability::NOT_KNOWN;
		}
	}


	// form on wing is always achievable if we are forming on Player, but it's up for grabs otherwise
	// if the wing target is valid then be sure to set the override bit so that it always
	// gets executed next
	if ( aigp->ai_mode == AI_GOAL_FORM_ON_WING ) {
		if (!target_ship_entry || !target_ship_entry->has_shipp())
			return ai_achievability::NOT_ACHIEVABLE;

		aigp->flags.set(AI::Goal_Flags::Goal_override);
		return ai_achievability::ACHIEVABLE;
	}

	// check to see if we have a valid list.  If not, then try to set one up.  If that
	// fails, then we must pitch this order
	if ( (aigp->ai_mode == AI_GOAL_WAYPOINTS_ONCE) || (aigp->ai_mode == AI_GOAL_WAYPOINTS) ) {
		if ( aigp->wp_list_index < 0 ) {
			aigp->wp_list_index = find_matching_waypoint_list_index(aigp->target_name);

			if ( aigp->wp_list_index < 0 ) {
				Warning(LOCATION, "Unknown waypoint list %s - not found in mission file.  Killing ai goal", aigp->target_name );
				return ai_achievability::NOT_ACHIEVABLE;
			}
		}
		return ai_achievability::ACHIEVABLE;
	}

	// chasing all ships of a certain ship class is achievable if there are ships of that class present;
	// if not, the status is not known because more ships of that class could arrive in the future
	// (c.f. AI_GOAL_CHASE_WING subsequent to the next switch statement)
	if ( aigp->ai_mode == AI_GOAL_CHASE_SHIP_CLASS ) {
		for (auto so: list_range(&Ship_obj_list)) {
			auto class_objp = &Objects[so->objnum];
			if (class_objp->flags[Object::Object_Flags::Should_be_dead])
				continue;
			if ((class_objp->type == OBJ_SHIP) && !strcmp(aigp->target_name, Ship_info[Ships[class_objp->instance].ship_info_index].name)) {
				return ai_achievability::ACHIEVABLE;
			}
		}
		return ai_achievability::NOT_KNOWN;
	}


	return_val = ai_achievability::SATISFIED;

	// next, determine if the goal has been completed successfully
	switch ( aigp->ai_mode )
	{
		case AI_GOAL_DOCK:
		case AI_GOAL_UNDOCK:
			//MWA 3/20/97 -- cannot short circuit a dock or undock goal already succeeded -- we must
			// rely on the goal removal code to just remove this goal.  This is because docking/undock
			// can happen > 1 time per mission per pair of ships.  The above checks will find only
			// if the ships docked or undocked at all, which is not what we want.
			status = 0;
			break;

		case AI_GOAL_DESTROY_SUBSYSTEM:
		{
			ship_subsys *ssp;

			// shipnum could be -1 depending on if the ship hasn't arrived or died.  only look for subsystem
			// destroyed when shipnum is valid

			// can't determine the status of this goal if ship not valid
			// or we haven't found a valid subsystem index yet
			if ( !target_ship_entry || !target_ship_entry->has_shipp() || (aigp->flags[AI::Goal_Flags::Subsys_needs_fixup]) ) {
				status = 0;
				break;
			}

			// if the ship is not in the mission or the subsystem name is still being stored, mark the status
			// as 0 so we can continue.  (The subsystem name must be turned into an index into the ship's subsystems
			// for this goal to be valid).
			Assert ( aigp->ai_submode >= 0 );
			ssp = ship_get_indexed_subsys( target_ship_entry->shipp(), aigp->ai_submode );
			if (ssp != NULL) {
				// see MWA 3/20/97 comment above - instead of checking the mission log, check the current hits
				status = (ssp->current_hits <= 0.0f) ? 1 : 0;
			} else {
				// not supposed to ever happen, but could if there is a mismatch between the table and model subsystems
				nprintf(("AI", "Couldn't find subsystem %d for ship %s\n", aigp->ai_submode, target_ship_entry->shipp()->ship_name));
				status = 0;
			}
			break;
		}

		case AI_GOAL_DISABLE_SHIP:
		case AI_GOAL_DISABLE_SHIP_TACTICAL:
		{
			// shipnum could be -1 depending on if the ship hasn't arrived or died.  only look for subsystem
			// destroyed when shipnum is valid

			// can't determine the status of this goal if ship not valid
			if (!target_ship_entry || !target_ship_entry->has_shipp()) {
				status = 0;
			} else {
				// see MWA 3/20/97 comment above - instead of checking the mission log, check the current hits
				status = (target_ship_entry->shipp()->subsys_info[SUBSYSTEM_ENGINE].aggregate_current_hits <= 0.0f) ? 1 : 0;
			}
			break;
		}

		case AI_GOAL_DISARM_SHIP:
		case AI_GOAL_DISARM_SHIP_TACTICAL:
		{
			// shipnum could be -1 depending on if the ship hasn't arrived or died.  only look for subsystem
			// destroyed when shipnum is valid

			// can't determine the status of this goal if ship not valid
			if (!target_ship_entry || !target_ship_entry->has_shipp()) {
				status = 0;
			} else {
				// see MWA 3/20/97 comment above - instead of checking the mission log, check the current hits
				status = (target_ship_entry->shipp()->subsys_info[SUBSYSTEM_TURRET].aggregate_current_hits <= 0.0f) ? 1 : 0;
			}
			break;
		}

		// to guard or ignore a ship, the goal cannot continue if the ship being guarded is either destroyed
		// or has departed.
		case AI_GOAL_CHASE:
		case AI_GOAL_GUARD:
		case AI_GOAL_IGNORE:
		case AI_GOAL_IGNORE_NEW:
		case AI_GOAL_EVADE_SHIP:
		case AI_GOAL_STAY_NEAR_SHIP:
		case AI_GOAL_FLY_TO_SHIP:
		case AI_GOAL_REARM_REPAIR:
		{
			// MWA -- 4/22/98.  Check for the ship actually being in the mission before
			// checking departure and destroyed.  In multiplayer, since ships can respawn,
			// they get log entries for being destroyed even though they have respawned.
			// Goober5000 - update this to use ship registry status with similar logic
			if (target_ship_entry && target_ship_entry->status == ShipStatus::EXITED) {
				status = 1;
				return_val = ai_achievability::NOT_ACHIEVABLE;
			} else {
				status = 0;
			}

			// fly-to-ship can potentially be determined
			if (aigp->ai_mode == AI_GOAL_FLY_TO_SHIP && target_ship_entry && target_ship_entry->has_objp()) {
				auto dist = vm_vec_dist(&target_ship_entry->objp()->pos, &objp->pos);
				if (dist < aigp->float_data) {
					return_val = ai_achievability::SATISFIED;
					status = 1;
				}
			}

			break;
		}

		case AI_GOAL_CHASE_WING:
		case AI_GOAL_GUARD_WING:
		{
			status = mission_log_get_time( LOG_WING_DEPARTED, aigp->target_name, NULL, NULL );
			if ( !status ) {
				status = mission_log_get_time( LOG_WING_DESTROYED, aigp->target_name, NULL, NULL);
				if ( status )
					return_val = ai_achievability::NOT_ACHIEVABLE;
			}

			break;
		}

		// the following case statement returns control to caller on all paths!!!!
		case AI_GOAL_CHASE_WEAPON:
		{
			// for chase weapon, we simply need to look at the weapon instance that we are trying to
			// attack and see if the object still exists, and has the same signature that we expect.
			Assert( aigp->target_instance >= 0 );

			if ( Weapons[aigp->target_instance].objnum == -1 )
				return ai_achievability::NOT_ACHIEVABLE;

			// if the signatures don't match, then goal isn't achievable.
			if ( Objects[Weapons[aigp->target_instance].objnum].signature != aigp->target_signature )
				return ai_achievability::NOT_ACHIEVABLE;

			// otherwise, we should be good to go
			return ai_achievability::ACHIEVABLE;

			break;
		}

		default:
			UNREACHABLE("Unhandled AI goal %d", aigp->ai_mode);
			status = 0;
			break;
	}

	// if status is true, then the mission log event was found and the goal was satisfied.  return
	// AI_GOAL_SATISFIED which should allow this ai object to move onto the next order
	if ( status )
		return return_val;

	// determine the status of the shipname that this object is acting on.  There are a couple of
	// special cases to deal with.  Both the chase wing and undock commands will return from within
	// the if statement.
	if ( (aigp->ai_mode == AI_GOAL_CHASE_WING) || (aigp->ai_mode == AI_GOAL_GUARD_WING) )
	{
		int wingnum = wing_name_lookup( aigp->target_name );

		if (wingnum < 0)
			return ai_achievability::NOT_KNOWN;

		wing *wingp = &Wings[wingnum];

		if ( wingp->flags[Ship::Wing_Flags::Gone] )
			return ai_achievability::NOT_ACHIEVABLE;
		else if ( wingp->total_arrived_count == 0 )
			return ai_achievability::NOT_KNOWN;
		else
			return ai_achievability::ACHIEVABLE;
	}
	// Goober5000 - undocking from an unspecified object is always achievable;
	// undocking from a specified object is handled below
	else if ( (aigp->ai_mode == AI_GOAL_UNDOCK) && (aigp->target_name == NULL) )
	{
			return ai_achievability::ACHIEVABLE;
	}
	else
	{
		if (!target_ship_entry)
		{
			status = SHIP_STATUS_UNKNOWN;
		}
		// goal ship is currently in mission
		else if (target_ship_entry->status == ShipStatus::PRESENT || target_ship_entry->status == ShipStatus::DEATH_ROLL)
		{
			status = SHIP_STATUS_ARRIVED;
		}
		// goal ship is still on the arrival list
		else if (target_ship_entry->status == ShipStatus::NOT_YET_PRESENT)
		{
			status = SHIP_STATUS_NOT_ARRIVED;
		}
		// goal ship has left the area
		else if (target_ship_entry->status == ShipStatus::EXITED)
		{
			status = SHIP_STATUS_GONE;
		}
		else
		{
			status = SHIP_STATUS_UNKNOWN;
		}

		if (status == SHIP_STATUS_UNKNOWN)
		{
			mprintf(("Potentially incorrect behaviour in AI goal for ship %s: Ship %s could not be found among currently active, departed, or yet-to-arrive ships.\nPlease check the mission file.\n", shipp->ship_name, aigp->target_name));
		}
	}

	// Goober5000 - before doing anything else, check if this is a disarm goal for an arrived ship...
	if ((status == SHIP_STATUS_ARRIVED) && (aigp->ai_mode == AI_GOAL_DISARM_SHIP || aigp->ai_mode == AI_GOAL_DISARM_SHIP_TACTICAL))
	{
		if (target_ship_entry && target_ship_entry->has_shipp()) {
			// if the ship has no turrets, we can't disarm it!
			if (target_ship_entry->shipp()->subsys_info[SUBSYSTEM_TURRET].type_count == 0)
				return ai_achievability::NOT_ACHIEVABLE;
		} else {
			UNREACHABLE("Target name %s is not an arrived ship!", aigp->target_name);
			return ai_achievability::NOT_ACHIEVABLE;			// force this goal to be invalid
		}
	}

	// if the goal is an ignore/disable/disarm goal, then 
	// Goober5000 - see note at PURGE_GOALS_ALL_SHIPS... this is bizarre
	if ((status == SHIP_STATUS_ARRIVED) && !(aigp->flags[AI::Goal_Flags::Goals_purged]))
	{
		if (purge_goals_all_ships(aigp->ai_mode)) {
			ai_goal_purge_all_invalid_goals(aigp);
			aigp->flags.set(AI::Goal_Flags::Goals_purged);
		}
		else if (purge_goals_one_ship(aigp->ai_mode)) {
			ai_goal_purge_invalid_goals(aigp, aip->goals, aip, -1);
			aigp->flags.set(AI::Goal_Flags::Goals_purged);
		}
	}	

	// if we are docking, validate the docking indices on both ships.  We might have to change names to indices.
	// only enter this calculation if the ship we are docking with has arrived.  If the ship is gone, then
	// this goal will get removed.
	if ( (aigp->ai_mode == AI_GOAL_DOCK) && (status == SHIP_STATUS_ARRIVED) ) {
		if (!(aigp->flags[AI::Goal_Flags::Docker_index_valid])) {
			int modelnum = Ship_info[shipp->ship_info_index].model_num;
			Assert( modelnum >= 0 );
			aigp->docker.index = model_find_dock_name_index(modelnum, aigp->docker.name);
			aigp->flags.set(AI::Goal_Flags::Docker_index_valid);
		}

		if (!(aigp->flags[AI::Goal_Flags::Dockee_index_valid])) {
			if (target_ship_entry && target_ship_entry->has_shipp()) {
				int modelnum = Ship_info[target_ship_entry->shipp()->ship_info_index].model_num;
				aigp->dockee.index = model_find_dock_name_index(modelnum, aigp->dockee.name);
				aigp->flags.set(AI::Goal_Flags::Dockee_index_valid);
			} else
				aigp->dockee.index = -1;		// this will force code into if statement below making goal not achievable.
		}

		if ( (aigp->dockee.index == -1) || (aigp->docker.index == -1) ) {
			Warning(LOCATION, "Cannot determine docking information for %s!", shipp->ship_name);			// for now, allender wants to know about these things!!!!
			return ai_achievability::NOT_ACHIEVABLE;
		}

		// if ship is disabled, don't know if it can dock or not
		if ( shipp->flags[Ship::Ship_Flags::Disabled] )
			return ai_achievability::NOT_KNOWN;

		// we must also determine if we're prevented from docking for any reason
		if (!target_ship_entry || !target_ship_entry->has_shipp()) {
			UNREACHABLE("Target name %s is not an arrived ship!", aigp->target_name);
			return ai_achievability::NOT_ACHIEVABLE;			// force this goal to be invalid
		}
		auto goal_objp = target_ship_entry->objp();

		// if the ship that I am supposed to dock with is docked with something else, then I need to put my goal on hold
		//	[MK, 4/23/98: With Mark, we believe this fixes the problem of Comet refusing to warp out after docking with Omega.
		//	This bug occurred only when mission goals were validated in the frame in which Comet docked, which happened about
		// once in 10-20 tries.]
		if ( object_is_docked(goal_objp) )
		{
			// if the dockpoint I need to dock to is occupied by someone other than me
			object *obstacle_objp = dock_find_object_at_dockpoint(goal_objp, aigp->dockee.index);
			if (obstacle_objp == NULL)
			{
				// nobody in the way... we're good
			}
			else if (obstacle_objp != objp)
			{
				// return NOT_KNOWN which will place the goal on hold until the dockpoint is clear
				return ai_achievability::NOT_KNOWN;
			}
		}

		// if this ship is docked and needs to get docked with something else, then undock this ship
		if ( object_is_docked(objp) )
		{
			// if the dockpoint I need to dock with is occupied by someone other than the guy I need to dock to
			object *obstacle_objp = dock_find_object_at_dockpoint(objp, aigp->docker.index);
			if (obstacle_objp == NULL)
			{
				// nobody in the way... we're good
			}
			else if (obstacle_objp != goal_objp)
			{
				// if this goal isn't on hold yet, then issue the undock goal
				if ( !(aigp->flags[AI::Goal_Flags::Goal_on_hold]) )
					ai_add_goal_ship_internal( aip, AI_GOAL_UNDOCK, Ships[obstacle_objp->instance].ship_name, -1, -1, 0 );

				// return NOT_KNOWN which will place the goal on hold until the undocking is complete.
				return ai_achievability::NOT_KNOWN;
			}
		}

	// Goober5000 - necessitated by the multiple ship docking
	} else if ( (aigp->ai_mode == AI_GOAL_UNDOCK) && (status == SHIP_STATUS_ARRIVED) ) {
		// Put this goal on hold if we're already undocking.  Otherwise the new goal will pre-empt
		// the current goal and strange things might happen.  One is that the object movement code
		// forgets the previous undocking and "re-docks" the previous goal's ship.  Other problems
		// might happen too, so err on the safe side.  (Yay for emergent paragraph justification!)
		if ((aip->mode == AIM_DOCK) && (aip->submode >= AIS_UNDOCK_0))
		{
			if ( target_ship_entry && target_ship_entry->has_objp() ) {
				// only put it on hold if it's someone other than the guy we're undocking from right now!!
				if (aip->goal_objnum != target_ship_entry->objnum)
					return ai_achievability::NOT_KNOWN;
			} else {
				UNREACHABLE("Target name %s is not an arrived ship!", aigp->target_name);
				return ai_achievability::NOT_ACHIEVABLE;			// force this goal to be invalid
			}
		}

	} else if ( (aigp->ai_mode == AI_GOAL_DESTROY_SUBSYSTEM) && (status == SHIP_STATUS_ARRIVED) ) {
		// if the ship has arrived, and the goal is destroy subsystem, then check to see that we
		// have fixed up the subsystem name (of the subsystem to destroy) into an index into
		// the ship's subsystem list
		if ( aigp->flags[AI::Goal_Flags::Subsys_needs_fixup] ) {
			if ( target_ship_entry && target_ship_entry->has_shipp() ) {
				aigp->ai_submode = ship_find_subsys( target_ship_entry->shipp(), aigp->docker.name );
				aigp->flags.remove(AI::Goal_Flags::Subsys_needs_fixup);
			} else {
				UNREACHABLE("Target name %s is not an arrived ship!", aigp->target_name);
				return ai_achievability::NOT_ACHIEVABLE;			// force this goal to be invalid
			}
		}
	} else if ( ((aigp->ai_mode == AI_GOAL_IGNORE) || (aigp->ai_mode == AI_GOAL_IGNORE_NEW)) && (status == SHIP_STATUS_ARRIVED) ) {
		// for ignoring a ship, call the ai_ignore object function, then declare the goal satisfied
		if (!target_ship_entry || !target_ship_entry->has_objp()) {
			UNREACHABLE("Target name %s is not an arrived ship!", aigp->target_name);
			return ai_achievability::NOT_ACHIEVABLE;			// force this goal to be invalid
		}
		auto ignored = target_ship_entry->objp();

		ai_ignore_object(objp, ignored, (aigp->ai_mode == AI_GOAL_IGNORE_NEW));

		return ai_achievability::SATISFIED;
	}

	switch ( aigp->ai_mode )
	{
		case AI_GOAL_CHASE:
		case AI_GOAL_CHASE_WING:
		case AI_GOAL_DOCK:
		case AI_GOAL_UNDOCK:
		case AI_GOAL_GUARD:
		case AI_GOAL_GUARD_WING:
		case AI_GOAL_DISABLE_SHIP:
		case AI_GOAL_DISABLE_SHIP_TACTICAL:
		case AI_GOAL_DISARM_SHIP:
		case AI_GOAL_DISARM_SHIP_TACTICAL:
		case AI_GOAL_DESTROY_SUBSYSTEM:
		case AI_GOAL_IGNORE:
		case AI_GOAL_IGNORE_NEW:
		case AI_GOAL_EVADE_SHIP:
		case AI_GOAL_STAY_NEAR_SHIP:
		case AI_GOAL_FLY_TO_SHIP:
		{
			if ( status == SHIP_STATUS_ARRIVED )
				return ai_achievability::ACHIEVABLE;
			else if ( status == SHIP_STATUS_NOT_ARRIVED )
				return ai_achievability::NOT_KNOWN;
			else if ( status == SHIP_STATUS_GONE )
				return ai_achievability::NOT_ACHIEVABLE;
			else if ( status == SHIP_STATUS_UNKNOWN )
				return ai_achievability::NOT_KNOWN;

			UNREACHABLE("Invalid status variable %d for ship %s; get Allender or a SCP member", status, shipp->ship_name);		// get allender -- bad logic
			break;
		}

		// for rearm repair ships, a goal is only achievable if the support ship isn't repairing anything
		// else at the time, or is set to repair the ship for this goal.  All other goals should be placed
		// on hold by returning GOAL_NOT_KNOWN.
		case AI_GOAL_REARM_REPAIR:
		{
			// short circuit a couple of cases.  Ship not arrived shouldn't happen.  Ship gone means
			// we mark the goal as not achievable.
			if ( status == SHIP_STATUS_NOT_ARRIVED ) {
				UNREACHABLE("Ship %s cannot rearm a target %s that hasn't arrived; get Allender or a SCP member", shipp->ship_name, aigp->target_name);	// get Allender.  this shouldn't happen!!!
				return ai_achievability::NOT_ACHIEVABLE;
			}

			if ( status == SHIP_STATUS_GONE )
				return ai_achievability::NOT_ACHIEVABLE;

			if ( !target_ship_entry || !target_ship_entry->has_shipp() ) {
				UNREACHABLE("Target name %s is not an arrived ship!", aigp->target_name);
				return ai_achievability::NOT_ACHIEVABLE;
			}

			// if destination currently being repaired, then goal is still active
			if ( Ai_info[target_ship_entry->shipp()->ai_index].ai_flags[AI::AI_Flags::Being_repaired] )
				return ai_achievability::ACHIEVABLE;

			// if the destination ship is dying or departing (but not completed yet), the mark goal as
			// not achievable.
			if ( target_ship_entry->shipp()->is_dying_or_departing())
				return ai_achievability::NOT_ACHIEVABLE;

			// if the destination object is no longer awaiting repair, then remove the item
			if ( !(Ai_info[target_ship_entry->shipp()->ai_index].ai_flags[AI::AI_Flags::Awaiting_repair]) )
				return ai_achievability::NOT_ACHIEVABLE;

			// not repairing anything means that he can do this goal!!!
			if ( !(aip->ai_flags[AI::AI_Flags::Repairing]) )
				return ai_achievability::ACHIEVABLE;

			// test code!!!
			if ( aip->goal_objnum == -1 ) {
				return ai_achievability::ACHIEVABLE;
			}

			// if he is repairing something, he can satisfy his repair goal (his goal_objnum)
			// return GOAL_NOT_KNOWN which is kind of a hack which puts the goal on hold until it can be
			// satisfied.  
			if ( aip->goal_objnum != target_ship_entry->objnum )
				return ai_achievability::NOT_KNOWN;

			return ai_achievability::ACHIEVABLE;
		}

		default:
			UNREACHABLE("Unhandled AI goal %d", aigp->ai_mode);			// invalid case in switch:
	}

	return ai_achievability::NOT_KNOWN;
}

//	Compare function for sorting ai_goals based on priority.
//	Return values set to sort array in _decreasing_ order.
int ai_goal_priority_compare(const ai_goal *ga, const ai_goal *gb)
{
	// first, sort based on whether or not the ON_HOLD flag is set for the goal.
	// If the flag is set, don't push the goal higher in the list even if priority
	// is higher since goal cannot currently be achieved.

	if ( (ga->flags[AI::Goal_Flags::Goal_on_hold]) && !(gb->flags[AI::Goal_Flags::Goal_on_hold]) )
		return 1;
	else if ( !(ga->flags[AI::Goal_Flags::Goal_on_hold]) && (gb->flags[AI::Goal_Flags::Goal_on_hold]) )
		return -1;

	// check whether or not the goal override flag is set.  If it is set, then push this goal higher
	// in the list

	// Goober5000: the ONLY goals we do not override are play-dead and play-dead-persistent,
	// because the override flag needs to work in any other situation

	else if ( (ga->flags[AI::Goal_Flags::Goal_override]) && !(gb->flags[AI::Goal_Flags::Goal_override]) && !(gb->ai_mode == AI_GOAL_PLAY_DEAD) && !(gb->ai_mode == AI_GOAL_PLAY_DEAD_PERSISTENT) )
		return -1;
	else if ( !(ga->flags[AI::Goal_Flags::Goal_override]) && (gb->flags[AI::Goal_Flags::Goal_override]) && !(ga->ai_mode == AI_GOAL_PLAY_DEAD) && !(ga->ai_mode == AI_GOAL_PLAY_DEAD_PERSISTENT))
		return 1;

	// now normal priority processing

	if (ga->priority > gb->priority)
		return -1;
	else if ( ga->priority < gb->priority )
		return 1;

	// check based on time goal was issued

	if ( ga->time > gb->time )
		return -1;
	// V had this check commented out and would always return 1 here, that messes up where multiple goals 
	// get assigned at the same time though, when the priorities are also the same (Enif station bug) - taylor
	else if ( ga->time < gb->time )
		return 1;

	// the two are equal
	return 0;
}

//	Prioritize goal list.
//	First sort on priority.
//	Then sort on time for goals of equivalent priority.
//	*aip		The AI info to act upon.  Goals are stored at aip->goals
void prioritize_goals(ai_info *aip)
{
	//	First sort based on priority field.
	insertion_sort(aip->goals, MAX_AI_GOALS, ai_goal_priority_compare);
}

//	Scan the list of goals at aip->goals.
//	Remove obsolete goals.
//	objnum	Object of interest.  Redundant with *aip.
//	*aip		contains goals at aip->goals.
void validate_mission_goals(int objnum, ai_info *aip)
{
	int	i;
	
	// loop through all of the goals to determine which goal should be followed.
	// This determination will be based on priority, and the time at which it was issued.
	for ( i = 0; i < MAX_AI_GOALS; i++ ) {
		ai_achievability state;
		ai_goal	*aigp;

		aigp = &aip->goals[i];

		// quick check to see if this goal is valid or not, or if we are trying to process the
		// current goal
		if (aigp->ai_mode == AI_GOAL_NONE)
			continue;

		// purge any goals which should get purged
		if ( aigp->flags[AI::Goal_Flags::Purge] ) {
			ai_remove_ship_goal( aip, i );
			continue;
		}

		state = ai_mission_goal_achievable( objnum, aigp );

		// if this order is no longer a valid one, remove it
		if ( (state == ai_achievability::NOT_ACHIEVABLE) || (state == ai_achievability::SATISFIED) ) {
			ai_remove_ship_goal( aip, i );
			continue;
		}

		// if the status is achievable, and the on_hold flag is set, clear the flagb
		if ( (state == ai_achievability::ACHIEVABLE) && (aigp->flags[AI::Goal_Flags::Goal_on_hold]) )
			aigp->flags.remove(AI::Goal_Flags::Goal_on_hold);

		// if the goal is not known, then set the ON_HOLD flag so that it doesn't get counted as
		// a goal to be pursued
		if (state == ai_achievability::NOT_KNOWN)
			aigp->flags.set(AI::Goal_Flags::Goal_on_hold);		// put this goal on hold until it becomes true
	}

	// if we had an active goal, and that goal is now in hold, make the mode AIM_NONE.  If a new valid
	// goal is produced after prioritizing, then the mode will get reset immediately.  Otherwise, setting
	// the mode to none will force ship to do default behavior.
	if ( (aip->goals[0].ai_mode != AI_GOAL_NONE) && (aip->goals[0].flags[AI::Goal_Flags::Goal_on_hold]) )
		aip->mode = AIM_NONE;

	// if the active goal is a rearm/repair or undock goal, 
	// then put all other valid goals (which are not rearm/repair or undock goals) on hold
	if ( (aip->goals[0].ai_mode == AI_GOAL_REARM_REPAIR || aip->goals[0].ai_mode == AI_GOAL_UNDOCK) && object_is_docked(&Objects[objnum]) ) {
		for ( i = 1; i < MAX_AI_GOALS; i++ ) {
			if ( aip->goals[i].ai_mode == AI_GOAL_NONE || aip->goals[i].ai_mode == AI_GOAL_REARM_REPAIR || aip->goals[i].ai_mode == AI_GOAL_UNDOCK )
				continue;
			aip->goals[i].flags.set(AI::Goal_Flags::Goal_on_hold);
		}
	}
}

//XSTR:OFF
/*
static char *Goal_text[5] = {
"EVENT_SHIP",
"EVENT_WING",
"PLAYER_SHIP",
"PLAYER_WING",
"DYNAMIC",
};
*/
//XSTR:ON

extern char *Mode_text[MAX_AI_BEHAVIORS];

// code to process ai "orders".  Orders include those determined from the mission file and those
// given by the player to a ship that is under his control.  This function gets called for every
// AI object every N seconds through the ai loop.
void ai_process_mission_orders( int objnum, ai_info *aip )
{
	object	*objp = &Objects[objnum];
	object	*other_obj;
	ai_goal	*current_goal;
	int		wingnum, shipnum;
	int		original_signature;

/*	if (!stricmp(Ships[objp->instance].ship_name, "gtt comet")) {
		for (int i=0; i<MAX_AI_GOALS; i++) {
			if (aip->goals[i].signature != -1) {
				nprintf(("AI", "%6.1f: mode=%s, type=%s, ship=%s\n", f2fl(Missiontime), Mode_text[aip->goals[i].ai_mode], Goal_text[aip->goals[i].type], aip->goals[i].ship_name));
			}
		}
		nprintf(("AI", "\n"));
	}
*/

	// AL 12-12-97: If a ship is entering/leaving a docking bay, wait until path
	//					 following is finished before pursuing goals.
	if ( aip->mode == AIM_BAY_EMERGE || aip->mode == AIM_BAY_DEPART ) {
		return;
	}

	//	Goal #0 is always the active goal, as we maintain a sorted list.
	//	Get the signature to see if sorting it again changes it.
	original_signature = aip->goals[0].signature;

	validate_mission_goals(objnum, aip);

	//	Sort the goal array by priority and other factors.
	prioritize_goals(aip);

	//	Make sure there's a goal to pursue, else return.
	if (aip->goals[0].signature == -1) {
		if (aip->mode == AIM_NONE)
			ai_do_default_behavior(objp);
		return;
	}

	//	If goal didn't change, return.
	if ((aip->active_goal != -1) && (aip->goals[0].signature == original_signature))
		return;

	// if the first goal in the list has the ON_HOLD flag, set, there is no current valid goal
	// to pursue.
	if ( aip->goals[0].flags[AI::Goal_Flags::Goal_on_hold] )
		return;

	//	Kind of a hack for now.  active_goal means the goal currently being pursued.
	//	It will always be #0 since the list is prioritized.
	aip->active_goal = 0;

	//nprintf(("AI", "New goal for %s = %i\n", Ships[objp->instance].ship_name, aip->goals[0].ai_mode));

	current_goal = &aip->goals[0];

	if ( MULTIPLAYER_MASTER ){
		send_ai_info_update_packet( objp, AI_UPDATE_ORDERS );
	}

	// if this object was flying in formation off of another object, remove the flag that tells him
	// to do this.  The form-on-my-wing command is removed from the goal list as soon as it is called, so
	// we are safe removing this bit here.
	int old_form_objnum = -1;
	if (aip->ai_flags[AI::AI_Flags::Formation_object]) {
		// save who he was following so we can tell any others 
		// who are following to re-organize
		old_form_objnum = aip->goal_objnum;
		aip->ai_flags.remove(AI::AI_Flags::Formation_object);
	}

	// Goober5000 - we may want to use AI for the player
	// AL 3-7-98: If this is a player ship, and the goal is not a formation goal, then do a quick out
	if ( !(Player_use_ai) && (objp->flags[Object::Object_Flags::Player_ship]) && (current_goal->ai_mode != AI_GOAL_FORM_ON_WING) )
	{
		return;
	}	



	switch ( current_goal->ai_mode ) {

	case AI_GOAL_CHASE:
		if ( current_goal->target_name ) {
			shipnum = ship_name_lookup( current_goal->target_name );
			Assert (shipnum != -1 );			// shouldn't get here if this is false!!!!
			other_obj = &Objects[Ships[shipnum].objnum];
		} else
			other_obj = NULL;						// we get this case when we tell ship to engage enemy!

		//	Mike -- debug code!
		//	If a ship has a subobject on it, attack that instead of the main ship!
		ai_attack_object( objp, other_obj);
		break;

	case AI_GOAL_CHASE_WEAPON:
		Assert( Weapons[current_goal->target_instance].objnum >= 0 );
		other_obj = &Objects[Weapons[current_goal->target_instance].objnum];
		Assert( other_obj->signature == current_goal->target_signature );
		ai_attack_object( objp, other_obj);
		break;

	case AI_GOAL_GUARD:
		shipnum = ship_name_lookup( current_goal->target_name );
		Assert (shipnum != -1 );			// shouldn't get here if this is false!!!!
		other_obj = &Objects[Ships[shipnum].objnum];
		// shipnum and other_obj are the shipnumber and object pointer of the object that you should
		// guard.
		if (objp != other_obj) {
			ai_set_guard_object(objp, other_obj);
			aip->submode_start_time = Missiontime;
		} else {
			mprintf(("Warning: Ship %s told to guard itself.  Goal ignored.\n", Ships[objp->instance].ship_name));
		}
		// -- What is this doing here?? -- MK, 7/30/97 -- ai_do_default_behavior( objp );
		break;

	case AI_GOAL_GUARD_WING:
		wingnum = wing_name_lookup( current_goal->target_name );
		Assert (wingnum != -1 );			// shouldn't get here if this is false!!!!
		ai_set_guard_wing(objp, wingnum);
		aip->submode_start_time = Missiontime;
		break;

	case AI_GOAL_WAYPOINTS:				// do nothing for waypoints
	case AI_GOAL_WAYPOINTS_ONCE: {
		int flags = 0;
		if (current_goal->ai_mode == AI_GOAL_WAYPOINTS)
			flags |= WPF_REPEAT;
		if (current_goal->flags[AI::Goal_Flags::Waypoints_in_reverse])
			flags |= WPF_BACKTRACK;
		ai_start_waypoints(objp, current_goal->wp_list_index, flags, current_goal->int_data);
		break;
	}

	case AI_GOAL_DOCK: {
		shipnum = ship_name_lookup( current_goal->target_name );
		Assert (shipnum != -1 );			// shouldn't get here if this is false!!!!
		other_obj = &Objects[Ships[shipnum].objnum];

		// be sure that we have indices for docking points here!  If we ever had names, they should
		// get fixed up in goal_achievable so that the points can be checked there for validity
		Assert (current_goal->flags[AI::Goal_Flags::Dockee_index_valid] && current_goal->flags[AI::Goal_Flags::Docker_index_valid]);
		ai_dock_with_object( objp, current_goal->docker.index, other_obj, current_goal->dockee.index, AIDO_DOCK );
		break;
	}

	case AI_GOAL_UNDOCK:
		// try to find the object which which this object is docked with.  Use that object as the
		// "other object" for the undocking proceedure.  If "other object" isn't found, then the undock
		// goal cannot continue.  Spit out a warning and remove the goal.

		// Goober5000 - do we have a specific ship to undock from?
		if ( current_goal->target_name != NULL )
		{
			shipnum = ship_name_lookup( current_goal->target_name );

			// hmm, perhaps he was destroyed
			if (shipnum == -1)
			{
				other_obj = NULL;
			}
			// he exists... let's undock from him
			else
			{
				other_obj = &Objects[Ships[shipnum].objnum];
			}
		}
		// no specific ship
		else
		{
			// are we docked?
			if (object_is_docked(objp))
			{
				// just pick the first guy we're docked to
				other_obj = dock_get_first_docked_object( objp );

				// and add the ship name so it displays on the HUD
				current_goal->target_name = Ships[other_obj->instance].ship_name;
			}
			// hmm, nobody exists that we can undock from
			else
			{
				other_obj = NULL;
			}
		}

		if ( other_obj == NULL ) {
			// assume that the guy he was docked with doesn't exist anymore.  (i.e. a cargo containuer
			// can get destroyed while docked with a freighter.)  We should just remove this goal and
			// let this ship pick up it's next goal.
			ai_mission_goal_complete( aip );		// mark as complete, so we can remove it and move on!!!
			break;
		}

		// Goober5000 - Sometimes a ship will be assigned a new goal before it can finish undocking.  Later,
		// when the ship returns to this goal, it will try to resume undocking from a ship it's not attached
		// to.  If this happens, remove the goal as above.
		if (!dock_check_find_direct_docked_object(objp, other_obj))
		{
			ai_mission_goal_complete( aip );
			break;
		}

		// passing 0, 0 is okay because the undock code will figure out where to undock from
		ai_dock_with_object( objp, 0, other_obj, 0, AIDO_UNDOCK );
		break;


	// when destroying a subsystem, we can destroy a specific instance of a subsystem
	// or all instances of a type of subsystem (i.e. a specific engine or all engines).
	// the ai_submode value is > 0 for a specific instance of subsystem and < 0 for all
	// instances of a specific type
	case AI_GOAL_DESTROY_SUBSYSTEM:
	case AI_GOAL_DISABLE_SHIP:
	case AI_GOAL_DISABLE_SHIP_TACTICAL:
	case AI_GOAL_DISARM_SHIP:
	case AI_GOAL_DISARM_SHIP_TACTICAL: {
		shipnum = ship_name_lookup( current_goal->target_name );
		Assert( shipnum >= 0 );
		other_obj = &Objects[Ships[shipnum].objnum];
		ai_attack_object( objp, other_obj);
		ai_set_attack_subsystem( objp, current_goal->ai_submode );		// submode stored the subsystem type

		// don't protect-ship for tactical goals
		if (current_goal->ai_mode != AI_GOAL_DESTROY_SUBSYSTEM && current_goal->ai_mode != AI_GOAL_DISABLE_SHIP_TACTICAL && current_goal->ai_mode != AI_GOAL_DISARM_SHIP_TACTICAL) {
			if (aip->target_objnum != -1) {
				int class_type = Ship_info[Ships[shipnum].ship_info_index].class_type;
				//	Only protect if _not_ a capital ship.  We don't want the Lucifer accidentally getting protected.
				if (class_type >= 0 && Ship_types[class_type].flags[Ship::Type_Info_Flags::AI_protected_on_cripple])
					Objects[aip->target_objnum].flags.set(Object::Object_Flags::Protected);
			}
		} else	//	Just in case this ship had been protected, unprotect it.
			if (aip->target_objnum != -1)
				Objects[aip->target_objnum].flags.remove(Object::Object_Flags::Protected);

		break;
	}

	case AI_GOAL_CHASE_WING:
		wingnum = wing_name_lookup( current_goal->target_name );
		Assertion( wingnum >= 0, "The target of AI_GOAL_CHASE_WING must refer to a valid wing!" );
		ai_attack_wing(objp, wingnum);
		break;

	case AI_GOAL_CHASE_ANY:
		ai_attack_object( objp, nullptr);
		break;

	// chase-ship-class is chase-any but restricted to a subset of ships
	case AI_GOAL_CHASE_SHIP_CLASS:
		shipnum = ship_info_lookup( current_goal->target_name );
		Assertion( shipnum >= 0, "The target of AI_GOAL_CHASE_SHIP_CLASS must refer to a valid ship class!" );
		ai_attack_object( objp, nullptr, shipnum );
		break;

	case AI_GOAL_WARP: {
		mission_do_departure( objp, true );
		break;
	}

	case AI_GOAL_EVADE_SHIP:
		shipnum = ship_name_lookup( current_goal->target_name );
		Assert( shipnum >= 0 );
		other_obj = &Objects[Ships[shipnum].objnum];
		ai_evade_object( objp, other_obj);
		break;

	case AI_GOAL_STAY_STILL:
		// for now, ignore any other parameters!!!!
		// clear out the object's goals.  Seems to me that if a ship is staying still for a purpose
		// then we need to clear everything out since there is not a real way to get rid of this goal
		// clearing out goals is okay here since we are now what mode to set this AI object to.
		ai_clear_ship_goals( aip );
		ai_stay_still( objp, NULL );
		break;

	case AI_GOAL_PLAY_DEAD:
		// if a ship is playing dead, MWA says that it shouldn't try to do anything else.
		// clearing out goals is okay here since we are now what mode to set this AI object to.
		ai_clear_ship_goals( aip );
		aip->mode = AIM_PLAY_DEAD;
		aip->submode = -1;
		aip->submode_start_time = Missiontime;
		break;

	case AI_GOAL_PLAY_DEAD_PERSISTENT:
		// same as above, but we don't clear out ship goals
		aip->mode = AIM_PLAY_DEAD;
		aip->submode = -1;
		aip->submode_start_time = Missiontime;
		break;

	case AI_GOAL_FORM_ON_WING:
		// get the ship first, since we're going to wipe it out next
		shipnum = ship_name_lookup( current_goal->target_name );
		Assert( shipnum >= 0 );
		other_obj = &Objects[Ships[shipnum].objnum];
		// for form on wing, we need to clear out all goals for this ship, and then call the form
		// on wing AI code
		// clearing out goals is okay here since we are now what mode to set this AI object to.
		ai_clear_ship_goals( aip );
		ai_form_on_wing( objp, other_obj );
		break;

// labels for support ship commands

	case AI_GOAL_STAY_NEAR_SHIP:
	case AI_GOAL_FLY_TO_SHIP:
	{
		shipnum = ship_name_lookup( current_goal->target_name );
		Assert( shipnum >= 0 );
		other_obj = &Objects[Ships[shipnum].objnum];
		float dist = current_goal->float_data;	//	How far away to stay from ship.  Should be set in SEXP?
		int additional_data = current_goal->int_data;	// Whether to target a particular point as if escorting
		ai_do_stay_near(objp, other_obj, dist, additional_data);
		break;
	}

	case AI_GOAL_KEEP_SAFE_DISTANCE:
		// todo MK: hook to keep support ship at a safe distance

		// Goober5000 - hmm, never implemented - let's see about that
		ai_do_safety(objp);
		break;

	case AI_GOAL_REARM_REPAIR:
		shipnum = ship_name_lookup( current_goal->target_name );
		Assert( shipnum >= 0 );
		other_obj = &Objects[Ships[shipnum].objnum];
		ai_rearm_repair( objp, current_goal->docker.index, other_obj, current_goal->dockee.index );
		break;
		
	case AI_GOAL_LUA:
		ai_lua_start(current_goal, objp);
		break;

	default:
		UNREACHABLE("unsupported goal of %d found in ai_process_mission_orders. Please report to the SCP", current_goal->ai_mode);
		break;
	}

	if (old_form_objnum >= 0)
		ai_formation_object_recalculate_slotnums(old_form_objnum);
}

void ai_update_goal_references(ai_goal *goals, sexp_ref_type type, const char *old_name, const char *new_name)
{
	int i, mode, flag, dummy;

	for (i=0; i<MAX_AI_GOALS; i++)  // loop through all the goals in the Ai_info entry
	{
		mode = goals[i].ai_mode;
		flag = 0;
		switch (type)
		{
			case sexp_ref_type::SHIP:
			case sexp_ref_type::PLAYER:
				switch (mode)
				{
					case AI_GOAL_CHASE:
					case AI_GOAL_DOCK:
					case AI_GOAL_DESTROY_SUBSYSTEM:
					case AI_GOAL_GUARD:
					case AI_GOAL_DISABLE_SHIP:
					case AI_GOAL_DISABLE_SHIP_TACTICAL:
					case AI_GOAL_DISARM_SHIP:
					case AI_GOAL_DISARM_SHIP_TACTICAL:
					case AI_GOAL_IGNORE:
					case AI_GOAL_IGNORE_NEW:
					case AI_GOAL_EVADE_SHIP:
					case AI_GOAL_STAY_NEAR_SHIP:
						flag = 1;
				}
				break;

			case sexp_ref_type::WING:
				switch (mode)
				{
					case AI_GOAL_CHASE_WING:
					case AI_GOAL_GUARD_WING:
						flag = 1;
				}
				break;

			case sexp_ref_type::WAYPOINT:
				switch (mode)
				{
					case AI_GOAL_WAYPOINTS:
					case AI_GOAL_WAYPOINTS_ONCE:
						flag = 1;
				}
				break;

			case sexp_ref_type::WAYPOINT_PATH:
				switch (mode)
				{
					case AI_GOAL_WAYPOINTS:
					case AI_GOAL_WAYPOINTS_ONCE:
					
						flag = 1;
				}
				break;

			default:
				Warning(LOCATION, "unhandled FRED reference type %d in ai_update_goal_references", static_cast<int>(type));
				break;
		}

		if (flag)  // is this a valid goal to parse for this conversion?
		{
			if (!stricmp(goals[i].target_name, old_name))
			{
				if (*new_name == '<')  // target was just deleted..
					goals[i].ai_mode = AI_GOAL_NONE;
				else
					goals[i].target_name = ai_get_goal_target_name(new_name, &dummy);
			}
		}
	}
}

bool query_referenced_in_ai_goals(ai_goal *goals, sexp_ref_type type, const char *name)
{
	int i, mode, flag;

	for (i=0; i<MAX_AI_GOALS; i++)  // loop through all the goals in the Ai_info entry
	{
		mode = goals[i].ai_mode;
		flag = 0;
		switch (type)
		{
			case sexp_ref_type::SHIP:
			case sexp_ref_type::PLAYER:
				switch (mode)
				{
					case AI_GOAL_CHASE:
					case AI_GOAL_DOCK:
					case AI_GOAL_DESTROY_SUBSYSTEM:
					case AI_GOAL_GUARD:
					case AI_GOAL_DISABLE_SHIP:
					case AI_GOAL_DISABLE_SHIP_TACTICAL:
					case AI_GOAL_DISARM_SHIP:
					case AI_GOAL_DISARM_SHIP_TACTICAL:
					case AI_GOAL_IGNORE:
					case AI_GOAL_IGNORE_NEW:
					case AI_GOAL_EVADE_SHIP:
					case AI_GOAL_STAY_NEAR_SHIP:
						flag = 1;
				}
				break;

			case sexp_ref_type::WING:
				switch (mode)
				{
					case AI_GOAL_CHASE_WING:
					case AI_GOAL_GUARD_WING:
						flag = 1;
				}
				break;

			case sexp_ref_type::WAYPOINT:
				switch (mode)
				{
					case AI_GOAL_WAYPOINTS:
					case AI_GOAL_WAYPOINTS_ONCE:
						flag = 1;
				}
				break;

			case sexp_ref_type::WAYPOINT_PATH:
				switch (mode)
				{
					case AI_GOAL_WAYPOINTS:
					case AI_GOAL_WAYPOINTS_ONCE:
						flag = 1;
				}
				break;
		}

		if (flag)  // is this a valid goal to parse for this conversion?
		{
			if (!stricmp(goals[i].target_name, name))
				return true;
		}
	}

	return false;
}

char *ai_add_dock_name(const char *str)
{
	char *ptr;
	int i;

	Assert(strlen(str) <= NAME_LENGTH - 1);
	for (i=0; i<Num_ai_dock_names; i++)
		if (!stricmp(Ai_dock_names[i], str))
			return Ai_dock_names[i];

	Assert(Num_ai_dock_names < MAX_AI_DOCK_NAMES);
	ptr = Ai_dock_names[Num_ai_dock_names++];
	strcpy(ptr, str);
	return ptr;
}
