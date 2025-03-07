/*
 * Copyright (C) Volition, Inc. 1999.  All rights reserved.
 *
 * All source code herein is the property of Volition, Inc. You may not sell
 * or otherwise commercially exploit the source or things you created based on the
 * source.
 *
 */

#include "gamesnd/eventmusic.h"

#include "cmdline/cmdline.h"
#include "globalincs/linklist.h"
#include "iff_defs/iff_defs.h"
#include "io/timer.h"
#include "localization/localize.h"
#include "mission/missiongoals.h"
#include "mission/missionparse.h"
#include "object/object.h"
#include "options/Option.h"
#include "parse/parselo.h"
#include "ship/ship.h"
#include "sound/audiostr.h"
#include "sound/sound.h"

#ifdef _MSC_VER
#pragma optimize("", off)
#endif

#define HULL_VALUE_TO_PLAY_INTENSE_BATTLE_MUSIC 0.75f

////////////////////////////
// Globals
////////////////////////////
int Event_Music_battle_started = 0;
float Default_music_volume = 0.5f;							// range is 0->1
float Master_event_music_volume = Default_music_volume;

static bool music_volume_change_listener(float new_val, bool /*initial*/)
{
	Assertion(new_val >= 0.0f && new_val <= 1.0f, "Invalid value %f supplied by options system!", new_val);

	event_music_set_volume(new_val);

	return true;
}

static void parse_music_volume_func()
{
	float volume;
	stuff_float(&volume);
	CLAMP(volume, 0.0f, 1.0f);
	Default_music_volume = volume;
}

static auto MusicVolumeOption __UNUSED = options::OptionBuilder<float>("Audio.Music",
                     std::pair<const char*, int>{"Music", 1371},
                     std::pair<const char*, int>{"Volume used for playing music", 1760})
                     .category(std::make_pair("Audio", 1826))
                     .default_func([]() { return Default_music_volume;})
                     .range(0.0f, 1.0f)
                     .change_listener(music_volume_change_listener)
                     .importance(1)
                     .flags({options::OptionFlags::RetailBuiltinOption})
                     .parser(parse_music_volume_func)
                     .finish();

typedef struct tagSNDPATTERN {
	int default_next_pattern;	// Needed so the next_pattern member can be reset
	int next_pattern;				// Next pattern to play at loop time (can be same pattern)
	int default_loop_for;		// Needed so the loop_for variable can be reset
	int loop_for;					// Number of times to loop before switching to next pattern
	int handle;						// handle to open audio stream
	int force_pattern;			// flag to indicate that we want to not continue loop, but go to next_pattern
	int can_force;					// whether this pattern can be interrupted
	int samples_per_measure;		// number of bytes in a measure
	float num_measures;				// number of measures in wave file
}	SNDPATTERN;

SNDPATTERN	Patterns[MAX_PATTERNS];	// holds data on sections of a SoundTrack

// Holds filenames for the different sections of a soundtrack
SCP_vector<SOUNDTRACK_INFO> Soundtracks;

int Current_soundtrack_num;	// Active soundtrack for the current mission.. index into Soundtracks[]

int Current_pattern = -1;		// currently playing part of track
TIMESTAMP Pattern_timer_id = TIMESTAMP::invalid();

// File Globals
static int Num_enemy_arrivals;
static int Num_friendly_arrivals;

constexpr int ARRIVAL_INTERVAL_TIMESTAMP =			5 * MILLISECONDS_PER_SECOND;
constexpr int BATTLE_CHECK_INTERVAL =				15 * MILLISECONDS_PER_SECOND;

static TIMESTAMP Battle_over_timestamp;
static TIMESTAMP Mission_over_timestamp;
static bool Victory2_music_played;
static TIMESTAMP Next_arrival_timestamp;
static TIMESTAMP Check_for_battle_music;	// this timestamp is actually never set

typedef struct pattern_info
{
	const char *pattern_name;
	const char *pattern_desc;
	int pattern_can_force;
	int pattern_loop_for;
	int pattern_default_next_fs1;
	int pattern_default_next_fs2;
} pattern_info;

pattern_info Pattern_info[] =
{
	{"NRML_1",	"Normal 1",			TRUE,	1,	SONG_NRML_2,	SONG_NRML_1 },
	{"NRML_2",	"Normal 2",			TRUE,	1,	SONG_NRML_1,	SONG_NRML_1	},
	{"NRML_3",	"Normal 3",			TRUE,	1,	SONG_NRML_1,	SONG_NRML_1	},
	{"AARV_1",	"Ally arrival 1",	FALSE,	1,	SONG_NRML_2,	SONG_NRML_1	},
	{"AARV_2",	"Ally arrival 2",	FALSE,	1,	SONG_BTTL_2,	SONG_BTTL_2	},
	{"EARV_1",	"Enemy arrival 1",	FALSE,	1,	SONG_BTTL_1,	SONG_BTTL_1	},
	{"EARV_2",	"Enemy arrival 2",	FALSE,	1,	SONG_BTTL_2,	SONG_BTTL_3	},
	{"BTTL_1",	"Battle 1",			TRUE,	1,	SONG_BTTL_2,	SONG_BTTL_2	},
	{"BTTL_2",	"Battle 2",			TRUE,	1,	SONG_BTTL_3,	SONG_BTTL_3	},
	{"BTTL_3",	"Battle 3",			TRUE,	1,	SONG_BTTL_1,	SONG_BTTL_1	},
	{"FAIL_1",	"Failure 1",		FALSE,	1,	SONG_NRML_3,	SONG_NRML_1	},
	{"VICT_1",	"Victory 1",		FALSE,	1,	SONG_NRML_3,	SONG_NRML_1	},
	{"VICT_2",	"Victory 2",		TRUE,	1,	SONG_NRML_3,	SONG_NRML_1	},
	{"DEAD_1",	"Dead 1",			TRUE,	1,	-1,				-1			},
};

int Num_pattern_types = sizeof(Pattern_info) / sizeof(pattern_info);

//Because of how inflexible music.tbl is, to make it flexible, we must keep the stuff
//in the old order. So to make it reasonable, we use this.
//This is a list of the old pattern indexes. If you add a song you should
//add it in the right spot in here, with a value of largest+1.
int New_pattern_order[] =
{
	0,	//normal 1
	12, //normal 2
	13, //normal 3
	1,  //friendly arrival 1
	6,  //friendly arrival 2
	2,	//enemy arrival 1
	7,	//enemy arrival 2
	3,	//battle 1
	4,	//battle 2
	5,	//battle 3
	10,	//goal failed 1
	8,	//victory 1
	9,	//victory 2
	11,	//death
};


