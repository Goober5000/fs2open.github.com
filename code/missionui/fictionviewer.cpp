/*
 * Created by Ian "Goober5000" Warfield for the FreeSpace2 Source Code Project.
 * You may not sell or otherwise commercially exploit the source or things you
 * create based on the source.
 */



#include "freespace.h"
#include "gamesequence/gamesequence.h"
#include "gamesnd/eventmusic.h"
#include "gamesnd/gamesnd.h"
#include "globalincs/alphacolors.h"
#include "io/key.h"
#include "localization/localize.h"
#include "mission/missionbriefcommon.h"
#include "missionui/fictionviewer.h"
#include "missionui/missioncmdbrief.h"
#include "missionui/missionscreencommon.h"
#include "missionui/missionshipchoice.h"
#include "missionui/missionweaponchoice.h"
#include "missionui/redalert.h"
#include "network/multi.h"
#include "mod_table/mod_table.h"
#include "network/multi_endgame.h"
#include "network/multiteamselect.h"
#include "parse/parselo.h"
#include "sound/audiostr.h"
#include "utils/encoding.h"


// ---------------------------------------------------------------------------------------------------------------------------------------
// MISSION FICTION VIEWER DEFINES/VARS
//
constexpr int NUM_FVW_LAYOUTS = 2;

const char *Fiction_viewer_ui_names[NUM_FVW_LAYOUTS] =
{
	"FS2",	// FreeSpace 2
	"WCS"	// Wing Commander Saga
};

const char *Fiction_viewer_screen_filename[NUM_FVW_LAYOUTS][GR_NUM_RESOLUTIONS] =
{
	{
		"FictionViewer",		// GR_640
		"2_FictionViewer"		// GR_1024
	},
	{
		"FictionViewerb",		// GR_640
		"2_FictionViewerb"		// GR_1024
	}
};

const char *Fiction_viewer_screen_mask[NUM_FVW_LAYOUTS][GR_NUM_RESOLUTIONS] =
{
	{
		"FictionViewer-m",		// GR_640
		"2_FictionViewer-m"		// GR_1024
	},
	{
		"FictionViewer-mb",		// GR_640
		"2_FictionViewer-mb"	// GR_1024
	}
};

int Fiction_viewer_text_coordinates[NUM_FVW_LAYOUTS][GR_NUM_RESOLUTIONS][4] =
{
	// standard FS2-style interface
	{
		{ // GR_640
			17,		37,		588,	344
		},
		{ // GR_1024
			25,		48,		944,	576
		}
	},
	// WCS-style interface
	{
		{ // GR_640
			44,		50,		522,	348
		},
		{ // GR_1024
			71,		80,		835,	556
		}
	}
};

#define NUM_FVW_BUTTONS			3
#define FVW_BUTTON_ACCEPT		0
#define FVW_BUTTON_SCROLL_UP	1
#define FVW_BUTTON_SCROLL_DOWN	2

// the xt and yt fields aren't normally used for width and height,
// but the fields would go unused here and this is more
// convenient for initialization
ui_button_info Fiction_viewer_buttons[NUM_FVW_LAYOUTS][GR_NUM_RESOLUTIONS][NUM_FVW_BUTTONS] =
{
	// standard FS2-style interface
	{
		{ // GR_640
			ui_button_info("fvw_accept_",	571,	425,	69,		55,		FVW_BUTTON_ACCEPT),
			ui_button_info("fvw_up_",		614,	14,		25,		31,		FVW_BUTTON_SCROLL_UP),
			ui_button_info("fvw_down_",		614,	370,	25,		31,		FVW_BUTTON_SCROLL_DOWN),
		//                 Filename          x       y     width  height
		},
		{ // GR_1024
			ui_button_info("2_fvw_accept_",	918,	688,	99,		77,		FVW_BUTTON_ACCEPT),
			ui_button_info("2_fvw_up_",		981,	16,		40,		50,		FVW_BUTTON_SCROLL_UP),
			ui_button_info("2_fvw_down_",	981,	606,	40,		50,		FVW_BUTTON_SCROLL_DOWN),
		}
	},
	// WCS-style interface
	{
		{ // GR_640
			ui_button_info("fvw_accept_",	105,	444,	37,		26,		FVW_BUTTON_ACCEPT),
			ui_button_info("fvw_up_",		576,	51,		37,		33,		FVW_BUTTON_SCROLL_UP),
			ui_button_info("fvw_down_",		576,	364,	37,		33,		FVW_BUTTON_SCROLL_DOWN),
		},
		{ // GR_1024
			ui_button_info("2_fvw_accept_",	168,	710,	59,		41,		FVW_BUTTON_ACCEPT),
			ui_button_info("2_fvw_up_",		922,	81,		59,		53,		FVW_BUTTON_SCROLL_UP),
			ui_button_info("2_fvw_down_",	922,	582,	59,		53,		FVW_BUTTON_SCROLL_DOWN),
		}
	}
};

