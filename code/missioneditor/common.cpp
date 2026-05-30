// methods and members common to any mission editor FSO may have
#include "common.h"
#include "ai/ai.h"
#include "globalincs/linklist.h"
#include "mission/missionparse.h"
#include "iff_defs/iff_defs.h"
#include "object/object.h"
#include "ship/ship.h"

#include <algorithm>
#include <climits>

// to keep track of data
char Voice_abbrev_briefing[NAME_LENGTH];
char Voice_abbrev_campaign[NAME_LENGTH];
char Voice_abbrev_command_briefing[NAME_LENGTH];
char Voice_abbrev_debriefing[NAME_LENGTH];
char Voice_abbrev_message[NAME_LENGTH];
char Voice_abbrev_mission[NAME_LENGTH];
bool Voice_no_replace_filenames;
char Voice_script_entry_format[NOTES_LENGTH];
int Voice_export_selection; // 0=everything, 1=cmd brief, 2=brief, 3=debrief, 4=messages
bool Voice_group_messages;

SCP_string Voice_script_default_string = "Sender: $sender\r\nPersona: $persona\r\nFile: $filename\r\nMessage: $message";
SCP_string Voice_script_instructions_string = "$name - name of the message\r\n"
                                              "$filename - name of the message file\r\n"
                                              "$message - text of the message\r\n"
                                              "$persona - persona of the sender\r\n"
                                              "$sender - name of the sender\r\n"
                                              "$note - message notes\r\n\r\n"
                                              "Note that $persona and $sender will only appear for the Message section.";

float normalize_degrees(float deg)
{
	while (deg < -180.0f)
		deg += 360.0f;
	while (deg > 180.0f)
		deg -= 360.0f;
	// check for negative zero
	if (deg == -0.0f)
		deg = 0.0f;
	return deg;
}

void time_to_mission_info_string(const std::tm* src, char* dest, size_t dest_max_len)
{
	std::strftime(dest, dest_max_len, "%x at %X", src);
}

void stuff_special_arrival_anchor_name(char* buf, int iff_index, int restrict_to_players, bool retail_format)
{
	const char* iff_name = Iff_info[iff_index].iff_name;

	// stupid retail hack
	if (retail_format && !stricmp(iff_name, "hostile") && !restrict_to_players)
		iff_name = "enemy";

	if (restrict_to_players)
		sprintf(buf, "<any %s player>", iff_name);
	else
		sprintf(buf, "<any %s>", iff_name);

	strlwr(buf);
}

void stuff_special_arrival_anchor_name(char* buf, int anchor_num, bool retail_format)
{
	// filter out iff
	int iff_index = anchor_num;
	iff_index &= ~ANCHOR_SPECIAL_ARRIVAL;
	iff_index &= ~ANCHOR_SPECIAL_ARRIVAL_PLAYER;

	// filter players
	int restrict_to_players = (anchor_num & ANCHOR_SPECIAL_ARRIVAL_PLAYER);

	// get name
	stuff_special_arrival_anchor_name(buf, iff_index, restrict_to_players, retail_format);
}

// Ship and wing arrival and departure anchors should always be ship registry entry indexes, except for a very brief window during mission parsing.
// But FRED and QtFRED dialogs use ship indexes instead.  So, rather than refactor all the dialogs, this converts between the two.  If an anchor
// is a valid ship registry index, the equivalent ship index is returned; otherwise the special value (-1 or a flag) is returned instead.
int anchor_to_target(anchor_t anchor)
{
	auto anchor_entry = ship_registry_get(anchor);
	return anchor_entry ? anchor_entry->shipnum : anchor.value();
}

// Ship and wing arrival and departure anchors should always be ship registry entry indexes, except for a very brief window during mission parsing.
// But FRED and QtFRED dialogs use ship indexes instead.  So, rather than refactor all the dialogs, this converts between the two.  If a target
// is a valid ship index, the equivalent ship registry index is returned; otherwise the special value (-1 or a flag) is returned instead.
anchor_t target_to_anchor(int target)
{
	if (target >= 0 && target < MAX_SHIPS)
		return anchor_t(ship_registry_get_index(Ships[target].ship_name));
	else
		return anchor_t(target);
}

