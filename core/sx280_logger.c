#include "sx280_logger.h"
#include "camera_info.h"
#include "conf.h"
#include "clock.h"
#include "battery.h"
#include "temperature.h"
#include "meminfo.h"
#include "file_counter.h"
#include "keyboard.h"
#include "shooting.h"
#include "lens.h"
#include "callfunc.h"
#include "stdlib.h"
#include "string.h"
#include "time.h"

extern unsigned _ExecuteEventProcedure(const char *name, ...);
extern void _SetAutoShutdownTime(int t);
extern void disable_shutdown(void);

// ============================================================================
// SX280 ENHANCED - NATIVE C FLIGHT RECORDER & POWER KEEPALIVE
// ============================================================================
// Design guarantees:
//   1. All hooks (mbox, console, module_load, raw, profile) append ONLY into a
//      4 KB static RAM ring buffer (non-blocking, safe from any task).
//   2. Only core_spytask() flushes to A/CHDK/LOGS/FLIGHT.LOG, and NEVER while
//      a still capture, RAW write, or 1080p video recording is in progress.
//   3. Auto-rotates FLIGHT.LOG -> FLIGHT.PREV.LOG above 512 KB.
//   4. Silently dumps Canon DryOS ROMLOG (including crashes from non-CHDK
//      boots!) to A/CHDK/LOGS/ROMLOG_BOOT.LOG once ~3s after boot.
//   5. Periodically reinforces LockMainPower + SetAutoShutdownTime(0) when
//      conf.alt_prevent_shutdown == ALT_PREVENT_SHUTDOWN_ALWAYS.
// ============================================================================

#define FLIGHT_LOG_PATH      "A/CHDK/LOGS/FLIGHT.LOG"
#define FLIGHT_LOG_PREV      "A/CHDK/LOGS/FLIGHT.PREV.LOG"
#define ROMLOG_BOOT_PATH     "A/CHDK/LOGS/ROMLOG_BOOT.LOG"
#define FLIGHT_LOG_MAX_BYTES (512 * 1024)
#define LOG_BUF_SIZE         4096

static char log_buf[LOG_BUF_SIZE];
static volatile int log_len = 0;
static volatile int log_dropped = 0;
static int logger_inited = 0;
static int romlog_dumped = 0;
static unsigned int boot_tick = 0;
static unsigned int last_flush_tick = 0;
static unsigned int last_hb_tick = 0;
static unsigned int last_pwr_tick = 0;

// Previous tracked state for instant delta logging
static int prev_mode_play = -1;
static int prev_mode_video = -1;
static int prev_mode_shooting = -1;
static int prev_gui_mode = -1;
static int prev_gui_alt = -1;
static int prev_movie_status = -1;
static int prev_shoot_prog = -1;
static int prev_shoot_in_prog = -1;
static int prev_script_run = -1;
static int prev_usb_bit = -1;
static int prev_zoom = -1;
static int prev_half_press = -1;
static int prev_last_key = -1;
static long prev_exp_cnt = -1;
static long prev_vbatt = -1;

// Sanitize newlines/control chars for single-line log entries
static void copy_clean(char *dst, const char *src, int maxlen) {
    int i = 0;
    if (!src) {
        dst[0] = 0;
        return;
    }
    while (src[i] && i < maxlen - 1) {
        char c = src[i];
        if (c == '\n' || c == '\r' || c == '\t') c = ' ';
        dst[i] = c;
        i++;
    }
    dst[i] = 0;
}

static void log_append_raw(const char *line) {
    if (!line) return;
    int n = strlen(line);
    if (n <= 0) return;

    int cur = log_len;
    if (cur + n + 2 >= LOG_BUF_SIZE) {
        log_dropped++;
        return;
    }
    memcpy(&log_buf[cur], line, n);
    cur += n;
    if (log_buf[cur - 1] != '\n') {
        log_buf[cur++] = '\n';
    }
    log_len = cur;
}

