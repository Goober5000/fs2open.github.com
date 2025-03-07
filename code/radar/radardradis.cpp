/*
 * Created by Olivier "LuaPineapple" Hamel for the Freespace 2 Source Code Project.
 * You may not sell or otherwise commercially exploit the source or things you
 * create based on the source.
 *
 */

#include "bmpman/bmpman.h"
#include "freespace.h"
#include "gamesnd/gamesnd.h"
#include "globalincs/linklist.h"
#include "globalincs/alphacolors.h"
#include "globalincs/systemvars.h"
#include "graphics/font.h"
#include "graphics/matrix.h"
#include "hud/hudwingmanstatus.h"
#include "iff_defs/iff_defs.h"
#include "io/timer.h"
#include "jumpnode/jumpnode.h"
#include "localization/localize.h"
#include "math/staticrand.h"
#include "network/multi.h"
#include "object/object.h"
#include "playerman/player.h"
#include "radar/radardradis.h"
#include "radar/radarorb.h"
#include "render/3d.h"
#include "ship/awacs.h"
#include "ship/ship.h"
#include "ship/subsysdamage.h"
#include "weapon/emp.h"
#include "weapon/weapon.h"

#define RADIANS_PER_DEGREE (PI / 180.0f)

HudGaugeRadarDradis::HudGaugeRadarDradis():
HudGaugeRadar(HUD_OBJECT_RADAR_BSG, 255, 255, 255), 
xy_plane(-1), xz_yz_plane(-1), sweep_plane(-1), target_brackets(-1), unknown_contact_icon(-1), sweep_duration(6 * MILLISECONDS_PER_SECOND), sweep_angle(0.0f), scale(1.20f), sub_y_clip(false)
{
	vm_vec_copy_scale(&sweep_normal_x, &vmd_zero_vector, 1.0f);
	vm_vec_copy_scale(&sweep_normal_y, &vmd_zero_vector, 1.0f);
	vm_vec_copy_scale(&sweep_normal_z, &vmd_zero_vector, 1.0f);

	// init the view perturb matrix
	view_perturb.a2d[0][0] = 1.0f;
	view_perturb.a2d[0][1] = 0.0f;
	view_perturb.a2d[0][2] = 0.0f;
	view_perturb.a2d[1][0] = 0.0f;
	view_perturb.a2d[1][1] = -1.0f;
	view_perturb.a2d[1][2] = 0.0f;
	view_perturb.a2d[2][0] = 0.0f;
	view_perturb.a2d[2][1] = 0.0f;
	view_perturb.a2d[2][2] = 1.0f;

	// init the orb eye position
	Orb_eye_position.a1d[0] = 0.0f;
	Orb_eye_position.a1d[1] = 0.0f;
	Orb_eye_position.a1d[2] = -2.5f;

	fx_guides0_0.a1d[0] = -1.0f;
	fx_guides0_0.a1d[1] = 0.0f;
	fx_guides0_0.a1d[2] = 0.0f;
	
	fx_guides0_1.a1d[0] = 1.0f;
	fx_guides0_1.a1d[1] = 0.0f;
	fx_guides0_1.a1d[2] = 0.0f;

	fx_guides1_0.a1d[0] = 0.0f;
	fx_guides1_0.a1d[1] = -1.0f;
	fx_guides1_0.a1d[2] = 0.0f;
	
	fx_guides1_1.a1d[0] = 0.0f;
	fx_guides1_1.a1d[1] = 1.0f;
	fx_guides1_1.a1d[2] = 0.0f;

	fx_guides2_0.a1d[0] = 0.0f;
	fx_guides2_0.a1d[1] = 0.0f;
	fx_guides2_0.a1d[2] = -1.0f;
	
	fx_guides2_1.a1d[0] = 0.0f;
	fx_guides2_1.a1d[1] = 0.0f;
	fx_guides2_1.a1d[2] = 1.0f;

	this->loop_sound_handle = sound_handle::invalid();
}