void update_custom_wing_indexes()
{
	int i;

	for (i = 0; i < MAX_STARTING_WINGS; i++)
		Starting_wings[i] = wing_name_lookup(Starting_wing_names[i], 1);

	for (i = 0; i < MAX_SQUADRON_WINGS; i++)
		Squadron_wings[i] = wing_name_lookup(Squadron_wing_names[i], 1);

	for (i = 0; i < MAX_TVT_WINGS; i++)
		TVT_wings[i] = wing_name_lookup(TVT_wing_names[i], 1);
}

void generate_weaponry_usage_list_team(int team, int* arr)
{
	int i;

	for (i = 0; i < MAX_WEAPON_TYPES; i++) {
		arr[i] = 0;
	}

	if (The_mission.game_type & MISSION_TYPE_MULTI_TEAMS) {
		Assert(team >= 0 && team < MAX_TVT_TEAMS);

		for (i = 0; i < MAX_TVT_WINGS_PER_TEAM; i++) {
			generate_weaponry_usage_list_wing(TVT_wings[(team * MAX_TVT_WINGS_PER_TEAM) + i], arr);
		}
	} else {
		for (i = 0; i < MAX_STARTING_WINGS; i++) {
			generate_weaponry_usage_list_wing(Starting_wings[i], arr);
		}
	}
}

void generate_weaponry_usage_list_wing(int wing_num, int* arr)
{
	int i, j;
	ship_weapon* swp;

	if (wing_num < 0) {
		return;
	}

	i = Wings[wing_num].wave_count;
	while (i--) {
		swp = &Ships[Wings[wing_num].ship_index[i]].weapons;
		j = swp->num_primary_banks;
		while (j--) {
			if (swp->primary_bank_weapons[j] >= 0 &&
				swp->primary_bank_weapons[j] < static_cast<int>(Weapon_info.size())) {
				arr[swp->primary_bank_weapons[j]]++;
			}
		}

		j = swp->num_secondary_banks;
		while (j--) {
			if (swp->secondary_bank_weapons[j] >= 0 &&
				swp->secondary_bank_weapons[j] < static_cast<int>(Weapon_info.size())) {
				arr[swp->secondary_bank_weapons[j]] +=
					(int)floor((swp->secondary_bank_ammo[j] * swp->secondary_bank_capacity[j] / 100.0f /
								   Weapon_info[swp->secondary_bank_weapons[j]].cargo_size) +
							   0.5f);
			}
		}
	}
}