/*
char* Pattern_names[MAX_PATTERNS] =
{
//XSTR:OFF
	"NRML_1",	// Normal Song 1
	"AARV_1",	// Allied Arrival 1
	"EARV_1",	// Enemy Arrival 1
	"BTTL_1",	// Battle Song 1
	"BTTL_2",	// Battle Song 2
	"BTTL_3",	// Battle Song 3
	"AARV_2",	// Allied Arrival 2
	"EARV_2",	// Enemy Arrival 2
	"VICT_1",	// Victory Song 1
	"VICT_2",	// Victory Song 2
	"FAIL_1",	// Goal Failed 1
	"DEAD_1",	// Death Song 1
	"NRML_2",	// Normal Song 2
	"NRML_3"	// Normal Song 3
//XSTR:ON
};

char* Pattern_description[MAX_PATTERNS] =
{
//XSTR:OFF
	"normal 1",
	"friendly arrival 1",
	"enemy arrival 2",
	"battle 1",
	"battle 2",
	"battle 3",
	"friendly arrival 2",
	"enemey arrival 2",
	"victory 1",
	"victory 2",
	"goal failed 1",
	"death",
	"normal 2",
	"normal 3"
//XSTR:ON
};

int Pattern_loop_for[MAX_PATTERNS] =
{
	1,	// Normal Song 1
	1,	// Allied Arrival 1
	1,	// Enemy Arrival 1
	1,	// Battle Song 1
	1,	// Battle Song 2
	1,	// Battle Song 3
	1,	// Allied Arrival 2
	1,	// Enemy Arrival 2
	1,	// Victory Song 1
	1,	// Victory Song 2
	1,	// Goal Failed 1
	1, 	// Death Song 1
	1,	// Normal Song 2
	1	// Normal Song 3
};

int Pattern_default_next[MAX_PATTERNS] =
{
	SONG_NRML_1,	// NRML_1 progresses to NRML_1 by default
	SONG_NRML_1,	// AARV_1 progresses to NRML_1 by default
	SONG_BTTL_1,	// EARV_1 progresses to BTTL_1 by default
	SONG_BTTL_2,	// BTTL_1 progresses to BTTL_2 by default
	SONG_BTTL_3,	// BTTL_2 progresses to BTTL_3 by default
	SONG_BTTL_1,	// BTTL_3 progresses to BTTL_1 by default
	SONG_BTTL_2,	// AARV_2 progresses to BTTL_2 by default
	SONG_BTTL_3,	// EARV_2 progresses to BTTL_3 by default
	SONG_NRML_1,	// VICT_1 progresses to NRML_1 by default
	SONG_NRML_1,	// VICT_2 progresses to NRML_1 by default
	SONG_NRML_1,	//	FAIL_1 progresses to NRML_1 by default
	-1,					// no music plays after dead
	SONG_NRML_1,	// NRML_2 progresses to NRML_1 by default (but we intercept this)
	SONG_NRML_1		// NRML_3 progresses to NRML_1 by default (but we intercept this)
};


// Certain patterns can be interrupted (such as the long-playing NRML and BTTL tracks).  
// Other shorter tracks (such as arrivals) play their entire duration.
int Pattern_can_force[MAX_PATTERNS] =
{
	TRUE,		// NRML_1 
	FALSE,	// AARV_1 
	FALSE,	// EARV_1 
	TRUE,		// BTTL_1
	TRUE,		// BTTL_2 
	TRUE,		// BTTL_3
	FALSE,	// AARV_2 
	FALSE,	// EARV_2 
	FALSE,	// VICT_1 
	TRUE,		// VICT_2 
	FALSE,	// FAIL_1
	TRUE,		// DEAD_1
	TRUE,		// NRML_2
	TRUE		// NRML_3
};
*/

int Event_music_enabled = TRUE;
static int Event_music_inited = FALSE;
static int Event_music_level_inited = FALSE;
static int Event_music_begun = FALSE;

// forward function declarations
int hostile_ships_present();
extern int hud_target_invalid_awacs(object *objp);

// Holds file names of spooled music that is played at menus, briefings, credits etc.
// Indexed into by a #define enumeration of the different kinds of spooled music
SCP_vector<menu_music> Spooled_music;

// Array that holds indicies into Spooled_music[], these specify which music is played in briefing/debriefing
int Mission_music[NUM_SCORES];


bool event_music_pattern_is_skippable(const SOUNDTRACK_PATTERN_INFO &pattern)
{
	// check for blank or "none"
	if (pattern.fname[0] == '\0' || !strnicmp(pattern.fname, NOX("none.wav"), 4))
		return true;

	// no content means this pattern isn't intended to play
	if (pattern.num_measures == 0 || pattern.samples_per_measure == 0)
		return true;

	return false;
}

// -------------------------------------------------------------------------------------------------
// event_music_init() 
//
// Called once at game start-up to parse music.tbl and set some state variables
//
void event_music_init()
{
	if(!Fred_running)
	{
		if ( snd_is_inited() == FALSE ) {
			Event_music_enabled = FALSE;
			return;
		}

		if ( Cmdline_freespace_no_music ) {
			return;
		}

		if ( Event_music_inited == TRUE )
			return;
	}

	//MUST be called before parsing stuffzors.
	Soundtracks.clear();
	Spooled_music.clear();
	event_music_reset_choices();

	//Do teh parsing
	event_music_parse_musictbl("music.tbl");

	// look for any modular tables
	parse_modular_table(NOX("*-mus.tbm"), event_music_parse_musictbl);


	// now that we've parsed everything, check the validity
	for (auto &soundtrack : Soundtracks)
	{
		bool all_patterns_valid = true;

		// Goober5000 - set the valid flag according to whether we can load all our patterns
		// (since someone may be running an enhanced music.tbl without warble_fs1 installed)
		for (int i = 0; i < soundtrack.num_patterns; i++)
		{
			auto filename = soundtrack.patterns[i].fname;

			// some patterns aren't played for various legitimate reasons
			if (event_music_pattern_is_skippable(soundtrack.patterns[i]))
				continue;

			// check for negative numbers (zero is allowed)
			if (soundtrack.patterns[i].num_measures < 0 || soundtrack.patterns[i].samples_per_measure < 0)
			{
				Warning(LOCATION, "Soundtrack file %s has a negative 'num_measures' or 'samples_per_measure'; this is not allowed", soundtrack.patterns[i].fname);
				all_patterns_valid = false;
				break;
			}

			// check for file with exact extension
			// (since event music specifies samples and measures, the audio format is important; so we don't want to assume any format will work)
			if (!cf_exists_full(filename, CF_TYPE_MUSIC))
			{
				static SCP_unordered_map<SCP_string, SCP_string, SCP_string_lcase_hash, SCP_string_lcase_equal_to> filename_substitutions;
				CFileLocationExt res;

				// see if we ran into this file already (case-insensitive)
				auto pair = filename_substitutions.find(filename);
				if (pair != filename_substitutions.end())
				{
					// don't warn, since we warned for this file name when we added it to the map
					strcpy_s(soundtrack.patterns[i].fname, pair->second.c_str());
				}
				// see if the file exists with a different extension
				else if (res = cf_find_file_location_ext(filename, NUM_AUDIO_EXT, audio_ext_list, CF_TYPE_MUSIC), res.found)
				{
					Warning(LOCATION, "Soundtrack file %s was not found with its specified extension, but another file %s exists in the modpack.  Please update the extension and adjust audio specifications if necessary.", filename, res.name_ext.c_str());
					filename_substitutions.emplace(filename, res.name_ext);
					strcpy_s(soundtrack.patterns[i].fname, res.name_ext.c_str());
				}
				// file does not exist
				else
				{
					Warning(LOCATION, "Soundtrack file %s was not found.  The soundtrack '%s' will not be used.", filename, soundtrack.name);
					all_patterns_valid = false;
					break;
				}
			}
		}

		if (all_patterns_valid)
			soundtrack.flags |= EMF_VALID;
	}
	
	Event_music_inited = TRUE;
	Event_music_begun = FALSE;
}

// -------------------------------------------------------------------------------------------------
// event_music_close() 
//
// Called once at game end
//
void event_music_close()
{
	if ( Event_music_inited == FALSE )
		return;

	Event_music_inited = FALSE;
}

