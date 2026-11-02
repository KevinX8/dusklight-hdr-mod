// Config vars and mods panel for the HDR grade.
#pragma once

#include "mods/svc/config.h"
#include "mods/svc/ui.h"

namespace hdr {

extern ConfigVarHandle cvar_paper_white;
extern ConfigVarHandle cvar_peak;
extern ConfigVarHandle cvar_eotf;
extern ConfigVarHandle cvar_pb_enable;
extern ConfigVarHandle cvar_pb0;
extern ConfigVarHandle cvar_pb1;
extern ConfigVarHandle cvar_pb2;
extern ConfigVarHandle cvar_pb3;
extern ConfigVarHandle cvar_color_boost;
extern ConfigVarHandle cvar_saturation;
extern ConfigVarHandle cvar_gamut;
extern ConfigVarHandle cvar_unclip_lighting;
extern ConfigVarHandle cvar_glow_strength;
extern ConfigVarHandle cvar_debug_view;

// Float-valued vars are stored as ints scaled by 1000.
int64_t cvar_int(ConfigVarHandle handle, int64_t default_value);
bool cvar_bool(ConfigVarHandle handle, bool default_value);

ModResult settings_register(ModError* error);

}  // namespace hdr