void HudGaugeRadarDradis::initBitmaps(char* fname_xy, char* fname_xz_yz, char* fname_sweep, char* fname_target_brackets, char* fname_unknown)
{
	xy_plane = bm_load(fname_xy); // Base
	if ( xy_plane < 0 ) {
		Warning(LOCATION,"Cannot load hud bitmap: %s\n", fname_xy);
	}

	xz_yz_plane = bm_load(fname_xz_yz); // Two vertical cross rings
	if ( xz_yz_plane < 0 ) {
		Warning(LOCATION,"Cannot load hud bitmap: %s\n", fname_xz_yz);
	}

	sweep_plane = bm_load(fname_sweep); // Sweep lines
	if ( sweep_plane < 0 ) {
		Warning(LOCATION,"Cannot load hud bitmap: %s\n", fname_sweep);
	}

	target_brackets = bm_load(fname_target_brackets);
	if ( target_brackets < 0 ) {
		Warning(LOCATION,"Cannot load hud bitmap: %s\n", fname_target_brackets);
	}

	unknown_contact_icon = bm_load(fname_unknown);
	if ( unknown_contact_icon < 0 ) {
		Warning(LOCATION,"Cannot load hud bitmap: %s\n", fname_unknown);
	}
}

void HudGaugeRadarDradis::plotBlip(blip* b, vec3d *pos, float *alpha)
{
	*pos = b->position;
	vm_vec_normalize(pos);

	auto objp = &Objects[b->objnum];

	if (ship_is_tagged(objp)) {
		*alpha = 1.0f;
		return;
	}

	float fade_multi = 1.5f;
	
	if (objp->type == OBJ_SHIP) {
		if (Ships[objp->instance].flags[Ship::Ship_Flags::Stealth]) {
			fade_multi *= 2.0f;
		}
	}

	auto& last_update = Blip_last_update[b->objnum];
	
	// If the blip has been pinged by the local x-axis sweep, update
	if (std::abs(vm_vec_dot(&sweep_normal_x, pos)) < 0.01f) {
		last_update = _timestamp();
	}

	if (last_update.isNever()) {
		*alpha = 0.0f;
	} else {
		*alpha = ((sweep_duration - timestamp_since(last_update)) / i2fl(sweep_duration)) * fade_multi / 2.0f;
		CLAMP(*alpha, 0.0f, 1.0f);
	}
}

void HudGaugeRadarDradis::drawContact(vec3d *pnt, int idx, int clr_idx, float  /*dist*/, float alpha, float scale_factor)
{
	vec3d  p;
	int h, w;
	vertex vert;
	float aspect_mp;

	if ((sub_y_clip && (pnt->xyz.y > 0)) || ((!sub_y_clip) && (pnt->xyz.y <= 0)))
		return;

    memset(&vert, 0, sizeof(vert));
    
	vm_vec_rotate(&p, pnt,  &vmd_identity_matrix); 
	g3_transfer_vertex(&vert, &p);
	
	float sizef = fl_sqrt(vm_vec_dist(&Orb_eye_position, pnt) * 8.0f) * scale_factor;

    if ( clr_idx >= 0 ) {
        bm_get_info(clr_idx, &w, &h);
        
        if (h == w) {
            aspect_mp = 1.0f;
        } else {
            aspect_mp = (((float) h) / ((float) w));
        }
        
        //gr_set_bitmap(clr_idx, GR_ALPHABLEND_FILTER, GR_BITBLT_MODE_NORMAL, alpha);
        //g3_draw_polygon(&p, &vmd_identity_matrix, sizef/35.0f, aspect_mp*sizef/35.0f, TMAP_FLAG_TEXTURED | TMAP_HTL_3D_UNLIT);
		material mat_params;
		material_set_unlit_color(&mat_params, clr_idx, &Color_bright_white, alpha, true, false);
		g3_render_rect_oriented(&mat_params, &p, &vmd_identity_matrix, sizef/35.0f, aspect_mp*sizef/35.0f);
    }
    
    if ( idx >= 0 ) {
        bm_get_info(idx, &w, &h);
        
        if (h == w) {
            aspect_mp = 1.0f;
        } else {
            aspect_mp = (((float) h) / ((float) w));
        }
        
        //gr_set_bitmap(idx, GR_ALPHABLEND_FILTER, GR_BITBLT_MODE_NORMAL, alpha);
        //g3_draw_polygon(&p, &vmd_identity_matrix, sizef/35.0f, aspect_mp*sizef/35.0f, TMAP_FLAG_TEXTURED | TMAP_FLAG_BW_TEXTURE | TMAP_HTL_3D_UNLIT);
		material mat_params;
		material_set_unlit_color(&mat_params, idx, &gr_screen.current_color, alpha, true, false);
		g3_render_rect_oriented(&mat_params, &p, &vmd_identity_matrix, sizef/35.0f, aspect_mp*sizef/35.0f);
    }
}