static void flush_to_sd(void) {
    if (log_len <= 0) return;

    // NEVER touch SD during active still shot, RAW save, or video recording
    if (shooting_in_progress()) return;
    if (camera_info.state.state_shooting_progress == SHOOTING_PROGRESS_PROCESSING) return;
    if (get_movie_status() == VIDEO_RECORD_IN_PROGRESS) return;

    // Rotate if log exceeds 512 KB
    struct stat st;
    if (stat(FLIGHT_LOG_PATH, &st) == 0 && st.st_size >= FLIGHT_LOG_MAX_BYTES) {
        remove(FLIGHT_LOG_PREV);
        rename(FLIGHT_LOG_PATH, FLIGHT_LOG_PREV);
    }

    int fd = open(FLIGHT_LOG_PATH, O_WRONLY | O_CREAT | O_APPEND, 0777);
    if (fd >= 0) {
        int to_write = log_len;
        write(fd, log_buf, to_write);
        if (log_dropped > 0) {
            char drop_msg[64];
            sprintf(drop_msg, "%08u WARN dropped=%d\n", (unsigned)get_tick_count(), log_dropped);
            write(fd, drop_msg, strlen(drop_msg));
            log_dropped = 0;
        }
        close(fd);
        log_len = 0;
        last_flush_tick = (unsigned)get_tick_count();
    }
}

static void dump_canon_romlog_silent(void) {
    unsigned args[3];
    unsigned rc_init;
    unsigned rc_dump;

    if (stat(ROMLOG_BOOT_PATH, 0) == 0) {
        remove(ROMLOG_BOOT_PATH);
    }

    args[0] = (unsigned)"SystemEventInit";
    rc_init = call_func_ptr(_ExecuteEventProcedure, args, 1);
    if (rc_init == (unsigned)-1) {
        args[0] = (unsigned)"System.Create";
        rc_init = call_func_ptr(_ExecuteEventProcedure, args, 1);
    }

    if (rc_init != (unsigned)-1) {
        args[0] = (unsigned)"GetLogToFile";
        args[1] = (unsigned)ROMLOG_BOOT_PATH;
        args[2] = 1;
        rc_dump = call_func_ptr(_ExecuteEventProcedure, args, 3);
    } else {
        rc_dump = (unsigned)-1;
    }

    char line[140];
    sprintf(line, "%08u ROMLOG init=%d dump=%d path=%s",
            (unsigned)get_tick_count(), (int)rc_init, (int)rc_dump, ROMLOG_BOOT_PATH);
    log_append_raw(line);
}

void sx280_logger_boot(void) {
    if (logger_inited) return;
    logger_inited = 1;
    boot_tick = (unsigned)get_tick_count();
    last_flush_tick = boot_tick;
    last_hb_tick = boot_tick;
    last_pwr_tick = boot_tick;

    mkdir_if_not_exist("A/CHDK");
    mkdir_if_not_exist("A/CHDK/LOGS");

    struct tm *ttm = get_localtime();
    cam_meminfo mi;
    GetMemInfo(&mi);

    long vbatt = stat_get_vbatt();
    int usb = get_usb_bit_physw_mod();
    long expc = get_exposure_counter();

    char line[256];
    sprintf(line, "=== BOOT %04d-%02d-%02d %02d:%02d:%02d tick=%u ver=%s-%s plat=%s/%s ===",
            ttm ? (1900 + ttm->tm_year) : 0,
            ttm ? (ttm->tm_mon + 1) : 0,
            ttm ? ttm->tm_mday : 0,
            ttm ? ttm->tm_hour : 0,
            ttm ? ttm->tm_min : 0,
            ttm ? ttm->tm_sec : 0,
            boot_tick,
            camera_info.build_number ? camera_info.build_number : "?",
            camera_info.build_svnrev ? camera_info.build_svnrev : "?",
            camera_info.platform ? camera_info.platform : "?",
            camera_info.platformsub ? camera_info.platformsub : "?");
    log_append_raw(line);

    sprintf(line, "%08u HEALTH vbatt=%ld topt=%d tccd=%d tbat=%d free=%d usb=%d exp=%ld zoom=%d",
            boot_tick, vbatt, get_optical_temp(), get_ccd_temp(), get_battery_temp(),
            mi.free_size, usb, expc, lens_get_zoom_point());
    log_append_raw(line);

    sprintf(line, "%08u CFG platid=%d raw=%d dng=%d dngver=%d vmode=%d vq=%d vbr=%d ovrdis=%d simple=%d prevsd=%d modlog=%d mcol=%02x%02x%02x%02x",
            boot_tick,
            conf.platformid, conf.save_raw, conf.dng_raw, conf.dng_version,
            conf.video_mode, conf.video_quality, conf.video_bitrate,
            conf.override_disable, conf.simple_ui_mode, conf.alt_prevent_shutdown,
            conf.module_logging,
            (unsigned)conf.menu_color.fg.col, (unsigned)conf.menu_color.fg.type,
            (unsigned)conf.menu_color.bg.col, (unsigned)conf.menu_color.bg.type);
    log_append_raw(line);

    // Reinforce anti-shutdown immediately on boot if configured ALWAYS
    if (conf.alt_prevent_shutdown == ALT_PREVENT_SHUTDOWN_ALWAYS) {
        disable_shutdown();
        _SetAutoShutdownTime(0);
    }

    prev_vbatt = vbatt;
    flush_to_sd();
}