const char *Fiction_viewer_slider_filename[NUM_FVW_LAYOUTS][GR_NUM_RESOLUTIONS] =
{
	// standard FS2-style interface
	{
		"slider",
		"2_slider"
	},
	// WCS-style interface
	{
		"fvw_slider_",
		"2_fvw_slider_"
	}
};

int Fiction_viewer_slider_coordinates[NUM_FVW_LAYOUTS][GR_NUM_RESOLUTIONS][4] =
{
	// standard FS2-style interface
	{
		{ // GR_640
			618,	48,		18,		320		//Initial position x, initial position y, width, height of the slider column
		},
		{ // GR_1024
			988,	70,		28,		532
		}
	},
	// WCS-style interface
	{
		{ // GR_640
			589,	83,		16,		280
		},
		{ // GR_1024
			944,	132,	25,		451
		}
	}
};

SCP_vector<fiction_viewer_stage> Fiction_viewer_stages;
int Fiction_viewer_active_stage = -1;

static int Top_fiction_viewer_text_line = 0;
static int Fiction_viewer_text_max_lines = 0;

static UI_WINDOW Fiction_viewer_window;
static UI_SLIDER2 Fiction_viewer_slider;
static int Fiction_viewer_bitmap = -1;
static int Fiction_viewer_inited = 0;

static int Fiction_viewer_old_fontnum = -1;
static int Fiction_viewer_fontnum = -1;

static char *Fiction_viewer_text = nullptr;
static int Fiction_viewer_voice = -1;

static int Fiction_viewer_ui = -1;

static void use_fv_font()
{
	// save old font and set new one
	if (Fiction_viewer_fontnum >= 0)
	{
		Fiction_viewer_old_fontnum = font::get_current_fontnum();
		font::set_font(Fiction_viewer_fontnum);
	}
	else
	{
		Fiction_viewer_old_fontnum = -1;
	}
}

static void use_std_font()
{
	// restore the old font
	if (Fiction_viewer_old_fontnum >= 0)
		font::set_font(Fiction_viewer_old_fontnum);
}

// ---------------------------------------------------------------------------------------------------------------------------------------
// FICTION VIEWER FUNCTIONS
//

void fiction_viewer_exit()
{
	if (mission_has_cmd_brief())
		gameseq_post_event(GS_EVENT_CMD_BRIEF);
	else if (red_alert_mission())
		gameseq_post_event(GS_EVENT_RED_ALERT);
	else
		gameseq_post_event(GS_EVENT_START_BRIEFING);
}

void fiction_viewer_scroll_up()
{
	Top_fiction_viewer_text_line--;
	if (Top_fiction_viewer_text_line < 0)
	{
		Top_fiction_viewer_text_line = 0;
		gamesnd_play_iface(InterfaceSounds::GENERAL_FAIL);
	}
	else
	{
		gamesnd_play_iface(InterfaceSounds::SCROLL);
	}
}

void fiction_viewer_scroll_down()
{
	Top_fiction_viewer_text_line++;
	if ((Num_brief_text_lines[0] - Top_fiction_viewer_text_line) < Fiction_viewer_text_max_lines)
	{
		Top_fiction_viewer_text_line--;
		gamesnd_play_iface(InterfaceSounds::GENERAL_FAIL);
	}
	else
	{
		gamesnd_play_iface(InterfaceSounds::SCROLL);
	}
}

void fiction_viewer_scroll_capture()
{
	// nothing to do
}

// button press
void fiction_viewer_button_pressed(int button)
{
	switch (button)
	{
		case FVW_BUTTON_ACCEPT:
			fiction_viewer_exit();
			gamesnd_play_iface(InterfaceSounds::COMMIT_PRESSED);
			break;

		case FVW_BUTTON_SCROLL_UP:
			fiction_viewer_scroll_up();
			Fiction_viewer_slider.forceUp();
			break;

		case FVW_BUTTON_SCROLL_DOWN:
			fiction_viewer_scroll_down();
			Fiction_viewer_slider.forceDown();
			break;

		default:
			Int3();	// unrecognized button
			break;
	}
}