// radar is damaged, so make blips dance around
void HudGaugeRadarDradis::blipDrawDistorted(blip *b, vec3d *pos, float alpha)
{
	float temp_scale;
	float dist = vm_vec_normalize(pos);
	vec3d out;
	float distortion_angle=20;

	// maybe alter the effect if EMP is active
	if (emp_active_local())
	{
		temp_scale = emp_current_intensity();
		dist *= frand_range(MAX(0.75f, 0.75f*temp_scale), MIN(1.25f, 1.25f*temp_scale));
		distortion_angle *= frand_range(-3.0f,3.0f)*frand_range(0.0f, temp_scale);

		if (dist > 1.0f) dist = 1.0f;
		if (dist < 0.1f) dist = 0.1f;
	}

	vm_vec_random_cone(&out, pos, distortion_angle);
	vm_vec_scale(&out, dist);

	drawContact(&out, -1, unknown_contact_icon, b->dist, alpha, 1.0f);
}

// blip is for a target immune to sensors, so cause to flicker in/out with mild distortion
void HudGaugeRadarDradis::blipDrawFlicker(blip *b, vec3d *pos, float alpha)
{
	int flicker_index;

	float dist=vm_vec_normalize(pos);
	vec3d out;
	float distortion_angle=10;

	if ((b-Blips) & 1)
		flicker_index=0;
	else
		flicker_index=1;
	

	if (timestamp_elapsed(Radar_flicker_timer[flicker_index])) {
		Radar_flicker_timer[flicker_index] = _timestamp_rand(50,1000);
		Radar_flicker_on[flicker_index] = !Radar_flicker_on[flicker_index];
	}

	if (!Radar_flicker_on[flicker_index])
		return;

	if (Random::flip_coin())
	{
		distortion_angle *= frand_range(0.1f,2.0f);
		dist *= frand_range(0.75f, 1.25f);

		if (dist > 1.0f) dist = 1.0f;
		if (dist < 0.1f) dist = 0.1f;
	}
	
	vm_vec_random_cone(&out,pos,distortion_angle);
	vm_vec_scale(&out,dist);

	drawContact(&out, -1, unknown_contact_icon, b->dist, alpha, 1.0f);
}

// Draw all the active radar blips
void HudGaugeRadarDradis::drawBlips(int blip_type, int bright, int distort)
{
	blip *b = NULL;
	blip *blip_head;
	vec3d pos;
	float alpha;
	
	Assert((blip_type >= 0) && (blip_type < MAX_BLIP_TYPES));
	
	//long frametime = timer_get_approx_seconds();
	// Need to set font.
	font::set_font(font::FONT1);

	if(bright) {
		blip_head = &Blip_bright_list[blip_type];
	} else {
		blip_head = &Blip_dim_list[blip_type];
	}
	
	float scale_factor = 1.0f;

	// draw all blips of this type
	for (b = GET_FIRST(blip_head); b != END_OF_LIST(blip_head); b = GET_NEXT(b))
	{
		plotBlip(b, &pos, &alpha);
		
		gr_set_color_fast(b->blip_color);

		scale_factor = 1.0f;

		// maybe draw cool blip to indicate current target
		if (b->flags & BLIP_CURRENT_TARGET)
		{
			if (radar_target_id_flags & RTIF_PULSATE) {
				scale_factor *= 1.3f + (sinf(10 * f2fl(Missiontime)) * 0.3f);
			}
			if (radar_target_id_flags & RTIF_BLINK) {
				if (Missiontime & 8192)
					continue;
			}
			if (radar_target_id_flags & RTIF_ENLARGE) {
				scale_factor *= 1.3f;
			}

			alpha = 1.0;
			b->rad = Radar_blip_radius_target;
			drawContact(&pos, -1, target_brackets, b->dist, alpha, scale_factor);
		}
		else {
			b->rad = Radar_blip_radius_normal;
		}

		// maybe distort blip
		if (distort) {
			blipDrawDistorted(b, &pos, alpha);
		} else {
			if (b->flags & BLIP_DRAW_DISTORTED) {
				blipDrawFlicker(b, &pos, alpha);
			} else if (b->radar_image_2d >= 0 || b->radar_color_image_2d >= 0) {
				drawContact(&pos, b->radar_image_2d, b->radar_color_image_2d, b->dist, alpha, scale_factor);
			} else {
				drawContact(&pos, -1, unknown_contact_icon, b->dist, alpha, scale_factor);
			}
		}
	}
}

