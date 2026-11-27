#include "profiles.h"
#include "conf.h"
#include "action_stack.h"
#include "shooting.h"
#include "lens.h"
#include "properties.h"
#include "sx280_logger.h"

/*
 * SX280 Enhanced Adaptive Photo Profiles (Phase 15 Corrected)
 * Target: Canon PowerShot SX280 HS (1/2.3" 12.1MP BSI-CMOS, 20x 4.5-90.0mm f/3.5-6.8)
 *
 * CRITICAL FIXES (Phase 15):
 *   1. ISO override: conf.iso_override_value expects MARKET ISO (80, 100, 400),
 *      NOT APEX96 Sv96 (461, 672)! Fixed via iso_market field.
 *   2. Tv override: conf.tv_enum_type MUST be set explicitly:
 *      - TV_OVERRIDE_SHORT_EXP (1): conf.tv_override_short_exp in 1/100000s (100 = 1/1000s)
 *      - TV_OVERRIDE_LONG_EXP  (2): conf.tv_override_long_exp in seconds (2 = 2.0s)
 *   3. Av override: index into aperture_sizes_table (1 = f/4.0 wide sweet spot),
 *      guarded against exceeding physical minimum aperture at tele zoom.
 *   4. Global override gate: conf.override_disable is cleared ONLY when a profile
 *      explicitly activates an override, otherwise left off for safe normal operation.
 *   5. Anti-Brick Safety: PROPCASE_IS_MODE writes remain strictly FROZEN.
 */

static sx280_profile_t active_profile = PROFILE_RAW_MAXIMUM;

