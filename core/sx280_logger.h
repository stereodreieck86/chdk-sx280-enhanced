#ifndef SX280_LOGGER_H
#define SX280_LOGGER_H

// ============================================================================
// SX280 ENHANCED - NATIVE C FLIGHT RECORDER & POWER KEEPALIVE (REV 9016)
// ============================================================================
// Runs automatically on every camera boot inside core_spytask() — no <ALT>
// mode or Lua script required, never blocks still photo or video capture.
// Writes rolling logs to A/CHDK/LOGS/FLIGHT.LOG (+ auto ROMLOG_BOOT.LOG).
// ============================================================================

void sx280_logger_boot(void);
void sx280_logger_tick(void);

void sx280_log_mbox(const char *title, const char *msg, unsigned int flags);
void sx280_log_console(const char *line);
void sx280_log_moderr(const char *name, const char *reason);
void sx280_log_raw(int dng_raw, int dng_ver, int ok, int elapsed_ms);
void sx280_log_profile(int profile_id, int dng, int iso, int tv_mode, int av_idx, int ovr_dis);

#endif // SX280_LOGGER_H