// Goober5000
int event_music_cycle_pattern()
{
	// default
	int new_pattern = Patterns[Current_pattern].next_pattern;

	// Goober5000 - not for FS1
	if (!(Soundtracks[Current_soundtrack_num].flags & EMF_CYCLE_FS1))
	{
		if (Current_pattern == SONG_BTTL_3 && new_pattern == SONG_BTTL_1)
		{
			// AL 06-24-99: maybe switch to battle 2 if hull is less than 70%
			if (Player_obj != NULL && Player_ship != NULL)
			{
				Assert(Player_ship->ship_max_hull_strength != 0.0f);

				float integrity = Player_obj->hull_strength / Player_ship->ship_max_hull_strength;
				if (integrity < HULL_VALUE_TO_PLAY_INTENSE_BATTLE_MUSIC)
					new_pattern = SONG_BTTL_2;
			}
		}
	}

	// make sure we have a valid BTTL track to switch to
	if ((new_pattern == SONG_BTTL_2) && (Patterns[SONG_BTTL_2].handle < 0))
	{
		new_pattern = SONG_BTTL_1;
	}
	else if ((new_pattern == SONG_BTTL_3) && (Patterns[SONG_BTTL_3].handle < 0))
	{
		if ((Current_pattern == SONG_BTTL_2) || (Patterns[SONG_BTTL_2].handle < 0))
			new_pattern = SONG_BTTL_1;
		else
			new_pattern = SONG_BTTL_2;
	}

	// make sure we have a valid NRML track to switch to
	if ((new_pattern == SONG_NRML_2) && (Patterns[SONG_NRML_2].handle < 0))
	{
		new_pattern = SONG_NRML_1;
	}
	else if ((new_pattern == SONG_NRML_3) && (Patterns[SONG_NRML_3].handle < 0))
	{
		new_pattern = SONG_NRML_1;
	}

	return new_pattern;
}

// -------------------------------------------------------------------------------------------------
// event_music_force_switch() 
//
// Performs a switch between patterns.  Sets the cutoff limit for the pattern that is being
// switch to.
//
void event_music_force_switch()
{
	if ( Event_music_enabled == FALSE )
		return;

	if ( Event_music_level_inited == FALSE )
		return;

	Patterns[Current_pattern].loop_for = Patterns[Current_pattern].default_loop_for;

	int new_pattern = event_music_cycle_pattern();

	if ( new_pattern == -1 ) {
		return;
	}

	if ( Patterns[new_pattern].num_measures == 0 )
		return;	// invalid pattern

	Assert(new_pattern >= 0 && new_pattern < MAX_PATTERNS);
	audiostream_play(Patterns[new_pattern].handle, (Master_event_music_volume * aav_music_volume), 0);	// no looping
	audiostream_set_sample_cutoff(Patterns[new_pattern].handle, fl2i(Patterns[new_pattern].num_measures * Patterns[new_pattern].samples_per_measure) );
	Patterns[Current_pattern].next_pattern = Patterns[Current_pattern].default_next_pattern;
	Patterns[Current_pattern].force_pattern = FALSE;
	nprintf(("EVENTMUSIC", "EVENTMUSIC => switching to %s from %s\n", Pattern_info[new_pattern].pattern_name, Pattern_info[Current_pattern].pattern_name));

	// actually switch the pattern
	Current_pattern = new_pattern;
}

// -------------------------------------------------------------------------------------------------
// event_music_do_frame() 
//
// Called once per game frame, to check for transitions of patterns (and to start the music off at
// the beginning).
//
void event_music_do_frame()
{
	if ( Event_music_level_inited == FALSE ) {
		return;
	}

	if ( Event_music_enabled == FALSE ) {
		return;
	}

	// start off the music delayed
	if ( timestamp_elapsed(Pattern_timer_id) ) {
		Pattern_timer_id = TIMESTAMP::never();
		Event_music_begun = TRUE;
		if ( Current_pattern != -1  && Patterns[Current_pattern].handle >= 0) {
			//WMC - removed in favor of if
			//Assert(Patterns[Current_pattern].handle >= 0 );
			audiostream_play(Patterns[Current_pattern].handle, (Master_event_music_volume * aav_music_volume), 0);	// no looping
			audiostream_set_sample_cutoff(Patterns[Current_pattern].handle, fl2i(Patterns[Current_pattern].num_measures * Patterns[Current_pattern].samples_per_measure) );
		}
	}

	if ( Event_music_begun == FALSE ) {
		return;
	}

	if ( Current_pattern != -1 ) {
		SNDPATTERN *pat;
		pat = &Patterns[Current_pattern];

		// First case: switching to a different track since first track is almost at end
		if ( audiostream_done_reading(pat->handle) ) {
			event_music_force_switch();	
		}
		// Second case: looping back to start of same track since at the end
		else if ( !audiostream_is_playing(pat->handle) && !audiostream_is_paused(pat->handle) ) {
			audiostream_stop(pat->handle);	// stop current and rewind
			pat->loop_for--;
			if ( pat->loop_for > 0 ) {
				audiostream_play(pat->handle, (Master_event_music_volume * aav_music_volume), 0);	// no looping
				audiostream_set_sample_cutoff(Patterns[Current_pattern].handle, fl2i(Patterns[Current_pattern].num_measures * Patterns[Current_pattern].samples_per_measure) );
			}
			else {
				event_music_force_switch();
			}
		}
		// Third case: switching to a different track by interruption
		else if ( (pat->force_pattern == TRUE && pat->can_force == TRUE) ) {
			int samples_streamed = audiostream_get_samples_committed(pat->handle);
			int measures_played = samples_streamed / pat->samples_per_measure;
			if ( measures_played < pat->num_measures ) {
				audiostream_set_sample_cutoff(pat->handle, pat->samples_per_measure * (measures_played+1) );
				pat->force_pattern = FALSE;
				pat->loop_for = 0;
			}
		}

		if (Event_Music_battle_started == 0) {
			if (Current_pattern == SONG_NRML_1 || Current_pattern == SONG_NRML_2 || Current_pattern == SONG_NRML_3 ) {
				if (timestamp_elapsed(Check_for_battle_music)) {
					Check_for_battle_music = _timestamp(1 * MILLISECONDS_PER_SECOND);
					if (hostile_ships_present() == TRUE) {
						Patterns[Current_pattern].next_pattern = SONG_BTTL_1;
						Patterns[Current_pattern].force_pattern = TRUE;
					}
				}
			} else if (Current_pattern == SONG_BTTL_1 || Current_pattern == SONG_BTTL_2 || Current_pattern == SONG_BTTL_3) {
				if (timestamp_elapsed(Battle_over_timestamp)) {
					Battle_over_timestamp = _timestamp(BATTLE_CHECK_INTERVAL);
					if (hostile_ships_present() == FALSE) {
						Patterns[Current_pattern].force_pattern = TRUE;

						if (Soundtracks[Current_soundtrack_num].flags & EMF_CYCLE_FS1) {
							Patterns[Current_pattern].next_pattern = SONG_NRML_3;
						} else {
							Patterns[Current_pattern].next_pattern = SONG_NRML_1;
						}
					}
				}
			}
		} else {
			// We want to go back to NRML track music if all the hostiles have been 
			// destroyed, and we are still playing the battle music
			if (Current_pattern == SONG_BTTL_1 || Current_pattern == SONG_BTTL_2 || Current_pattern == SONG_BTTL_3) {
				if (timestamp_elapsed(Battle_over_timestamp)) {
					if (hostile_ships_present() == FALSE) {
						if (Patterns[Current_pattern].next_pattern != SONG_VICT_2) {
							// Goober5000
							if (Soundtracks[Current_soundtrack_num].flags & EMF_CYCLE_FS1) {
								Patterns[Current_pattern].next_pattern = SONG_NRML_3;
							} else {
								Patterns[Current_pattern].next_pattern = SONG_NRML_1;
							}

							Patterns[Current_pattern].force_pattern = TRUE;
							Event_Music_battle_started = 0;
						}
					}
				}
			}
		}

		if ( !Victory2_music_played ) {
			if ( timestamp_elapsed(Mission_over_timestamp) ) {
				Mission_over_timestamp = _timestamp(BATTLE_CHECK_INTERVAL);
				if ( mission_goals_met() && (!hostile_ships_present()) ) {
					Patterns[Current_pattern].next_pattern = SONG_VICT_2;
					Patterns[Current_pattern].force_pattern = TRUE;
					Victory2_music_played = true;
				}
			}
		}
	}
}

