//
//

#include "tables.h"

#include "scripting/api/objs/decaldefinition.h"
#include "scripting/api/objs/fireballclass.h"
#include "scripting/api/objs/intelentry.h"
#include "scripting/api/objs/shipclass.h"
#include "scripting/api/objs/shiptype.h"
#include "scripting/api/objs/team_colors.h"
#include "scripting/api/objs/weaponclass.h"
#include "scripting/api/objs/wingformation.h"

#include "decals/decals.h"
#include "fireball/fireballs.h"
#include "menuui/techmenu.h"
#include "mission/missionmessage.h"
#include "ship/ship.h"
#include "weapon/weapon.h"


extern bool Ships_inited;
extern bool Weapons_inited;
extern bool Intel_inited;


namespace scripting {
namespace api {

//**********LIBRARY: Tables
ADE_LIB(l_Tables, "Tables", "tb", "Tables library");

//*****SUBLIBRARY: Tables/ShipClasses
ADE_LIB_DERIV(l_Tables_ShipClasses, "ShipClasses", NULL, NULL, l_Tables);
ADE_INDEXER(l_Tables_ShipClasses, "number/string IndexOrName", "Array of ship classes", "shipclass", "Ship class handle, or invalid handle if index is invalid")
{
	if(!Ships_inited)
		return ade_set_error(L, "o", l_Shipclass.Set(-1));

	const char* name;
	if(!ade_get_args(L, "*s", &name))
		return ade_set_error(L, "o", l_Shipclass.Set(-1));

	int idx = ship_info_lookup(name);

	if(idx < 0) {
		try {
			idx = std::stoi(name);
			idx--; // Lua->FS2
		} catch (const std::exception&) {
			// Not a number
			return ade_set_error(L, "o", l_Shipclass.Set(-1));
		}

		if (idx < 0 || idx >= ship_info_size()) {
			return ade_set_error(L, "o", l_Shipclass.Set(-1));
		}
	}

	return ade_set_args(L, "o", l_Shipclass.Set(idx));
}

ADE_FUNC(__len, l_Tables_ShipClasses, NULL, "Number of ship classes", "number", "Number of ship classes, or 0 if ship classes haven't been loaded yet")
{
	if(!Ships_inited)
		return ade_set_args(L, "i", 0);	//No ships loaded...should be 0

	return ade_set_args(L, "i", Ship_info.size());
}

//*****SUBLIBRARY: Tables/ShipTypes
ADE_LIB_DERIV(l_Tables_ShipTypes, "ShipTypes", nullptr, nullptr, l_Tables);
ADE_INDEXER(l_Tables_ShipTypes, "number/string IndexOrName", "Array of ship types", "shiptype", "Ship type handle, or invalid handle if index is invalid")
{
	if (!Ships_inited)
		return ade_set_error(L, "o", l_Shiptype.Set(-1));

	const char* name;
	if (!ade_get_args(L, "*s", &name))
		return ade_set_error(L, "o", l_Shiptype.Set(-1));

	int idx = ship_type_name_lookup(name);

	if (idx < 0) {
		try {
			idx = std::stoi(name);
			idx--; // Lua->FS2
		}
		catch (const std::exception&) {
			// Not a number
			return ade_set_error(L, "o", l_Shiptype.Set(-1));
		}

		if (idx < 0 || idx >= (int)Ship_types.size()) {
			return ade_set_error(L, "o", l_Shiptype.Set(-1));
		}
	}

	return ade_set_args(L, "o", l_Shiptype.Set(idx));
}

ADE_FUNC(__len, l_Tables_ShipTypes, nullptr, "Number of ship types", "number", "Number of ship types, or 0 if ship types haven't been loaded yet")
{
	if (!Ships_inited)
		return ade_set_args(L, "i", 0);	//No ship types loaded...should be 0

	return ade_set_args(L, "i", Ship_types.size());
}

//*****SUBLIBRARY: Tables/WeaponClasses
ADE_LIB_DERIV(l_Tables_WeaponClasses, "WeaponClasses", NULL, NULL, l_Tables);

ADE_INDEXER(l_Tables_WeaponClasses, "number/string IndexOrWeaponName", "Array of weapon classes", "weaponclass", "Weapon class handle, or invalid handle if index is invalid")
{
	if(!Weapons_inited)
		return ade_set_error(L, "o", l_Weaponclass.Set(-1));

	const char* name;
	if(!ade_get_args(L, "*s", &name))
		return 0;

	int idx = weapon_info_lookup(name);

	if(idx < 0) {
		idx = atoi(name);

		// atoi is good enough here, 0 is invalid anyway
		if (idx > 0)
		{
			idx--; // Lua --> C/C++
		}
		else
		{
			return ade_set_args(L, "o", l_Weaponclass.Set(-1));
		}
	}

	return ade_set_args(L, "o", l_Weaponclass.Set(idx));
}

ADE_FUNC(__len, l_Tables_WeaponClasses, NULL, "Number of weapon classes", "number", "Number of weapon classes, or 0 if weapon classes haven't been loaded yet")
{
	if(!Weapons_inited)
		return ade_set_args(L, "i", 0);

	return ade_set_args(L, "i", weapon_info_size());
}

//*****SUBLIBRARY: Tables/IntelEntries
ADE_LIB_DERIV(l_Tables_IntelEntries, "IntelEntries", nullptr, nullptr, l_Tables);
ADE_INDEXER(l_Tables_IntelEntries, "number/string IndexOrName", "Array of intel entries", "intel_entry", "Intel entry handle, or invalid handle if index is invalid")
{
	if(!Intel_inited)
		return ade_set_error(L, "o", l_Intelentry.Set(-1));

	const char* name;
	if(!ade_get_args(L, "*s", &name))
		return ade_set_error(L, "o", l_Intelentry.Set(-1));

	int idx = intel_info_lookup(name);

	if(idx < 0) {
		try {
			idx = std::stoi(name);
			idx--; // Lua->FS2
		} catch (const std::exception&) {
			// Not a number
			return ade_set_error(L, "o", l_Intelentry.Set(-1));
		}

		if (idx < 0 || idx >= intel_info_size()) {
			return ade_set_error(L, "o", l_Intelentry.Set(-1));
		}
	}

	return ade_set_args(L, "o", l_Intelentry.Set(idx));
}

ADE_FUNC(__len, l_Tables_IntelEntries, nullptr, "Number of intel entries", "number", "Number of intel entries, or 0 if intel entries haven't been loaded yet")
{
	if(!Intel_inited)
		return ade_set_args(L, "i", 0);	//No intel loaded...should be 0

	return ade_set_args(L, "i", intel_info_size());
}

//*****SUBLIBRARY: Tables/FireballClasses
ADE_LIB_DERIV(l_Tables_FireballClasses, "FireballClasses", NULL, NULL, l_Tables);

ADE_INDEXER(l_Tables_FireballClasses, "number/string IndexOrFireballUniqueID", "Array of fireball classes", "fireballclass", "Fireball class handle, or invalid handle if index is invalid")
{
	if (!fireballs_inited)
		return ade_set_error(L, "o", l_Fireballclass.Set(-1));

	const char* name;
	if (!ade_get_args(L, "*s", &name))
		return 0;

	int idx = fireball_info_lookup(name);

	if (idx < 0) {
		idx = atoi(name);

		// atoi is good enough here, 0 is invalid anyway
		if (idx > 0)
		{
			idx--; // Lua --> C/C++
		}
		else
		{
			return ade_set_args(L, "o", l_Fireballclass.Set(-1));
		}
	}

	return ade_set_args(L, "o", l_Fireballclass.Set(idx));
}

ADE_FUNC(__len, l_Tables_FireballClasses, NULL, "Number of fireball classes", "number", "Number of fireball classes, or 0 if fireball classes haven't been loaded yet")
{
	if (!fireballs_inited)
		return ade_set_args(L, "i", 0);

	return ade_set_args(L, "i", static_cast<int>(Fireball_info.size()));
}

//*****SUBLIBRARY: Tables/SimulatedSpeechOverrides
ADE_LIB_DERIV(l_Tables_SimulatedSpeechOverrides, "SimulatedSpeechOverrides", NULL, NULL, l_Tables);

ADE_INDEXER(l_Tables_SimulatedSpeechOverrides, "number Index", nullptr, "string", "Truncated filenames of simulated speech overrides or empty string if index is out of range.")
{
	int idx = -1;
	if (!ade_get_args(L, "*i", &idx)) {
		return ade_set_error(L, "s", "");
	}
	if (idx > 0 && (size_t) idx <= Generic_message_filenames.size()) {
		idx--; //Convert from Lua to C, as lua indices start from 1, not 0
		return ade_set_args(L, "s", Generic_message_filenames[idx]);
	}

	return ade_set_error(L, "s", "");
}

ADE_FUNC(__len, l_Tables_SimulatedSpeechOverrides, nullptr, "Number of simulated speech overrides", "number", "Number of simulated speech overrides")
{
	return ade_set_args(L, "i", Generic_message_filenames.size());
}

//*****SUBLIBRARY: Tables/DecalDefinitions
ADE_LIB_DERIV(l_Tables_DecalDefinitions, "DecalDefinitions", nullptr, nullptr, l_Tables);

ADE_INDEXER(l_Tables_DecalDefinitions, "number/string IndexOrDecalName", "Array of decal definitions", "decaldefinition", "Decal definition handle, or invalid handle if name is invalid")
{
	const char* name;
	if (!ade_get_args(L, "*s", &name))
		return 0;

	int idx = decals::findDecalDefinition(name);

	if (idx < 0)
	{
		idx = atoi(name);

		// atoi is good enough here, 0 is invalid anyway
		if (idx > 0)
		{
			idx--; // Lua --> C/C++
		}
		else
		{
			return ade_set_args(L, "o", l_DecalDefinitionclass.Set(-1));
		}
	}

	return ade_set_args(L, "o", l_DecalDefinitionclass.Set(idx));
}

ADE_FUNC(__len, l_Tables_DecalDefinitions, nullptr, "Number of decal definitions", "number", "Number of decal definitions, or 0 if decal definitions haven't been loaded yet")
{
	return ade_set_args(L, "i", static_cast<int>(decals::DecalDefinitions.size()));
}

ADE_FUNC(isDecalSystemActive, l_Tables, nullptr, "Returns whether the decal system is able to work on the current machine", "boolean", "true if active, false if inactive")
{
	return ade_set_args(L, "b", decals::Decal_system_active);
}

ADE_VIRTVAR(DecalOptionActive, l_Tables, "boolean", "Gets or sets whether the decal option is active (note, decals will only work if the decal system is able to work on the current machine)", "boolean", "true if active, false if inactive")
{
	bool choice = decals::Decal_option_active;

	if (ADE_SETTING_VAR && ade_get_args(L, "*b", &choice)) {
		decals::Decal_option_active = choice;
	}

	return ade_set_args(L, "b", choice);
}

//*****SUBLIBRARY: Tables/WingFormations
ADE_LIB_DERIV(l_Tables_WingFormations, "WingFormations", nullptr, nullptr, l_Tables);

ADE_INDEXER(l_Tables_WingFormations, "number/string IndexOrName", "Array of wing formations", "wingformation", "Wing formation handle, or invalid handle if name is invalid")
{
	const char* name;
	if (!ade_get_args(L, "*s", &name))
		return 0;

	// default is always the first formation
	if (!stricmp(name, "Default"))
		return ade_set_args(L, "o", l_WingFormation.Set(0));

	// look up by name
	int idx = wing_formation_lookup(name);

	// if found, add 1 since "Default" is always first
	if (idx >= 0)
	{
		idx++;
	}
	// look up by number
	else
	{
		// atoi is good enough here, 0 is invalid anyway
		idx = atoi(name);
		if (idx > 0)
		{
			idx--; // Lua --> C/C++
		}
		else
		{
			return ade_set_args(L, "o", l_WingFormation.Set(-1));
		}
	}

	return ade_set_args(L, "o", l_WingFormation.Set(idx));
}

ADE_FUNC(__len, l_Tables_WingFormations, nullptr, "Number of wing formations", "number", "Number of wing formations")
{
	// add 1 since "Default" is always first
	return ade_set_args(L, "i", static_cast<int>(Wing_formations.size()) + 1);
}

//*****SUBLIBRARY: Tables/TeamColors
ADE_LIB_DERIV(l_Tables_TeamColors, "TeamColors", nullptr, nullptr, l_Tables);

ADE_INDEXER(l_Tables_TeamColors, "number/string IndexOrName", "Array of team colors", "teamcolor", "Team color handle, or invalid handle if name is invalid")
{
	const char* name;
	if (!ade_get_args(L, "*s", &name))
		return ade_set_error(L, "o", l_TeamColor.Set(-1));

	// look up by name
	for (int i = 0; i < static_cast<int>(Team_Names.size()); ++i) {
		if (!stricmp(Team_Names[i].c_str(), name)) {
			return ade_set_args(L, "o", l_TeamColor.Set(i));
		}
	}

	// look up by number
	int idx = atoi(name);
	if (idx > 0) {
		idx--; // Lua --> C/C++
	} else {
		return ade_set_args(L, "o", l_TeamColor.Set(-1));
	}

	return ade_set_args(L, "o", l_TeamColor.Set(idx));
}

ADE_FUNC(__len, l_Tables_TeamColors, nullptr, "Number of  team colors", "number", "Number of team colors")
{
	return ade_set_args(L, "i", static_cast<int>(Team_Names.size()));
}

}
}
