/*
 * Copyright (C) Volition, Inc. 1999.  All rights reserved.
 *
 * All source code herein is the property of Volition, Inc. You may not sell 
 * or otherwise commercially exploit the source or things you created based on the 
 * source.
 *
*/


#include "autopilot/autopilot.h"
#include "camera/camera.h"
#include "controlconfig/controlsconfig.h"
#include "debugconsole/console.h"
#include "freespace.h"
#include "gamesequence/gamesequence.h"
#include "gamesnd/gamesnd.h"
#include "globalincs/linklist.h"
#include "hud/hud.h"
#include "hud/hudmessage.h"
#include "hud/hudsquadmsg.h"
#include "hud/hudtargetbox.h"
#include "io/joy.h"
#include "io/joy_ff.h"
#include "io/mouse.h"
#include "io/timer.h"
#include "headtracking/headtracking.h"
#include "mission/missiongoals.h"
#include "mission/missionmessage.h"
#include "network/multi_obj.h"
#include "network/multiutil.h"
#include "object/object.h"
#include "object/objectdock.h"
#include "observer/observer.h"
#include "options/Option.h"
#include "parse/parselo.h"
#include "playerman/player.h"
#include "ship/ship.h"
#include "ship/shipfx.h"
#include "weapon/weapon.h"

#ifndef NDEBUG
#include "io/key.h"
#endif


////////////////////////////////////////////////////////////
// Global object and other interesting player type things
////////////////////////////////////////////////////////////
player	Players[MAX_PLAYERS];

int		Player_num;
player	*Player = NULL;

// Goober5000
bool	Player_use_ai = false;

int		lua_game_control = 0;

physics_info Descent_physics;			// used when we want to control the player like the descent ship

angles chase_slew_angles;

angles Player_flight_cursor;

FlightMode Player_flight_mode = FlightMode::ShipLocked;
bool Perspective_locked = false;
bool Slew_locked = false;

static void parse_flight_mode_func()
{
	SCP_string mode;
	stuff_string(mode, F_NAME);

	// Convert to lowercase once
	SCP_tolower(mode);

	// Use a map to associate strings with their respective actions
	static const std::unordered_map<std::string, std::function<void()>> effectActions =
	{ 
		{"ship locked", []() { Player_flight_mode = FlightMode::ShipLocked; }},
		{"flight cursor", []() { Player_flight_mode = FlightMode::FlightCursor; }}
	};

	auto it = effectActions.find(mode);
	if (it != effectActions.end()) {
		it->second(); // Execute the corresponding action
	} else {
		error_display(0, "%s is not a valid flight mode setting", mode.c_str());
	}
}

auto FlightModeOption = options::OptionBuilder<FlightMode>("Game.FlightMode",
	std::pair<const char*, int>{"Flight Mode", 1842},
	std::pair<const char*, int>{"Choose the flying style to use during gameplay.", 1843})
	.category(std::make_pair("Game", 1824))
	.level(options::ExpertLevel::Beginner)
	.values({ {FlightMode::ShipLocked, {"Ship Locked", 1844}},
			{FlightMode::FlightCursor, {"Flight Cursor", 1845}} })
		.default_func([]() { return FlightMode::ShipLocked; })
	.bind_to(&Player_flight_mode)
	.flags({ options::OptionFlags::ForceMultiValueSelection })
	.importance(45)
	.parser(parse_flight_mode_func)
	.finish();

static SCP_string degrees_display(float val)
{
	auto degrees = fl_degrees(val);
	SCP_string out;
	sprintf(out, u8"%.1f\u00B0", degrees);
	return out;
}

float Flight_cursor_extent;

static void parse_flight_cursor_extent_func()
{
	float value;
	stuff_float(&value);
	CLAMP(value, 0.0f, 0.698f);
	Flight_cursor_extent = value;
}

auto FlightCursorExtentOption = options::OptionBuilder<float>("Game.FlightCursorExtent",
	std::pair<const char*, int>{"Flight Cursor Extent", 1846},
	std::pair<const char*, int>{"How far from the center the cursor can go.", 1847})
	.category(std::make_pair("Game", 1824))
	.range(0.0f, 0.698f)
	.display(degrees_display)
	.default_func([]() { return 0.348f; })
	.level(options::ExpertLevel::Beginner)
	.bind_to(&Flight_cursor_extent)
	.importance(44)
	.parser(parse_flight_cursor_extent_func)
	.finish();

float Flight_cursor_deadzone;

static void parse_cursor_deadzone_func()
{
	float value;
	stuff_float(&value);
	CLAMP(value, 0.0f, 0.349f);
	Flight_cursor_deadzone = value;
}

auto FlightCursorDeadzoneOption = options::OptionBuilder<float>("Game.FlightCursorDeadzone",
	std::pair<const char*, int>{"Flight Cursor Deadzone", 1848},
	std::pair<const char*, int>{"How far from the center the cursor needs to go before registering.", 1849})
	.category(std::make_pair("Game", 1824))
	.range(0.0f, 0.349f)
	.display(degrees_display)
	.default_func([]() { return 0.02f; })
	.level(options::ExpertLevel::Beginner)
	.bind_to(&Flight_cursor_deadzone)
	.importance(43)
	.parser(parse_cursor_deadzone_func)
	.finish();

int toggle_glide = 0;
int press_glide = 0;

////////////////////////////////////////////////////////////
// Module data
////////////////////////////////////////////////////////////
static int Player_all_alone_msg_inited=0;	// flag used for initializing a player-specific voice msg

float Player_warpout_speed = 40.0f;
float Target_warpout_match_percent = 0.05f;
float Minimum_player_warpout_time = 3.0f;

#define MAX_CHASE_EXTERN_CAM_DISTANCE 30000.f

#ifndef NDEBUG
	int Show_killer_weapon = 0;
	DCF_BOOL( show_killer_weapon, Show_killer_weapon )
#endif

/**
 * @brief Slew angles chase towards a value like they're on a spring.
 *
 * @param[in,out] ap The current view angles. Is modified towards target angles.
 * @param[in]     bp The target view angles
 *
 * @details When furthest away, move fastest. Minimum speed set so that doesn't take too long. When gets close, clamps to the value.
 */
void chase_angles_to_value(angles *ap, angles *bp, int scale)
{
	float sk;
	angles delta;

	//	Make sure we actually need to do all this math.
	if ((ap->p == bp->p) && (ap->h == bp->h))
		return;

	sk = 1.0f - scale*flRealframetime;

	CLAMP(sk, 0.0f, 1.0f);

	delta.p = ap->p - bp->p;
	delta.h = ap->h - bp->h;

	//	If we're very close, put ourselves at goal.
	if ((fl_abs(delta.p) < 0.005f) && (fl_abs(delta.h) < 0.005f)) {
		ap->p = bp->p;
		ap->h = bp->h;
	} else {
		// Else, apply the changes
		ap->p -= (delta.p * (1.0f - sk));
		ap->h -= (delta.h * (1.0f - sk));
	}
}

/**
 * @brief Resets the given angles to 0.0f without delay
 *
 * @param[in,out] Angles to reset
 */
void reset_angles(angles *ap) {
	ap->p = 0.0f;
	ap->h = 0.0f;
	ap->b = 0.0f;
}

angles	Viewer_slew_angles_delta;
angles	Viewer_external_angles_delta;

/**
 * @brief Modifies the camera view angles according to its current view mode: External, External Locked,
 *   TrackIR, Freelook (Unlocked), Normal (Locked), and Centering
 *
 * @param[in,out]   ma      The camera view angles to modify (magnitude is saturated to be within max_p and max_h).
 * @param[out]  da      The delta angles applied to ma (magnitude is saturated to 1 radian).
 * @param[in]   max_p   The maximum pitch magnitude ma may have (radians).
 * @param[in]   max_h   The maximum heading magnitude ma may have (radians).
 */
void view_modify(angles *ma, angles *da, float max_p, float max_h)
{
	// Digital inputs
	float	t = 0;
	float   u = 0;

	// Analog inputs
	int axis[Action::NUM_VALUES];
	float h = 0.0f;
	float p = 0.0f;

	if (Viewer_mode & VM_CENTERING) {
		// Center view then bail
		ma->p = 0.0f;
		ma->h = 0.0f;
		ma->b = 0.0f;
		return;

	} else if (Viewer_mode & VM_CAMERA_LOCKED) {
		if (Viewer_mode & VM_EXTERNAL) {
			// External camera is locked in place, nothing to do here
			return;
		}

		if (Viewer_mode & VM_PADLOCK_ANY) {
			// Do Padlock view then bail
			if (Viewer_mode & VM_PADLOCK_UP) {
				ma->h = 0.0f;
				ma->p = -max_p;

			} else if (Viewer_mode & VM_PADLOCK_REAR) {
				ma->h = -PI;
				ma->p = 0.0f;

			} else if (Viewer_mode & VM_PADLOCK_RIGHT) {
				ma->h = max_h;
				ma->p = 0.0f;

			} else if (Viewer_mode & VM_PADLOCK_LEFT) {
				ma->h = -max_h;
				ma->p = 0.0f;
			} // Else, don't do any adjustments. do_view_slew() will reset the states

			return;

		} else if (headtracking::isEnabled()) {
			// Do TrackIR
			vec3d trans = ZERO_VECTOR;

			headtracking::query();

			headtracking::HeadTrackingStatus* status = headtracking::getStatus();

			ma->h = -PI2*(status->yaw);
			ma->p = PI2*(status->pitch);

			trans.xyz.x = -0.4f*status->x;
			trans.xyz.y = 0.4f*status->y;
			trans.xyz.z = -status->z;

			if (trans.xyz.z < 0) {
				trans.xyz.z = 0.0f;
			}

			vm_vec_unrotate(&leaning_position,&trans,&Eye_matrix);

		} else {
			// Do slew
			/* These have been commented out until the controls are added into Controls_config[]
			t = (check_control_timef(SLEW_LEFT) - check_control_timef(SLEW_RIGHT));
			u = (check_control_timef(SLEW_UP) - check_control_timef(SLEW_DOWN);
			*/
		}	// Else, don't do any slewing

	} else {
		// Camera is unlocked - Pitch and Yaw axes control X and Y slew axes
		t = (check_control_timef(YAW_RIGHT) - check_control_timef(YAW_LEFT));
		u = (check_control_timef(PITCH_FORWARD) - check_control_timef(PITCH_BACK));

		control_get_axes_readings(axis, flRealframetime);

		// Does the same thing as t and u but for the joystick input 
		h = f2fl(axis[Action::HEADING]);
		p = -f2fl(axis[Action::PITCH]);
	}

	// Combine Analog and Digital slew commands
	da->h = 0.0f;
	da->p = 0.0f;
	da->b = 0.0f;

	if (t != 0.0f) {
		da->h += t;
	}
	if (h != 0.0f) {
		da->h += h;
	}

	if (u != 0.0f) {
		da->p += u;
	} 
	if (p != 0.0f) {
		da->p += p;
	}

	// Clamp deltas to be within 1 radian
	CLAMP(da->h, -1.0f, 1.0f);
	CLAMP(da->p, -1.0f, 1.0f);

	// Apply view modifications to camera
	if ((Game_time_compression >= F1_0) && !(Viewer_mode & VM_EXTERNAL)) {
		ma->p += 2*da->p * flFrametime;
		ma->b += 2*da->b * flFrametime;
		ma->h += 2*da->h * flFrametime;

	} else {
		//If time compression is less than normal, still move camera at same speed
		//This gives a cool matrix effect
		ma->p += da->p * flRealframetime;
		ma->b += da->b * flRealframetime;
		ma->h += da->h * flRealframetime;
	}

	// Clamp resulting angles to their maximums
	CLAMP(ma->p, -max_p, max_p);
	CLAMP(ma->h, -max_h, max_h);
}