void HudGaugeRadarDradis::setupViewHtl()
{
	setClip(position[0], position[1], Radar_radius[0], Radar_radius[1]);
	gr_set_proj_matrix(.625f * PI_2, i2fl(Radar_radius[0])/i2fl(Radar_radius[1]), 0.001f, 5.0f);
	gr_set_view_matrix(&Orb_eye_position, &vmd_identity_matrix);

	gr_zbuffer_set(GR_ZBUFF_NONE);
}

void HudGaugeRadarDradis::doneDrawingHtl()
{
	gr_end_view_matrix();
	gr_end_proj_matrix();
	
	//hud_save_restore_camera_data(0);

	gr_zbuffer_set(GR_ZBUFF_FULL);
}

void HudGaugeRadarDradis::drawOutlinesHtl()
{
	matrix base_tilt = vmd_identity_matrix;
	vec3d base_tilt_norm;

	if ((xy_plane == -1) || (xz_yz_plane == -1))
		return;
	
	g3_start_instance_matrix(&vmd_zero_vector, /*&Player_obj->orient*/&vmd_identity_matrix, true);
		
		// Tilt the base disc component of DRADIS-style radar 30 degrees down
		vm_angle_2_matrix(&base_tilt, PI/6, 0);
		vm_vec_rotate(&base_tilt_norm, &vmd_y_vector, &base_tilt);
		
		//gr_set_bitmap(xy_plane, GR_ALPHABLEND_FILTER, GR_BITBLT_MODE_NORMAL, 1.0f); // base
		//g3_draw_polygon(&vmd_zero_vector, &base_tilt_norm, scale, scale, TMAP_FLAG_TEXTURED | TMAP_HTL_3D_UNLIT);
		material mat_params;
		material_set_unlit(&mat_params, xy_plane, 1.0f, true, false);
		g3_render_rect_oriented(&mat_params, &vmd_zero_vector, &base_tilt_norm, scale, scale);
		
		//gr_set_bitmap(xz_yz_plane, GR_ALPHABLEND_FILTER, GR_BITBLT_MODE_NORMAL, 1.0f);
		
		//g3_draw_polygon(&vmd_zero_vector, &vmd_x_vector, scale, scale, TMAP_FLAG_TEXTURED | TMAP_HTL_3D_UNLIT); // forward facing ring
		//g3_draw_polygon(&vmd_zero_vector, &vmd_z_vector, scale, scale, TMAP_FLAG_TEXTURED | TMAP_HTL_3D_UNLIT); // side facing ring

		material_set_unlit(&mat_params, xz_yz_plane, 1.0f, true, false);
		g3_render_rect_oriented(&mat_params, &vmd_zero_vector, &vmd_x_vector, scale, scale); // forward facing ring
		g3_render_rect_oriented(&mat_params, &vmd_zero_vector, &vmd_z_vector, scale, scale); // side facing ring
	g3_done_instance(true);
}