// -------------------------------------------------------------------------------------------------
// event_music_level_start() 
//
// Called at the start of a mission (level).  Sets up the pattern data, and kicks off the
// first track to play().
//
// input:	force_soundtrack	=>		OPTIONAL parameter (default value -1)
//												forces the soundtrack to ignore the music.tbl assignment
//
void event_music_level_start(int force_soundtrack)
{
	int					i;
	SOUNDTRACK_INFO	*strack;

	if (Cmdline_freespace_no_music)
		return;

	if (!audiostream_is_inited())
		return;

	if (Event_music_level_inited == TRUE)
		return;

	Current_pattern = -1;

	if (Event_music_inited == FALSE)
		return;
	
	if (force_soundtrack != -1)
		Current_soundtrack_num = force_soundtrack;

	// Check that soundtrack index is in range, except that < 0 means no soundtrack
	if (Current_soundtrack_num < 0)
	{
		Pattern_timer_id = TIMESTAMP::never();	// update the timestamp as if a soundtrack did start
		return;
	}
	Assert(Current_soundtrack_num < (int)Soundtracks.size());
	if (Current_soundtrack_num >= (int)Soundtracks.size())
		return;

	strack = &Soundtracks[Current_soundtrack_num];

	// open the pattern files, and get ready to play them
	// Goober5000 - changed from strack->num_patterns to MAX_PATTERNS so that *all*
	// patterns would be checked; the behavior is the same because they were previously
	// set to none.wav in event_music_parse_musictbl, but this change was needed because
	// the NRML_2 and NRML_3 at the end of the pattern array kept getting spurious music
	// tracks because their patterns weren't -1
	for (i = 0; i < MAX_PATTERNS; i++)
	{
		if (event_music_pattern_is_skippable(strack->patterns[i]))
		{
			Patterns[i].handle = -1;
			continue;
		}

		Patterns[i].handle = audiostream_open( strack->patterns[i].fname, ASF_EVENTMUSIC );

		if (Patterns[i].handle >= 0)
		{
			Event_music_level_inited = TRUE;
			Event_music_enabled = TRUE;
		}

		pattern_info *pip = &Pattern_info[i];

		// Goober5000
		if (strack->flags & EMF_CYCLE_FS1)
			Patterns[i].default_next_pattern = pip->pattern_default_next_fs1;
		else
			Patterns[i].default_next_pattern = pip->pattern_default_next_fs2;

		Patterns[i].next_pattern = Patterns[i].default_next_pattern;
		Patterns[i].default_loop_for = pip->pattern_loop_for;
		Patterns[i].loop_for = Patterns[i].default_loop_for;
		Patterns[i].force_pattern = FALSE;
		Patterns[i].can_force = pip->pattern_can_force;
		Patterns[i].samples_per_measure = strack->patterns[i].samples_per_measure;
		Patterns[i].num_measures = strack->patterns[i].num_measures;
	}

	Num_enemy_arrivals = 0;
	Num_friendly_arrivals = 0;
	Battle_over_timestamp = _timestamp(BATTLE_CHECK_INTERVAL);
	Mission_over_timestamp = _timestamp(BATTLE_CHECK_INTERVAL);
	Next_arrival_timestamp = TIMESTAMP::immediate();
	Victory2_music_played = false;
	Check_for_battle_music = TIMESTAMP::invalid();		// this should probably be immediate; we might introduce a flag later

	if (Event_music_level_inited)
	{
		if (force_soundtrack >= 0)
			event_music_first_pattern();
	}
}

// -------------------------------------------------------------------------------------------------
// event_music_first_pattern() 
//
// Picks the first pattern to play, based on whether the battle has started.
void event_music_first_pattern()
{
	if ( Event_music_inited == FALSE ) {
		return;
	}

	if ( Event_music_enabled == FALSE ) {
		return;
	}

	if ( Event_music_level_inited == FALSE ) {
		event_music_level_start();
	}

	if ( Event_music_level_inited == FALSE ) { //-V581
		return;
	}

	if ( Event_music_begun == TRUE ) {
		return;
	}

	if ( Current_pattern != -1 ) {
		if (  audiostream_is_playing(Patterns[Current_pattern].handle) )
			audiostream_stop( Patterns[Current_pattern].handle );
	}

	// if we really are initializing the level
	if (Missiontime == 0) {
		Pattern_timer_id = TIMESTAMP::invalid();	// don't let this start quite yet; see event_music_set_start_delay()
	}
	// don't step on a timestamp in progress; only set it if it has elapsed at least once already
	else if (Pattern_timer_id.isNever()) {
		Pattern_timer_id = TIMESTAMP::immediate();	// start immediately
	}
	
	Event_music_begun = FALSE;
	if ( Event_Music_battle_started == TRUE ) {
		Current_pattern = SONG_BTTL_1;
	}
	else {
		Current_pattern = SONG_NRML_1;
	}
}

// Event music was originally hard-coded to start at 2 seconds, regardless of the timestamp.  Since the default player entry delay is 1 second,
// this yields a relative start time of 1 second.  Mission start will now use relative rather than absolute delay.
void event_music_set_start_delay()
{
	if (Event_music_inited == FALSE) {
		return;
	}

	if (Event_music_enabled == FALSE) {
		return;
	}

	if (Event_music_level_inited == FALSE) {
		return;
	}

	Pattern_timer_id = _timestamp(1 * MILLISECONDS_PER_SECOND);
}

// -------------------------------------------------------------------------------------------------
// event_music_level_close() 
//
// Called at the end of each mission (level).  Stops any playing patterns by fading them out.
//
void event_music_level_close()
{
	int i;

	if ( Event_music_level_inited == FALSE )
		return;

	if ( Current_soundtrack_num >= 0 && Current_soundtrack_num < (int)Soundtracks.size() ) {
		auto strack = &Soundtracks[Current_soundtrack_num];

		// close the pattern files
		for ( i = 0; i < strack->num_patterns; i++ ) {
			if ( i == Current_pattern ) {
				if (  audiostream_is_playing(Patterns[Current_pattern].handle) )
					audiostream_close_file( Patterns[i].handle );
				else
					audiostream_close_file( Patterns[i].handle, 0 );
			}
			else
				audiostream_close_file( Patterns[i].handle, 0 );
		}
	} else {
		// close em all down then
		audiostream_close_all(0);
	}

	Current_soundtrack_num = -1;
	Current_pattern = -1;
	Event_music_level_inited = FALSE;
	Event_Music_battle_started = FALSE;
	Event_music_enabled = 0;
	Event_music_begun = FALSE;
}

