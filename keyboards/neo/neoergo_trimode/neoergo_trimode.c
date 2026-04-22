// Copyright 2025 emolitor (github.com/emolitor)
// Copyright 2024 Westberry Technology (ChangZhou) Corp., Ltd
// SPDX-License-Identifier: GPL-2.0-or-later

#include QMK_KEYBOARD_H
#include "wireless.h"
#include "custom_keycodes.h"


// Forward declarations for functions used across translation units
void wireless_task(void);
bool smsg_is_busy(void);
extern host_driver_t wireless_driver;

// ──────────────────────────────────────────────
// State
// ──────────────────────────────────────────────

static bool rgb_fake_off = false;
static bool bat_debug_on = false;
static bool wpm_mode_on = false;

uint8_t indicator_brightness = 128; // 0-255

// Battery readout state — driven by KC_BAT keypress
static bool     bat_reporting    = false;
static uint16_t bat_report_timer = 0;
static uint8_t  bat_flashes_done = 0;
static uint8_t  bat_flashes_yellow  = 0;  // fifties digit of battery %
static uint8_t  bat_flashes_red  = 0;  // tens digit of battery %
static uint8_t  bat_flashes_blue = 0;  // ones digit of battery %
static bool     bat_led_on       = false;

// ──────────────────────────────────────────────
// EEPROM config
// ──────────────────────────────────────────────

typedef union {
    uint32_t raw;
    struct {
        uint8_t flag : 1;
        uint8_t devs : 3;
    };
} confinfo_t;
confinfo_t confinfo;

// ──────────────────────────────────────────────
// Blink timing
// blink_index wraps intentionally at 255 -> 0; the % 64 / % 128 toggle
// logic below depends on this natural uint8_t rollover for timing.
// ──────────────────────────────────────────────

uint8_t blink_index = 0;
bool    blink_fast  = true;
bool    blink_slow  = true;

uint32_t post_init_timer = 0x00;

// ──────────────────────────────────────────────
// Device list — circular linked list for KC_NXT cycling
// ──────────────────────────────────────────────

struct devs_list {
    int               devs;
    struct devs_list *next;
};

struct devs_list devs[] = {
    {.devs = DEVS_USB, .next = &devs[1]},
    {.devs = DEVS_BT1, .next = &devs[2]},
    {.devs = DEVS_BT2, .next = &devs[3]},
    {.devs = DEVS_BT3, .next = &devs[4]},
    {.devs = DEVS_2G4, .next = &devs[0]},
};

struct devs_list *current_dev = &devs[0]; // default to USB

// ──────────────────────────────────────────────
// Forward declarations
// ──────────────────────────────────────────────

void set_indicator(uint8_t index, uint8_t r, uint8_t g, uint8_t b);

// Battery indicators always use full brightness regardless of indicator_brightness
// so that low-battery warnings are always clearly visible.
void set_indicator_battery(uint8_t index, uint8_t r, uint8_t g, uint8_t b);

// Hack
void md_send_devinfo(const char *name);

// ──────────────────────────────────────────────
// Per-key tapping term
// Wireless keys get a longer tapping term to match default firmware behaviour.
// ──────────────────────────────────────────────

uint16_t get_tapping_term(uint16_t keycode, keyrecord_t *record) {
    switch (keycode) {
        case LT(0, KC_BT1):
        case LT(0, KC_BT2):
        case LT(0, KC_BT3):
        case LT(0, KC_2G4):
            return WIRELESS_TAPPING_TERM;
        default:
            return TAPPING_TERM;
    }
}

// ──────────────────────────────────────────────
// process_record_user
// ──────────────────────────────────────────────

bool process_record_user(uint16_t keycode, keyrecord_t *record) {

    if (record->event.pressed) {

        switch (keycode) {

            case RGB_TOG:
                rgb_fake_off = !rgb_fake_off;
                if (rgb_fake_off) {
                    rgb_matrix_sethsv_noeeprom(0, 0, 0);
                } else {
                    rgb_matrix_sethsv_noeeprom(0, 255, 255);
                }
                return false;

            case IND_BRIGHT_DN:
                if (indicator_brightness > 16)
                    indicator_brightness -= 16;
                return false;

            case IND_BRIGHT_UP:
                if (indicator_brightness < 240)
                    indicator_brightness += 16;
                return false;

            case KC_BAT: {
                // Trigger battery readout on LED 14.
                // Red flashes = tens digit, blue flashes = ones digit.
                // e.g. 73% = 7 red flashes then 3 blue flashes.
                uint8_t pct        = *md_getp_bat();
                bat_flashes_yellow = pct / 50;
                bat_flashes_red    = (pct % 50) / 10;
                bat_flashes_blue   = pct % 10;
                bat_flashes_done   = 0;
                bat_led_on         = false;
                bat_report_timer   = timer_read();
                bat_reporting      = true;
                return false;
            }
            case BAT_DBG:
				bat_debug_on = !bat_debug_on;
				if (!bat_debug_on) {
					rgb_matrix_set_color(14, RGB_OFF);
				}
				return false;

            case WPM_TOG:
                wpm_mode_on = !wpm_mode_on;
                return false;
        }
    }

    return true;
}

