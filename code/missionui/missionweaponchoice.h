/*
 * Copyright (C) Volition, Inc. 1999.  All rights reserved.
 *
 * All source code herein is the property of Volition, Inc. You may not sell 
 * or otherwise commercially exploit the source or things you created based on the 
 * source.
 *
*/ 

#include "globalincs/globals.h"
#include "mod_table/mod_table.h"


#ifndef __MISSION_WEAPON_CHOICE_H__
#define __MISSION_WEAPON_CHOICE_H__

class p_object;
struct wss_unit;
class ship_weapon;
struct team_data;

#define WEAPON_DESC_MAX_LINES			7				// max lines in the description incl. title
#define WEAPON_DESC_MAX_LENGTH		50				// max chars per line of description text

extern int Weapon_select_overlay_id;

void weapon_select_init();
void weapon_select_common_init(bool API_Access = false);
void weapon_select_do(float frametime);
void weapon_select_close();
void weapon_select_close_team();

void draw_3d_overhead_view(int model_num,
	int ship_class,
	float* rotation_buffer,
	float frametime,
	int weapon_array[MAX_SHIP_WEAPONS],
	int selected_weapon_class,
	int hovered_weapon_slot,
	int x1,
	int y1,
	int x2,
	int y2,
	int resize_mode,
	int bank1_x,
	int bank1_y,
	int bank2_x,
	int bank2_y,
	int bank3_x,
	int bank3_y,
	int bank4_x,
	int bank4_y,
	int bank5_x,
	int bank5_y,
	int bank6_x,
	int bank6_y,
	int bank7_x,
	int bank7_y,
	int bank_prim_offset = 106,
	int bank_sec_offset = -50,
	int bank_y_offset = 12,
	overhead_style style = Default_overhead_ship_style,
	const SCP_string& tcolor = "");

void	wl_update_parse_object_weapons(p_object *pobjp, wss_unit *slot);
int	wl_update_ship_weapons(int objnum, wss_unit *slot);
void	wl_bash_ship_weapons(ship_weapon *swp, wss_unit *slot);

void wl_set_default_weapons(int index, int ship_class);
void wl_reset_to_defaults();
void wl_init_pool(team_data* td);
void wl_fill_slots();

// Set selected slot to first placed ship
void wl_reset_selected_slot();

void wl_remove_weps_from_pool(int *wep, int *wep_count, int ship_class);
void wl_get_ship_class_weapons(int ship_class, int *wep, int *wep_count);
void wl_get_default_weapons(int ship_class, int slot_num, int *wep, int *wep_count);
int eval_weapon_flag_for_game_type(int weapon_flags);
int wl_calc_missile_fit(int wi_index, int capacity);

void wl_synch_interface();
int wl_apply(int mode,int from_bank,int from_list,int to_bank,int to_list,int ship_slot,int player_index = -1, bool dont_play_sound = false);
int wl_drop(int from_bank,int from_list,int to_bank,int to_list, int ship_slot,int player_index = -1, bool dont_play_sound = false);

#endif /* __MISSION_WEAPON_CHOICE_H__ */