// -------------------------------------------------------------------------------------------------
// event_music_battle_start() 
//
// Start the battle music.  If the music is already started before, do nothing.
//
int event_music_battle_start()
{	
	if ( !hostile_ships_present() ) {
		return 0;
	}

	//	No special tracks in training.
	if ( The_mission.game_type & MISSION_TYPE_TRAINING )
		return -1;

	// Check to see if we've already started off the battle song
	if ( Event_Music_battle_started == 1 ) {
		return 0;
	}

	if ( Event_music_enabled == FALSE )
		return -1;

	if ( Event_music_level_inited == FALSE )
		return -1;

	if ( Current_pattern == SONG_BTTL_1 )
		return 0;	// already playing

	if ( Current_pattern == SONG_DEAD_1 )
		return 0;	// death is the last song to play

	if ( Current_pattern != -1 ) {
		Patterns[Current_pattern].next_pattern = SONG_BTTL_1;
		Patterns[Current_pattern].force_pattern = TRUE;
	}

	Event_Music_battle_started = 1;	// keep track of this state though, need on restore
	Battle_over_timestamp = _timestamp(BATTLE_CHECK_INTERVAL);

	return 0;
}

// -------------------------------------------------------------------------------------------------
// event_music_enemy_arrival() 
//
// An enemy has arrived, play an enemy arrival pattern.
//
int event_music_enemy_arrival()
{
	if ( Event_music_enabled == FALSE ) {
		return -1;
	}

	if ( Event_music_level_inited == FALSE ) {
		return -1;
	}

	int next_pattern;
	if ( Event_Music_battle_started == TRUE ) {
		next_pattern = SONG_EARV_2;
	}
	else {
		next_pattern = SONG_EARV_1;
	}
    
	if ( Current_pattern < 0 )
		return 0;

	if ( Current_pattern == next_pattern )
		return 0;	// already playing

	if ( Current_pattern == SONG_DEAD_1 )
		return 0;	// death is the last song to play

	if ( (Current_pattern == SONG_VICT_1) || (Current_pattern == SONG_VICT_2) )
		return 0;

	if ( Patterns[Current_pattern].next_pattern != Patterns[Current_pattern].default_next_pattern )
		return 0;	// don't squash a pending pattern

	Num_enemy_arrivals++;


	// Goober5000 - cycle according to flag
	if (Soundtracks[Current_soundtrack_num].flags & EMF_CYCLE_FS1)
	{
		// AL 11-03-97:
		// Alternate between BTTL_2 and BTTL_3 following enemy arrivals
		if ( Num_enemy_arrivals & 1 ) {
			Patterns[next_pattern].next_pattern = SONG_BTTL_2;
		} else {
			if ( Patterns[SONG_BTTL_3].handle != -1 ) {
				Patterns[next_pattern].next_pattern = SONG_BTTL_3;
			} else {
				Patterns[next_pattern].next_pattern = SONG_BTTL_2;
			}
		}
	}
	else
	{
		// AL 7-25-99: If hull is less than 70% then switch to battle 2 or 3, otherwise switch to 1 or 2
		bool play_intense_battle_music = false;
		if (Player_obj != NULL && Player_ship != NULL) {
			Assert(Player_ship->ship_max_hull_strength != 0);

			float integrity = Player_obj->hull_strength / Player_ship->ship_max_hull_strength;
			if (integrity < HULL_VALUE_TO_PLAY_INTENSE_BATTLE_MUSIC)
				play_intense_battle_music = true;
		}

		if (play_intense_battle_music == true) {
			if (Current_pattern == SONG_BTTL_2) {
				Patterns[next_pattern].next_pattern = SONG_BTTL_3;
			} else {
				Patterns[next_pattern].next_pattern = SONG_BTTL_2;
			}
		} else {
			if (Current_pattern == SONG_BTTL_1) {
				Patterns[next_pattern].next_pattern = SONG_BTTL_2;
			} else if (Current_pattern == SONG_BTTL_2) {
				Patterns[next_pattern].next_pattern = SONG_BTTL_3;
			} else {
				Patterns[next_pattern].next_pattern = SONG_BTTL_1;
			}
		}
	}

	if ( Current_pattern != -1 ) {
		Patterns[Current_pattern].next_pattern = next_pattern;
		Patterns[Current_pattern].force_pattern = TRUE;
	}

	Battle_over_timestamp = _timestamp(BATTLE_CHECK_INTERVAL);

	return 0;
}

// -------------------------------------------------------------------------------------------------
// event_music_friendly_arrival() 
//
// An friendly has arrived, play a friendly arrival pattern.
//
int event_music_friendly_arrival()
{
	if ( Event_music_enabled == FALSE )
		return -1;

	if ( Event_music_level_inited == FALSE )
		return -1;

	if (timestamp_elapsed(Next_arrival_timestamp) == false) {
		return 0;
	}

	int next_pattern;
	if ( Event_Music_battle_started == TRUE ) {
		next_pattern = SONG_AARV_2;
	}
	else {
		next_pattern = SONG_AARV_1;
	}

	if ( Current_pattern == next_pattern )
		return 0;	// already playing

	if ( Current_pattern == SONG_DEAD_1 )
		return 0;	// death is the last song to play

	if ( (Current_pattern == SONG_VICT_1) || (Current_pattern == SONG_VICT_2) )
		return 0;

	// Goober5000 - to avoid array out-of-bounds
	//Assert(Current_pattern >= 0 && Current_pattern < MAX_PATTERNS);

	if(Current_pattern < 0 || Current_pattern >= MAX_PATTERNS)
		return 0;

	if ( Patterns[Current_pattern].next_pattern != Patterns[Current_pattern].default_next_pattern )
		return 0;	// don't squash a pending pattern

	// After the second friendly arrival, default to SONG_BTTL_3
	Num_friendly_arrivals++;


	if ( Current_pattern != -1 )
	{
		// AL 06-24-99: always overlay allied arrivals
		// Goober5000 - do this based on a flag (set in music.tbl for FS1 soundtracks)

		// overlay
		if (Soundtracks[Current_soundtrack_num].flags & EMF_ALLIED_ARRIVAL_OVERLAY)
		{
			// Goober5000 - This is how retail did it.  For some reason, FS2 only has one
			// arrival music pattern, and this is it
			if (Patterns[SONG_AARV_1].handle >= 0)
			{
				audiostream_play(Patterns[SONG_AARV_1].handle, (Master_event_music_volume * aav_music_volume), 0);	// no looping
				audiostream_set_sample_cutoff(Patterns[SONG_AARV_1].handle, fl2i(Patterns[SONG_AARV_1].num_measures * Patterns[SONG_AARV_1].samples_per_measure));
			}
		}
		// don't overlay
		else
		{
			Patterns[Current_pattern].next_pattern = next_pattern;
			Patterns[Current_pattern].force_pattern = TRUE;

			// Goober5000 - default to SONG_BTTL_3 as specified above
			// (this is my attempted recreation of the FS1 behavior)
			if (Soundtracks[Current_soundtrack_num].flags & EMF_CYCLE_FS1)
			{
				if ((Event_Music_battle_started == TRUE) && (Num_friendly_arrivals > 2))
				{
					if (Patterns[SONG_BTTL_3].handle != -1)
					{
						Patterns[next_pattern].next_pattern = SONG_BTTL_3;
					}
				}
			}
		}
	}

	Next_arrival_timestamp = _timestamp(ARRIVAL_INTERVAL_TIMESTAMP);

	Battle_over_timestamp = _timestamp(BATTLE_CHECK_INTERVAL);

	return 0;
}

//	Play arrival music keyed to team "team".
void event_music_arrival(int team)
{
	//	No friendly arrival music in a training mission.
	if ( The_mission.game_type & MISSION_TYPE_TRAINING )
		return;

	// check if ship is enemy ship (we attack it)
	if (iff_x_attacks_y(Player_ship->team, team))
		event_music_enemy_arrival();
	else
		event_music_friendly_arrival();
}

