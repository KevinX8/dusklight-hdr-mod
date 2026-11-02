#include "settings.hpp"

#include "mods/service.hpp"
#include "mods/svc/config.h"
#include "mods/svc/log.h"
#include "mods/svc/ui.h"

// Service pointers are defined by IMPORT_SERVICE in mod.cpp.
extern const LogService* svc_log;
extern const ConfigService* svc_config;
extern const UiService* svc_ui;

namespace hdr {

ConfigVarHandle cvar_paper_white = 0;
ConfigVarHandle cvar_peak = 0;
ConfigVarHandle cvar_eotf = 0;
ConfigVarHandle cvar_pb_enable = 0;
ConfigVarHandle cvar_pb0 = 0;
ConfigVarHandle cvar_pb1 = 0;
ConfigVarHandle cvar_pb2 = 0;
ConfigVarHandle cvar_pb3 = 0;
ConfigVarHandle cvar_color_boost = 0;
ConfigVarHandle cvar_saturation = 0;
ConfigVarHandle cvar_gamut = 0;
ConfigVarHandle cvar_unclip_lighting = 0;
ConfigVarHandle cvar_debug_view = 0;

int64_t cvar_int(ConfigVarHandle handle, int64_t default_value) {
    int64_t value = default_value;
    if (handle == 0 || svc_config->get_int(mod_ctx, handle, &value) != MOD_OK) {
        return default_value;
    }
    return value;
}

bool cvar_bool(ConfigVarHandle handle, bool default_value) {
    bool value = default_value;
    if (handle == 0 || svc_config->get_bool(mod_ctx, handle, &value) != MOD_OK) {
        return default_value;
    }
    return value;
}

namespace {

ModResult register_int(
    const char* name, int64_t defaultValue, ConfigVarHandle& outHandle, ModError* error) {
    ConfigVarDesc desc = CONFIG_VAR_DESC_INIT;
    desc.name = name;
    desc.type = CONFIG_VAR_INT;
    desc.default_int = defaultValue;
    if (svc_config->register_var(mod_ctx, &desc, &outHandle) != MOD_OK) {
        return mods::set_error(error, MOD_ERROR, "failed to register HDR option");
    }
    return MOD_OK;
}

ModResult register_bool(
    const char* name, bool defaultValue, ConfigVarHandle& outHandle, ModError* error) {
    ConfigVarDesc desc = CONFIG_VAR_DESC_INIT;
    desc.name = name;
    desc.type = CONFIG_VAR_BOOL;
    desc.default_bool = defaultValue;
    if (svc_config->register_var(mod_ctx, &desc, &outHandle) != MOD_OK) {
        return mods::set_error(error, MOD_ERROR, "failed to register HDR option");
    }
    return MOD_OK;
}

void add_control(UiElementHandle pane, const UiControlDesc& desc) {
    svc_ui->pane_add_control(mod_ctx, pane, &desc, nullptr);
}

void add_toggle(
    UiElementHandle pane, const char* label, ConfigVarHandle cvar, const char* help) {
    UiControlDesc control = UI_CONTROL_DESC_INIT;
    control.kind = UI_CONTROL_TOGGLE;
    control.label = label;
    control.help_rml = help;
    control.binding = UI_BINDING_CONFIG_VAR;
    control.config_var = cvar;
    add_control(pane, control);
}

void add_number(UiElementHandle pane, const char* label, ConfigVarHandle cvar, int64_t min,
    int64_t max, int64_t step, const char* help) {
    UiControlDesc control = UI_CONTROL_DESC_INIT;
    control.kind = UI_CONTROL_NUMBER;
    control.label = label;
    control.help_rml = help;
    control.binding = UI_BINDING_CONFIG_VAR;
    control.config_var = cvar;
    control.min = min;
    control.max = max;
    control.step = step;
    add_control(pane, control);
}

UiWindowHandle g_controlsWindow = 0;

ModResult build_controls_tab(
    ModContext*, UiWindowHandle, UiElementHandle left, UiElementHandle right, void*, ModError*) {
    (void)right;

    svc_ui->pane_add_section(mod_ctx, left, "HDR+ Grade");
    static const char* kEotfOptions[] = {"Power 2.2", "sRGB curve"};
    UiControlDesc control = UI_CONTROL_DESC_INIT;
    control.kind = UI_CONTROL_SELECT;
    control.label = "SDR EOTF";
    control.help_rml = "Transfer function the game's SDR frame uses. The sRGB curve matches "
                       "games that use the exact sRGB decode.";
    control.binding = UI_BINDING_CONFIG_VAR;
    control.config_var = cvar_eotf;
    control.options = kEotfOptions;
    control.option_count = 2;
    add_control(left, control);

    add_number(left, "Paper White (nits)", cvar_paper_white, 50, 1000, 10,
        "SDR white mapped to this brightness.");
    add_number(left, "Peak (nits)", cvar_peak, 100, 10000, 50,
        "Display peak brightness used for the grade and tone mapping.");

    add_toggle(left, "Perceptual Boost", cvar_pb_enable,
        "Special K Brightness: boosts highlights in PQ space around the paper white.");
    add_number(left, "Boost 0", cvar_pb0, 200, 20000, 50,
        "Special K PQBoost0 x1000 (1000 = 1.0): PQ encode range.");
    add_number(left, "Boost 1", cvar_pb1, 10, 20000, 10,
        "Special K PQBoost1 x1000 (100 = 0.1): PQ decode range; higher = brighter.");
    add_number(left, "Boost 2", cvar_pb2, 500, 1500, 5,
        "Special K PQBoost2 x1000 (1273): highlight expansion strength.");
    add_number(left, "Boost 3", cvar_pb3, 500, 1500, 5,
        "Special K PQBoost3 x1000 (500): output divisor; lower = brighter.");
    add_number(left, "Color Boost", cvar_color_boost, 0, 1000, 10,
        "How much the boost keeps per-channel color (x1000, 333 = 33%).");

    add_number(left, "Saturation", cvar_saturation, 0, 2000, 10, "x1000, 1000 = unchanged.");
    add_number(left, "Gamut Expansion", cvar_gamut, 0, 1000, 1,
        "Expands bright saturated colors toward BT.2020 (x1000, 10 = 1%).");

    add_toggle(left, "Unclip Lighting", cvar_unclip_lighting,
        "Surfaces lit past white (sun, torches, lanterns) keep that extra brightness as HDR "
        "highlights. Materials still render exactly as on GameCube.");

    static const char* kDebugOptions[] = {
        "Full grade", "1: EOTF", "2: Boost", "3: Saturation", "4: Gamut", "5: Clamp"};
    control = UI_CONTROL_DESC_INIT;
    control.kind = UI_CONTROL_SELECT;
    control.label = "Debug view";
    control.help_rml = "Stop the grade after a stage to diagnose the pipeline. Full grade "
                       "renders all stages.";
    control.binding = UI_BINDING_CONFIG_VAR;
    control.config_var = cvar_debug_view;
    control.options = kDebugOptions;
    control.option_count = 6;
    add_control(left, control);
    return MOD_OK;
}

void on_controls_window_closed(ModContext*, UiWindowHandle, void*) {
    g_controlsWindow = 0;
}

void on_open_controls(ModContext*, void*) {
    if (g_controlsWindow != 0) {
        return;
    }
    UiTabDesc tabs[1] = {UI_TAB_DESC_INIT};
    tabs[0].title = "Controls";
    tabs[0].build = build_controls_tab;
    UiWindowDesc desc = UI_WINDOW_DESC_INIT;
    desc.tabs = tabs;
    desc.tab_count = 1;
    desc.on_closed = on_controls_window_closed;
    if (svc_ui->window_push(mod_ctx, &desc, &g_controlsWindow) != MOD_OK) {
        svc_log->warn(mod_ctx, "failed to open HDR grade controls window");
    }
}

ModResult build_panel(ModContext*, UiElementHandle panel, void*, ModError*) {
    UiControlDesc control = UI_CONTROL_DESC_INIT;
    control.kind = UI_CONTROL_BUTTON;
    control.label = "Open Controls";
    control.help_rml = "HDR grade settings (EOTF, brightness mapping, boost, saturation, "
                       "gamut, unclip).";
    control.on_pressed = on_open_controls;
    add_control(panel, control);
    return MOD_OK;
}

}  // namespace

ModResult settings_register(ModError* error) {
    struct IntVar {
        const char* name;
        int64_t def;
        ConfigVarHandle& handle;
    } ints[] = {
        {"paperWhite", 200, cvar_paper_white},
        {"peak", 1000, cvar_peak},
        {"eotf", 0, cvar_eotf},
        {"pb0", 1000, cvar_pb0},
        {"pb1", 100, cvar_pb1},
        {"pb2", 1273, cvar_pb2},
        {"pb3", 500, cvar_pb3},
        {"colorBoost", 333, cvar_color_boost},
        {"saturation", 1000, cvar_saturation},
        {"gamut", 10, cvar_gamut},
        {"debugView", 0, cvar_debug_view},
    };
    for (auto& var : ints) {
        if (register_int(var.name, var.def, var.handle, error) != MOD_OK) {
            return MOD_ERROR;
        }
    }
    struct BoolVar {
        const char* name;
        bool def;
        ConfigVarHandle& handle;
    } bools[] = {
        {"pbEnable", true, cvar_pb_enable},
        {"unclipLighting", false, cvar_unclip_lighting},
    };
    for (auto& var : bools) {
        if (register_bool(var.name, var.def, var.handle, error) != MOD_OK) {
            return MOD_ERROR;
        }
    }

    // Debug view is a diagnostic: never let it persist across launches.
    svc_config->set_int(mod_ctx, cvar_debug_view, 0);

    UiModsPanelDesc panelDesc = UI_MODS_PANEL_DESC_INIT;
    panelDesc.build = build_panel;
    svc_ui->register_mods_panel(mod_ctx, &panelDesc);
    return MOD_OK;
}

}  // namespace hdr