// ──────────────────────────────────────────────
// Keyboard init
// ──────────────────────────────────────────────

void keyboard_post_init_kb(void) {
    confinfo.raw = eeconfig_read_kb();
    if (!confinfo.raw) {
        confinfo.flag = true;
        confinfo.devs = DEVS_USB;
        eeconfig_update_kb(confinfo.raw);
    }

    gpio_write_pin_low(LED_POWER_EN_PIN);
    gpio_set_pin_output_open_drain(LED_POWER_EN_PIN);

    //gpio_set_pin_output(ESCAPE_PIN);
    //gpio_set_pin_output(DEVS_BT1_PIN);
    //gpio_set_pin_output(DEVS_BT2_PIN);
    //gpio_set_pin_output(DEVS_BT3_PIN);
    //gpio_set_pin_output(DEVS_2G4_PIN);

    // Set GPIO as high input for battery charging state
    gpio_set_pin_input(BT_CABLE_PIN);
    gpio_set_pin_input_high(BT_CHARGE_PIN);

    // Set USB_POWER_EN_PIN state before enabling the output to avoid instability
    if (confinfo.devs == DEVS_USB && gpio_read_pin(BT_CABLE_PIN)) {
        gpio_write_pin_low(USB_POWER_EN_PIN);
    } else {
        gpio_write_pin_high(USB_POWER_EN_PIN);
    }
    gpio_set_pin_output(USB_POWER_EN_PIN);

    wireless_init();
    md_send_devinfo(MD_BT_NAME);
    wait_ms(10);
    wireless_devs_change(!confinfo.devs, confinfo.devs, false);
    post_init_timer = timer_read32();

    keyboard_post_init_user();
}

void usb_power_connect(void) {
    gpio_write_pin_low(USB_POWER_EN_PIN);
}

void usb_power_disconnect(void) {
    gpio_write_pin_high(USB_POWER_EN_PIN);
}

void suspend_power_down_kb(void) {
    gpio_write_pin_high(LED_POWER_EN_PIN);
    suspend_power_down_user();
}

void suspend_wakeup_init_kb(void) {
    gpio_write_pin_low(LED_POWER_EN_PIN);
    wireless_devs_change(wireless_get_current_devs(), wireless_get_current_devs(), false);
    suspend_wakeup_init_user();
}

bool lpwr_is_allow_timeout_hook(void) {
    if (wireless_get_current_devs() == DEVS_USB) {
        return false;
    }
    return true;
}

// ──────────────────────────────────────────────
// Wireless tasks
// ──────────────────────────────────────────────

void wireless_post_task(void) {
    if (post_init_timer && timer_elapsed32(post_init_timer) >= 100) {
        md_send_devctrl(MD_SND_CMD_DEVCTRL_FW_VERSION);   // get the module fw version
        md_send_devctrl(MD_SND_CMD_DEVCTRL_SLEEP_BT_EN);  // timeout 30min to sleep in bt mode, enable
        md_send_devctrl(MD_SND_CMD_DEVCTRL_SLEEP_2G4_EN); // timeout 30min to sleep in 2.4g mode, enable
        wireless_devs_change(!confinfo.devs, confinfo.devs, false);
        post_init_timer = 0x00;
    }
}

