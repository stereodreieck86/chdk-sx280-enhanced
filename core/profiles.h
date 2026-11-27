#ifndef PROFILES_H
#define PROFILES_H

#include "camera_info.h"

// SX280 Enhanced Profile IDs (Canon PowerShot SX280 HS: 20x 4.5-90.0mm f/3.5-6.8)
typedef enum {
    PROFILE_RAW_MAXIMUM = 0,
    PROFILE_JPEG_MAXIMUM,
    PROFILE_DAYLIGHT,
    PROFILE_LOW_LIGHT,
    PROFILE_NIGHT_TRIPOD,
    PROFILE_ACTION_SPORTS,
    PROFILE_MACRO,
    PROFILE_HDR_BRACKET,
    PROFILE_TIMELAPSE,
    PROFILE_CUSTOM,
    PROFILE_COUNT
} sx280_profile_t;

typedef struct {
    sx280_profile_t id;
    const char *name_de;
    const char *name_en;
    const char *description_de;
    int enable_dng;
    int iso_market;         // 0 = Auto (no override), else Market ISO (e.g. 80, 100, 200, 400, 800)
    int tv_mode;            // -1 = Auto (no override), 0 = EV_STEP, 1 = SHORT_EXP (1/100000s units), 2 = LONG_EXP (seconds)
    int tv_param;           // Value interpreted according to tv_mode
    int av_table_index;     // -1 = Auto (no override), else index into aperture_sizes_table (1 = f/4.0 at wide)
    int is_mode;            // Canon Propset 6: 0 = Continuous, 1 = Shoot Only, 2 = OFF (Writes Frozen for Anti-Brick Safety)
    int my_colors_mode;     // Advisory (user sets in Canon menu)
    int custom_sharpness;   // Advisory (-1 or -2)
    int custom_contrast;    // Advisory (-1)
    int icontrast_mode;     // Advisory (0 = Off)
} sx280_profile_config_t;

// Function Prototypes
void sx280_profiles_init(void);
int sx280_apply_profile(sx280_profile_t profile_id);
int sx280_check_conflicts(void);
const sx280_profile_config_t* sx280_get_profile(sx280_profile_t profile_id);
sx280_profile_t sx280_get_active_profile(void);

#endif // PROFILES_H