void do_view_track_target()
{
	vec3d view_vector;
	vec3d targetpos_rotated;
	vec3d playerpos_rotated;
	vec3d forwardvec_rotated;
	vec3d target_pos;
	angles view_angles;
	angles forward_angles;

	if ((Player_ai->target_objnum == -1) || (Viewer_mode & VM_OTHER_SHIP)) {
	 // If the object isn't targeted or we're viewing from the target's perspective, center the view and turn off target padlock
	 // because the target won't be at the angle we've calculated from the player's perspective.
		Viewer_mode ^= VM_TRACK;
		chase_slew_angles.p = 0.0f;
		chase_slew_angles.h = 0.0f;
		return;
	}

	object * targetp = &Objects[Player_ai->target_objnum];

	// check to see if there is even a current target. if not, switch off the 
	// target padlock tracking flag, make the camera slew to the center,
	// and exit the procedure
	if ( targetp == &obj_used_list ) {
		Viewer_mode ^= VM_TRACK;
		chase_slew_angles.p = 0.0f;
		chase_slew_angles.h = 0.0f;
		return;
	}

	// look at a subsystem if there is one.
	if ( Player_ai->targeted_subsys != NULL ) {
		get_subsystem_world_pos(targetp, Player_ai->targeted_subsys, &target_pos);

	} else {
		target_pos = targetp->pos;
	}

	vm_vec_rotate(&targetpos_rotated, &target_pos, &Player_obj->orient);
	vm_vec_rotate(&playerpos_rotated, &Player_obj->pos, &Player_obj->orient);
	vm_vec_rotate(&forwardvec_rotated, &Player_obj->orient.vec.fvec, &Player_obj->orient);

	vm_vec_normalized_dir(&view_vector,&targetpos_rotated,&playerpos_rotated);
	vm_extract_angles_vector(&view_angles,&view_vector);
	vm_extract_angles_vector(&forward_angles,&forwardvec_rotated);
	chase_slew_angles.h = forward_angles.h - view_angles.h;
	chase_slew_angles.p = -(forward_angles.p - view_angles.p);

	// the gimbal limits of the player's virtual neck.
	// These nested ifs prevent the player from looking up and 
	// down beyond 90 degree angles.
	if (chase_slew_angles.p > PI_2)
		chase_slew_angles.p = PI_2;
	else if (chase_slew_angles.p < -PI_2)
		chase_slew_angles.p = -PI_2;

	// prevents the player from looking completely behind himself; just over his shoulder
	if (chase_slew_angles.h > PI2/3)
		chase_slew_angles.h = PI2/3;
	else if (chase_slew_angles.h < -PI2/3)
		chase_slew_angles.h = -PI2/3;
}

/**
 * @brief When VIEW_SLEW is pressed, pitch and heading axes controls viewer direction slewing.
 *
 * @param[in] frame_time The frame time at which this function is called
 *
 * @details Prevents the player's "head" from swiveling unrealistacally far with a max_p of Pi/2 and a max_h of2/3 Pi.
 *
 * @note Some mods may prefer to set their own limits, so, maybe make this as a table option in the future
 */
void do_view_slew()
{
	view_modify(&chase_slew_angles, &Viewer_slew_angles_delta, PI_2, PI2/3);

	// Check Track target
	if (Viewer_mode & VM_TRACK && !Slew_locked) {
		// Player's vision will track current target.
		do_view_track_target();
		Viewer_mode |= VM_CAMERA_LOCKED;
		return;
	}

	// Check Padlock controls
	// (Check Slew_locked second so that we don't short-circuit before processing the control)
	if (check_control(PADLOCK_UP) && !Slew_locked) {
		Viewer_mode |= (VM_PADLOCK_UP | VM_CAMERA_LOCKED);
		Viewer_mode &= ~(VM_CENTERING);
		return;

	} else if (check_control(PADLOCK_DOWN) && !Slew_locked) {
		Viewer_mode |= (VM_PADLOCK_REAR | VM_CAMERA_LOCKED);
		Viewer_mode &= ~(VM_CENTERING);
		return;

	} else if (check_control(PADLOCK_RIGHT) && !Slew_locked) {
		Viewer_mode |= (VM_PADLOCK_RIGHT | VM_CAMERA_LOCKED);
		Viewer_mode &= ~(VM_CENTERING);
		return;

	} else if (check_control(PADLOCK_LEFT) && !Slew_locked) {
		Viewer_mode |= (VM_PADLOCK_LEFT | VM_CAMERA_LOCKED);
		Viewer_mode &= ~(VM_CENTERING);
		return;

	} else if (Viewer_mode & VM_PADLOCK_ANY) {
		// at this point the view is in padlock mode but no controls are currently pressed;
		// clear padlock views and center the view once 
		// the player lets go of a padlock control
		Viewer_mode &= ~(VM_PADLOCK_ANY);
		Viewer_mode |= (VM_CENTERING | VM_CAMERA_LOCKED);
	}

	if (Viewer_mode & VM_CENTERING) {
		// If we're centering the view, check to see if we're actually centered and bypass any view modifications
		// until the view has finally been centered.
		if ((Viewer_slew_angles.h == 0.0f) && (Viewer_slew_angles.p == 0.0f)) {
			// View has been centered, allow the player to freelook again.
			Viewer_mode &= ~VM_CENTERING;
		}
		Viewer_mode |= VM_CAMERA_LOCKED;
	}

	if (!(Viewer_mode & VM_PADLOCK_ANY)) {
		if (headtracking::isEnabled()) {
			// Can't do slewing if TrackIR is enabled
			Viewer_mode |= VM_CAMERA_LOCKED;
			return;
		}
		
		if (check_control_timef(VIEW_SLEW) && !Slew_locked) {
			// Enable freelook mode
			Viewer_mode &= ~VM_CAMERA_LOCKED;

		} else if (check_control_timef(VIEW_CENTER) || !(Viewer_mode & VM_CAMERA_LOCKED)) {
			// Start centering the view if:
			//  VIEW_CENTER was pressed, or
			//  The player let go of VIEW_SLEW
			Viewer_mode |= (VM_CENTERING | VM_CAMERA_LOCKED);
		}
	}
}

float camera_zoom_scale = 1.0f;

DCF(camera_speed, "Sets the camera zoom scale")
{
	if (dc_optional_string_either("status", "--status") || dc_optional_string_either("?", "--?")) {
		dc_printf("Camera zoom scale is %f\n", camera_zoom_scale);
		return;
	}

	dc_stuff_float(&camera_zoom_scale);

	dc_printf("Camera zoom scale set to %f\n", camera_zoom_scale);
}

void do_view_chase()
{
	float t;

	if (Viewer_mode & VM_TRACK) {
		// Snap back to zero and disable target tracking
		reset_angles(&Viewer_slew_angles);
		reset_angles(&chase_slew_angles);
		Viewer_mode &= ~VM_TRACK;
	}

	// Process centering key.
	if (check_control_timef(VIEW_CENTER)) {
		Viewer_chase_info.distance = 0.0f;
	}

	object* viewer_obj = Player_obj;

	if (Viewer_mode & VM_OTHER_SHIP) {
		if (Player_ai->target_objnum != -1) {
			viewer_obj = &Objects[Player_ai->target_objnum];
		}
	}

	t = check_control_timef(VIEW_DIST_INCREASE) - check_control_timef(VIEW_DIST_DECREASE);

	float current_distance = 2 * viewer_obj->radius + Viewer_chase_info.distance;
	Viewer_chase_info.distance += -t * (current_distance / 35) * camera_zoom_scale;
	if (Viewer_chase_info.distance < 0.0f)
		Viewer_chase_info.distance = 0.0f;
	if (Viewer_chase_info.distance > MAX_CHASE_EXTERN_CAM_DISTANCE)
		Viewer_chase_info.distance = MAX_CHASE_EXTERN_CAM_DISTANCE;



	Viewer_mode |= VM_CAMERA_LOCKED;
}

void do_view_external()
{
	float	t;

	object* viewer_obj = Player_obj;

	if (Viewer_mode & VM_OTHER_SHIP) {
		if (Player_ai->target_objnum != -1) {
			viewer_obj = &Objects[Player_ai->target_objnum];
		}
	}

	if (Viewer_mode & VM_TRACK) {
		// Snap back to zero and disable target tracking
		reset_angles(&Viewer_slew_angles);
		reset_angles(&chase_slew_angles);
		Viewer_mode &= ~VM_TRACK;
	}

	view_modify(&Viewer_external_info.angles, &Viewer_external_angles_delta, PI2, PI2);

	//	Process centering key.
	if (check_control_timef(VIEW_CENTER)) {
		Viewer_external_info.angles.p = 0.0f;
		Viewer_external_info.angles.h = 0.0f;
		Viewer_external_info.preferred_distance = 2 * viewer_obj->radius;
	}

	t = check_control_timef(VIEW_DIST_INCREASE) - check_control_timef(VIEW_DIST_DECREASE);
	if (t != 0.f) {
		Viewer_external_info.preferred_distance = Viewer_external_info.current_distance + (t * (Viewer_external_info.current_distance / 25) * camera_zoom_scale);
		if (Viewer_external_info.preferred_distance < 0.0f)
			Viewer_external_info.preferred_distance = 0.0f;
		if (Viewer_external_info.preferred_distance > MAX_CHASE_EXTERN_CAM_DISTANCE)
			Viewer_external_info.preferred_distance = MAX_CHASE_EXTERN_CAM_DISTANCE;
	}

	//	Do over-the-top correction.

	if (Viewer_external_info.angles.p > PI)
		Viewer_external_info.angles.p = -PI2 + Viewer_external_info.angles.p;
	else if (Viewer_external_info.angles.p < -PI)
		Viewer_external_info.angles.p = PI2 + Viewer_external_info.angles.p;

	if (Viewer_external_info.angles.h > PI)
		Viewer_external_info.angles.h = -PI2 + Viewer_external_info.angles.h;
	else if (Viewer_external_info.angles.h < -PI)
		Viewer_external_info.angles.h = PI2 + Viewer_external_info.angles.h;
}

/**
 * Called by single and multiplayer modes to reset information inside of control info structure
 */
void player_control_reset_ci( control_info *ci )
{
	float t1, t2, oldspeed;

	t1 = ci->heading;
	t2 = ci->pitch;
	oldspeed = ci->forward_cruise_percent;
	memset( ci, 0, sizeof(control_info) );
	ci->heading = t1;
	ci->pitch = t2;
	ci->forward_cruise_percent = oldspeed;
}