void HudGaugeRadarDradis::drawSweeps()
{
	if (sweep_plane == -1)
		return;
	
	float modulo = fmod(f2fl(game_get_overall_frametime()) * MILLISECONDS_PER_SECOND, static_cast<float>(sweep_duration));
	float fraction = modulo / sweep_duration;
	sweep_angle = fraction * PI2; // convert to radians from 0 <-> 1
	float sweep_angle_z = sweep_angle * -0.5f;

	vec3d sweep_a;
	vec3d sweep_b;
	vec3d sweep_c;
	
	vm_rot_point_around_line(&sweep_a, &vmd_y_vector, sweep_angle, &vmd_zero_vector, &vmd_z_vector); // Sweep line: XZ
	vm_rot_point_around_line(&sweep_b, &vmd_y_vector, sweep_angle, &vmd_zero_vector, &vmd_x_vector); // Sweep line: YZ
	vm_rot_point_around_line(&sweep_c, &vmd_x_vector, sweep_angle_z, &vmd_zero_vector, &vmd_y_vector); // Sweep line: XY
	
	vm_vec_copy_scale(&sweep_normal_x, &sweep_a, 1.0f);
	vm_vec_copy_scale(&sweep_normal_y, &sweep_b, 1.0f);
	
	g3_start_instance_matrix(&vmd_zero_vector, /*&Player_obj->orient*/&vmd_identity_matrix, true);

		material mat_params;
		material_set_unlit(&mat_params, sweep_plane, 1.0f, true, false);
		g3_render_rect_oriented(&mat_params, &vmd_zero_vector, &sweep_a, scale, scale);
		g3_render_rect_oriented(&mat_params, &vmd_zero_vector, &sweep_b, scale, scale);
		g3_render_rect_oriented(&mat_params, &vmd_zero_vector, &sweep_c, scale, scale);

		vm_rot_point_around_line(&sweep_a, &vmd_y_vector, sweep_angle, &vmd_zero_vector, &vmd_z_vector); // Sweep line: XZ
		vm_rot_point_around_line(&sweep_b, &vmd_y_vector, sweep_angle, &vmd_zero_vector, &vmd_x_vector); // Sweep line: YZ
		vm_rot_point_around_line(&sweep_c, &vmd_x_vector, sweep_angle_z, &vmd_zero_vector, &vmd_y_vector); // Sweep line: YZ
		
		g3_render_rect_oriented(&mat_params, &vmd_zero_vector, &sweep_a, scale, scale); // Sweep line: XZ
		g3_render_rect_oriented(&mat_params, &vmd_zero_vector, &sweep_b, scale, scale); // Sweep line: YZ
		g3_render_rect_oriented(&mat_params, &vmd_zero_vector, &sweep_c, scale, scale);
		
	g3_done_instance(true);
}

void HudGaugeRadarDradis::drawBlipsSorted(int distort)
{
	GR_DEBUG_SCOPE("Draw Dradis blips");

	matrix base_tilt = vmd_identity_matrix;
	
	vm_angle_2_matrix(&base_tilt, -PI/6, 0);
	
	for(int is_bright = 0; is_bright < 2; is_bright++) {
		sub_y_clip = true;
		g3_start_instance_matrix(&vmd_zero_vector, /*&Player_obj->orient*/&base_tilt, true);
			drawBlips(BLIP_TYPE_BOMB, is_bright, distort);
			drawBlips(BLIP_TYPE_JUMP_NODE, is_bright, distort);
			drawBlips(BLIP_TYPE_NORMAL_SHIP, is_bright, distort);
			drawBlips(BLIP_TYPE_TAGGED_SHIP, is_bright, distort);
			drawBlips(BLIP_TYPE_WARPING_SHIP, is_bright, distort);
			drawBlips(BLIP_TYPE_NAVBUOY_CARGO, is_bright, distort);
		g3_done_instance(true);
		
		drawOutlinesHtl();

		sub_y_clip = false;
		g3_start_instance_matrix(&vmd_zero_vector, /*&Player_obj->orient*/&base_tilt, true);
			drawBlips(BLIP_TYPE_BOMB, is_bright, distort);
			drawBlips(BLIP_TYPE_JUMP_NODE, is_bright, distort);
			drawBlips(BLIP_TYPE_NORMAL_SHIP, is_bright, distort);
			drawBlips(BLIP_TYPE_TAGGED_SHIP, is_bright, distort);
			drawBlips(BLIP_TYPE_WARPING_SHIP, is_bright, distort);
			drawBlips(BLIP_TYPE_NAVBUOY_CARGO, is_bright, distort);
		g3_done_instance(true);
	}
}