void md_devs_change(uint8_t devs, bool reset) {
    switch (devs) {
        case DEVS_USB: {
            md_send_devctrl(MD_SND_CMD_DEVCTRL_USB);
        } break;
        case DEVS_2G4: {
            md_send_devctrl(MD_SND_CMD_DEVCTRL_2G4);
            if (reset) {
                md_send_devctrl(MD_SND_CMD_DEVCTRL_PAIR);
            }
        } break;
        case DEVS_BT1: {
            md_send_devctrl(MD_SND_CMD_DEVCTRL_BT1);
            if (reset) {
                md_send_devctrl(MD_SND_CMD_DEVCTRL_PAIR);
            }
        } break;
        case DEVS_BT2: {
            md_send_devctrl(MD_SND_CMD_DEVCTRL_BT2);
            if (reset) {
                md_send_devctrl(MD_SND_CMD_DEVCTRL_PAIR);
            }
        } break;
        case DEVS_BT3: {
            md_send_devctrl(MD_SND_CMD_DEVCTRL_BT3);
            if (reset) {
                md_send_devctrl(MD_SND_CMD_DEVCTRL_PAIR);
            }
        } break;
        default:
            break;
    }
}

// ──────────────────────────────────────────────
// process_record_kb
// ──────────────────────────────────────────────

bool process_record_kb(uint16_t keycode, keyrecord_t *record) {
    if (process_record_user(keycode, record) != true) {
        return false;
    }

    switch (keycode) {
        case KC_USB: {
            wireless_devs_change(wireless_get_current_devs(), DEVS_USB, false);
            return false;
        }
        case KC_NXT: {
            if (record->event.pressed) {
                current_dev = current_dev->next;
                wireless_devs_change(wireless_get_current_devs(), current_dev->devs, false);
            }
            return false;
        }
        case LT(0, KC_BT1): {
            if (record->tap.count && record->event.pressed) {
                wireless_devs_change(wireless_get_current_devs(), DEVS_BT1, false);
            } else if (record->event.pressed && *md_getp_state() != MD_STATE_PAIRING) {
                wireless_devs_change(wireless_get_current_devs(), DEVS_BT1, true);
            }
            return false;
        }
        case LT(0, KC_BT2): {
            if (record->tap.count && record->event.pressed) {
                wireless_devs_change(wireless_get_current_devs(), DEVS_BT2, false);
            } else if (record->event.pressed && *md_getp_state() != MD_STATE_PAIRING) {
                wireless_devs_change(wireless_get_current_devs(), DEVS_BT2, true);
            }
            return false;
        }
        case LT(0, KC_BT3): {
            if (record->tap.count && record->event.pressed) {
                wireless_devs_change(wireless_get_current_devs(), DEVS_BT3, false);
            } else if (record->event.pressed && *md_getp_state() != MD_STATE_PAIRING) {
                wireless_devs_change(wireless_get_current_devs(), DEVS_BT3, true);
            }
            return false;
        }
        case LT(0, KC_2G4): {
            if (record->tap.count && record->event.pressed) {
                wireless_devs_change(wireless_get_current_devs(), DEVS_2G4, false);
            } else if (record->event.pressed && *md_getp_state() != MD_STATE_PAIRING) {
                wireless_devs_change(wireless_get_current_devs(), DEVS_2G4, true);
            }
            return false;
        }
        default:
            return true;
    }
}

void wireless_devs_change_kb(uint8_t old_devs, uint8_t new_devs, bool reset) {
    if (confinfo.devs != wireless_get_current_devs()) {
        confinfo.devs = wireless_get_current_devs();
        eeconfig_update_kb(confinfo.raw);
    }
}

// ──────────────────────────────────────────────
// RGB indicator helpers
// ──────────────────────────────────────────────

void set_indicator(uint8_t index, uint8_t r, uint8_t g, uint8_t b) {
    rgb_matrix_set_color(
        index,
        (r * indicator_brightness) / 255,
        (g * indicator_brightness) / 255,
        (b * indicator_brightness) / 255
    );
}

// Battery indicators bypass indicator_brightness so warnings are always
// clearly visible regardless of the user's brightness setting.
void set_indicator_battery(uint8_t index, uint8_t r, uint8_t g, uint8_t b) {
    rgb_matrix_set_color(index, r, g, b);
}

void blink(uint8_t key_index, uint8_t r, uint8_t g, uint8_t b, bool on) {
    if (on) {
        set_indicator(key_index, r, g, b);
    } else {
        set_indicator(key_index, RGB_OFF);
    }
}

void blink_battery(uint8_t key_index, uint8_t r, uint8_t g, uint8_t b, bool on) {
    if (on) {
        set_indicator_battery(key_index, r, g, b);
    } else {
        rgb_matrix_set_color(key_index, RGB_OFF);
    }
}

// ──────────────────────────────────────────────
// RGB matrix indicators
// ──────────────────────────────────────────────