void reassign_ship_slot(int from, int to, const FredShipSlotConfig& cfg)
{
	Assertion(from != to, "reassign_ship_slot: from == to (%d)", from);
	Assertion(from >= 0 && from < MAX_SHIPS, "reassign_ship_slot: 'from' slot %d out of range", from);
	Assertion(to >= 0 && to < MAX_SHIPS, "reassign_ship_slot: 'to' slot %d out of range", to);
	Assertion(Ships[from].objnum >= 0, "reassign_ship_slot: source slot %d is empty", from);
	Assertion(Ships[to].objnum < 0, "reassign_ship_slot: destination slot %d is occupied", to);

	// Move the ship struct itself.  Per the engine's convention, a slot with
	// objnum < 0 is considered empty; other fields in the vacated slot are
	// left as-is (unreachable through the "is this slot used" guard).
	// Move (not copy) because ship contains a unique_ptr member.
	Ships[to] = std::move(Ships[from]);
	Ships[from].objnum = -1;

	// subsys_list is an intrusive doubly-linked list whose head sentinel's
	// address is meaningful: real nodes' prev/next bookend back to &head, and
	// an empty list is self-referential.  The move copied those pointers
	// verbatim, so they still reference the old (vacated) sentinel address.
	{
		auto old_head = &Ships[from].subsys_list;
		auto new_head = &Ships[to].subsys_list;
		if (new_head->next == old_head)
		{
			// Empty list: re-init self-referential on the new head.
			new_head->next = new_head;
			new_head->prev = new_head;
		}
		else
		{
			// Non-empty: repoint the first node's prev and the last node's next.
			new_head->next->prev = new_head;
			new_head->prev->next = new_head;
		}
	}

	// Move FRED-side parallel arrays if the caller supplied them.
	if (cfg.fred_alt_names != nullptr)
	{
		strcpy_s(cfg.fred_alt_names[to], cfg.fred_alt_names[from]);
		cfg.fred_alt_names[from][0] = '\0';
	}
	if (cfg.fred_callsigns != nullptr)
	{
		strcpy_s(cfg.fred_callsigns[to], cfg.fred_callsigns[from]);
		cfg.fred_callsigns[from][0] = '\0';
	}

	// Object back-reference.
	Objects[Ships[to].objnum].instance = to;

	// Keep obj_used_list iteration order in sync with Ships[] slot order.
	resort_ships_in_obj_used_list();

	// AI back-reference (the one invariant codified by internal_integrity_check).
	Ai_info[Ships[to].ai_index].shipnum = to;
	Assertion(Ai_info[Ships[to].ai_index].shipnum == to,
		"reassign_ship_slot: Ai_info[%d].shipnum invariant broken after fixup", Ships[to].ai_index);

	// Wing membership: scan every wing and re-point any reference to the old slot.
	// (wing.special_ship is wing-relative, NOT a Ships[] index, so it is intentionally
	// not touched here.)
	for (int w = 0; w < MAX_WINGS; ++w)
	{
		if (Wings[w].wave_count == 0)
			continue;
		for (int k = 0; k < Wings[w].wave_count; ++k)
		{
			if (Wings[w].ship_index[k] == from)
				Wings[w].ship_index[k] = to;
		}
	}

	// Single-player start.
	if (Player_start_shipnum == from)
		Player_start_shipnum = to;

	// Ship_registry caches the shipnum on its entries (lookup is by name, but the
	// cached integer would otherwise go stale).
	int reg = ship_registry_get_index(Ships[to].ship_name);
	if (reg >= 0)
		Ship_registry[reg].shipnum = to;

	// FRED's current-ship pointer, if the caller is tracking one.
	if (cfg.cur_ship != nullptr && *cfg.cur_ship == from)
		*cfg.cur_ship = to;
}

void swap_ship_slots(int a, int b, const FredShipSlotConfig& cfg)
{
	if (a == b)
		return;

	Assertion(a >= 0 && a < MAX_SHIPS, "swap_ship_slots: slot 'a' %d out of range", a);
	Assertion(b >= 0 && b < MAX_SHIPS, "swap_ship_slots: slot 'b' %d out of range", b);
	Assertion(Ships[a].objnum >= 0 && Ships[b].objnum >= 0,
		"swap_ship_slots: both slots must be valid (a=%d, b=%d)", a, b);

	// Find a free temporary slot.
	int tmp = -1;
	for (int i = 0; i < MAX_SHIPS; ++i)
	{
		if (Ships[i].objnum < 0)
		{
			tmp = i;
			break;
		}
	}
	Assertion(tmp >= 0, "swap_ship_slots: no free Ships[] slot available for the temporary leg");

	// Three-leg swap; each call's preconditions hold by construction.
	reassign_ship_slot(a, tmp, cfg);
	reassign_ship_slot(b, a, cfg);
	reassign_ship_slot(tmp, b, cfg);
}