void read_keyboard_controls( control_info * ci, float frame_time, physics_info *pi )
{
	float kh=0.0f, scaled, newspeed, delta, oldspeed;
	int axis[Action::NUM_VALUES];
	static int afterburner_last = 0;
	static float analog_throttle_last = 9e9f;
	static int override_analog_throttle = 0; 
	static float savedspeed = ci->forward_cruise_percent;	//Backslash
	int centering_speed = 7; // the scale speed in which the camera will smoothly center when the player presses Center View

	oldspeed = ci->forward_cruise_percent;
	player_control_reset_ci( ci );
	control_reset_lua_cache();

	// Camera & View controls
	if ( Viewer_mode & VM_EXTERNAL ) {
		// External mode
		control_used(VIEW_EXTERNAL);
		do_view_external();

	} else if ( Viewer_mode & VM_CHASE ) {
		// Chase mode
		do_view_chase();

	} else {
		// We're in the cockpit. 
		do_view_slew();
	}

	chase_angles_to_value(&Viewer_slew_angles, &chase_slew_angles, centering_speed);

	// Ship controls
	if (Viewer_mode & VM_CAMERA_LOCKED) {
		// From keyboard...
		if ( check_control(BANK_WHEN_PRESSED) ) {
			ci->bank = check_control_timef(BANK_LEFT) + check_control_timef(YAW_LEFT) - check_control_timef(YAW_RIGHT) - check_control_timef(BANK_RIGHT);
			ci->heading = 0.0f;
		} else {
			kh = (check_control_timef(YAW_RIGHT) - check_control_timef(YAW_LEFT)) / 8.0f;
			if (kh == 0.0f) {
				ci->heading = 0.0f;

			} else if (kh > 0.0f) {
				if (ci->heading < 0.0f)
					ci->heading = 0.0f;

			} else {  // kh < 0
				if (ci->heading > 0.0f)
					ci->heading = 0.0f;
			}

			ci->bank = check_control_timef(BANK_LEFT) - check_control_timef(BANK_RIGHT);
		}

		ci->heading += kh;

		kh = (check_control_timef(PITCH_FORWARD) - check_control_timef(PITCH_BACK)) / 8.0f;
		if (kh == 0.0f) {
			ci->pitch = 0.0f;
		} else if (kh > 0.0f) {
			if (ci->pitch < 0.0f)
				ci->pitch = 0.0f;

		} else {  // kh < 0
			if (ci->pitch > 0.0f)
				ci->pitch = 0.0f;
		}

		ci->pitch += kh;
	}

	if (!(Game_mode & GM_DEAD)) {
		// Thrust controls
		ci->forward = check_control_timef(FORWARD_THRUST) - check_control_timef(REVERSE_THRUST);
		ci->sideways = (check_control_timef(RIGHT_SLIDE_THRUST) - check_control_timef(LEFT_SLIDE_THRUST)); // for slideing-Bobboau
		ci->vertical = (check_control_timef(UP_SLIDE_THRUST) - check_control_timef(DOWN_SLIDE_THRUST)); // for slideing-Bobboau

		// Throttle controls
		if ( button_info_query(&Player->bi, ONE_THIRD_THROTTLE) ) {
			control_used(ONE_THIRD_THROTTLE);
			player_clear_speed_matching();
			if ( Player->ci.forward_cruise_percent < 33.3f ) {
				snd_play( gamesnd_get_game_sound(ship_get_sound(Player_obj, GameSounds::THROTTLE_UP)), 0.0f );

			} else if ( Player->ci.forward_cruise_percent > 33.3f ) {
				snd_play( gamesnd_get_game_sound(ship_get_sound(Player_obj, GameSounds::THROTTLE_DOWN)), 0.0f );
			}

			Player->ci.forward_cruise_percent = 33.3f;
			override_analog_throttle = 1;
		}

		if ( button_info_query(&Player->bi, TWO_THIRDS_THROTTLE) ) {
			control_used(TWO_THIRDS_THROTTLE);
			player_clear_speed_matching();
			if ( Player->ci.forward_cruise_percent < 66.6f ) {
				snd_play( gamesnd_get_game_sound(ship_get_sound(Player_obj, GameSounds::THROTTLE_UP)), 0.0f );

			} else if (Player->ci.forward_cruise_percent > 66.6f) {
				snd_play( gamesnd_get_game_sound(ship_get_sound(Player_obj, GameSounds::THROTTLE_DOWN)), 0.0f );
			}

			Player->ci.forward_cruise_percent = 66.6f;
			override_analog_throttle = 1;
		}

		if ( button_info_query(&Player->bi, PLUS_5_PERCENT_THROTTLE) ) {
			control_used(PLUS_5_PERCENT_THROTTLE);
			Player->ci.forward_cruise_percent += 5.0f;
			if (Player->ci.forward_cruise_percent > 100.0f)
				Player->ci.forward_cruise_percent = 100.0f;
		}

		if ( button_info_query(&Player->bi, MINUS_5_PERCENT_THROTTLE) ) {
			control_used(MINUS_5_PERCENT_THROTTLE);
			Player->ci.forward_cruise_percent -= 5.0f;
			if (Player->ci.forward_cruise_percent < 0.0f)
				Player->ci.forward_cruise_percent = 0.0f;
		}

		if ( button_info_query(&Player->bi, ZERO_THROTTLE) ) {
			control_used(ZERO_THROTTLE);
			player_clear_speed_matching();
			if ( ci->forward_cruise_percent > 0.0f && Player_obj->phys_info.fspeed > 0.5) {
				snd_play( gamesnd_get_game_sound(ship_get_sound(Player_obj, GameSounds::ZERO_THROTTLE)), 0.0f );
			}

			ci->forward_cruise_percent = 0.0f;
			override_analog_throttle = 1;
		}

		if ( button_info_query(&Player->bi, MAX_THROTTLE) ) {
			control_used(MAX_THROTTLE);
			player_clear_speed_matching();
			if ( ci->forward_cruise_percent < 100.0f ) {
				snd_play( gamesnd_get_game_sound(ship_get_sound(Player_obj, GameSounds::FULL_THROTTLE)), 0.0f );
			}

			ci->forward_cruise_percent = 100.0f;
			override_analog_throttle = 1;
		}

		// allow a minimum speed to be set for the player ship (only)
		if ( Player_ship != nullptr && Player_ship->ship_info_index >= 0 ) {
			float z_min = Ship_info[Player_ship->ship_info_index].min_vel.xyz.z;
			if (z_min > 0.0f && pi->speed <= z_min) {
				ci->forward = MAX(ci->forward, 0.0f);
				ci->forward_cruise_percent = MAX(ci->forward_cruise_percent, z_min / Ship_info[Player_ship->ship_info_index].max_vel.xyz.z * 100.0f);
				override_analog_throttle = 1;
			}
		}

		// AL 12-29-97: If afterburner key is down, player should have full forward thrust (even if afterburners run out)
		if ( check_control(AFTERBURNER) ) {
			ci->forward = 1.0f;
		}

		if ( check_control(REVERSE_THRUST) && check_control(AFTERBURNER) ) {
			ci->forward = -pi->max_rear_vel * 1.0f;
		}

		if ( Player->flags & PLAYER_FLAGS_MATCH_TARGET ) {
			if ( (Player_ai->last_target == Player_ai->target_objnum) && (Player_ai->target_objnum != -1) && ( ci->forward_cruise_percent == oldspeed) ) {
				float tspeed, pmax_speed;
				object *targeted_objp = &Objects[Player_ai->target_objnum];

				tspeed = targeted_objp->phys_info.speed;	//changed from fspeed. If target is reversing, sliding, or gliding we still want to keep up. -- Backslash

				// maybe need to get speed from docked partner
				if ( tspeed < MATCH_SPEED_THRESHOLD ) {
					Assert(targeted_objp->type == OBJ_SHIP);

					// Goober5000
					if (object_is_docked(targeted_objp))
					{
						tspeed = dock_calc_docked_speed(targeted_objp);	//changed from fspeed
					}
				}

				//	Note, if closer than 100 units, scale down speed a bit.  Prevents repeated collisions. -- MK, 12/17/97
				float dist = vm_vec_dist(&Player_obj->pos, &targeted_objp->pos);

				if (dist < 100.0f) {
					tspeed = tspeed * (0.5f + dist/200.0f);
				}

				//SUSHI: If gliding, don't do anything for speed matching
				if (!( (Objects[Player->objnum].phys_info.flags & PF_GLIDING) || (Objects[Player->objnum].phys_info.flags & PF_FORCE_GLIDE) )) {
					pmax_speed = Player_obj->phys_info.max_vel.xyz.z;
					if (pmax_speed > 0.0f) {
						ci->forward_cruise_percent = (tspeed / pmax_speed) * 100.0f;
					} else {
						ci->forward_cruise_percent = 0.0f;
					}
					override_analog_throttle = 1;
				}

			} else
				Player->flags &= ~PLAYER_FLAGS_MATCH_TARGET;
		}

		// code to read joystick axis for pitch/heading.  Code to read joystick buttons
		// for bank.
		if ( !(Game_mode & GM_DEAD) )	{
			control_get_axes_readings(axis, frame_time);
		} else {
			axis[0] = axis[1] = axis[2] = axis[3] = axis[4] = 0;
		}

		if (Viewer_mode & VM_CAMERA_LOCKED) {
			// Player has control of the ship
			// Set heading
			if ( check_control(BANK_WHEN_PRESSED) ) {
				delta = f2fl( axis[Action::HEADING] );
				if ( (delta > 0.05f) || (delta < -0.05f) ) {
					ci->bank -= delta;
				}
			} else {
				ci->heading += f2fl( axis[Action::HEADING] );
			}
			// Set pitch
			ci->pitch -= f2fl( axis[Action::PITCH] );

		} else {
			// Player has control of the camera
			ci->pitch = 0.0f;
			ci->heading = 0.0f;
		}

		// Set bank
		ci->bank -= f2fl( axis[Action::BANK] ) * 1.5f;

		if (!Control_config[JOY_ABS_THROTTLE_AXIS].empty()) {
			scaled = (float) axis[Action::ABS_THROTTLE] * 1.2f / (float) F1_0 - 0.1f;  // convert to -0.1 - 1.1 range
			oldspeed = ci->forward_cruise_percent;

			newspeed = (1.0f - scaled) * 100.0f;

			delta = analog_throttle_last - newspeed;
			if (!override_analog_throttle || (delta < -1.5f) || (delta > 1.5f)) {
				ci->forward_cruise_percent = newspeed;
				analog_throttle_last = newspeed;
				override_analog_throttle = 0;
			}
		}

		if (!Control_config[JOY_REL_THROTTLE_AXIS].empty())
			ci->forward_cruise_percent += f2fl(axis[Action::REL_THROTTLE]) * 100.0f * frame_time;

		CLAMP(ci->forward_cruise_percent, 0.0f, 100.0f);

		// set up the firing stuff.  Read into control info ala Descent so that weapons will be
		// created during the object simulation phase, and not immediately as was happening before.

		//keyboard: fire the current primary weapon
		if (check_control(FIRE_PRIMARY)) {
			ci->fire_primary_count++;
		}

		// for debugging, check to see if the debug key is down -- if so, make fire the debug laser instead
#ifndef NDEBUG
		if ( key_is_pressed(KEY_DEBUG_KEY) ) {
			ci->fire_debug_count = ci->fire_primary_count;
			ci->fire_primary_count = 0;
		}
#endif

		// keyboard: fire the current secondary weapon
		if (check_control(FIRE_SECONDARY)) {
			ci->fire_secondary_count++;

			// if we're a multiplayer client, set our accum bits now
			if( MULTIPLAYER_CLIENT && (Net_player != NULL)){
				Net_player->s_info.accum_buttons |= OOC_FIRE_CONTROL_PRESSED;
			}
		}

		// keyboard: launch countermeasures, but not if AI controlling Player
		if (button_info_query(&Player->bi, LAUNCH_COUNTERMEASURE) && !Player_use_ai) {
			control_used(LAUNCH_COUNTERMEASURE);
			ci->fire_countermeasure_count++;
			hud_gauge_popup_start(HUD_CMEASURE_GAUGE);
		}

		// see if the afterburner has been started (keyboard + joystick)
		if (check_control(AFTERBURNER) && !Player_use_ai) {
			if (!afterburner_last) {
				Assert(Player_ship);
				if ( !(Ship_info[Player_ship->ship_info_index].flags[Ship::Info_Flags::Afterburner]) ) {
					gamesnd_play_error_beep();
				} else {
					ci->afterburner_start = 1;
				}
			}

			afterburner_last = 1;

		} else {
			if (afterburner_last)
				ci->afterburner_stop = 1;

			afterburner_last = 0;
		}

		// new gliding systems combining code by Backslash, Turey, Kazan, and WMCoolmon

		// Check for toggle button pressed.
		if ( button_info_query(&Player->bi, TOGGLE_GLIDING) ) {
			control_used(TOGGLE_GLIDING);
			if ( Player_obj != NULL && Ship_info[Player_ship->ship_info_index].can_glide ) {
				toggle_glide = !toggle_glide;
			}
		}
		// This logic is a bit tricky. It checks to see if the glide_when_pressed button is in a different state
		// than press_glide. Since it sets press_glide equal to glide_when_pressed inside of this if statement,
		//  this only evaluates to true when the state of the button is different than it was last time. 
		if ( check_control(GLIDE_WHEN_PRESSED) != press_glide ) {
			if ( Player_obj != NULL && Ship_info[Player_ship->ship_info_index].can_glide ) {
				// This only works if check_control returns only 1 or 0. Shouldn't be a problem,
				// but this comment's here just in case it is.
				press_glide = !press_glide;
			}
		}

		// if the player is warping out, cancel gliding
		if (Player_ship->flags[Ship::Ship_Flags::Depart_warp]) {
			toggle_glide = 0;
			press_glide = 0;
		}

		// Do we want to be gliding?
		if ( toggle_glide || press_glide ) {
			// Probably don't need to do this check, but just in case...
			if ( Player_obj != NULL && Ship_info[Player_ship->ship_info_index].can_glide ) {
				// Only bother doing this if we need to.
				if ( toggle_glide && press_glide ) {
					// Overkill -- if gliding is toggled on and glide_when_pressed is pressed, turn glide off
					if ( object_get_gliding(Player_obj) && !object_glide_forced(Player_obj) ) {
						object_set_gliding(Player_obj, false);
						ci->forward_cruise_percent = savedspeed;
						press_glide = !press_glide;
						snd_play( gamesnd_get_game_sound(ship_get_sound(Player_obj, GameSounds::THROTTLE_UP)), 0.0f );
					}
				} else if ( !object_get_gliding(Player_obj) ) {
					object_set_gliding(Player_obj, true);
					savedspeed = ci->forward_cruise_percent;
					ci->forward_cruise_percent = 0.0f;
					override_analog_throttle = 1;
					if (Ship_info[Player_ship->ship_info_index].glide_start_snd.isValid()) {
						//If a custom glide start sound was specified, play it
						snd_play( gamesnd_get_game_sound(Ship_info[Player_ship->ship_info_index].glide_start_snd), 0.0f );
					} else {
						//If glide_start_snd wasn't set (probably == 0), use the default throttle down sound
						snd_play( gamesnd_get_game_sound(ship_get_sound(Player_obj, GameSounds::THROTTLE_DOWN)), 0.0f );
					}
				}
			}
		} else {
			// Probably don't need to do the second half of this check, but just in case...
			if ( Player_obj != NULL && Ship_info[Player_ship->ship_info_index].can_glide ) {
				// Only bother doing this if we need to.
				if ( object_get_gliding(Player_obj) && !object_glide_forced(Player_obj) ) {
					object_set_gliding(Player_obj, false);
					ci->forward_cruise_percent = savedspeed;
					if (Ship_info[Player_ship->ship_info_index].glide_end_snd.isValid()) {
						//If a custom glide end sound was specified, play it
						snd_play( gamesnd_get_game_sound(Ship_info[Player_ship->ship_info_index].glide_end_snd), 0.0f );
					} else {
						//If glide_end_snd wasn't set (probably == 0), use the default throttle up sound
						snd_play( gamesnd_get_game_sound(ship_get_sound(Player_obj, GameSounds::THROTTLE_UP)), 0.0f );
					}
				}
			}
		}

	}

	if ( (Viewer_mode & VM_EXTERNAL) ) {
		if (!(Viewer_mode & VM_CAMERA_LOCKED)) {
			ci->heading=0.0f;
			ci->pitch=0.0f;
			ci->bank=0.0f;
		}
	}
}