void HudGaugeRadarDradis::render(float  /*frametime*/, bool config)
{
	// Not yet supported in config
	if (config) {
		return;
	}
	
	float sensors_str;
	int   ok_to_blit_radar;
	
	ok_to_blit_radar = 1;

	sensors_str = ship_get_subsystem_strength(Player_ship, SUBSYSTEM_SENSORS);

	if (ship_subsys_disrupted(Player_ship, SUBSYSTEM_SENSORS))
		sensors_str = MIN_SENSOR_STR_TO_RADAR - 1;

	// note that on lowest skill level, there is no radar effects due to sensors damage
	if ( ((Game_skill_level == 0) || (sensors_str > SENSOR_STR_RADAR_NO_EFFECTS)) && !Sensor_static_forced )
	{
		Radar_static_playing = false;
		Radar_static_next = TIMESTAMP::never();
		Radar_death_timer = TIMESTAMP::never();
		Radar_avail_prev_frame = true;
	}
	else
		if (sensors_str < MIN_SENSOR_STR_TO_RADAR)
		{
			if (Radar_avail_prev_frame)
			{
				Radar_death_timer = _timestamp(2000);
				Radar_static_next = TIMESTAMP::immediate();
			}

			Radar_avail_prev_frame = false;
		}
		else
		{
			Radar_death_timer = TIMESTAMP::never();

			if (Radar_static_next.isNever())
				Radar_static_next = TIMESTAMP::immediate();
		}

	if (timestamp_elapsed(Radar_death_timer))
		ok_to_blit_radar = 0;

	setupViewHtl();

	//WMC - This strikes me as a bit hackish
	bool g3_yourself = !g3_in_frame();
	if(g3_yourself)
		g3_start_frame(1);

	drawSweeps();

	if (timestamp_elapsed(Radar_static_next))
	{
		Radar_static_playing = !Radar_static_playing;
		Radar_static_next = _timestamp_rand(50, 750);
	}

	// if the emp effect is active, always draw the radar wackily
	if (emp_active_local())
		Radar_static_playing = true;

	if (ok_to_blit_radar)
	{
		if (Radar_static_playing)
		{
			drawBlipsSorted(1);	// passing 1 means to draw distorted

			if (!Radar_static_looping.isValid())
				Radar_static_looping = snd_play_looping(gamesnd_get_game_sound(GameSounds::STATIC));
		}
		else
		{
			drawBlipsSorted(0);

			if (Radar_static_looping.isValid()) {
				snd_stop(Radar_static_looping);
				Radar_static_looping = sound_handle::invalid();
			}
		}
	}
	else
	{
		if (Radar_static_looping.isValid()) {
			snd_stop(Radar_static_looping);
			Radar_static_looping = sound_handle::invalid();
		}
	}

	if(g3_yourself)
		g3_end_frame();

	doneDrawingHtl();
}

void HudGaugeRadarDradis::pageIn()
{
	bm_page_in_texture(xy_plane);
	bm_page_in_texture(xz_yz_plane);
	bm_page_in_texture(sweep_plane);
	bm_page_in_texture(target_brackets);
	bm_page_in_texture(unknown_contact_icon);
}

void HudGaugeRadarDradis::doLoopSnd()
{
	if (!this->m_loop_snd.isValid())
	{
		return;
	}

	if (!this->shouldDoSounds())
	{
		if (loop_sound_handle.isValid() && snd_is_playing(loop_sound_handle)) {
			snd_stop(loop_sound_handle);
			loop_sound_handle = sound_handle::invalid();
		}
	} else if (!this->loop_sound_handle.isValid() || !snd_is_playing(this->loop_sound_handle)) {
		loop_sound_handle = snd_play(gamesnd_get_game_sound(m_loop_snd), 0.0f, loop_sound_volume);
	}
}