bool rgb_matrix_indicators_advanced_kb(uint8_t led_min, uint8_t led_max) {

    uint8_t battery_percent = *md_getp_bat();

    // Uncomment for serial debug output.
    // Requires CONSOLE_ENABLE = yes and the following disabled in rules.mk:
    //   MOUSEKEY_ENABLE, EXTRAKEY_ENABLE, NKRO_ENABLE
    //uprintf("Battery: %d%% | Dev: %d\n", battery_percent, confinfo.devs);

    // AUTO RGB OFF — kill the matrix below 20% to save power.
    // Indicators (connection, caps lock, battery LEDs) remain active.
    if (battery_percent <= 20 && !rgb_fake_off) {
        rgb_matrix_set_color_all(RGB_OFF);
    }

    if (!rgb_matrix_indicators_advanced_user(led_min, led_max)) {
        return false;
    }

    // BATTERY READOUT (KC_BAT) — LED 14
    // Red flashes = tens digit, blue flashes = ones digit.
    // e.g. 73% = 7 red flashes, then 3 blue flashes.
    // Skips all other indicators while the sequence is running.
    if (bat_reporting) {
        if (timer_elapsed(bat_report_timer) >= 500) {
            bat_report_timer = timer_read();
            bat_led_on = !bat_led_on;

            if (bat_flashes_done < bat_flashes_yellow) {
				// Yellow phase — fifties
				rgb_matrix_set_color(14, bat_led_on ? 255 : 0, bat_led_on ? 255 : 0, 0);
				if (!bat_led_on) bat_flashes_done++;
			} else if (bat_flashes_done < bat_flashes_yellow + bat_flashes_red) {
				// Red phase — tens
				rgb_matrix_set_color(14, bat_led_on ? 255 : 0, 0, 0);
				if (!bat_led_on) bat_flashes_done++;
			} else if (bat_flashes_done < bat_flashes_yellow + bat_flashes_red + bat_flashes_blue) {
				// Blue phase — ones
				rgb_matrix_set_color(14, 0, 0, bat_led_on ? 255 : 0);
				if (!bat_led_on) bat_flashes_done++;
			} else {
				// Sequence complete
				bat_reporting = false;
				rgb_matrix_set_color(14, RGB_OFF);
			}
        } else {
            // Hold current LED state between timer ticks
            if (bat_flashes_done < bat_flashes_yellow) {
				rgb_matrix_set_color(14, bat_led_on ? 255 : 0, bat_led_on ? 255 : 0, 0);
			} else if (bat_flashes_done < bat_flashes_yellow + bat_flashes_red) {
				rgb_matrix_set_color(14, bat_led_on ? 255 : 0, 0, 0);
			} else {
				rgb_matrix_set_color(14, 0, 0, bat_led_on ? 255 : 0);
			}
        }
        return true;
    }

    // Blink timing — blink_index wraps at 255 naturally as a uint8_t
    blink_index++;
    blink_fast = (blink_index % 64  == 0) ? !blink_fast : blink_fast;
    blink_slow = (blink_index % 128 == 0) ? !blink_slow : blink_slow;

    uint8_t state = *md_getp_state();

    // CONNECTION INDICATORS (LEDs 0-4)
    switch (confinfo.devs) {
        case DEVS_USB:
            set_indicator(0, RGB_IND_WHITE);
            break;
        case DEVS_BT1:
            if (state == MD_STATE_PAIRING) {
                blink(1, RGB_IND_WHITE, blink_fast);
            } else if (state != MD_STATE_CONNECTED) {
                blink(1, RGB_IND_WHITE, blink_slow);
            } else {
                set_indicator(1, RGB_IND_WHITE);
            }
            break;
        case DEVS_BT2:
            if (state == MD_STATE_PAIRING) {
                blink(2, 255, 255, 255, blink_fast);
            } else if (state != MD_STATE_CONNECTED) {
                blink(2, 255, 255, 255, blink_slow);
            } else {
                set_indicator(2, 255, 255, 255);
            }
            break;
        case DEVS_BT3:
            if (state == MD_STATE_PAIRING) {
                blink(3, 255, 255, 255, blink_fast);
            } else if (state != MD_STATE_CONNECTED) {
                blink(3, 255, 255, 255, blink_slow);
            } else {
                set_indicator(3, 255, 255, 255);
            }
            break;
        case DEVS_2G4:
            if (state == MD_STATE_PAIRING) {
                blink(4, 255, 255, 255, blink_fast);
            } else if (state != MD_STATE_CONNECTED) {
                blink(4, 255, 255, 255, blink_slow);
            } else {
                set_indicator(4, 255, 255, 255);
            }
            break;
    }

    // CAPS LOCK (LED 10)
    if (host_keyboard_led_state().caps_lock) {
        set_indicator(10, 255, 255, 255);
    }

    // FN LAYERS / WPM (LED 9)
    if (wpm_mode_on) {
        uint8_t wpm = get_current_wpm();
        if (wpm >= 100) {
            if (blink_fast) {
                rgb_matrix_set_color_all(0, 0, 255);
            } else
                rgb_matrix_set_color_all(RGB_OFF);
            }
        }else if (wpm >= 80) {
            set_indicator(9, 0, 0, 255);
        } else if (wpm >= 65) {
            set_indicator(9, 0, 255, 0);
        } else if (wpm >= 50) {
            set_indicator(9, 255, 255, 0);
        } else {
            set_indicator(9, 255, 0, 0);
        }
    } else if (layer_state_is(3)) {
        set_indicator(9, 255, 0, 255);
    } else if (layer_state_is(2)) {
        set_indicator(9, 255, 255, 0);
    } else if (layer_state_is(1)) {
        set_indicator(9, 0, 255, 255);
    } else {
        set_indicator(9, 0, 0, 0);
    }

    // BATTERY LEVEL (LEDs 5-7, 12)
    // Uses set_indicator_battery (full brightness) so warnings are always visible.
    if (battery_percent < 5) {
        // Critical — Battery bar and LED 12 blink red
        blink_battery(5,  255, 0, 0, blink_fast);
        blink_battery(6,  255, 0, 0, blink_fast);
        blink_battery(7,  255, 0, 0, blink_fast);
        if (!gpio_read_pin(BT_CABLE_PIN)) {
			blink_battery(12, 255, 0, 0, blink_fast);
		}
    } else if (battery_percent < 20) {
        // Low — three LEDs slow-blink red
        blink_battery(5, 255, 0, 0, blink_slow);
        blink_battery(6, 255, 0, 0, blink_slow);
        blink_battery(7, 255, 0, 0, blink_slow);
    } else {
        if (battery_percent >= 80) {

            set_indicator_battery(5, 0, 255, 0);
            set_indicator_battery(6, 0, 255, 0);
            set_indicator_battery(7, 0, 255, 0);
        } else if (battery_percent >= 60) {
            set_indicator_battery(5, 0, 0, 255);
            set_indicator_battery(6, 0, 0, 255);
            set_indicator_battery(7, 0, 0, 255);
        } else if (battery_percent >= 40) {
            set_indicator_battery(5, 0, 0, 255);
            set_indicator_battery(6, 0, 0, 255);
        } else {
            set_indicator_battery(5, 0, 0, 255);
        }
    }

    // CHARGING INDICATOR (LED 12)
    // Runs after the battery level section so it can override LED 12 at any
    // battery level. Critical blink (<5%) is the one exception — at that point
    // the battery section already set LED 12 to fast red which is more urgent,
    // and the cable is unlikely to be in at that point anyway.
    // BT_CABLE_PIN  (B8): high when cable is plugged in
    // BT_CHARGE_PIN (B9): low while charging, high when fully charged
    {
        bool cable_in    = gpio_read_pin(BT_CABLE_PIN);
        bool charge_done = gpio_read_pin(BT_CHARGE_PIN);

        if (cable_in) {
            if (charge_done) {
                // Fully charged — solid green.
                // Hidden in USB/wired mode since you're plugged in anyway.
                if (confinfo.devs != DEVS_USB) {
                    set_indicator_battery(12, 0, 255, 0);
                } else {
                    set_indicator(12, RGB_OFF);
                }
            } else {
                // Charging — slow amber pulse.
                blink_battery(12, 255, 128, 0, blink_slow);
            }
        }
        // Cable not connected: LED 12 left to the battery level section above
        // (fast red blink at <5%, off otherwise).
    }