void copy_control_info(control_info *dest_ci, control_info *src_ci, int control_copy_method)
{
	if (dest_ci == nullptr)
		return;

	// check which type of controls are being copied --wookieejedi
	if (control_copy_method & LGC_FULL) {
		if (src_ci == nullptr) {
			dest_ci->pitch = 0.0f;
			dest_ci->vertical = 0.0f;
			dest_ci->heading = 0.0f;
			dest_ci->sideways = 0.0f;
			dest_ci->bank = 0.0f;
			dest_ci->forward = 0.0f;
			dest_ci->forward_cruise_percent = 0.0f;
			dest_ci->fire_countermeasure_count = 0;
			dest_ci->fire_secondary_count = 0;
			dest_ci->fire_primary_count = 0;
		} else {
			dest_ci->pitch = src_ci->pitch;
			dest_ci->vertical = src_ci->vertical;
			dest_ci->heading = src_ci->heading;
			dest_ci->sideways = src_ci->sideways;
			dest_ci->bank = src_ci->bank;
			dest_ci->forward = src_ci->forward;
			dest_ci->forward_cruise_percent = src_ci->forward_cruise_percent;
		}
	} else if (control_copy_method & LGC_STEERING) {
		if (src_ci == nullptr) {
			dest_ci->pitch = 0.0f;
			dest_ci->heading = 0.0f;
			dest_ci->bank = 0.0f;
		} else {
			dest_ci->pitch = src_ci->pitch;
			dest_ci->heading = src_ci->heading;
			dest_ci->bank = src_ci->bank;
		}
	}

}

void read_player_controls(object *objp, float frametime)
{
	float diff;
	float target_warpout_speed;

	joy_ff_adjust_handling((int) objp->phys_info.speed);

	switch( Player->control_mode )
	{
		case PCM_SUPERNOVA:
			break;

		case PCM_NORMAL:
			read_keyboard_controls(&(Player->ci), frametime, &objp->phys_info );

			if (Player_obj->type == OBJ_SHIP) {
				auto sip = &Ship_info[Ships[Player_obj->instance].ship_info_index];

				if ((Player_flight_mode == FlightMode::FlightCursor || sip->aims_at_flight_cursor)) {

					if (Viewer_mode & VM_CAMERA_LOCKED) {
						float max_aim_angle = Flight_cursor_extent;

						if (sip->aims_at_flight_cursor)
							max_aim_angle = sip->flight_cursor_aim_extent;

						Player_flight_cursor.p += Player->ci.pitch * 0.015f;
						Player_flight_cursor.h += Player->ci.heading * 0.015f;

						float mag = powf(powf(Player_flight_cursor.p, 2.0f) + powf(Player_flight_cursor.h, 2.0f), 0.5f);
						if (mag > max_aim_angle) {
							Player_flight_cursor.p *= max_aim_angle / mag;
							Player_flight_cursor.h *= max_aim_angle / mag;
							mag = max_aim_angle;
						}

						float deadzone = Flight_cursor_deadzone;
						if (mag > deadzone) {
							float p = Player_flight_cursor.p * ((mag - deadzone) / mag);
							float h = Player_flight_cursor.h * ((mag - deadzone) / mag);

							Player->ci.pitch = p / (max_aim_angle - deadzone);
							Player->ci.heading = h / (max_aim_angle - deadzone);
						}
						else {
							Player->ci.pitch = 0.0f;
							Player->ci.heading = 0.0f;
						}
					} else {
						Player_flight_cursor = vmd_zero_angles;
						Player->ci.pitch = 0.0f;
						Player->ci.heading = 0.0f;
					}
				}

				// this is similar to ai_control_info_check
				if (sip->flags[Ship::Info_Flags::Dont_bank_when_turning])
					Player->ci.control_flags |= CIF_DONT_BANK_WHEN_TURNING;
				if (sip->flags[Ship::Info_Flags::Dont_clamp_max_velocity])
					Player->ci.control_flags |= CIF_DONT_CLAMP_MAX_VELOCITY;
				if (sip->flags[Ship::Info_Flags::Instantaneous_acceleration])
					Player->ci.control_flags |= CIF_INSTANTANEOUS_ACCELERATION;
			}

			if ((lua_game_control & LGC_STEERING) || (lua_game_control & LGC_FULL)) {
				// first copy over the new values, then reset
				control_info temp = Player->ci;
				copy_control_info(&(Player->ci), &(Player->lua_ci), lua_game_control);
				Player->lua_ci = temp;
			} else {
				// just copy the ci should that be needed in scripting
				Player->lua_ci = Player->ci;
			}
			break;

		case PCM_WARPOUT_STAGE1:	// Accelerate to 40 km/s
		case PCM_WARPOUT_STAGE2:	// Go 40 km/s steady up to the effect
		case PCM_WARPOUT_STAGE3:	// Go 40 km/s steady through the effect
		{
			memset(&(Player->ci), 0, sizeof(control_info) );		// set the controls to 0

			if ( (objp->type == OBJ_SHIP) && (!(Game_mode & GM_DEAD)) )
			{
				Warpout_time += flFrametime;

				target_warpout_speed = ship_get_warpout_speed(objp);

				// check if warp ability has been disabled
				// but only in the first stage
				if (!(Warpout_forced) && !(ship_can_warp_full_check(&Ships[objp->instance])) && (Player->control_mode == PCM_WARPOUT_STAGE1)) {
					HUD_sourced_printf(HUD_SOURCE_HIDDEN, "%s", XSTR( "Cannot warp out at this time.", 81));
					snd_play(gamesnd_get_game_sound(GameSounds::PLAYER_WARP_FAIL));
					gameseq_post_event( GS_EVENT_PLAYER_WARPOUT_STOP );
				} else {
					if ( Warpout_forced ) {
						objp->phys_info.max_vel.xyz.z = target_warpout_speed * 2.0f;
					} else if (objp->phys_info.max_vel.xyz.z < target_warpout_speed) {
						objp->phys_info.max_vel.xyz.z = target_warpout_speed + 5.0f;
					}

					diff = target_warpout_speed - objp->phys_info.fspeed;

					if ( diff < 0.0f ) 
						diff = 0.0f;
					
					Player->ci.forward = ((target_warpout_speed + diff) / objp->phys_info.max_vel.xyz.z);
				}
			
				if ( Player->control_mode == PCM_WARPOUT_STAGE1 )
				{
					float warpout_delay;
					int warpout_engage_time = Warp_params[Ships[objp->instance].warpout_params_index].warpout_engage_time;

					if (warpout_engage_time >= 0)
						warpout_delay = warpout_engage_time / 1000.0f;
					else
						warpout_delay = Minimum_player_warpout_time;

					// Wait at least 3 seconds before making sure warp speed is set.
					if ( Warpout_time > warpout_delay) {
						// If we are going around 5% of the target speed, progress to next stage
						float diffSpeed = objp->phys_info.fspeed;
						if(target_warpout_speed != 0.0f) {
							diffSpeed = fl_abs(objp->phys_info.fspeed - target_warpout_speed )/target_warpout_speed;
						}
						if ( diffSpeed < Target_warpout_match_percent)	{
							gameseq_post_event( GS_EVENT_PLAYER_WARPOUT_DONE_STAGE1 );
						}
					}
				}
			}

			break;
		}
	}

	if(Player_obj->type == OBJ_SHIP && !Player_use_ai){	
		// only read player control info if player ship is not dead
		// or if Player_use_ai is disabed
		if ( !(Ships[Player_obj->instance].flags[Ship::Ship_Flags::Dying]) ) {
			vec3d wash_rot;
			if ((Ships[objp->instance].wash_intensity > 0) && !((Player->control_mode == PCM_WARPOUT_STAGE1) || (Player->control_mode == PCM_WARPOUT_STAGE2) || (Player->control_mode == PCM_WARPOUT_STAGE3)) ) {
				float intensity = 0.3f * MIN(Ships[objp->instance].wash_intensity, 1.0f);
				vm_vec_copy_scale(&wash_rot, &Ships[objp->instance].wash_rot_axis, intensity);
				physics_read_flying_controls( &objp->orient, &objp->phys_info, &(Player->ci), flFrametime, &wash_rot);
			} else {
				physics_read_flying_controls( &objp->orient, &objp->phys_info, &(Player->ci), flFrametime);
			}
		}
	} else if(Player_obj->type == OBJ_OBSERVER){
		physics_read_flying_controls(&objp->orient,&objp->phys_info,&(Player->ci), flFrametime);
	}
}