void HudGaugeRadarDradis::doBeeps()
{
	if (!this->shouldDoSounds())
	{
		return;
	}

	if (Missiontime == 0 || Missiontime == Frametime)
	{
		// don't play sounds in first frame
		return;
	}

	if (!arrival_beep_snd.isValid() &&
		!departure_beep_snd.isValid() &&
		!stealth_arrival_snd.isValid() &&
		!stealth_departure_snd.isValid())
	{
		return;
	}

	bool departure_happened = false;
	bool stealth_departure_happened = false;

	bool arrival_happened = false;
	bool stealth_arrival_happened = false;
	
	for (int i = 0; i < MAX_SHIPS; i++)
	{
		ship * shipp = &Ships[i];

		if (shipp->objnum >= 0)
		{
			if (shipp->radar_visible_since >= 0 || shipp->radar_last_contact >= 0)
			{
				if (shipp->radar_visible_since == Missiontime)
				{
					if (shipp->radar_current_status == DISTORTED)
					{
						stealth_arrival_happened = true;
					}
					else
					{
						arrival_happened = true;
					}
				}
				else if (shipp->radar_visible_since < 0 && shipp->radar_last_contact == Missiontime)
				{
					if (shipp->radar_last_status == DISTORTED)
					{
						stealth_departure_happened = true;
					}
					else
					{
						departure_happened = true;
					}
				}
			}
		}
	}
	
	if (timestamp_elapsed(arrival_beep_next_check))
	{
		if (arrival_beep_snd.isValid() && arrival_happened)
		{
			snd_play(gamesnd_get_game_sound(arrival_beep_snd));

			arrival_beep_next_check = _timestamp(arrival_beep_delay);
		}
		else if (stealth_arrival_snd.isValid() && stealth_arrival_happened)
		{
			snd_play(gamesnd_get_game_sound(stealth_arrival_snd));

			arrival_beep_next_check = _timestamp(arrival_beep_delay);
		}

	}

	if (timestamp_elapsed(departure_beep_next_check))
	{
		if (departure_beep_snd.isValid() && departure_happened)
		{
			snd_play(gamesnd_get_game_sound(departure_beep_snd));

			departure_beep_next_check = _timestamp(departure_beep_delay);
		}
		else if (stealth_departure_snd.isValid() && stealth_departure_happened)
		{
			snd_play(gamesnd_get_game_sound(stealth_departure_snd));

			departure_beep_next_check = _timestamp(departure_beep_delay);
		}
	}
}

void HudGaugeRadarDradis::initSound(gamesnd_id loop_snd, float _loop_snd_volume, gamesnd_id arrival_snd, gamesnd_id departure_snd, gamesnd_id _stealth_arrival_snd, gamesnd_id _stealth_departure_snd, float arrival_delay, float departure_delay)
{
	this->m_loop_snd = loop_snd;
	this->loop_sound_handle = sound_handle::invalid();
	this->loop_sound_volume = _loop_snd_volume;

	this->arrival_beep_snd = arrival_snd;
	this->departure_beep_snd = departure_snd;

	this->stealth_arrival_snd = _stealth_arrival_snd;
	this->stealth_departure_snd = _stealth_departure_snd;

	this->arrival_beep_delay = fl2i(arrival_delay * 1000.0f);
	this->departure_beep_delay = fl2i(departure_delay * 1000.0f);
}

void HudGaugeRadarDradis::onFrame(float  /*frametime*/)
{
	// Play the specified radar sound
	this->doLoopSnd();

	// Play beeps for ship arrival and departure
	this->doBeeps();
}

void HudGaugeRadarDradis::initialize()
{
	HudGaugeRadar::initialize();

	this->arrival_beep_next_check = _timestamp();
	this->departure_beep_next_check = _timestamp();
}

bool HudGaugeRadarDradis::shouldDoSounds()
{
	if (hud_disabled())
		return false;

	if (Viewer_mode & (VM_EXTERNAL | VM_CHASE | VM_DEAD_VIEW | VM_OTHER_SHIP))
		return false;

	return true;
}