#ifdef BATTERY_DEBUG
    // Battery debug indicator on LED 14 (spare LED).
    // Changes color every 2% so you can visually confirm battery readings
    // are updating correctly without needing a serial console.
    // To enable:  add "OPT_DEFS += -DBATTERY_DEBUG" to rules.mk
    // To disable: remove that line — compiles out completely with no overhead.
    //
    // Color map (repeating every 8%):
    //   98%+ -> Blue
    //   96%  -> Yellow
    //   94%  -> Green
    //   92%  -> White
    //   90%  -> Red
    //   88%  -> Cyan
    //   86%  -> Magenta
    //   84%  -> Orange
    //   ...repeats
	static const uint8_t debug_colors[][3] = {
        {0,   0,   255}, // Blue
        {255, 255, 0  }, // Yellow
        {0,   255, 0  }, // Green
        {255, 255, 255}, // White
        {255, 0,   0  }, // Red
        {0,   255, 255}, // Cyan
        {255, 0,   255}, // Magenta
        {255, 128, 0  }, // Orange
    };

    if (bat_debug_on) {
     uint8_t color_index = ((100 - battery_percent) / 2) % 8;
    rgb_matrix_set_color(14, debug_colors[color_index][0],
                             debug_colors[color_index][1],
                             debug_colors[color_index][2]);
	}