// init
void fiction_viewer_init()
{
	if (Fiction_viewer_inited)
		return;

	// no stage loaded?
	if (Fiction_viewer_active_stage < 0)
		return;

	// no fiction document?
	if (!mission_has_fiction())
		return;

	// for multiplayer, change the state in my netplayer structure
	if (Game_mode & GM_MULTIPLAYER) {
		Net_player->state = NETPLAYER_STATE_FICTION_VIEWER;
	}

	// music
	common_music_init(SCORE_FICTION_VIEWER);

	Fiction_viewer_bitmap = -1;

	if (*Fiction_viewer_stages[Fiction_viewer_active_stage].background[gr_screen.res] != '\0')
	{
		Fiction_viewer_bitmap = bm_load(Fiction_viewer_stages[Fiction_viewer_active_stage].background[gr_screen.res]);
		if (Fiction_viewer_bitmap < 0) {
			mprintf(("Failed to load custom background bitmap %s!\n",
					 Fiction_viewer_stages[Fiction_viewer_active_stage].background[gr_screen.res]));
		} else if (Fiction_viewer_ui < 0) {
			Fiction_viewer_ui = 0;
		}
	}

	// if special background failed to load, or if no special background was supplied, load the standard bitmap
	if (Fiction_viewer_bitmap < 0)
	{
		if (Fiction_viewer_ui < 0)
		{
			// no UI specified; use the last UI in the array that has an available background bitmap
			for (Fiction_viewer_ui = NUM_FVW_LAYOUTS - 1; Fiction_viewer_ui >= 0; Fiction_viewer_ui--)
			{
				Fiction_viewer_bitmap = bm_load(Fiction_viewer_screen_filename[Fiction_viewer_ui][gr_screen.res]);
				if (Fiction_viewer_bitmap >= 0)
					break;
			}
		}
		else
		{
			// use the specified UI's background bitmap
			Fiction_viewer_bitmap = bm_load(Fiction_viewer_screen_filename[Fiction_viewer_ui][gr_screen.res]);
		}
	}
	
	// no ui is valid?
	if (Fiction_viewer_ui < 0)
	{
		Warning(LOCATION, "No fiction viewer graphics -- cannot display fiction viewer!");
		return;
	}

	// set up fiction viewer font
	use_fv_font();

	// calculate text area lines from font
	Fiction_viewer_text_max_lines = Fiction_viewer_text_coordinates[Fiction_viewer_ui][gr_screen.res][3] / gr_get_font_height();

	// window
	Fiction_viewer_window.create(0, 0, gr_screen.max_w_unscaled, gr_screen.max_h_unscaled, 0, Fiction_viewer_fontnum);
	Fiction_viewer_window.set_mask_bmap(Fiction_viewer_screen_mask[Fiction_viewer_ui][gr_screen.res]);	

	// add the buttons
	for (int i = 0; i < NUM_FVW_BUTTONS; i++)
	{
		int repeat = (i == FVW_BUTTON_SCROLL_UP || i == FVW_BUTTON_SCROLL_DOWN);
		ui_button_info *b = &Fiction_viewer_buttons[Fiction_viewer_ui][gr_screen.res][i];

		b->button.create(&Fiction_viewer_window, "", b->x, b->y, b->xt, b->yt, repeat, 1);		
		b->button.set_highlight_action(common_play_highlight_sound);
		b->button.set_bmaps(b->filename);
		b->button.link_hotspot(b->hotspot);
	}

	// set up hotkeys for buttons
	Fiction_viewer_buttons[Fiction_viewer_ui][gr_screen.res][FVW_BUTTON_ACCEPT].button.set_hotkey(KEY_CTRLED | KEY_ENTER);
	Fiction_viewer_buttons[Fiction_viewer_ui][gr_screen.res][FVW_BUTTON_SCROLL_UP].button.set_hotkey(KEY_UP);
	Fiction_viewer_buttons[Fiction_viewer_ui][gr_screen.res][FVW_BUTTON_SCROLL_DOWN].button.set_hotkey(KEY_DOWN);

	// init brief text
	brief_color_text_init(Fiction_viewer_text, Fiction_viewer_text_coordinates[Fiction_viewer_ui][gr_screen.res][2], default_fiction_viewer_color, 0, 0);

	// if the story is going to overflow the screen, add a slider
	if (Num_brief_text_lines[0] > Fiction_viewer_text_max_lines)
	{
		Fiction_viewer_slider.create(&Fiction_viewer_window,
			Fiction_viewer_slider_coordinates[Fiction_viewer_ui][gr_screen.res][0],
			Fiction_viewer_slider_coordinates[Fiction_viewer_ui][gr_screen.res][1],
			Fiction_viewer_slider_coordinates[Fiction_viewer_ui][gr_screen.res][2],
			Fiction_viewer_slider_coordinates[Fiction_viewer_ui][gr_screen.res][3],
			Num_brief_text_lines[0] - Fiction_viewer_text_max_lines,
			Fiction_viewer_slider_filename[Fiction_viewer_ui][gr_screen.res],
			&fiction_viewer_scroll_up,
			&fiction_viewer_scroll_down,
			&fiction_viewer_scroll_capture);
	}

	if (Fiction_viewer_voice >= 0)
	{
		audiostream_play(Fiction_viewer_voice, Master_voice_volume, 0);
	}

	Fiction_viewer_inited = 1;
}

