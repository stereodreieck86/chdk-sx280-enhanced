--[[
@title SX280 Diag
@chdk_version 1.7.0
@param m Test Mode (0=Diag,1=Photo)
@default m 0
@param s Safe Shot Count (0..3)
@default s 0
]]

-- ============================================================================
-- SX280 ENHANCED - USB-UNABHAENGIGE KAMERAINTERNE DIAGNOSE (MASTERPROMPT 15)
-- ============================================================================
-- Nutzt ausschliesslich sichere, dokumentierte CHDK-Lua-APIs.
-- Keine nativen Funktionsaufrufe (call_func_ptr), keine Flash/EEPROM-Zugriffe.
-- Schreibt strukturierte JSONL-Telemetrie nach A/CHDK/LOGS/DIAG_STANDALONE.JSONL
-- Funktioniert vollstaendig ohne USB-Kabel im normalen Kamerabetrieb.
-- ============================================================================

local LOG_DIR  = "A/CHDK/LOGS"
local LOG_FILE = "A/CHDK/LOGS/DIAG_STANDALONE.JSONL"

local function json_escape(str)
    if str == nil then return "" end
    local s = tostring(str)
    s = string.gsub(s, "\\", "\\\\")
    s = string.gsub(s, '"', '\\"')
    s = string.gsub(s, "\n", "\\n")
    s = string.gsub(s, "\r", "\\r")
    return s
end

local function safe_call(fn, ...)
    if type(fn) ~= "function" then return nil end
    local ok, a, b, c = pcall(fn, ...)
    if not ok then return nil end
    return a, b, c
end

local function ensure_log_dir()
    if os.stat then
        local st = os.stat(LOG_DIR)
        if not st then
            pcall(os.mkdir, LOG_DIR)
        end
    end
end

local function append_jsonl(record_str)
    ensure_log_dir()
    local f = io.open(LOG_FILE, "a")
    if f then
        f:write(record_str .. "\n")
        f:close()
        return true
    end
    return false
end