// -------------------------------------------------------------------------------------------------
// event_music_primary_goals_met() 
//
// A primary goal has failed
//
int event_music_primary_goal_failed()
{
	int next_pattern;

	//	No special tracks in training.
	if ( The_mission.game_type & MISSION_TYPE_TRAINING )
		return -1;

	if ( Event_music_enabled == FALSE )
		return -1;

	if ( Event_music_level_inited == FALSE )
		return -1;

	if ( Current_pattern == SONG_DEAD_1 )
		return 0;	// death is the last song to play

	if ( Patterns[SONG_FAIL_1].handle < 0 )	// can't play if music file doesn't exist
		return 0;

	if ( hostile_ships_present() ) {
		next_pattern = SONG_BTTL_1;
	}
	else {
		next_pattern = SONG_NRML_1;
		Event_Music_battle_started = 0;
	}

	if ( Current_pattern != -1 ) {
		Assert( Current_pattern >= 0 && Current_pattern < MAX_PATTERNS );
		Patterns[Current_pattern].next_pattern = next_pattern;
		Patterns[Current_pattern].force_pattern = TRUE;
	}

	return 0;
}

// -------------------------------------------------------------------------------------------------
// event_music_primary_goals_met() 
//
// A goal has been achieved, play the appropriate victory music.
//
int event_music_primary_goals_met()
{
	int next_pattern = SONG_VICT_1;

	//	No special tracks in training.
	if ( The_mission.game_type & MISSION_TYPE_TRAINING )
		return -1;

	if ( Event_music_enabled == FALSE )
		return -1;

	if ( Event_music_level_inited == FALSE )
		return -1;

	if ( (Current_pattern == SONG_VICT_1) || (Current_pattern == SONG_VICT_2) )
		return 0;	// already playing

	if ( Current_pattern == SONG_DEAD_1 )
		return 0;	// death is the last song to play

	if ( hostile_ships_present() ) {
		Patterns[SONG_VICT_1].next_pattern = SONG_BTTL_1;
	}
	else {
		Patterns[SONG_VICT_1].next_pattern = SONG_VICT_2;
		Event_Music_battle_started = 0;

		// If the mission goals aren't met (or there are no goals), or if victory 2 music has already played, then go
		// to the next default track
		if ( !mission_goals_met() || Victory2_music_played || Mission_goals.empty()) {
			Patterns[next_pattern].next_pattern = Patterns[next_pattern].default_next_pattern;
		} else {
			Victory2_music_played = true;
		}
	}

	if ( Current_pattern != -1 ) {
		Patterns[Current_pattern].next_pattern = next_pattern;
		Patterns[Current_pattern].force_pattern = TRUE;
	}

	return 0;
}

// -------------------------------------------------------------------------------------------------
// event_music_player_death() 
//
// The player has died, play death pattern.
//
int event_music_player_death()
{
	if ( Event_music_enabled == FALSE )
		return -1;

	if ( Event_music_level_inited == FALSE )
		return -1;

	if ( Current_pattern == SONG_DEAD_1 )
		return 0;	// already playing

	if ( Current_pattern != -1 ) {
		Assert( Current_pattern >= 0 && Current_pattern < MAX_PATTERNS );
		Patterns[Current_pattern].next_pattern = SONG_DEAD_1;
		Patterns[Current_pattern].force_pattern = TRUE;
	}

	return 0;
}

// -------------------------------------------------------------------------------------------------
// event_music_player_respawn() 
//
// Player has respawned (multiplayer only)
//
int event_music_player_respawn()
{
	if ( Event_music_enabled == FALSE )
		return -1;

	if ( Event_music_level_inited == FALSE )
		return -1;

//	Assert(Current_pattern == SONG_DEAD_1);

	Event_Music_battle_started = 0;
	Patterns[Current_pattern].next_pattern = SONG_NRML_1;
	Patterns[Current_pattern].force_pattern = TRUE;

	return 0;
}

// -------------------------------------------------------------------------------------------------
// event_music_player_respawn_as_observer() 
//
// Player has respawned (multiplayer only)
//
int event_music_player_respawn_as_observer()
{
	if ( Event_music_enabled == FALSE )
		return -1;

	if ( Event_music_level_inited == FALSE )
		return -1;

	if ( Current_pattern >= 0 ) {
		if ( audiostream_is_playing(Patterns[Current_pattern].handle) ) {
			audiostream_stop(Patterns[Current_pattern].handle);
			Current_pattern = -1;
		}
	}

	return 0;
}

bool parse_soundtrack_line(int strack_idx, int pattern_idx)
{
	char fname[MAX_FILENAME_LEN];
	char line_buf[MAX_PATH_LEN];
	char *token;
	int count = 0;

	// line_buf holds 3 fields:  filename, num measures, bytes per measure
	stuff_string(line_buf, F_NAME, sizeof(line_buf));

	//Check if we can add this pattern
	if( pattern_idx >= MAX_PATTERNS ) {
		Warning(LOCATION, "Too many $Name: entries for soundtrack %s", Soundtracks[strack_idx].name);
		return false;
	}

	//We can apparently still add this pattern, so go ahead and do it.
	token = strtok( line_buf, NOX(" ,\t"));
	strcpy_s(fname, token);
	while ( token != NULL )
	{
		token = strtok( NULL, NOX(" ,\t") );
		//If we have no more items, get out and return
		if ( token == NULL && count != 2)
		{
			Warning(LOCATION, "Missing or additional field for soundtrack %s, pattern %s", Soundtracks[strack_idx].name, Pattern_info[pattern_idx].pattern_desc);
			break;
		}


		if (count == 0) {
			Soundtracks[strack_idx].patterns[pattern_idx].num_measures = (float)atof(token);	//Num_measures
		} else if (count == 1) {
			Soundtracks[strack_idx].patterns[pattern_idx].samples_per_measure = atoi(token);	//Samples per measure
		}

		count++;
	}	// end while

	strcpy_s(Soundtracks[strack_idx].patterns[pattern_idx].fname, fname);
	return true;
}