// close
void fiction_viewer_close()
{
	if (!Fiction_viewer_inited)
		return;
	
	// free the fiction
	fiction_viewer_reset();

	// destroy the window
	Fiction_viewer_window.destroy();

	// restore the old font
	use_std_font();

	// free the bitmap
	if (Fiction_viewer_bitmap >= 0)
		bm_release(Fiction_viewer_bitmap);
	Fiction_viewer_bitmap = -1;

	// maybe stop music
	if (Mission_music[SCORE_FICTION_VIEWER] != Mission_music[SCORE_BRIEFING])
		common_music_close();
	
	game_flush();

	Fiction_viewer_inited = 0;
}

// do
void fiction_viewer_do_frame(float frametime)
{
	int i, k, w, h;

	// make sure we exist
	if (!Fiction_viewer_inited)
	{
		fiction_viewer_exit();
		return;
	}

	// process keys
	k = Fiction_viewer_window.process() & ~KEY_DEBUGGED;	

	switch (k)
	{
		case KEY_ESC:
			if (Game_mode & GM_MULTIPLAYER) {
				gamesnd_play_iface(InterfaceSounds::USER_SELECT);
				multi_quit_game(PROMPT_ALL);
			} else {
				common_music_close();
				gameseq_post_event(GS_EVENT_MAIN_MENU);
			}
			return;
	}

	// process button presses
	for (i = 0; i < NUM_FVW_BUTTONS; i++)
		if (Fiction_viewer_buttons[Fiction_viewer_ui][gr_screen.res][i].button.pressed())
			fiction_viewer_button_pressed(i);
	
	common_music_do();

	// clear
	GR_MAYBE_CLEAR_RES(Fiction_viewer_bitmap);
	if (Fiction_viewer_bitmap >= 0)
	{
		gr_set_bitmap(Fiction_viewer_bitmap);
		gr_bitmap(0, 0, GR_RESIZE_MENU);
	} 
	
	// draw the window
	Fiction_viewer_window.draw();		

	// render the briefing text
	brief_render_text(Top_fiction_viewer_text_line, Fiction_viewer_text_coordinates[Fiction_viewer_ui][gr_screen.res][0], Fiction_viewer_text_coordinates[Fiction_viewer_ui][gr_screen.res][1], Fiction_viewer_text_coordinates[Fiction_viewer_ui][gr_screen.res][3], frametime);

	// maybe output the "more" indicator
	if ((Fiction_viewer_text_max_lines + Top_fiction_viewer_text_line) < Num_brief_text_lines[0])
	{
		use_std_font();

		// can be scrolled down
		int more_txt_x = Fiction_viewer_text_coordinates[Fiction_viewer_ui][gr_screen.res][0] + (Fiction_viewer_text_coordinates[Fiction_viewer_ui][gr_screen.res][2]/2) - 10;
		int more_txt_y = Fiction_viewer_text_coordinates[Fiction_viewer_ui][gr_screen.res][1] + Fiction_viewer_text_coordinates[Fiction_viewer_ui][gr_screen.res][3];				// located below text, centered

		gr_get_string_size(&w, &h, XSTR("more", 1469), 1.0f, (int)strlen(XSTR("more", 1469)));
		gr_set_color_fast(&Color_black);
		gr_rect(more_txt_x-2, more_txt_y, w+3, h, GR_RESIZE_MENU);
		gr_set_color_fast(&Color_more_indicator);
		gr_string(more_txt_x, more_txt_y, XSTR("more", 1469), GR_RESIZE_MENU);  // base location on the input x and y?

		use_fv_font();
	}

	gr_flip();
}

void fiction_viewer_pause()
{
	if (Fiction_viewer_voice >= 0)
	{
		audiostream_pause(Fiction_viewer_voice);
	}
}