static const sx280_profile_config_t profile_configs[PROFILE_COUNT] = {
    {
        .id = PROFILE_RAW_MAXIMUM,
        .name_de = "1. RAW Maximum",
        .name_en = "1. RAW Maximum",
        .description_de = "12-Bit DNG RAW, ISO 80, f/4.0 WW-Optimum, IS Shoot Only",
        .enable_dng = 1,
        .iso_market = 80,
        .tv_mode = -1,
        .tv_param = 0,
        .av_table_index = 1, // f/4.0 at wide angle (diffraction-safe sweet spot)
        .is_mode = 1,
        .my_colors_mode = 0,
        .custom_sharpness = -1,
        .custom_contrast = -1,
        .icontrast_mode = 0
    },
    {
        .id = PROFILE_JPEG_MAXIMUM,
        .name_de = "2. JPEG Maximum",
        .name_en = "2. JPEG Maximum",
        .description_de = "Superfine JPEG, ISO 80, Neutral, DNG Aus",
        .enable_dng = 0,
        .iso_market = 80,
        .tv_mode = -1,
        .tv_param = 0,
        .av_table_index = 1, // f/4.0 at wide angle
        .is_mode = 1,
        .my_colors_mode = 1,
        .custom_sharpness = -1,
        .custom_contrast = -1,
        .icontrast_mode = 0
    },
    {
        .id = PROFILE_DAYLIGHT,
        .name_de = "3. Tageslicht",
        .name_en = "3. Daylight",
        .description_de = "Landschaft Freihand, f/4.0 WW, Auto-ISO, DNG Aktiv",
        .enable_dng = 1,
        .iso_market = 0,     // Canon Auto-ISO
        .tv_mode = -1,
        .tv_param = 0,
        .av_table_index = 1, // f/4.0 at wide angle
        .is_mode = 0,
        .my_colors_mode = 0,
        .custom_sharpness = -1,
        .custom_contrast = -1,
        .icontrast_mode = 0
    },
    {
        .id = PROFILE_LOW_LIGHT,
        .name_de = "4. Schwaches Licht",
        .name_en = "4. Low Light",
        .description_de = "Abend / Innenraum, Offenblende, ISO 400, DNG Aktiv",
        .enable_dng = 1,
        .iso_market = 400,
        .tv_mode = -1,
        .tv_param = 0,
        .av_table_index = -1, // Keep widest physical aperture across zoom range
        .is_mode = 0,
        .my_colors_mode = 0,
        .custom_sharpness = -1,
        .custom_contrast = -1,
        .icontrast_mode = 0
    },
    {
        .id = PROFILE_NIGHT_TRIPOD,
        .name_de = "5. Nacht / Stativ",
        .name_en = "5. Night / Tripod",
        .description_de = "Langzeitbelichtung 2s, ISO 80, DNG Aktiv (IS im Canon-Menue Aus)",
        .enable_dng = 1,
        .iso_market = 80,
        .tv_mode = TV_OVERRIDE_LONG_EXP,
        .tv_param = 2,       // 2 seconds (integer seconds for TV_OVERRIDE_LONG_EXP)
        .av_table_index = 1, // f/4.0 at wide angle
        .is_mode = 2,
        .my_colors_mode = 0,
        .custom_sharpness = -1,
        .custom_contrast = -1,
        .icontrast_mode = 0
    },
    {
        .id = PROFILE_ACTION_SPORTS,
        .name_de = "6. Bewegte Motive",
        .name_en = "6. Action / Sports",
        .description_de = "Sport / Action, Kurzzeitbelichtung 1/1000s, Auto-ISO, DNG Aktiv",
        .enable_dng = 1,
        .iso_market = 0,
        .tv_mode = TV_OVERRIDE_SHORT_EXP,
        .tv_param = 100,     // 100 / 100000 s = 0.001 s = 1/1000 s
        .av_table_index = -1, // Open aperture for maximum light
        .is_mode = 0,
        .my_colors_mode = 0,
        .custom_sharpness = 0,
        .custom_contrast = 0,
        .icontrast_mode = 0
    },
    {
        .id = PROFILE_MACRO,
        .name_de = "7. Makro",
        .name_en = "7. Macro",
        .description_de = "Nahaufnahme, f/4.0 WW-Optimum, ISO 80, DNG Aktiv",
        .enable_dng = 1,
        .iso_market = 80,
        .tv_mode = -1,
        .tv_param = 0,
        .av_table_index = 1, // f/4.0 sweet spot (avoids >f/5.6 diffraction on 1.34um pitch)
        .is_mode = 1,
        .my_colors_mode = 0,
        .custom_sharpness = -1,
        .custom_contrast = -1,
        .icontrast_mode = 0
    },
    {
        .id = PROFILE_HDR_BRACKET,
        .name_de = "8. HDR-Aufnahmereihe",
        .name_en = "8. HDR Bracketing",
        .description_de = "Belichtungsreihe, f/4.0 WW, ISO 80, DNG Aktiv",
        .enable_dng = 1,
        .iso_market = 80,
        .tv_mode = -1,
        .tv_param = 0,
        .av_table_index = 1,
        .is_mode = 2,
        .my_colors_mode = 0,
        .custom_sharpness = -1,
        .custom_contrast = -1,
        .icontrast_mode = 0
    },
    {
        .id = PROFILE_TIMELAPSE,
        .name_de = "9. Zeitraffer",
        .name_en = "9. Time-Lapse",
        .description_de = "Intervallaufnahme, Auto-Belichtung, DNG Aktiv",
        .enable_dng = 1,
        .iso_market = 0,
        .tv_mode = -1,
        .tv_param = 0,
        .av_table_index = -1,
        .is_mode = 1,
        .my_colors_mode = 0,
        .custom_sharpness = -1,
        .custom_contrast = -1,
        .icontrast_mode = 0
    },
    {
        .id = PROFILE_CUSTOM,
        .name_de = "10. Standard / Aus",
        .name_en = "10. Standard / Reset",
        .description_de = "Alle CHDK-Overrides deaktiviert (Standard Canon Automatik)",
        .enable_dng = 0,
        .iso_market = 0,
        .tv_mode = -1,
        .tv_param = 0,
        .av_table_index = -1,
        .is_mode = 1,
        .my_colors_mode = 0,
        .custom_sharpness = 0,
        .custom_contrast = 0,
        .icontrast_mode = 0
    }
};

void sx280_profiles_init(void) {
    active_profile = PROFILE_RAW_MAXIMUM;
}