local function collect_snapshot(event_tag)
    local bi = safe_call(get_buildinfo) or {}
    local is_rec, is_vid, mode_id = safe_call(get_mode)
    local alt_mode = safe_call(get_alt_mode)
    local gui_mode = safe_call(get_gui_mode)
    local usb_state = safe_call(get_usb_power, 1)
    local vbatt = safe_call(get_vbatt)
    local t_opt = safe_call(get_temperature, 0)
    local t_ccd = safe_call(get_temperature, 1)
    local t_bat = safe_call(get_temperature, 2)
    local free_kb = safe_call(get_free_disk_space)
    local disk_kb = safe_call(get_disk_size)
    local mem = safe_call(get_meminfo) or {}
    local zoom = safe_call(get_zoom)
    local exp_cnt = safe_call(get_exp_count)

    -- Critical config values (CORE = 1000+, OSD = 2000+)
    local cfg_platformid     = safe_call(get_config_value, 1001)
    local cfg_simple_ui      = safe_call(get_config_value, 1274)
    local cfg_alt_btn        = safe_call(get_config_value, 1220)
    local cfg_prevent_sd     = safe_call(get_config_value, 1221)
    local cfg_video_qual     = safe_call(get_config_value, 1081)
    local cfg_video_br       = safe_call(get_config_value, 1082)
    local cfg_save_raw       = safe_call(get_config_value, 1020)
    local cfg_dng_raw        = safe_call(get_config_value, 1036)
    local cfg_show_osd       = safe_call(get_config_value, 2001)
    local cfg_menu_color     = safe_call(get_config_value, 2055)
    local cfg_title_color    = safe_call(get_config_value, 2056)
    local cfg_cursor_color   = safe_call(get_config_value, 2057)
    local cfg_symbol_color   = safe_call(get_config_value, 2058)
    local cfg_font_cp        = safe_call(get_config_value, 2136)
    local cfg_alt_helper     = safe_call(get_config_value, 2260)
    local cfg_osd_platformid = safe_call(get_config_value, 2298)

    -- Canon Propset 6 conflict check (SX280 HS: AF_FRAME=8, SERVO_AF=299, IS_MODE=145)
    local props = nil
    local ok_p, pmod = pcall(require, "propcase")
    if ok_p and type(pmod) == "table" then
        props = pmod
    end
    local af_frame = props and props.AF_FRAME and safe_call(get_prop, props.AF_FRAME) or -1
    local servo_af = props and props.SERVO_AF and safe_call(get_prop, props.SERVO_AF) or -1
    local is_mode  = props and props.IS_MODE  and safe_call(get_prop, props.IS_MODE)  or -1

    local build_flavor = "UNKNOWN"
    local rev_str = tostring(bi.build_revision or "0")
    local rev_num = tonumber(rev_str) or 0
    if rev_num >= 9015 then
        build_flavor = "SX280_ENHANCED_REV" .. rev_str
    elseif rev_str == "6358" then
        build_flavor = "STOCK_CHDK_PSFI2_REV6358"
    else
        build_flavor = "OTHER_REV_" .. rev_str
    end

    local menu_colors_ok = (cfg_menu_color and cfg_menu_color ~= 0 and cfg_title_color and cfg_title_color ~= 0 and cfg_cursor_color and cfg_cursor_color ~= 0)
    local ts = safe_call(os.date, "%Y-%m-%dT%H:%M:%S") or "1970-01-01T00:00:00"
    local tick = safe_call(get_tick_count) or 0

    local json = string.format(
        '{"ts":"%s","tick":%d,"event":"%s","usb_power":%d,"standalone":%s,' ..
        '"build":{"flavor":"%s","version":"%s","number":"%s","revision":"%s","date":"%s","time":"%s","platform":"%s","platsub":"%s"},' ..
        '"state":{"rec":%s,"video":%s,"mode_id":%d,"alt":%s,"gui_mode":%d,"zoom":%d,"exp_count":%d},' ..
        '"health":{"vbatt_mv":%d,"temp_opt":%d,"temp_ccd":%d,"temp_batt":%d,"free_mem":%d,"free_disk_kb":%d,"disk_kb":%d},' ..
        '"config":{"platformid":%d,"osd_platformid":%d,"simple_ui":%d,"alt_btn":%d,"prevent_shutdown":%d,' ..
        '"video_quality":%d,"video_bitrate":%d,"save_raw":%d,"dng_raw":%d,"show_osd":%d,"alt_helper":%d,"font_cp":%d,' ..
        '"menu_color":%d,"title_color":%d,"cursor_color":%d,"symbol_color":%d,"menu_colors_valid":%s},' ..
        '"canon_props":{"af_frame":%d,"servo_af":%d,"is_mode":%d}}',
        json_escape(ts), tick, json_escape(event_tag),
        tonumber(usb_state) or -1,
        (tonumber(usb_state) == 0) and "true" or "false",
        json_escape(build_flavor),
        json_escape(bi.version or ""),
        json_escape(bi.build_number or ""),
        json_escape(rev_str),
        json_escape(bi.build_date or ""),
        json_escape(bi.build_time or ""),
        json_escape(bi.platform or ""),
        json_escape(bi.platsub or ""),
        tostring(is_rec == true), tostring(is_vid == true), tonumber(mode_id) or -1,
        tostring(alt_mode == true), tonumber(gui_mode) or -1,
        tonumber(zoom) or -1, tonumber(exp_cnt) or -1,
        tonumber(vbatt) or -1, tonumber(t_opt) or -99, tonumber(t_ccd) or -99, tonumber(t_bat) or -99,
        tonumber(mem.free_size) or -1, tonumber(free_kb) or -1, tonumber(disk_kb) or -1,
        tonumber(cfg_platformid) or -1, tonumber(cfg_osd_platformid) or -1,
        tonumber(cfg_simple_ui) or -1, tonumber(cfg_alt_btn) or -1, tonumber(cfg_prevent_sd) or -1,
        tonumber(cfg_video_qual) or -1, tonumber(cfg_video_br) or -1,
        tonumber(cfg_save_raw) or -1, tonumber(cfg_dng_raw) or -1,
        tonumber(cfg_show_osd) or -1, tonumber(cfg_alt_helper) or -1, tonumber(cfg_font_cp) or -1,
        tonumber(cfg_menu_color) or 0, tonumber(cfg_title_color) or 0,
        tonumber(cfg_cursor_color) or 0, tonumber(cfg_symbol_color) or 0,
        tostring(menu_colors_ok == true),
        tonumber(af_frame) or -1, tonumber(servo_af) or -1, tonumber(is_mode) or -1
    )

    return json, build_flavor, menu_colors_ok, (tonumber(usb_state) == 0), tonumber(vbatt) or 0
end

safe_call(set_console_layout, 1, 2, 42, 11)
safe_call(set_console_autoredraw, 1)
safe_call(cls)
print("=== SX280 Diag P15 ===")
local j0, flavor, colors_ok, is_standalone, vbatt = collect_snapshot("BOOT_AUDIT")
local wrote = append_jsonl(j0)
print("Build: " .. flavor)
print("USB:   " .. (is_standalone and "OFF (Standalone)" or "ON (USB Power)"))
print("MenuC: " .. (colors_ok and "OK (Deckend)" or "FAIL (Transparent)"))
print("Batt:  " .. tostring(vbatt) .. " mV")
print("Log:   " .. (wrote and "GESPEICHERT (OK)" or "SCHREIBFEHLER!"))

-- Optional safe photo test ONLY if explicitly requested (m == 1 and s > 0) and in Rec mode
if m == 1 and s > 0 and s <= 3 then
    local is_rec = safe_call(get_mode)
    if is_rec then
        for i = 1, s do
            print("Testfoto " .. tostring(i) .. "/" .. tostring(s) .. "...")
            local before_cnt = safe_call(get_exp_count) or 0
            shoot()
            sleep(500)
            local after_cnt = safe_call(get_exp_count) or 0
            local js = collect_snapshot("SAFE_SHOT_" .. tostring(i) .. "_DELTA_" .. tostring(after_cnt - before_cnt))
            append_jsonl(js)
        end
    else
        local jw = collect_snapshot("SHOT_SKIPPED_PLAYBACK_MODE")
        append_jsonl(jw)
        print("Foto uebersprungen (Wiedergabe)")
    end
end

print("=== Fertig (5s / Taste) ===")
safe_call(console_redraw)
if type(wait_click) == "function" then
    wait_click(5000)
else
    sleep(5000)
end