void sx280_logger_tick(void) {
    if (!logger_inited) {
        sx280_logger_boot();
        return;
    }

    unsigned int now = (unsigned)get_tick_count();

    // 1. Silent Canon ROMLOG export once ~3s after boot (captures even non-CHDK crashes!)
    if (!romlog_dumped && (now - boot_tick) >= 3000) {
        if (camera_info.state.state_shooting_progress != SHOOTING_PROGRESS_PROCESSING &&
            get_movie_status() != VIDEO_RECORD_IN_PROGRESS) {
            romlog_dumped = 1;
            dump_canon_romlog_silent();
        }
    }

    // 2. Reinforce anti-shutdown every 10s when ALT_PREVENT_SHUTDOWN_ALWAYS is active
    if (conf.alt_prevent_shutdown == ALT_PREVENT_SHUTDOWN_ALWAYS && (now - last_pwr_tick) >= 10000) {
        last_pwr_tick = now;
        disable_shutdown();
        _SetAutoShutdownTime(0);
    }

    // 3. Instant delta tracking of all critical camera states
    int m_play  = camera_info.state.mode_play;
    int m_vid   = camera_info.state.mode_video;
    int m_shoot = camera_info.state.mode_shooting;
    int g_mode  = camera_info.state.gui_mode;
    int g_alt   = camera_info.state.gui_mode_alt;
    int mov_st  = get_movie_status();
    int sh_prog = camera_info.state.state_shooting_progress;
    int sh_in   = shooting_in_progress();
    int scr_run = camera_info.state.state_kbd_script_run;
    int usb_bit = get_usb_bit_physw_mod();
    int zoom    = lens_get_zoom_point();
    int hpress  = camera_info.state.is_shutter_half_press;
    int lkey    = camera_info.state.kbd_last_clicked;
    long expc   = get_exposure_counter();

    int changed = (
        m_play != prev_mode_play || m_vid != prev_mode_video || m_shoot != prev_mode_shooting ||
        g_mode != prev_gui_mode || g_alt != prev_gui_alt || mov_st != prev_movie_status ||
        sh_prog != prev_shoot_prog || sh_in != prev_shoot_in_prog || scr_run != prev_script_run ||
        usb_bit != prev_usb_bit || zoom != prev_zoom || hpress != prev_half_press ||
        lkey != prev_last_key || expc != prev_exp_cnt
    );

    if (changed) {
        char line[220];
        sprintf(line, "%08u STATE play=%d vid=%d smode=%d gui=%d alt=%d mov=%d sh=%d/%d scr=%d usb=%d zm=%d hp=%d key=%d exp=%ld",
                now, m_play, m_vid, m_shoot, g_mode, g_alt, mov_st,
                sh_prog, sh_in, scr_run, usb_bit, zoom, hpress, lkey, expc);
        log_append_raw(line);

        prev_mode_play = m_play;
        prev_mode_video = m_vid;
        prev_mode_shooting = m_shoot;
        prev_gui_mode = g_mode;
        prev_gui_alt = g_alt;
        prev_movie_status = mov_st;
        prev_shoot_prog = sh_prog;
        prev_shoot_in_prog = sh_in;
        prev_script_run = scr_run;
        prev_usb_bit = usb_bit;
        prev_zoom = zoom;
        prev_half_press = hpress;
        prev_last_key = lkey;
        prev_exp_cnt = expc;
    }

    // 4. Periodic 5s heartbeat OR sudden battery voltage drop (>150mV or <3650mV)
    long vbatt = stat_get_vbatt();
    int vdrop = (prev_vbatt > 0 && ((prev_vbatt - vbatt) >= 150 || (vbatt < 3650 && prev_vbatt >= 3650)));
    if (vdrop || (now - last_hb_tick) >= 5000) {
        last_hb_tick = now;
        prev_vbatt = vbatt;
        cam_meminfo mi;
        GetMemInfo(&mi);
        char line[180];
        sprintf(line, "%08u %s vbatt=%ld topt=%d tccd=%d tbat=%d free=%d usb=%d mov=%d exp=%ld",
                now, vdrop ? "PWR_DROP" : "HB",
                vbatt, get_optical_temp(), get_ccd_temp(), get_battery_temp(),
                mi.free_size, usb_bit, mov_st, expc);
        log_append_raw(line);
    }

    // 5. Safe flush every 1.5s or when buffer >= 2 KB
    if (log_len > 0 && ((now - last_flush_tick) >= 1500 || log_len >= (LOG_BUF_SIZE / 2))) {
        flush_to_sd();
    }
}