void reassign_wing_slot(int from, int to, const FredWingSlotConfig& cfg)
{
	Assertion(from != to, "reassign_wing_slot: from == to (%d)", from);
	Assertion(from >= 0 && from < MAX_WINGS, "reassign_wing_slot: 'from' slot %d out of range", from);
	Assertion(to >= 0 && to < MAX_WINGS, "reassign_wing_slot: 'to' slot %d out of range", to);
	Assertion(Wings[from].wave_count > 0, "reassign_wing_slot: source slot %d is empty", from);
	Assertion(Wings[to].wave_count == 0, "reassign_wing_slot: destination slot %d is occupied", to);

	// Move the wing struct itself.  wing::clear() is the engine's canonical
	// empty-slot state (matches ship_level_init); wave_count == 0 is the sentinel.
	Wings[to] = Wings[from];
	Wings[from].clear();

	// Move FRED-side parallel array if the caller supplied it.
	if (cfg.wing_objects != nullptr)
	{
		for (int k = 0; k < MAX_SHIPS_PER_WING; ++k)
		{
			cfg.wing_objects[to][k] = cfg.wing_objects[from][k];
			cfg.wing_objects[from][k] = -1;
		}
	}

	// Per-ship parent-wing back-reference.
	for (int i = 0; i < MAX_SHIPS; ++i)
	{
		if (Ships[i].objnum < 0)
			continue;
		if (Ships[i].wingnum == from)
			Ships[i].wingnum = to;
	}

	// FRED's current-wing pointer, if the caller is tracking one.
	if (cfg.cur_wing != nullptr && *cfg.cur_wing == from)
		*cfg.cur_wing = to;

	// Rebuild Starting/Squadron/TVT_wings caches from the parallel name arrays.
	update_custom_wing_indexes();
}

void swap_wing_slots(int a, int b, const FredWingSlotConfig& cfg)
{
	if (a == b)
		return;

	Assertion(a >= 0 && a < MAX_WINGS, "swap_wing_slots: slot 'a' %d out of range", a);
	Assertion(b >= 0 && b < MAX_WINGS, "swap_wing_slots: slot 'b' %d out of range", b);
	Assertion(Wings[a].wave_count > 0 && Wings[b].wave_count > 0,
		"swap_wing_slots: both slots must be valid (a=%d, b=%d)", a, b);

	// Find a free temporary slot.
	int tmp = -1;
	for (int i = 0; i < MAX_WINGS; ++i)
	{
		if (Wings[i].wave_count == 0)
		{
			tmp = i;
			break;
		}
	}
	Assertion(tmp >= 0, "swap_wing_slots: no free Wings[] slot available for the temporary leg");

	// Three-leg swap; each call's preconditions hold by construction.
	reassign_wing_slot(a, tmp, cfg);
	reassign_wing_slot(b, a, cfg);
	reassign_wing_slot(tmp, b, cfg);
}

// Bulk-re-sort one type's subset of obj_used_list while keeping non-matching
// entries in their original relative positions.  Each callsite supplies a
// type matcher and a key function; the i-th matching slot (in original list
// order) receives the i-th smallest matching node by key.
static void resort_obj_used_list_subset(
	bool (*matches_type)(int),
	int (*key)(const object*))
{
	SCP_vector<object*> all;
	for (auto o : list_range(&obj_used_list))
		all.push_back(o);

	SCP_vector<object*> matched;
	for (auto o : all)
		if (matches_type(o->type))
			matched.push_back(o);

	std::sort(matched.begin(), matched.end(),
		[&](const object* a, const object* b) { return key(a) < key(b); });

	list_init(&obj_used_list);
	auto it = matched.begin();
	for (auto o : all)
	{
		if (matches_type(o->type))
		{
			list_append(&obj_used_list, *it);
			++it;
		}
		else
		{
			list_append(&obj_used_list, o);
		}
	}
}

void resort_ships_in_obj_used_list()
{
	resort_obj_used_list_subset(
		[](int t) { return t == OBJ_SHIP || t == OBJ_START; },
		[](const object* o) { return o->instance; });
}