void fiction_viewer_unpause()
{
	if (Fiction_viewer_voice >= 0)
	{
		audiostream_unpause(Fiction_viewer_voice);
	}
}

bool mission_has_fiction()
{
	if (Fred_running)
		return !Fiction_viewer_stages.empty();
	else
		return (Fiction_viewer_text != nullptr);
}

int fiction_viewer_ui_name_to_index(const char *ui_name)
{
	int i;
	for (i = 0; i < NUM_FVW_LAYOUTS; i++)
	{
		if (!stricmp(ui_name, Fiction_viewer_ui_names[i]))
		{
			return i;
		}
	}

	return -1;
}

void fiction_viewer_reset()
{
	if (Fiction_viewer_text != nullptr)
		vm_free(Fiction_viewer_text);
	Fiction_viewer_text = nullptr;

	Fiction_viewer_stages.clear();
	Fiction_viewer_active_stage = -1;

	Fiction_viewer_ui = -1;

	Top_fiction_viewer_text_line = 0;

	if (Fiction_viewer_voice >= 0)
	{
		audiostream_close_file(Fiction_viewer_voice);
		Fiction_viewer_voice = -1;
	}
}

SCP_string get_localized_fiction_filename(const char* filename)
{
	SCP_string this_filename = filename;
	
	// setup the localized filename string
	int lang = lcl_get_current_lang_index();
	if (lang > 0) {
		size_t lastindex = this_filename.find_last_of(".");
		this_filename = this_filename.substr(0, lastindex);
		this_filename = this_filename + "-" + Lcl_languages[lang].lang_ext + ".txt";
	}

	// return the localized version only if it exists
	if (cf_exists_full(this_filename.c_str(), CF_TYPE_FICTION))
		return this_filename;

	// if localized doesn't exist then return the base filename
	return filename;
}

void fiction_viewer_load(int stage)
{
	Assertion(stage >= 0 && static_cast<size_t>(stage) < Fiction_viewer_stages.size(), "stage parameter must be in range of Fiction_viewer_stages!");

	// just to be sure
	if (Fiction_viewer_text != nullptr)
	{
		Assertion(Fiction_viewer_text == nullptr, "Fiction viewer text should be a null pointer, but instead is '%s'. Trace out and fix!\n", Fiction_viewer_text);
		return;
	}

	Fiction_viewer_active_stage = stage;
	fiction_viewer_stage *stagep = &Fiction_viewer_stages[stage];

	// load the ui index
	Fiction_viewer_ui = Default_fiction_viewer_ui;
	if (*stagep->ui_name)
	{
		int ui_index = fiction_viewer_ui_name_to_index(stagep->ui_name);
		if (ui_index >= 0)
			Fiction_viewer_ui = ui_index;
		else
			Warning(LOCATION, "Unrecognized fiction viewer UI: %s", stagep->ui_name);
	}

	// see if we have a matching font
	Fiction_viewer_fontnum = font::FontManager::getFontIndex(stagep->font_filename);

	Fiction_viewer_voice = audiostream_open(stagep->voice_filename, ASF_VOICE);

	// load up the text
	SCP_string localized_filename = get_localized_fiction_filename(stagep->story_filename);

	CFILE *fp = cfopen(localized_filename.c_str(), "rb", CF_TYPE_FICTION);
	if (fp == NULL)
	{
		Warning(LOCATION, "Unable to load story file '%s'.", localized_filename.c_str());
	}
	else
	{
		// allocate space for raw text
		int file_length = util::check_encoding_and_skip_bom(fp, localized_filename.c_str());

		char *Fiction_viewer_text_raw = (char *) vm_malloc(file_length + 1);
		Fiction_viewer_text_raw[file_length] = '\0';

		// copy all the text
		cfread(Fiction_viewer_text_raw, file_length, 1, fp);

		// we're done with the file, close it out
		cfclose(fp);

		if (Unicode_text_mode) {
			// Copy the pointer since we assume that we don't need to adjust the text anymore
			Fiction_viewer_text = Fiction_viewer_text_raw;
			Fiction_viewer_text_raw = nullptr; // Zero out the pointer so there are no accidental accesses anymore
		} else {
			// allocate space for converted text, then perform the character conversion
			auto length = get_converted_string_length(Fiction_viewer_text_raw) + 1;
			Fiction_viewer_text = (char *) vm_malloc(length);

			maybe_convert_foreign_characters(Fiction_viewer_text_raw, Fiction_viewer_text);

			// deallocate space for raw text
			vm_free(Fiction_viewer_text_raw);
		}
	}
}