void player_controls_init()
{
	static int initted = 0;

	if (initted)
		return;

	initted = 1;
	physics_init( &Descent_physics );
	Descent_physics.flags |= PF_ACCELERATES | PF_SLIDE_ENABLED;

	Viewer_slew_angles_delta.p = 0.0f;
	Viewer_slew_angles_delta.b = 0.0f;
	Viewer_slew_angles_delta.h = 0.0f;
}

/**
 * Clear current speed matching and auto-speed matching flags
 */
void player_clear_speed_matching()
{
	if ( !Player ) {
		Int3();	// why is Player NULL?
		return;
	}

	Player->flags &= ~PLAYER_FLAGS_MATCH_TARGET;
	Player->flags &= ~PLAYER_FLAGS_AUTO_MATCH_SPEED;
}

/**
 * Computes the forward_thrust_time needed for the player ship to match velocities with the currently selected target
 *
 * @param no_target_text Default parm (NULL), used to override HUD output when no target exists
 * @param match_off_text Default parm (NULL), used to overide HUD output when matching toggled off
 * @param match_on_text	Default parm (NULL), used to overide HUD output when matching toggled on
 */
void player_match_target_speed(char *no_target_text, char *match_off_text, char *match_on_text)
{
	// multiplayer observers can't match target speed
	if((Game_mode & GM_MULTIPLAYER) && (Net_player != NULL) && ((Net_player->flags & NETINFO_FLAG_OBSERVER) || (Player_obj->type == OBJ_OBSERVER)) ){
		return;
	}

	if ( Player_ai->target_objnum == -1) {
		if ( no_target_text ) {
			if ( no_target_text[0] ) {
				HUD_sourced_printf(HUD_SOURCE_HIDDEN, "%s", no_target_text );
			}
		} else {
//			HUD_sourced_printf(HUD_SOURCE_HIDDEN, XSTR("No currently selected target.",-1) );
		}
		return;
	}

	object *targeted_objp = &Objects[Player_ai->target_objnum];

	if ( targeted_objp->type != OBJ_SHIP ) {
		return;
	}

	if ( Player->flags & PLAYER_FLAGS_MATCH_TARGET ) {
		Player->flags &= ~PLAYER_FLAGS_MATCH_TARGET;
		if ( match_off_text ) {
			if ( match_off_text[0] ) {
				HUD_sourced_printf(HUD_SOURCE_HIDDEN, "%s", match_off_text );
			}
		} else {
//			HUD_sourced_printf(HUD_SOURCE_HIDDEN, XSTR("No longer matching speed with current target.",-1) );
		}
	} else {
		int can_match=0;

		if ( targeted_objp->phys_info.fspeed > MATCH_SPEED_THRESHOLD ) {
			can_match=1;
		} else {
			// account for case of matching speed with docked ship 
			if (object_is_docked(targeted_objp))
			{
				if (dock_calc_docked_fspeed(targeted_objp) > MATCH_SPEED_THRESHOLD)
				{
					can_match=1;
				}
			}
		}

		if ( can_match ) {
			Player->flags |= PLAYER_FLAGS_MATCH_TARGET;
			if ( match_on_text ) {
				if ( match_on_text[0] ) {
					HUD_sourced_printf(HUD_SOURCE_HIDDEN, "%s", match_on_text );
				}
			} else {
//				HUD_sourced_printf(HUD_SOURCE_HIDDEN, XSTR("Matching speed with current target.",-1) );
			}
		}
	}
}

// toggle_player_object toggles between the player objects (i.e. the ship they are currently flying)
// and a descent style ship.

int use_descent = 0;
LOCAL physics_info phys_save;

void toggle_player_object()
{
	if ( use_descent ) {
		memcpy( &Player_obj->phys_info, &phys_save, sizeof(physics_info) );
	} else {
		memcpy( &phys_save, &Player_obj->phys_info, sizeof(physics_info) );
		memcpy( &Player_obj->phys_info, &Descent_physics, sizeof(physics_info) );
	}
	use_descent = !use_descent;

	HUD_sourced_printf(HUD_SOURCE_HIDDEN, NOX("Using %s style physics for player ship."), use_descent ? NOX("DESCENT") : NOX("FreeSpace"));
}

/**
 * Initialise the data required for determining whether 'all alone' message should play
 */
void player_init_all_alone_msg()
{
	ship_obj	*so;
	object	*objp;

	Player->check_for_all_alone_msg=timestamp(0);

	// See if there are any friendly ships present, if so return without preventing msg
	for ( so = GET_FIRST(&Ship_obj_list); so != END_OF_LIST(&Ship_obj_list); so = GET_NEXT(so) ) {
		objp = &Objects[so->objnum];
		if (objp->flags[Object::Object_Flags::Should_be_dead])
			continue;

		if ( objp == Player_obj ) {
			continue;
		}

		if ( Ships[objp->instance].team == Player_ship->team ) {
			int ship_type = Ship_info[Ships[objp->instance].ship_info_index].class_type;
			if ( ship_type != -1 && (Ship_types[ship_type].flags[Ship::Type_Info_Flags::Counts_for_alone]) ) {
				return;
			}
		}
	}

	// There must be no friendly ships present, so prevent the 'all alone' message from ever playing
	Player->flags |= PLAYER_FLAGS_NO_CHECK_ALL_ALONE_MSG;
}

/**
 * Called when a new pilot is created
 */
void player_set_pilot_defaults(player *p)
{
	// Enable auto-targeting by default for all new pilots
	p->flags |= PLAYER_FLAGS_AUTO_TARGETING;
	p->save_flags |= PLAYER_FLAGS_AUTO_TARGETING;

	p->auto_advance = 1;
}

/**
 * Store some player preferences to ::Player->save_flags
 */
void player_save_target_and_weapon_link_prefs()
{
	Player->save_flags = 0;
	if ( Player->flags & PLAYER_FLAGS_AUTO_TARGETING ) {
		Player->save_flags |= PLAYER_FLAGS_AUTO_TARGETING;
	}


	if ( Player->flags & PLAYER_FLAGS_AUTO_MATCH_SPEED ) {
		// multiplayer observers can't match target speed
		if(!((Game_mode & GM_MULTIPLAYER) && (Net_player != NULL) && ((Net_player->flags & NETINFO_FLAG_OBSERVER) || (Player_obj->type == OBJ_OBSERVER))) )
		{
			Player->save_flags |= PLAYER_FLAGS_AUTO_MATCH_SPEED;
		}		
	}

	// if we're in multiplayer mode don't do this because we will desync ourselves with the server
	if(!(Game_mode & GM_MULTIPLAYER)){
		if ( Player_ship->flags[Ship::Ship_Flags::Primary_linked] ) {
			Player->save_flags |= PLAYER_FLAGS_LINK_PRIMARY;
		} else {
			Player->flags &= ~PLAYER_FLAGS_LINK_PRIMARY;
		}
		if ( Player_ship->flags[Ship::Ship_Flags::Secondary_dual_fire] ) {
			Player->save_flags |= PLAYER_FLAGS_LINK_SECONDARY;
		} else {
			Player->flags &= ~PLAYER_FLAGS_LINK_SECONDARY;
		}
	}
}

/**
 * Store some player preferences to ::Player->save_flags
 */
void player_restore_target_and_weapon_link_prefs()
{
	ship_info *player_sip;
	player_sip = &Ship_info[Player_ship->ship_info_index];
	polymodel *pm = model_get(player_sip->model_num);

	//	Don't restores the save flags in training, as we must ensure certain things are off, such as speed matching.
	if ( !(The_mission.game_type & MISSION_TYPE_TRAINING )) {
		Player->flags |= Player->save_flags;
	}

	if ( Player->flags & PLAYER_FLAGS_LINK_PRIMARY && !(player_sip->flags[Ship::Info_Flags::No_primary_linking]) ) {
		if ( Player_ship->weapons.num_primary_banks > 1 ) {
			Player_ship->flags.set(Ship::Ship_Flags::Primary_linked);
		}
	}

	if ( Player->flags & PLAYER_FLAGS_LINK_SECONDARY && (pm->n_missiles > 0 && pm->missile_banks[0].num_slots > 1) ) {
		Player_ship->flags.set(Ship::Ship_Flags::Secondary_dual_fire);
	}
}

/**
 * Initialise player statistics on a per mission basis
 * @todo Don't use memset(0) approach to setting up Player->ci
 */
void player_level_init()
{
	toggle_glide = 0;
	press_glide = 0;
	memset(&(Player->ci), 0, sizeof(control_info) );		// set the controls to 0

	Viewer_slew_angles.p = 0.0f;	Viewer_slew_angles.b = 0.0f;	Viewer_slew_angles.h = 0.0f;
	Viewer_external_info.angles.p = 0.0f;
	Viewer_external_info.angles.b = 0.0f;
	Viewer_external_info.angles.h = 0.0f;
	Viewer_external_info.preferred_distance = 0.0f;
	Viewer_external_info.current_distance = 0.0f;

	Player_flight_cursor = vmd_zero_angles;

	
	if (Default_start_chase_view != The_mission.flags[Mission::Mission_Flags::Toggle_start_chase_view])
	{
		Viewer_mode = VM_CHASE;
	}
	else
	{
		Viewer_mode = 0;
	}
 
	Player_obj = NULL;
	Player_ship = NULL;
	Player_ai = NULL;

	Player_use_ai = false;	// Goober5000

	if(Player == NULL)
		return;

	Player->flags = PLAYER_FLAGS_STRUCTURE_IN_USE;			// reset the player flags
	Player->flags |= Player->save_flags;
	
	//	Init variables for friendly fire monitoring.
	Player->friendly_last_hit_time = 0;
	Player->friendly_hits = 0;
	Player->friendly_damage = 0.0f;
	Player->last_warning_message_time = 0;

	Player->control_mode = PCM_NORMAL;

	Player->allow_warn_timestamp = 1;		// init timestamp that is used for managing attack warnings sent to player
	Player->check_warn_timestamp = 1;
	Player->warn_count = 0;						// number of attack warnings player has received this mission

	Player->distance_warning_count = 0;		// Number of warning too far from origin
	Player->distance_warning_time = 0;		// Time at which last warning was given

	Player->praise_count = 0;					// number of praises player has received this mission
	Player->allow_praise_timestamp = 1;		// timestamp until next praise is allowed
	Player->praise_delay_timestamp = 0;		// timstamp used to delay praises given to the player

	Player->ask_help_count = 0;				// number of times player has been asked for help by wingmen
	Player->allow_ask_help_timestamp = 1;	// timestamp until next ask_help is allowed

	Player->scream_count = 0;					// number of times player has heard wingman screams this mission
	Player->allow_scream_timestamp = 1;		// timestamp until next wingman scream is allowed
	
	Player->low_ammo_complaint_count = 0;	// number of complaints about low ammo received in this mission
	Player->allow_ammo_timestamp = 1;		// timestamp until next 'Ammo low' message can be played

	Player->praise_self_count = 0;			// number of boasts about kills received in this mission
	Player->praise_self_timestamp = 1;		// timestamp marking time until next boast is allowed

	Player->request_repair_timestamp = 1;	// timestamp until next 'requesting repair sir' message can be played

	Player->repair_sound_loop  = sound_handle::invalid();
	Player->cargo_scan_loop    = sound_handle::invalid();
	Player->cargo_inspect_time = 0;			// time that current target's cargo has been inspected for

	Player->target_is_dying = -1;				// Whether the player target is dying, -1 if no target
	Player->current_target_sx = -1;			// Screen x-pos of current target (or subsystem if applicable)
	Player->current_target_sy = -1;			// Screen y-pos of current target (or subsystem if applicable)
	Player->target_in_lock_cone = -1;		// Is the current target in secondary weapon lock cone?
	Player->locking_subsys=NULL;				// Subsystem pointer that missile lock is trying to seek
	Player->locking_on_center=0;				// boolean, whether missile lock is trying for center of ship or not
	Player->locking_subsys_parent=-1;

	Player->killer_objtype=-1;					// type of object that killed player
	Player->killer_weapon_index = -1;			// weapon used to kill player (if applicable)
	Player->killer_parent_name[0]=0;			// name of parent object that killed the player

	Player_all_alone_msg_inited=0;
	Player->flags &= ~PLAYER_FLAGS_NO_CHECK_ALL_ALONE_MSG;

	Player->death_message = "";
}