void parse_soundtrack()
{
	char namebuf[NAME_LENGTH];
	int i, strack_idx = -1;
	bool nocreate = false;

	//Get the name, and do we have this track already?
	required_string("$SoundTrack Name:");
	stuff_string(namebuf, F_NAME, NAME_LENGTH);
	strack_idx = event_music_get_soundtrack_index(namebuf);

	//Do we have a nocreate?
	if(optional_string("+nocreate")) {
		nocreate = true;
	}

	//Get a valid strack_idx
	if (strack_idx < 0 && nocreate)
	{
		//Track doesn't exist and has nocreate, so don't create it
		if ( !skip_to_start_of_string_either("#SoundTrack Start", "#Menu Music Start") && !skip_to_string("#SoundTrack End")) {
			error_display(1, "Couldn't find #Soundtrack Start, #Menu Music Start or #Soundtrack End.");
		}
		return;
	}
	else if (strack_idx < 0)
	{
		//If we don't have this soundtrack already, create it
		strack_idx = (int)Soundtracks.size();
		Soundtracks.emplace_back();

		Soundtracks[strack_idx].flags = 0;
		Soundtracks[strack_idx].num_patterns = 0;
		strcpy_s(Soundtracks[strack_idx].name, namebuf);

		// set all the filenames to "none" so we're compatible with the extra NRMLs in FS1 music
		for (auto &pattern : Soundtracks[strack_idx].patterns)
			strcpy_s(pattern.fname, NOX("none.wav"));
	}

	// Goober5000
	if (optional_string("+Cycle:"))
	{
		char temp[NAME_LENGTH];
		stuff_string(temp, F_NAME, NAME_LENGTH);
		if (!stricmp(temp, "FS1"))
			Soundtracks[strack_idx].flags |= EMF_CYCLE_FS1;
	}

	// Goober5000
	if (optional_string("+Allied Arrival Overlay:"))
	{
		stuff_boolean_flag(&Soundtracks[strack_idx].flags, EMF_ALLIED_ARRIVAL_OVERLAY);
	}
	else
	{
		// default to on
		Soundtracks[strack_idx].flags |= EMF_ALLIED_ARRIVAL_OVERLAY;
	}

	//If the next string is $Name:, use default Volition stuff
	if(check_for_string("$Name:"))
	{
		int old_pattern_num = 0;

		while (required_string_either("#SoundTrack End","$Name:"))
		{
			required_string("$Name:");

			//Find which new pattern index this corresponds to
			for(i = 0; i < Num_pattern_types; i++)
			{
				if(New_pattern_order[i] == old_pattern_num)
				{
					if(parse_soundtrack_line(strack_idx, i))
					{
						//If new pattern is higher, change the old value
						if(i+1 > Soundtracks[strack_idx].num_patterns) {
							Soundtracks[strack_idx].num_patterns = i+1;
						}
					}

					//Get out of the loop
					break;
				}
			}

			if(i == Num_pattern_types)
			{
				Warning(LOCATION, "Could not find new index for pattern %d of soundtrack '%s'", old_pattern_num, Soundtracks[strack_idx].name);
			}

			old_pattern_num++;
		}
	}
	else
	{
		//Use our new stuff
		char tagbuf[64];

		//try the new pattern order
		for(i = 0; i < Num_pattern_types; i++)
		{
			//Check for the tag based on description
			sprintf(tagbuf, "$%s:", Pattern_info[i].pattern_desc);
			if(optional_string(tagbuf))
			{
				//Parse it
				if(parse_soundtrack_line(strack_idx, i))
				{
					//If the new pattern is higher than the old one, change num_patterns
					if(i+1 > Soundtracks[strack_idx].num_patterns) {
						Soundtracks[strack_idx].num_patterns = i+1;
					}
				}
			}
		}
	}

	//We're done here.
	required_string("#SoundTrack End");
}

void parse_menumusic()
{
	char spoolname[NAME_LENGTH];
	char fname[MAX_FILENAME_LEN];
	bool nocreate = false;

	required_string("$Name:");
	stuff_string(spoolname, F_NAME, NAME_LENGTH);

	if(optional_string("+nocreate")) {
		nocreate = true;
	}

	int idx = event_music_get_spooled_music_index(spoolname);
	
	if (idx < 0 && nocreate)
	{
		if (!skip_to_start_of_string_either("$Name:", "#Menu Music End")) {
			Error(LOCATION, "Couldn't find $Name or #Menu Music End. Music.tbl or -mus.tbm is invalid.\n");
		}
		return;
	}
	else if (idx < 0)
	{
		idx = (int)Spooled_music.size();
		Spooled_music.emplace_back();

		Spooled_music[idx].flags = 0;
		strcpy_s( Spooled_music[idx].name, spoolname );
		strcpy_s( Spooled_music[idx].filename, "");
	}

	if(optional_string("$Filename:"))
	{
		stuff_string(fname, F_LNAME, MAX_FILENAME_LEN);
		if ( strnicmp(fname, NOX("none.wav"), 4) != 0  ) {
			strcpy_s( Spooled_music[idx].filename, fname );
		}
		else
		{
			//Clear this
			strcpy_s( Spooled_music[idx].filename, "");
		}
	}

	// Goober5000 - check for existence of file
	// taylor - check for all file types
	// chief1983 - use type list defined in ffmpeg.h
	if ( cf_exists_full_ext(Spooled_music[idx].filename, CF_TYPE_MUSIC, NUM_AUDIO_EXT, audio_ext_list) )
		Spooled_music[idx].flags |= SMF_VALID;
}

// -------------------------------------------------------------------------------------------------
// event_music_parse_musictbl() will parse the music.tbl file, and set up the Mission_songs[]
// array
//
void event_music_parse_musictbl(const char *filename)
{
	try
	{
		read_file_text(filename, CF_TYPE_TABLES);
		reset_parse();

		while ( check_for_string("#Soundtrack Start") || check_for_string("#Menu Music Start") )
		{
			if ( optional_string("#Soundtrack Start") )
			{
				parse_soundtrack( );
			}
			if ( optional_string("#Menu Music Start") )
			{
				while ( required_string_either("$Name", "#Menu Music End") == 0 )
				{
					parse_menumusic( );
				}
			}
		}
	}
	catch (const parse::ParseException& e)
	{
		mprintf(("TABLES: Unable to parse '%s'!  Error message = %s.\n", filename, e.what()));
	}
}

// -------------------------------------------------------------------------------------------------
// event_music_change_pattern()
//
// Force a particular pattern to play.  This is used for debugging purposes.
//
void event_music_change_pattern(int new_pattern)
{
	if ( Event_music_enabled == FALSE ) {
		nprintf(("EVENTMUSIC", "EVENTMUSIC ==> Requested a song switch when event music is not enabled\n"));
		return;
	}

	if ( Event_music_level_inited == FALSE ) {
		nprintf(("EVENTMUSIC", "EVENTMUSIC ==> Event music is not enabled\n"));
		return;
	}

	if ( Current_pattern == new_pattern )
		return;	// already playing

	if ( Current_pattern != -1 ) {
		Assert( Current_pattern >= 0 && Current_pattern < MAX_PATTERNS );
		Patterns[Current_pattern].next_pattern = new_pattern;
		Patterns[Current_pattern].force_pattern = TRUE;
	}
}

// -------------------------------------------------------------------------------------------------
// event_music_return_current_pattern()
//
// Simply return what the current pattern being played is.  Don't want to make global.
//
int event_music_return_current_pattern()
{
	return Current_pattern;
}

// -------------------------------------------------------------------------------------------------
// event_music_disable()
//
// Stop any patterns that are playing, and prevent any further patterns from playing (ie
// set Event_music_enabled = FALSE).  We don't uninit event music, since it might be toggled
// back on in this level.
//
void event_music_disable()
{
	if ( Event_music_level_inited == FALSE )
		return;

	if ( Event_music_enabled == FALSE )
		return;

	if (Current_pattern == -1)
		return;

	Assert( Current_pattern >= 0 && Current_pattern < MAX_PATTERNS );
	if (  audiostream_is_playing(Patterns[Current_pattern].handle) ) {
			audiostream_stop(Patterns[Current_pattern].handle);	// stop current and rewind
	}

	Event_music_begun = FALSE;
	Event_music_enabled = FALSE;
	Current_pattern = -1;
}

// -------------------------------------------------------------------------------------------------
// event_music_enable()
//
// Init the event music (ie load the patterns) if required.  Set up the first song to play, and
// set Event_music_enabled = TRUE to allow patterns to play.
//
void event_music_enable()
{
	if ( Event_music_enabled == TRUE )
		return;

	Event_music_enabled = TRUE;

	if ( Event_music_level_inited == FALSE ) {
		event_music_level_start();
		// start the first pattern to play (delayed)
		if ( Game_mode & GM_IN_MISSION ) {
			event_music_first_pattern();
		}
	}
	else {
		// start a new pattern
		Event_music_begun = FALSE;
		Pattern_timer_id = _timestamp(150);
		event_music_start_default();
	}
}

