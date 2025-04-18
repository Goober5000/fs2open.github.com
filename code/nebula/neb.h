/*
 * Copyright (C) Volition, Inc. 1999.  All rights reserved.
 *
 * All source code herein is the property of Volition, Inc. You may not sell 
 * or otherwise commercially exploit the source or things you created based on the 
 * source.
 *
*/



#ifndef _FS2_NEB2_EFFECT_HEADER_FILE
#define _FS2_NEB2_EFFECT_HEADER_FILE

// --------------------------------------------------------------------------------------------------------
// NEBULA DEFINES/VARS
//
#include "camera/camera.h"
#include "globalincs/globals.h"
#include "globalincs/pstypes.h"
#include "graphics/generic.h"
#include "utils/RandomRange.h"

class ship;
class object;

extern bool Nebula_sexp_used;
// fog near and far values for rendering the background nebula
extern float Neb_backg_fog_near;
extern float Neb_backg_fog_far;

// nebula rendering mode
#define NEB2_RENDER_NONE								0			// no rendering
#define NEB2_RENDER_POF									1			// background is the nice pof file -- used by FRED
#define NEB2_RENDER_HTL									2			// We are using proper fogging now 
extern int Neb2_render_mode;

// the AWACS suppresion level for the nebula
extern float Neb2_awacs;

// The visual render distance multipliers for the nebula
extern float Neb2_fog_near_mult;
extern float Neb2_fog_far_mult;

extern float Neb2_fog_visibility_trail;
extern float Neb2_fog_visibility_thruster;
extern float Neb2_fog_visibility_weapon;
extern float Neb2_fog_visibility_shield;
extern float Neb2_fog_visibility_glowpoint;
extern float Neb2_fog_visibility_beam_const;
extern float Neb2_fog_visibility_beam_scaled_factor;
extern float Neb2_fog_visibility_particle_const;
extern float Neb2_fog_visibility_particle_scaled_factor;
extern float Neb2_fog_visibility_shockwave;
extern float Neb2_fog_visibility_fireball_const;
extern float Neb2_fog_visibility_fireball_scaled_factor;

extern std::unique_ptr<ubyte[]> Neb2_poof_flags;

// pof texture filenames
extern SCP_vector<SCP_string> Neb2_bitmap_filenames;

// texture to use for this level
extern char Neb2_texture_name[MAX_FILENAME_LEN];

typedef struct poof_info {
	char name[NAME_LENGTH];
	char bitmap_filename[MAX_FILENAME_LEN];
	generic_anim bitmap;
	::util::ParsedRandomFloatRange scale;
	float density;						 // poofs per square meter; can get *really* small but vague approximation is ok at those levels
	::util::ParsedRandomFloatRange rotation;
	float view_dist;
	::util::ParsedRandomFloatRange alpha;
	vec3d alignment;

	// These values are dynamic, unlike the above and can change during a mission.
	// They are used for fading poof types in and out via sexp
	TIMESTAMP fade_start;			// when the fade began
	int fade_duration;		// the length of the fade in milliseconds
	bool fade_in;			// true if fading the poof type in, false if fading out
	float fade_multiplier;	// the current multiplier for a poof's alpha transparency used to render the poofs of this type

	poof_info() : 
		scale(::util::UniformFloatRange(175.f)),
		rotation(::util::UniformFloatRange(-3.7f, 3.7f)),
		alpha(::util::UniformFloatRange(0.8f))
	{
		bitmap_filename[0] = '\0';
		generic_anim_init(&bitmap);
		density = 1 / (110.f * 110.f * 110.f);
		view_dist = 250.f;
		fade_start = TIMESTAMP::invalid();
		fade_duration = -1;
		fade_in = true;
		fade_multiplier = -1.0f;
		alignment = vmd_zero_vector;
	}
} poof_info;

extern SCP_vector<poof_info> Poof_info;

// the color of the fog/background
extern ubyte Neb2_fog_color[3];

// nebula poofs
typedef struct poof {
	vec3d	pt;				// point in space
	size_t		poof_info_index;
	float		radius;
	vec3d		up_vec;			// to keep track of the poofs rotation
								// must be the full vector instead of an angle to prevent parallel transport when looking around
	float		rot_speed;		// rotation speed, deg/sec
	float		flash;			// lightning flash
	float		alpha;			// base amount of alpha to start with
	float		anim_time;		// how far along the animation is
} poof;

extern SCP_vector<poof> Neb2_poofs;

// nebula detail level
typedef struct neb2_detail {
	float max_alpha_glide;		// max alpha for this detail level in Glide
	float max_alpha_d3d;		// max alpha for this detail level in D3d
	float break_alpha;			// break alpha (below which, poofs don't draw). this affects the speed and visual quality a lot
	float cube_dim;				// total dimension of player poof cube
	float cube_inner;			// inner radius of the player poof cube
	float cube_outer;			// outer radius of the player pood cube
	float prad;					// radius of the poofs
	float wj, hj, dj;			// width, height, depth jittering. best left at 1.0
} neb2_detail;


// --------------------------------------------------------------------------------------------------------
// NEBULA FUNCTIONS
//

// neb2 stuff (specific nebula densities) -----------------------------------

// initialize neb2 stuff at game startup
void neb2_init();

// set poof bits using a list of poof names
void neb2_set_poof_bits(const SCP_vector<SCP_string>& list);

//init neb stuff  - WMC
void neb2_level_init();

void neb2_pre_level_init();

// initialize nebula stuff - call from game_post_level_init(), so the mission has been loaded
void neb2_post_level_init(bool fog_color_override);

// helper function, used in above and in FRED
void neb2_generate_fog_color(const char *fog_color_palette, ubyte fog_color[]);

// shutdown nebula stuff
void neb2_level_close();

// call before beginning all rendering
void neb2_render_setup(camid cid);

// turns a poof on or off
void neb2_toggle_poof(int poof_idx, bool enabling);

// fades poofs
void neb2_fade_poofs(int poof_idx, int time, bool type);

// render the player nebula
void neb2_render_poofs();

// get near and far fog values based upon object type and rendering mode
void neb2_get_fog_values(float *fnear, float *ffar, object *obj = NULL);

// get adjusted near and far fog values (allows mission-specific fog adjustments)
void neb2_get_adjusted_fog_values(float *fnear, float *ffar, float *fdensity = nullptr, object *obj = nullptr);

// given a position, returns 0 - 1 the fog visibility of that position, 0 = completely obscured
// distance_mult will multiply the result, use for things that can be obscured but can 'shine through' the nebula more than normal
// Don't use unless you know what you're doing. Use nebula_handle_alpha instead.
float neb2_get_fog_visibility (const vec3d* pos, float distance_mult);

// should we not render this object because its obscured by the nebula?
int neb2_skip_render(object *objp, float z_depth);

// extend LOD 
float neb2_get_lod_scale(int objnum);

// fogging stuff --------------------------------------------------

void neb2_get_fog_color(ubyte *r, ubyte *g, ubyte *b);

// given a position, multiplies alpha by 0 - 1 based on the visibility of Fullneb2 / Volumetric Fogging if present in mission
// distance_mult will multiply the result, use for things that can be obscured but can 'shine through' the nebula more than normal
// returns true if there is a nebula present that could have influence
bool nebula_handle_alpha(float& alpha, const vec3d* pos, float distance_mult);

#endif