/**
 * Initializes global variables once a game -- needed because of mallocing that
 * goes on in structures in the player file
 */
void player_init()
{
	Player_num = 0;
	Player = &Players[Player_num];
	Player->flags |= PLAYER_FLAGS_STRUCTURE_IN_USE;
	Player->failures_this_session = 0;
	Player->show_skip_popup = (ubyte) 1;
}

/**
 * Stop any looping sounds associated with the Player, called from ::game_stop_looped_sounds().
 */
void player_stop_looped_sounds()
{
	Assert(Player);
	if (Player->repair_sound_loop.isValid()) {
		snd_stop(Player->repair_sound_loop);
		Player->repair_sound_loop = sound_handle::invalid();
	}
	if (Player->cargo_scan_loop.isValid()) {
		snd_stop(Player->cargo_scan_loop);
		Player->cargo_scan_loop = sound_handle::invalid();
	}
}

/**
 * Start the repair sound if it hasn't already been started.  Called when a player ship is being
 * repaired by a support ship
 */
void player_maybe_start_repair_sound()
{
	Assert(Player);
	if (!Player->repair_sound_loop.isValid()) {
		Player->repair_sound_loop = snd_play_looping( gamesnd_get_game_sound(GameSounds::SHIP_REPAIR) );
	}
}

/**
 * Stop the player repair sound if it is already playing
 */
void player_stop_repair_sound()
{
	Assert(Player);
	if (Player->repair_sound_loop.isValid()) {
		snd_stop(Player->repair_sound_loop);
		Player->repair_sound_loop = sound_handle::invalid();
	}
}

/**
 * Start the cargo scanning sound if it hasn't already been started
 */
void player_maybe_start_cargo_scan_sound()
{
	Assert(Player);
	if (!Player->cargo_scan_loop.isValid()) {
		Player->cargo_scan_loop = snd_play_looping( gamesnd_get_game_sound(ship_get_sound(Player_obj, GameSounds::CARGO_SCAN)) );
	}
}

/**
 * Stop the player repair sound if it is already playing
 */
void player_stop_cargo_scan_sound()
{
	Assert(Player);
	if (Player->cargo_scan_loop.isValid()) {
		snd_stop(Player->cargo_scan_loop);
		Player->cargo_scan_loop = sound_handle::invalid();
	}
}

/**
 * @brief See if there is a praise message to deliver to the player.  We want to delay the praise messages
 * a bit, to make them more realistic
 *
 * @return 1 if a praise message was delivered to the player, or a praise is pending; 0	if no praise is pending
 */
int player_process_pending_praise()
{
	// in multiplayer, never praise
	if(Game_mode & GM_MULTIPLAYER){
		return 0;
	}

	if ( timestamp_elapsed(Player->praise_delay_timestamp) ) {
		int ship_index;

		Player->praise_delay_timestamp = 0;
		ship_index = ship_get_random_player_wing_ship( SHIP_GET_UNSILENCED, 1000.0f );
		if ( ship_index >= 0 ) {
			// Only praise if above 50% integrity
			if ( get_hull_pct(&Objects[Ships[ship_index].objnum]) > 0.5f ) {
				// This cutoff should probably be in the AI profile or mission file rather than hardcoded
				auto message = (Player->stats.m_kill_count_ok > 10) ? MESSAGE_HIGH_PRAISE : MESSAGE_PRAISE;
				if (message_send_builtin(message, &Ships[ship_index], nullptr, -1, -1)) {
					Player->allow_praise_timestamp = timestamp(Builtin_messages[MESSAGE_PRAISE].min_delay * (Game_skill_level+1) );
					Player->allow_scream_timestamp = timestamp(20000);		// prevent death scream following praise
					Player->praise_count++;
					return 1;
				}
			}
		}
	}

	if ( Player->praise_delay_timestamp == 0 ) {
		return 0;
	}

	return 1;
}

bool player_inspect_cap_subsys_cargo(float frametime, char *outstr);

/**
 * See if the player should be inspecting cargo, and update progress.
 * 
 * @param frametime	Time since last frame in seconds
 * @param outstr (output parm) holds string that HUD should display
 *
 * @return true if player should display outstr on HUD; false if don't display cargo on HUD
 */
bool player_inspect_cargo(float frametime, char *outstr)
{
	object		*cargo_objp;
	ship		*cargo_sp;
	ship_info	*cargo_sip;

	outstr[0] = 0;

	if ( Player_ai->target_objnum < 0 || Player_ship->flags[Ship::Ship_Flags::Cannot_perform_scan] ) {
		return false;
	}

	cargo_objp = &Objects[Player_ai->target_objnum];
	Assert(cargo_objp->type == OBJ_SHIP);
	cargo_sp = &Ships[cargo_objp->instance];
	cargo_sip = &Ship_info[cargo_sp->ship_info_index];

	if (Use_new_scanning_behavior) {
		// If this flag is active, no matter the ship class, we do subsystem scanning
		if (cargo_sp->flags[Ship::Ship_Flags::Toggle_subsystem_scanning]) {
			return player_inspect_cap_subsys_cargo(frametime, outstr);
		}
	} else {
		// Goober5000 - possibly swap cargo scan behavior
		int scan_subsys = cargo_sip->is_huge_ship();
		if (cargo_sp->flags[Ship::Ship_Flags::Toggle_subsystem_scanning])
			scan_subsys = !scan_subsys;
		if (scan_subsys)
			return player_inspect_cap_subsys_cargo(frametime, outstr);
	}

	if (Use_new_scanning_behavior) {
		// If this flag is inactive then the ship cannot be scanned at all
		if (!(cargo_sp->flags[Ship::Ship_Flags::Scannable])) {
			return false;
		}
	} else {
		// scannable cargo behaves differently.  Scannable cargo is either "scanned" or "not scanned".  This flag
		// can be set on any ship.  Any ship with this set won't have "normal" cargo behavior
		if (!(cargo_sp->flags[Ship::Ship_Flags::Scannable])) {
			if (!(cargo_sip->flags[Ship::Info_Flags::Cargo] || cargo_sip->flags[Ship::Info_Flags::Transport])) {
				return false;
			}
		}
	}

	// Whether or not we are scanning in a mode that will reveal the cargo contents
	bool reveal_cargo = false;
	if (Use_new_scanning_behavior && !(cargo_sp->flags[Ship::Ship_Flags::No_scanned_cargo])) {
		reveal_cargo = true;
	} else if (!(cargo_sp->flags[Ship::Ship_Flags::Scannable])) {
		reveal_cargo = true;
	}

	// if cargo is already revealed
	if ( cargo_sp->flags[Ship::Ship_Flags::Cargo_revealed] ) {
		if (reveal_cargo) {
			auto cargo_name = (cargo_sp->cargo1 & CARGO_INDEX_MASK) == 0
				? XSTR("Nothing", 1674)
				: Cargo_names[cargo_sp->cargo1 & CARGO_INDEX_MASK];
			//Why was this assert here? I'm not sure it makes much sense because any ship can be scanned and have cargo revealed...
            //Assert(cargo_sip->flags[Ship::Info_Flags::Cargo] || cargo_sip->flags[Ship::Info_Flags::Transport]);

			if (cargo_sp->cargo_title[0] != '\0') {
				if (cargo_sp->cargo_title[0] == '#') {
					sprintf(outstr, "%s", cargo_name);
				} else {
					sprintf(outstr, "%s: %s", cargo_sp->cargo_title, cargo_name);
				}
			} else {
				if (cargo_name[0] == '#') {
					sprintf(outstr, XSTR("passengers: %s", 83), cargo_name + 1);
				} else {
					sprintf(outstr, XSTR("cargo: %s", 84), cargo_name);
				}
			}
		} else {
			strcpy(outstr, XSTR( "Scanned", 85) );
		}

		// always bash cargo_inspect_time to 0 since AI ships can reveal cargo that we
		// are in the process of scanning
		Player->cargo_inspect_time = 0;

		return true;
	}

	// see if player is within inspection range
	ship_info* player_sip = &Ship_info[Player_ship->ship_info_index];
	float scan_dist = MAX(player_sip->scan_range_normal, (cargo_objp->radius + player_sip->scan_range_normal - CARGO_RADIUS_REAL_DELTA));
	scan_dist *= player_sip->scanning_range_multiplier;

	if ( Player_ai->current_target_distance < scan_dist ) {
		vec3d vec_to_cargo;

		// check if player is facing cargo, do not proceed with inspection if not
		vm_vec_normalized_dir(&vec_to_cargo, &cargo_objp->pos, &Player_obj->pos);
		float dot = vm_vec_dot(&vec_to_cargo, &Player_obj->orient.vec.fvec);
		if ( dot < CARGO_MIN_DOT_TO_REVEAL ) {
			if (reveal_cargo) {
				if (cargo_sp->cargo_title[0] != '\0') {
					if (cargo_sp->cargo_title[0] == '#') {
						strcpy(outstr, XSTR("<unknown>", 1852));
					} else {
						sprintf(outstr, XSTR("%s: <unknown>", 1850), cargo_sp->cargo_title);
					}
				} else {
					strcpy(outstr, XSTR("cargo: <unknown>", 86));
				}
			} else {
				strcpy(outstr, XSTR("not scanned", 87));
			}

			hud_targetbox_end_flash(TBOX_FLASH_CARGO);
			Player->cargo_inspect_time = 0;
			return true;
		}

		// player is facing the cargo, and within range, so proceed with inspection
		if ( hud_sensors_ok(Player_ship, 0) ) {
			Player->cargo_inspect_time += (int)std::lround(frametime*1000);
		}

		if (reveal_cargo) {
			if (cargo_sp->cargo_title[0] != '\0') {
				if (cargo_sp->cargo_title[0] == '#') {
					strcpy(outstr, XSTR("inspecting", 1853));
				} else {
					sprintf(outstr, XSTR("%s: inspecting", 1851), cargo_sp->cargo_title);
				}
			} else {
				strcpy(outstr, XSTR("cargo: inspecting", 88));
			}
		} else {
			strcpy(outstr, XSTR("scanning", 89));
		}

		float scan_time = i2fl(cargo_sip->scan_time);
		scan_time *= player_sip->scanning_time_multiplier;

		if ( Player->cargo_inspect_time > scan_time ) {
			ship_do_cargo_revealed( cargo_sp );
			snd_play( gamesnd_get_game_sound(GameSounds::CARGO_REVEAL), 0.0f );
			Player->cargo_inspect_time = 0;
		}
	} else {
		if (reveal_cargo){
			if (cargo_sp->cargo_title[0] != '\0') {
				if (cargo_sp->cargo_title[0] == '#') {
					strcpy(outstr, XSTR("<unknown>", 1852));
				} else {
					sprintf(outstr, XSTR("%s: <unknown>", 1850), cargo_sp->cargo_title);
				}
			} else {
				strcpy(outstr, XSTR("cargo: <unknown>", 86));
			}
		} else {
			strcpy(outstr, XSTR("not scanned", 87));
		}
	}

	return true;
}