// -------------------------------------------------------------------------------------------------
// event_music_start_default()
//
//	Start playing a default track, based on how far the mission has progressed
//
void event_music_start_default()
{	
	int next_pattern;

	if ( Event_Music_battle_started == TRUE ) {
		if ( hostile_ships_present() ) {
			next_pattern = SONG_BTTL_1;
		}
		else {
			Event_Music_battle_started = FALSE;
			next_pattern = SONG_NRML_1;
		}
	}
	else
	{
		next_pattern = SONG_NRML_1;	
	}

	// switch now
	if ( Current_pattern == -1 ) {
		Current_pattern = next_pattern;
	}
	// switch later
	else {
		Patterns[Current_pattern].next_pattern = next_pattern;
		Patterns[Current_pattern].force_pattern = TRUE;
	}

}

// -------------------------------------------------------------------------------------------------
// event_music_set_volume_all()
//
//	Set the volume of the event driven music.  Used when using the game-wide music volume is changed
// by the user.
//
void event_music_set_volume_all(float volume)
{
	audiostream_set_volume_all(volume, ASF_EVENTMUSIC);
}

// ----------------------------------------------------------------------
// hostile_ships_present()
//
// Determine if there are any non-friendly ships in existance
//
// returns: 1 =>	there are non-friendly ships in existance
//				0 =>  any ships in existance are friendly
int hostile_ships_present()
{
	ship		*shipp;
	ship_obj *so;

	for ( so = GET_FIRST(&Ship_obj_list); so != END_OF_LIST(&Ship_obj_list); so = GET_NEXT(so) )
	{
		if (Objects[so->objnum].flags[Object::Object_Flags::Should_be_dead])
			continue;
		shipp = &Ships[Objects[so->objnum].instance];

		// skip ourselves
		if ( shipp == Player_ship )
			continue;

		// check if ship is enemy ship (we attack it)
		if (!(iff_x_attacks_y(Player_ship->team, shipp->team)))
			continue;

		// check if ship is threatening
		if ( shipp->is_dying_or_departing() || shipp->flags[Ship::Ship_Flags::Disabled] || (ship_subsys_disrupted(shipp, SUBSYSTEM_ENGINE)) ) 
			continue;
	
		// check if ship is flyable
        if (!Ship_info[shipp->ship_info_index].is_flyable()) {
			continue;
		}

		// check if ship is visible by player's team
		if ( hud_target_invalid_awacs(&Objects[so->objnum]) == 1 ) {
			continue;
		}

		return 1;
	}

	return 0;
}

// ----------------------------------------------------------------
// event_music_get_info()
//
// Return information about the event music in the buffer outbuf
// NOTE: callers to this function are advised to allocate a 256 byte buffer
void event_music_get_info(char *outbuf)
{
	if ( Event_music_enabled == FALSE || Event_music_level_inited == FALSE || Current_soundtrack_num < 0 || Current_pattern < 0 ) {
		strcpy(outbuf,XSTR( "Event music is not playing", 213));
	}
	else {	
		sprintf(outbuf,XSTR( "soundtrack: %s [%s]", 214), Soundtracks[Current_soundtrack_num].name, Pattern_info[Current_pattern].pattern_desc);
	}
}

// ----------------------------------------------------------------
// event_music_next_soundtrack()
//
// input:	delta		=>		1 or -1, depending if you want to go to next or previous song
//
// returns: New soundtrack number if successfully changed, otherwise return -1
//
int event_music_next_soundtrack(int delta)
{
	int new_soundtrack;

	if ( Event_music_enabled == FALSE || Event_music_level_inited == FALSE || Soundtracks.empty() ) {
		return -1;
	}
		
	new_soundtrack = Current_soundtrack_num + delta;
	if ( new_soundtrack >= (int)Soundtracks.size() )
		new_soundtrack = 0;

	event_music_level_close();
	event_music_level_start(new_soundtrack);

	return Current_soundtrack_num;
}

// Goober5000 - along the same lines; this is for the sexp
void event_sexp_change_soundtrack(const char *name)
{
	Assert(name);

	int i, new_soundtrack = -1;

	for (i = 0; i < (int)Soundtracks.size(); i++) {
		if ( !stricmp(name, Soundtracks[i].name) ) {
			new_soundtrack = i;
		}
	}

	// if we are already on this soundtrack then bail
	if (new_soundtrack == Current_soundtrack_num) {
		return;
	}

	event_music_level_close();
	Current_soundtrack_num = -1;
	event_music_level_start(new_soundtrack);
}

// ----------------------------------------------------------------
// event_music_get_soundtrack_name()
//
// Return information about the event music in the buffer outbuf
// NOTE: callers to this function are advised to allocate a NAME_LENGTH buffer
void event_music_get_soundtrack_name(char *outbuf)
{
	if ( Event_music_enabled == FALSE || Event_music_level_inited == FALSE || Current_soundtrack_num < 0 ) {
		strcpy(outbuf, XSTR( "Event music is not playing", 213));
	}
	else {
		strcpy(outbuf, Soundtracks[Current_soundtrack_num].name);
	}
}

// set the current soundtrack based on name
void event_music_set_soundtrack(const char *name)
{
	int index = event_music_get_soundtrack_index(name);

	if (index >= 0 && Soundtracks[index].flags & EMF_VALID)
		Current_soundtrack_num = index;
	else
		Current_soundtrack_num = -1;

	if (Current_soundtrack_num == -1)
		mprintf(("Current soundtrack set to -1 in event_music_set_soundtrack\n"));
}

int event_music_get_soundtrack_index(const char *name)
{
	// find the correct index for the event music
	for ( int i = 0; i < (int)Soundtracks.size(); i++ ) {
		if ( !stricmp(name, Soundtracks[i].name) ) {
			return i;
		}
	}

	return -1;
}

int event_music_get_spooled_music_index(const char *name)
{
	// find the correct index for the spooled music
	for ( int i = 0; i < (int)Spooled_music.size(); i++ ) {
		if ( !stricmp(name, Spooled_music[i].name) ) {
			return i;
		}
	}

	return -1;
}

int event_music_get_spooled_music_index(const SCP_string& name)
{
	return event_music_get_spooled_music_index(name.c_str());
}

// set a score based on name
void event_music_set_score(int score_index, const char *name)
{
	Assert(score_index < NUM_SCORES);

	int index = event_music_get_spooled_music_index(name);

	if (index >= 0 && Spooled_music[index].flags & SMF_VALID)
		Mission_music[score_index] = index;
	else
		Mission_music[score_index] = -1;
}

// reset what sort of music is to be used for this mission
void event_music_reset_choices()
{
	Current_soundtrack_num = -1;
	mprintf(("Current soundtrack set to -1 in event_music_reset_choices\n"));
	Mission_music[SCORE_BRIEFING] = -1;
	Mission_music[SCORE_FICTION_VIEWER] = -1;
	event_music_set_score(SCORE_DEBRIEFING_SUCCESS, "Success");
	event_music_set_score(SCORE_DEBRIEFING_AVERAGE, "Average");
	event_music_set_score(SCORE_DEBRIEFING_FAILURE, "Failure");

	// Goober5000
	strcpy_s(The_mission.substitute_briefing_music_name, "None");
	strcpy_s(The_mission.substitute_event_music_name, "None");
}

void event_music_hostile_ship_destroyed()
{
	Battle_over_timestamp = _timestamp(BATTLE_CHECK_INTERVAL);
}

void event_music_set_volume(float volume)
{
	Assertion(volume >= 0.0f && volume <= 1.0f, "Invalid event music volume %f!", volume);

	Master_event_music_volume = volume;
	event_music_set_volume_all(Master_event_music_volume);

	if (Master_event_music_volume > 0.0f) {
		event_music_enable();
	} else {
		event_music_disable();
	}
}

#ifdef _MSC_VER
#pragma optimize("", on)
#endif