#endif

    return true;
}

// ──────────────────────────────────────────────
// Hardware init / exception handling
// ──────────────────────────────────────────────

// Temporary workaround for WS2812 pin init
void board_init(void) {
    gpio_set_pin_output(WS2812_DI_PIN);
    gpio_write_pin_low(WS2812_DI_PIN);
}

// Force MCU reset on unhandled exception
void _unhandled_exception(void) {
    mcu_reset();
}

// ──────────────────────────────────────────────
// Wireless NKRO
// Experimental fix for duplicate and hung key presses on wireless
// ──────────────────────────────────────────────

void wireless_send_nkro(report_nkro_t *report) {
    static report_keyboard_t temp_report_keyboard                  = {0};
    uint8_t                  wls_report_nkro[MD_SND_CMD_NKRO_LEN]  = {0};

#ifdef NKRO_ENABLE
    if (report != NULL) {
        report_nkro_t temp_report_nkro = *report;
        uint8_t       key_count        = 0;

        temp_report_keyboard.mods = temp_report_nkro.mods;
        for (uint8_t i = 0; i < NKRO_REPORT_BITS; i++) {
            key_count += __builtin_popcount(temp_report_nkro.bits[i]);
        }

        /*
         * Use NKRO for sending when more than 6 keys are pressed
         * to solve the issue of the lack of a protocol flag in wireless mode.
         */

        for (uint8_t i = 0; i < key_count; i++) {
            uint8_t usageid;
            uint8_t idx, n = 0;

            for (n = 0; n < NKRO_REPORT_BITS && !temp_report_nkro.bits[n]; n++) {
            }
            usageid = (n << 3) | biton(temp_report_nkro.bits[n]);
            del_key_bit(&temp_report_nkro, usageid);

            for (idx = 0; idx < WLS_KEYBOARD_REPORT_KEYS; idx++) {
                if (temp_report_keyboard.keys[idx] == usageid) {
                    goto next;
                }
            }

            for (idx = 0; idx < WLS_KEYBOARD_REPORT_KEYS; idx++) {
                if (temp_report_keyboard.keys[idx] == 0x00) {
                    temp_report_keyboard.keys[idx] = usageid;
                    break;
                }
            }
        next:
            if (idx == WLS_KEYBOARD_REPORT_KEYS && (usageid < (MD_SND_CMD_NKRO_LEN * 8))) {
                wls_report_nkro[usageid / 8] |= 0x01 << (usageid % 8);
            }
        }

        temp_report_nkro = *report;

        // find key up and del it
        uint8_t nkro_keys = key_count;
        for (uint8_t i = 0; i < WLS_KEYBOARD_REPORT_KEYS; i++) {
            report_nkro_t found_report_nkro;
            uint8_t       usageid = 0x00;
            uint8_t       n;

            found_report_nkro = temp_report_nkro;

            for (uint8_t c = 0; c < nkro_keys; c++) {
                for (n = 0; n < NKRO_REPORT_BITS && !found_report_nkro.bits[n]; n++) {
                }
                usageid = (n << 3) | biton(found_report_nkro.bits[n]);
                del_key_bit(&found_report_nkro, usageid);
                if (usageid == temp_report_keyboard.keys[i]) {
                    del_key_bit(&temp_report_nkro, usageid);
                    nkro_keys--;
                    break;
                }
            }

            if (usageid != temp_report_keyboard.keys[i]) {
                temp_report_keyboard.keys[i] = 0x00;
            }
        }

    } else {
        memset(&temp_report_keyboard, 0, sizeof(temp_report_keyboard));
    }
#endif

    while (smsg_is_busy()) {
        wireless_task();
    }
    wireless_driver.send_keyboard(&temp_report_keyboard);
    md_send_nkro(wls_report_nkro);
}