/**
 * @return 1 if player should display outstr on HUD; 0 if don't display cargo on HUD
 */
bool player_inspect_cap_subsys_cargo(float frametime, char *outstr)
{
	object		*cargo_objp;
	ship		*cargo_sp;
	ship_info	*cargo_sip;
	ship_subsys	*subsys;

	outstr[0] = 0;
	subsys = Player_ai->targeted_subsys;

	if ( subsys == NULL || Player_ship->flags[Ship::Ship_Flags::Cannot_perform_scan] ) {
		return false;
	}

	cargo_objp = &Objects[Player_ai->target_objnum];
	Assert(cargo_objp->type == OBJ_SHIP);
	cargo_sp = &Ships[cargo_objp->instance];
	cargo_sip = &Ship_info[cargo_sp->ship_info_index];

	// If we're using the new scanning behavior then we have to check that the ship is actually scannable first
	if (Use_new_scanning_behavior && !(cargo_sp->flags[Ship::Ship_Flags::Scannable])) {
		return false;
	}

	// don't do any sort of scanning thing unless capship has a non-"nothing" cargo
	// this compensates for changing the "no display" index from -1 to 0
	if (subsys->subsys_cargo_name == 0) {
		return false;
	}

	// Whether or not we are scanning in a mode that will reveal the cargo contents
	bool reveal_cargo = false;
	if (Use_new_scanning_behavior && !(cargo_sp->flags[Ship::Ship_Flags::No_scanned_cargo])) {
		reveal_cargo = true;
	} else if (!(cargo_sp->flags[Ship::Ship_Flags::Scannable])) {
		reveal_cargo = true;
	}

	// if cargo is already revealed
	if (subsys->flags[Ship::Subsystem_Flags::Cargo_revealed]) {
		if (reveal_cargo) {
			auto cargo_name = (subsys->subsys_cargo_name & CARGO_INDEX_MASK) == 0
				? XSTR("Nothing", 1674)
				: Cargo_names[subsys->subsys_cargo_name & CARGO_INDEX_MASK];
			if (subsys->subsys_cargo_title[0] != '\0') {
				if (subsys->subsys_cargo_title[0] == '#') {
					sprintf(outstr, "%s", cargo_name);
				} else {
					sprintf(outstr, "%s: %s", subsys->subsys_cargo_title, cargo_name);
				}
			} else {
				if (cargo_name[0] == '#') {
					sprintf(outstr, XSTR("passengers: %s", 83), cargo_name + 1);
				} else {
					sprintf(outstr, XSTR("cargo: %s", 84), cargo_name);
				}
			}
		} else {
			strcpy(outstr, XSTR( "Scanned", 85) );
		}
	
		// always bash cargo_inspect_time to 0 since AI ships can reveal cargo that we
		// are in the process of scanning
		Player->cargo_inspect_time = 0;

		return true;
	}

	// see if player is within inspection range [ok for subsys]
	vec3d	subsys_pos;
	float		subsys_rad;
	int		subsys_in_view, x, y;
	float scan_dist;

	get_subsystem_world_pos(cargo_objp, Player_ai->targeted_subsys, &subsys_pos);
	subsys_rad = subsys->system_info->radius;

	// Goober5000
	ship_info* player_sip = &Ship_info[Player_ship->ship_info_index];
    if (cargo_sip->is_huge_ship()) {
		scan_dist = MAX(player_sip->scan_range_capital, (subsys_rad + player_sip->scan_range_capital - CARGO_RADIUS_REAL_DELTA));
	} else {
		scan_dist = MAX(player_sip->scan_range_normal, (subsys_rad + player_sip->scan_range_normal - CARGO_RADIUS_REAL_DELTA));
	}
	scan_dist *= player_sip->scanning_range_multiplier;

	if ( Player_ai->current_target_distance < scan_dist ) {
		vec3d vec_to_cargo;

		// check if player is facing cargo, do not proceed with inspection if not
		vm_vec_normalized_dir(&vec_to_cargo, &subsys_pos, &Player_obj->pos);
		float dot = vm_vec_dot(&vec_to_cargo, &Player_obj->orient.vec.fvec);
		int hud_targetbox_subsystem_in_view(object *target_objp, int *sx, int *sy);
		subsys_in_view = hud_targetbox_subsystem_in_view(cargo_objp, &x, &y);

		if ( (dot < CARGO_MIN_DOT_TO_REVEAL) || (!subsys_in_view) ) {
			if (reveal_cargo) {
				if (subsys->subsys_cargo_title[0] != '\0') {
					if (subsys->subsys_cargo_title[0] == '#') {
						strcpy(outstr, XSTR("<unknown>", 1852));
					} else {
						sprintf(outstr, XSTR("%s: <unknown>", 1850), subsys->subsys_cargo_title);
					}
				} else {
					strcpy(outstr, XSTR("cargo: <unknown>", 86));
				}
			} else {
				strcpy(outstr,XSTR( "not scanned", 87));
			}

			hud_targetbox_end_flash(TBOX_FLASH_CARGO);
			Player->cargo_inspect_time = 0;
			return true;
		}

		// player is facing the cargo, and within range, so proceed with inspection
		if ( hud_sensors_ok(Player_ship, 0) ) {
			Player->cargo_inspect_time += (int)std::lround(frametime*1000);
		}

		if (reveal_cargo)
			if (subsys->subsys_cargo_title[0] != '\0') {
				if (subsys->subsys_cargo_title[0] == '#') {
					strcpy(outstr, XSTR("inspecting", 1853));
				} else {
					sprintf(outstr, XSTR("%s: inspecting", 1851), subsys->subsys_cargo_title);
				}
			} else {
				strcpy(outstr, XSTR("cargo: inspecting", 88));
			}
		else
			strcpy(outstr,XSTR( "scanning", 89));

		float scan_time;
		if (subsys->system_info->scan_time > 0)
			scan_time = i2fl(subsys->system_info->scan_time);
		else
			scan_time = i2fl(cargo_sip->scan_time);
		scan_time *= player_sip->scanning_time_multiplier;

		if ( Player->cargo_inspect_time > scan_time ) {
			ship_do_cap_subsys_cargo_revealed( cargo_sp, subsys, 0);
			snd_play( gamesnd_get_game_sound(GameSounds::CARGO_REVEAL), 0.0f );
			Player->cargo_inspect_time = 0;
		}
	} else {
		if (reveal_cargo)
			if (subsys->subsys_cargo_title[0] != '\0') {
				if (subsys->subsys_cargo_title[0] == '#') {
					strcpy(outstr, XSTR("<unknown>", 1852));
				} else {
					sprintf(outstr, XSTR("%s: <unknown>", 1850), subsys->subsys_cargo_title);
				}
			} else {
				strcpy(outstr, XSTR("cargo: <unknown>", 86));
			}
		else
			strcpy(outstr,XSTR( "not scanned", 87));
	}

	return true;
}


/**
 * Get the maximum weapon range for the player (of both primary and secondary)
 * @return Maximum weapon range
 */
float	player_farthest_weapon_range()
{
	float prange,srange;

	hud_get_best_primary_bank(&prange);
	srange=ship_get_secondary_weapon_range(Player_ship);

	return MAX(prange,srange);
}

/**
 * Generates the message for death of a player given the information stored in the player object.
 */
void player_generate_death_message(player *player_p)
{
	SCP_string &msg = player_p->death_message;

	// killer_parent_name is always a ship name or a callsign (or blank).  If it's a ship name, get the ship and use the ship's display name
	auto ship_entry = ship_registry_get(player_p->killer_parent_name);
	auto shipp = ship_entry && ship_entry->has_shipp() ? ship_entry->shipp() : nullptr;
	auto killer_display_name = shipp ? shipp->get_display_name() : player_p->killer_parent_name;

	switch (player_p->killer_objtype)
	{
		case OBJ_SHOCKWAVE:
			if (player_p->killer_weapon_index >= 0)
			{
				sprintf(msg, XSTR( "%s was killed by a missile shockwave", 92), player_p->callsign);
			}
			else
			{
				sprintf(msg, XSTR( "%s was killed by a shockwave from %s exploding", 93), player_p->callsign, killer_display_name);
			}
			break;

		case OBJ_WEAPON:
			// is this from a friendly ship?
			if (shipp && Player_ship && (Player_ship->team == shipp->team))
			{
				sprintf(msg, XSTR( "%s was killed by friendly fire from %s", 1338), player_p->callsign, killer_display_name);
			}
			else
			{
				sprintf(msg, XSTR( "%s was killed by %s", 94), player_p->callsign, killer_display_name);
			}
			break;

		case OBJ_SHIP:
			if (player_p->flags & PLAYER_FLAGS_KILLED_BY_EXPLOSION)
			{
				sprintf(msg, XSTR( "%s was killed by a blast from %s exploding", 95), player_p->callsign, killer_display_name);
			}
			else if (player_p->flags & PLAYER_FLAGS_KILLED_BY_ENGINE_WASH)
			{
				sprintf(msg, XSTR( "%s was killed by engine wash from %s", 1494), player_p->callsign, killer_display_name);
			}
			else
			{
				sprintf(msg, XSTR( "%s was killed by a collision with %s", 96), player_p->callsign, killer_display_name);
			}
			break;

		case OBJ_DEBRIS:
			sprintf(msg, XSTR( "%s was killed by a collision with debris", 97), player_p->callsign);
			break;

		case OBJ_ASTEROID:
			sprintf(msg, XSTR( "%s was killed by a collision with an asteroid", 98), player_p->callsign);
			break;

		case OBJ_BEAM:
			if (strlen(player_p->killer_parent_name) <= 0)
			{
				Warning(LOCATION, "Killer_parent_name not specified for beam!");
				sprintf(msg, XSTR( "%s was killed by a beam from an unknown source", 1081), player_p->callsign);
			}
			else
			{
				// is this from a friendly ship?
				if (shipp && Player_ship && (Player_ship->team == shipp->team))
				{
					sprintf(msg, XSTR( "%s was destroyed by friendly beam fire from %s", 1339), player_p->callsign, killer_display_name);
				}
				else
				{
					sprintf(msg, XSTR( "%s was destroyed by a beam from %s", 1082), player_p->callsign, killer_display_name);
				}			
			}
			break;

		default:
			sprintf(msg, XSTR( "%s was killed by unknown causes", 99), player_p->callsign);
			break;
	}
}