const sx280_profile_config_t* sx280_get_profile(sx280_profile_t profile_id) {
    if (profile_id < 0 || profile_id >= PROFILE_COUNT) {
        return &profile_configs[PROFILE_RAW_MAXIMUM];
    }
    return &profile_configs[profile_id];
}

sx280_profile_t sx280_get_active_profile(void) {
    return active_profile;
}

int sx280_check_conflicts(void) {
    // Check Canon propcases for Tracking AF or Servo AF conflicts
#ifdef PROPCASE_SERVO_AF
    int af_frame = shooting_get_prop(PROPCASE_AF_FRAME);
    int servo_af = shooting_get_prop(PROPCASE_SERVO_AF);

    if (af_frame == 2 || servo_af == 1) {
        // Warning: Overrides are bypassed by Canon firmware during Tracking/Servo AF
        return 1;
    }
#endif
    return 0; // No conflicts
}

int sx280_apply_profile(sx280_profile_t profile_id) {
    const sx280_profile_config_t *cfg = sx280_get_profile(profile_id);
    if (!cfg) return 0;

    active_profile = profile_id;
    int any_override = 0;

    // 1. DNG Output Toggle
    conf.save_raw = cfg->enable_dng;
    conf.dng_raw = cfg->enable_dng;

    // 2. ISO Override (Market ISO, NOT Sv96!)
    if (cfg->iso_market > 0) {
        conf.iso_override_value = cfg->iso_market;
        conf.iso_override_koef = 1;
        any_override = 1;
    } else {
        conf.iso_override_value = 0;
        conf.iso_override_koef = 0;
    }

    // 3. Tv Shutter Override (Explicit tv_enum_type handling!)
    if (cfg->tv_mode == TV_OVERRIDE_SHORT_EXP && cfg->tv_param > 0) {
        conf.tv_override_enabled = 1;
        conf.tv_enum_type = TV_OVERRIDE_SHORT_EXP;
        conf.tv_override_short_exp = cfg->tv_param;
        any_override = 1;
    } else if (cfg->tv_mode == TV_OVERRIDE_LONG_EXP && cfg->tv_param > 0) {
        conf.tv_override_enabled = 1;
        conf.tv_enum_type = TV_OVERRIDE_LONG_EXP;
        conf.tv_override_long_exp = cfg->tv_param;
        any_override = 1;
    } else if (cfg->tv_mode == TV_OVERRIDE_EV_STEP) {
        conf.tv_override_enabled = 1;
        conf.tv_enum_type = TV_OVERRIDE_EV_STEP;
        conf.tv_override_value = cfg->tv_param;
        any_override = 1;
    } else {
        conf.tv_override_enabled = 0;
    }

    // 4. Av Aperture Optimization (Index into aperture_sizes_table)
    // On SX280 HS (4.5-90.0mm f/3.5-6.8), Index 1 = f/4.0 (384 Av96).
    // Only apply f/4.0 override if lens is near wide end (zoom_point <= 20),
    // otherwise let Canon use physical open/metered aperture (avoids f/4.0 request at f/5.6-6.8 tele).
    if (cfg->av_table_index >= 0 && lens_get_zoom_point() <= 20) {
        conf.av_override_enabled = 1;
        conf.av_override_value = cfg->av_table_index;
        any_override = 1;
    } else {
        conf.av_override_enabled = 0;
    }

    // 5. Master override enable gate
    conf.override_disable = any_override ? 0 : 1;

    // 6. Anti-Brick Safety: PROPCASE_IS_MODE writing is intentionally FROZEN.
    // Overwriting IS_MODE on DIGIC 6 cameras can trigger firmware assertion crashes
    // and gyro race conditions. Users set IS via standard Canon menu.

    // 7. Conflicts check
    sx280_check_conflicts();

    sx280_log_profile((int)profile_id, cfg->enable_dng, cfg->iso_market, cfg->tv_mode,
                      conf.av_override_enabled ? conf.av_override_value : -1, conf.override_disable);

    return 1; // Profile successfully applied
}