void sx280_log_mbox(const char *title, const char *msg, unsigned int flags) {
    char tbuf[48], mbuf[140], line[230];
    copy_clean(tbuf, title, sizeof(tbuf));
    copy_clean(mbuf, msg, sizeof(mbuf));
    sprintf(line, "%08u MBOX flags=0x%x title=\"%s\" msg=\"%s\"",
            (unsigned)get_tick_count(), flags, tbuf, mbuf);
    log_append_raw(line);
}

void sx280_log_console(const char *str) {
    char cbuf[140], line[180];
    copy_clean(cbuf, str, sizeof(cbuf));
    sprintf(line, "%08u CONS \"%s\"", (unsigned)get_tick_count(), cbuf);
    log_append_raw(line);
}

void sx280_log_moderr(const char *name, const char *reason) {
    char nbuf[48], rbuf[80], line[180];
    copy_clean(nbuf, name, sizeof(nbuf));
    copy_clean(rbuf, reason, sizeof(rbuf));
    sprintf(line, "%08u MODERR mod=\"%s\" err=\"%s\"", (unsigned)get_tick_count(), nbuf, rbuf);
    log_append_raw(line);
}

void sx280_log_raw(int dng_raw, int dng_ver, int ok, int elapsed_ms) {
    char line[128];
    sprintf(line, "%08u RAW dng=%d dngver=%d ok=%d ms=%d exp=%ld",
            (unsigned)get_tick_count(), dng_raw, dng_ver, ok, elapsed_ms, get_exposure_counter());
    log_append_raw(line);
}

void sx280_log_profile(int profile_id, int dng, int iso, int tv_mode, int av_idx, int ovr_dis) {
    char line[140];
    sprintf(line, "%08u PROFILE id=%d dng=%d iso=%d tvmode=%d av=%d ovrdis=%d",
            (unsigned)get_tick_count(), profile_id, dng, iso, tv_mode, av_idx, ovr_dis);
    log_append_raw(line);
}