/**
 * Display what/who killed the player
 */
void player_show_death_message()
{
	SCP_string &msg = Player->death_message;

	// make sure we don't already have a death message
	if (msg.empty())
	{
		// check if player killed self
		if (Player->flags & PLAYER_KILLED_SELF)
		{
			// reasons he killed himself
			if (Player->flags & PLAYER_FLAGS_KILLED_SELF_SHOCKWAVE)
			{
				msg = XSTR("You have killed yourself with a shockwave from your own weapon", 1421);
			}
			else if (Player->flags & PLAYER_FLAGS_KILLED_SELF_MISSILES)
			{
				msg = XSTR("You have killed yourself with your own missiles", 1422);
			}
			else
			{
				msg = XSTR("You have killed yourself", 100);
			}
	
			Player->flags &= ~PLAYER_KILLED_SELF;
		}
		else
		{
			player_generate_death_message(Player);
		}
	}
	color col;
	gr_init_color(&col, 255, 0, 0);
	// display the message
	HUD_fixed_printf(30.0f, col, "%s", msg.c_str());
}

void player_set_next_all_alone_msg_timestamp()
{
	Player->check_for_all_alone_msg=timestamp(30000);
}

/**
 * Maybe play message from Terran Command 'You're all alone now, pilot'
 */
void player_maybe_play_all_alone_msg()
{
	if ( Game_mode & GM_MULTIPLAYER ){
		return;
	}

	if ( !Player_all_alone_msg_inited ) {
		player_init_all_alone_msg();
		Player_all_alone_msg_inited=1;
		return;
	}

	if ( Player->flags & PLAYER_FLAGS_NO_CHECK_ALL_ALONE_MSG ) {
		return;
	}

	// only check every N seconds
	if ( !timestamp_elapsed(Player->check_for_all_alone_msg) ) {
		return;
	}

	player_set_next_all_alone_msg_timestamp();
	
	// at least one primary objective must be not complete (but not failed)
	if ( !mission_goals_incomplete(PRIMARY_GOAL) ) {
		Player->flags |= PLAYER_FLAGS_NO_CHECK_ALL_ALONE_MSG;
		return;
	}

	// there must be no reinforcements available, hold off on message
	if ( (Player_ship != NULL) && hud_squadmsg_reinforcements_available(Player_ship->team) ) {
		return;
	}

	// there must be no ships present that are on the same team as the player
	ship_obj *so;
	object	*objp;

	for ( so = GET_FIRST(&Ship_obj_list); so != END_OF_LIST(&Ship_obj_list); so = GET_NEXT(so) ) {
		objp = &Objects[so->objnum];
		if (objp->flags[Object::Object_Flags::Should_be_dead])
			continue;

		if ( objp == Player_obj ) {
			continue;
		}

		if ( Ships[objp->instance].team == Player_ship->team ) {
			int ship_type = Ship_info[Ships[objp->instance].ship_info_index].class_type;
			if ( ship_type != -1 && (Ship_types[ship_type].flags[Ship::Type_Info_Flags::Counts_for_alone]) ) {
				return;
			}
		}
	}

	message_send_builtin(MESSAGE_ALL_ALONE, nullptr, nullptr, -1, -1);
	Player->flags |= PLAYER_FLAGS_NO_CHECK_ALL_ALONE_MSG;
} 

void player_display_padlock_view()
{
	int padlock_view_index=0;

	if ( Viewer_mode & VM_PADLOCK_UP ) {
		padlock_view_index = 0;
	} else if ( Viewer_mode & VM_PADLOCK_REAR ) {
		padlock_view_index = 1;
	} else if ( Viewer_mode & VM_PADLOCK_LEFT ) {
		padlock_view_index = 2;
	} else if ( Viewer_mode & VM_PADLOCK_RIGHT ) {
		padlock_view_index = 3;
	} else {
		Int3();
		return;
	}

	char str[128];

	if ( !(Viewer_mode & (VM_CHASE|VM_EXTERNAL)) ) {
		switch (padlock_view_index) {
		case 0:
			strcpy_s(str, XSTR( "top view", 101));	break;
		case 1:
			strcpy_s(str, XSTR( "rear view", 102));	break;
		case 2:
			strcpy_s(str, XSTR( "left view", 103));	break;
		case 3:
			strcpy_s(str, XSTR( "right view", 104));	break;
			}
		color col;
		gr_init_color(&col, 0, 255, 0);
		HUD_fixed_printf(0.01f, col, "%s", str);
	}
}

extern vec3d Dead_camera_pos;
extern vec3d Dead_player_last_vel;

#define	MIN_DIST_TO_DEAD_CAMERA			50.0f
/**
 * Get the player's eye position and orient
 */
camid player_get_cam()
{
	static camid player_camera;
	if(!player_camera.isValid())
	{
		player_camera = cam_create("Player camera");
	}

	object *viewer_obj = NULL;
	vec3d eye_pos = vmd_zero_vector;
	matrix eye_orient = vmd_identity_matrix;
	vec3d tmp_dir;

	// if the player object is NULL, return
	if(Player_obj == NULL){
		return camid();
	}

	// standalone servers can bail here
	if(Game_mode & GM_STANDALONE_SERVER){
		return camid();
	}

	// if we're not in-mission, don't do this
	if(!(Game_mode & GM_IN_MISSION)){
		return camid();
	}

	if (Game_mode & GM_DEAD) {
		vec3d	vec_to_deader, view_pos;
		float		dist;		
		if (Player_ai->target_objnum != -1) {
			int view_from_player = 1;

			if (Viewer_mode & VM_OTHER_SHIP) {
				//	View from target.
				viewer_obj = &Objects[Player_ai->target_objnum];
				object_get_eye( &eye_pos, &eye_orient, viewer_obj );
				view_from_player = 0;
			}

			if ( view_from_player ) {
				//	View target from player ship.
				viewer_obj = NULL;
				eye_pos = Player_obj->pos;

				vm_vec_normalized_dir(&tmp_dir, &Objects[Player_ai->target_objnum].pos, &eye_pos);
				vm_vector_2_matrix_norm(&eye_orient, &tmp_dir, nullptr, nullptr);
			}
		} else {
			dist = vm_vec_normalized_dir(&vec_to_deader, &Player_obj->pos, &Dead_camera_pos);
			
			if (dist < MIN_DIST_TO_DEAD_CAMERA){
				dist += flFrametime * 16.0f;
			}

			vm_vec_scale(&vec_to_deader, -dist);
			vm_vec_add(&Dead_camera_pos, &Player_obj->pos, &vec_to_deader);
			
			view_pos = Player_obj->pos;

			if (!(Game_mode & GM_DEAD_BLEW_UP)) {								
			} else if (Player_ai->target_objnum != -1) {
				view_pos = Objects[Player_ai->target_objnum].pos;
			} else {
				//	Make camera follow explosion, but gradually slow down.
				vm_vec_scale_add2(&Player_obj->pos, &Dead_player_last_vel, flFrametime);
				view_pos = Player_obj->pos;				
			}

			eye_pos = Dead_camera_pos;

			vm_vec_normalized_dir(&tmp_dir, &Player_obj->pos, &eye_pos);

			vm_vector_2_matrix_norm(&eye_orient, &tmp_dir, nullptr, nullptr);
			viewer_obj = NULL;
		}
	} 
	
	//	If already blown up, these other modes can override.
	if (!(Game_mode & (GM_DEAD | GM_DEAD_BLEW_UP))) {
		if(!(Viewer_mode & VM_FREECAMERA))
				viewer_obj = Player_obj;
 
		if (Viewer_mode & VM_OTHER_SHIP) {
			if (Player_ai->target_objnum != -1){
				viewer_obj = &Objects[Player_ai->target_objnum];
			} 
		}
		if(Viewer_mode & VM_FREECAMERA) {
				Viewer_obj = NULL;
				return cam_get_current();
		} else if (Viewer_mode & VM_EXTERNAL) {
			Assert(viewer_obj != NULL);
			matrix	tm, tm2;
			vec3d uvec;

			vm_angles_2_matrix(&tm2, &Viewer_external_info.angles);
			vm_matrix_x_matrix(&tm, &viewer_obj->orient, &tm2);

			Viewer_external_info.current_distance = cam_get_bbox_dist(viewer_obj, Viewer_external_info.preferred_distance, &tm2);

			vm_vec_scale_add(&eye_pos, &viewer_obj->pos, &tm.vec.fvec, Viewer_external_info.current_distance);

			vm_vec_normalized_dir(&tmp_dir, &viewer_obj->pos, &eye_pos);
			vm_vec_copy_normalize(&uvec, &viewer_obj->orient.vec.uvec);	// out of an abundance of caution
			vm_vector_2_matrix_norm(&eye_orient, &tmp_dir, &uvec, nullptr);
 			viewer_obj = NULL;

			//	Modify the orientation based on head orientation.
			compute_slew_matrix(&eye_orient, &Viewer_slew_angles);
		} else if ( Viewer_mode & VM_CHASE ) {
			vec3d	move_dir;

			if ( viewer_obj->phys_info.speed < 0.1 ){
				move_dir = viewer_obj->orient.vec.fvec;
			} else {
				move_dir = viewer_obj->phys_info.vel;
				vm_vec_normalize_safe(&move_dir);
			}

			vm_vec_scale_add(&eye_pos, &viewer_obj->pos, &move_dir, -3.0f * viewer_obj->radius - Viewer_chase_info.distance);
			vm_vec_scale_add2(&eye_pos, &viewer_obj->orient.vec.uvec, 0.75f * viewer_obj->radius);
			vm_vec_normalized_dir(&tmp_dir, &viewer_obj->pos, &eye_pos);

			// JAS: I added the following code because if you slew up using
			// Descent-style physics, eye_dir and Viewer_obj->orient.vec.uvec are
			// equal, which causes a zero-length vector in the vm_vector_2_matrix
			// call because the up and the forward vector are the same.   I fixed
			// it by adding in a fraction of the right vector all the time to the
			// up vector.
			vec3d tmp_up = viewer_obj->orient.vec.uvec;
			vm_vec_scale_add2( &tmp_up, &viewer_obj->orient.vec.rvec, 0.00001f );

			vm_vector_2_matrix(&eye_orient, &tmp_dir, &tmp_up, NULL);
			viewer_obj = NULL;

			//	Modify the orientation based on head orientation.
			compute_slew_matrix(&eye_orient, &Viewer_slew_angles);
		} else if ( Viewer_mode & VM_WARP_CHASE ) {
			Warp_camera.get_info(&eye_pos, NULL);

			ship * shipp = &Ships[Player_obj->instance];
			vec3d uvec, warp_pos = Player_obj->pos;
			shipp->warpout_effect->getWarpPosition(&warp_pos);

			vm_vec_normalized_dir(&tmp_dir, &warp_pos, &eye_pos);
			vm_vec_copy_normalize(&uvec, &Player_obj->orient.vec.uvec);	// out of an abundance of caution
			vm_vector_2_matrix_norm(&eye_orient, &tmp_dir, &uvec, nullptr);
			viewer_obj = NULL;
		} else {
			// get an eye position for the player object
			object_get_eye( &eye_pos, &eye_orient, viewer_obj );
		}
	}

	player_camera.getCamera()->set_position(&eye_pos);
	player_camera.getCamera()->set_rotation(&eye_orient);

	return player_camera;
}
