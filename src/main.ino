/* lvgl + fastled controller on P169H002-CTP display */

#include "cst816t.h"
#include "lvgl.h"
#include <Arduino.h>
#include <FastLED.h>
#include <Preferences.h>
#include <SPI.h>
#include <TFT_eSPI.h>
#include <Wire.h>

// display
#define TFT_X 240
#define TFT_Y 280
#define TFT_ROTATION 0
#define DISPLAY_SLEEP_TIMEOUT_MS (5UL * 60UL * 1000UL)
#define DISPLAY_BACKLIGHT_ON_LEVEL 127
#define DISPLAY_BACKLIGHT_OFF_LEVEL 0

// touch
#define TP_SDA 4
#define TP_SCL 5
#define TP_RST 3
#define TP_IRQ 2

// FastLED
#define LED_DATA_PIN 20
#define LED_TYPE WS2811
#define LED_COLOR_ORDER GRB
#define MAX_LEDS 600
#define DEFAULT_LED_COUNT 300
#define LED_FPS 120
#define PATTERN_SWITCH_MS 10000

// Touch orientation tuning.
#define TOUCH_SWAP_XY 0
#define TOUCH_FLIP_X 0
#define TOUCH_FLIP_Y 0

enum ClockTarget {
  CLOCK_TARGET_NOW = 0,
  CLOCK_TARGET_RANGE1_START = 1,
  CLOCK_TARGET_RANGE1_END = 2,
  CLOCK_TARGET_RANGE2_START = 3,
  CLOCK_TARGET_RANGE2_END = 4
};

enum ScheduleEditTarget { SCHEDULE_EDIT_RANGE1 = 0, SCHEDULE_EDIT_RANGE2 = 1 };

TwoWire touchI2C = TwoWire(0);
cst816t touchpad(touchI2C, TP_RST, TP_IRQ);
TFT_eSPI tft = TFT_eSPI();
CRGB leds[MAX_LEDS];
Preferences gPrefs;

static lv_display_t *disp = nullptr;
static lv_indev_t *indev = nullptr;
static uint16_t lv_buf_1[TFT_X * 20];
static uint16_t lv_buf_2[TFT_X * 20];

static lv_obj_t *main_page = nullptr;
static lv_obj_t *settings_page = nullptr;
static lv_obj_t *schedule_page = nullptr;
static lv_obj_t *schedule_edit_page = nullptr;
static lv_obj_t *led_strip_page = nullptr;
static lv_obj_t *clock_dialog = nullptr;
static lv_obj_t *clock_title_label = nullptr;
static lv_obj_t *clock_hour_spin = nullptr;
static lv_obj_t *clock_min_spin = nullptr;
static lv_obj_t *clock_active_spin = nullptr;

static lv_obj_t *time_label = nullptr;
static lv_obj_t *remaining_label = nullptr;
static lv_obj_t *main_day_bar_track = nullptr;
static lv_obj_t *main_day_bar_seg_0 = nullptr;
static lv_obj_t *main_day_bar_seg_1 = nullptr;
static lv_obj_t *main_day_bar_seg_2 = nullptr;
static lv_obj_t *main_day_bar_seg_3 = nullptr;
static lv_obj_t *main_day_bar_marker = nullptr;
static lv_obj_t *pattern_label = nullptr;
static lv_obj_t *brightness_label = nullptr;
static lv_obj_t *brightness_slider = nullptr;
static lv_obj_t *run_switch = nullptr;
static lv_obj_t *run_switch_label = nullptr;
static lv_obj_t *schedule_summary_r1_label = nullptr;
static lv_obj_t *schedule_summary_r2_label = nullptr;
static lv_obj_t *schedule_summary_r1_time_label = nullptr;
static lv_obj_t *schedule_summary_r2_time_label = nullptr;
static lv_obj_t *schedule_summary_day_bar_track = nullptr;
static lv_obj_t *schedule_summary_day_bar_seg_0 = nullptr;
static lv_obj_t *schedule_summary_day_bar_seg_1 = nullptr;
static lv_obj_t *schedule_summary_day_bar_seg_2 = nullptr;
static lv_obj_t *schedule_summary_day_bar_seg_3 = nullptr;
static lv_obj_t *schedule_summary_day_bar_marker = nullptr;
static lv_obj_t *schedule_edit_range1_enable_switch = nullptr;
static lv_obj_t *schedule_edit_title_label = nullptr;
static lv_obj_t *schedule_range1_start_btn = nullptr;
static lv_obj_t *schedule_range1_end_btn = nullptr;
static lv_obj_t *schedule_range1_start_time_label = nullptr;
static lv_obj_t *schedule_range1_end_time_label = nullptr;
static lv_obj_t *led_count_spin = nullptr;

static uint8_t gCurrentPatternNumber = 0;
static uint8_t gHue = 0;
static uint8_t gBrightness = 96;
static uint16_t gLedCount = DEFAULT_LED_COUNT;
static bool gLedRunning = true;
static bool gLedsOn = false;
static bool gTouchPressed = false;
static uint32_t gLastTouchMs = 0;
static uint32_t gLastBackSwipeMs = 0;
static bool gDisplaySleeping = false;

static int gClockHour = 19;
static int gClockMinute = 0;
static int gSchedule1StartMinutes = 5 * 60;
static int gSchedule1EndMinutes = 7 * 60;
static int gSchedule2StartMinutes = 19 * 60;
static int gSchedule2EndMinutes = 23 * 60;
static uint32_t gClockCarryMs = 0;
static uint32_t gLastClockTickMs = 0;
static ClockTarget gClockTarget = CLOCK_TARGET_NOW;
static ScheduleEditTarget gScheduleEditTarget = SCHEDULE_EDIT_RANGE1;
static bool gPrevScheduleActive = false;
static bool gSchedule1Enabled = true;
static bool gSchedule2Enabled = true;
static bool gSchedulePrefsReady = false;

// Forward declarations for FastLED patterns
static void rainbow();
static void rainbowWithGlitter();
static void confetti();
static void sinelon();
static void juggle();
static void bpm();
static void addGlitter(fract8 chanceOfGlitter);
static void nextPattern();
static void update_main_day_bar();
static void update_schedule_day_bar();
static void show_main_page();
static void show_settings_page();
static void show_schedule_page();
static void show_schedule_edit_page(ScheduleEditTarget target);
static void show_led_strip_page();

typedef void (*SimplePatternList[])();
SimplePatternList gPatterns = {
    rainbow, rainbowWithGlitter, confetti, sinelon, juggle, bpm};
static const char *kPatternNames[] = {
    "Rainbow", "Rainbow + Glitter", "Confetti", "Sinelon", "Juggle", "BPM"};

static void disp_flush_cb(lv_display_t *display, const lv_area_t *area,
                          uint8_t *px_map) {
  uint32_t w = static_cast<uint32_t>(area->x2 - area->x1 + 1);
  uint32_t h = static_cast<uint32_t>(area->y2 - area->y1 + 1);
  uint32_t len = w * h;

  tft.startWrite();
  tft.setAddrWindow(area->x1, area->y1, w, h);
  tft.pushPixels(px_map, len);
  tft.endWrite();

  lv_display_flush_ready(display);
}

static void turn_leds_off() {
  fill_solid(leds, MAX_LEDS, CRGB::Black);
  FastLED.show();
  gLedsOn = false;
}

static void set_backlight(uint8_t level) { analogWrite(TFT_BL, level); }

static void sleep_display() {
  if (gDisplaySleeping)
    return;
  tft.fillScreen(TFT_BLACK);
  set_backlight(DISPLAY_BACKLIGHT_OFF_LEVEL);
  gDisplaySleeping = true;
}

static void wake_display() {
  if (!gDisplaySleeping)
    return;
  set_backlight(DISPLAY_BACKLIGHT_ON_LEVEL);
  gDisplaySleeping = false;
  if (lv_screen_active()) {
    lv_obj_invalidate(lv_screen_active());
  }
}

static int now_minutes() { return (gClockHour * 60) + gClockMinute; }

static bool is_in_range(int now, int start_min, int end_min) {
  if (start_min <= end_min) {
    return (now >= start_min) && (now < end_min);
  }
  return (now >= start_min) || (now < end_min);
}

static bool is_any_schedule_enabled() {
  return gSchedule1Enabled || gSchedule2Enabled;
}

static bool is_schedule_active_now() {
  const int now = now_minutes();
  const bool r1_active =
      gSchedule1Enabled &&
      is_in_range(now, gSchedule1StartMinutes, gSchedule1EndMinutes);
  const bool r2_active =
      gSchedule2Enabled &&
      is_in_range(now, gSchedule2StartMinutes, gSchedule2EndMinutes);
  return r1_active || r2_active;
}

static bool should_render_leds() { return gLedRunning; }

static int minutes_until_next_daily_time(int target_minutes) {
  const int now = now_minutes();
  int diff = target_minutes - now;
  if (diff <= 0)
    diff += 24 * 60;
  return diff;
}

static int minutes_until_next_transition(bool currently_active) {
  const int now = now_minutes();
  int best = 24 * 60;

  if (!is_any_schedule_enabled())
    return best;

  if (currently_active) {
    if (gSchedule1Enabled &&
        is_in_range(now, gSchedule1StartMinutes, gSchedule1EndMinutes)) {
      best = min(best, minutes_until_next_daily_time(gSchedule1EndMinutes));
    }
    if (gSchedule2Enabled &&
        is_in_range(now, gSchedule2StartMinutes, gSchedule2EndMinutes)) {
      best = min(best, minutes_until_next_daily_time(gSchedule2EndMinutes));
    }
  } else {
    if (gSchedule1Enabled) {
      best = min(best, minutes_until_next_daily_time(gSchedule1StartMinutes));
    }
    if (gSchedule2Enabled) {
      best = min(best, minutes_until_next_daily_time(gSchedule2StartMinutes));
    }
  }
  return best;
}

static void format_time_12h(int hour24, int minute, char *out, size_t out_len) {
  int hour12 = hour24 % 12;
  if (hour12 == 0)
    hour12 = 12;
  const char *ampm = (hour24 < 12) ? "AM" : "PM";
  snprintf(out, out_len, "%d:%02d %s", hour12, minute, ampm);
}

static void update_time_label() {
  if (!time_label)
    return;
  char buf[24];
  format_time_12h(gClockHour, gClockMinute, buf, sizeof(buf));
  lv_label_set_text_fmt(time_label, "Lampu Raya  %s", buf);
  update_main_day_bar();
  update_schedule_day_bar();
}

static void update_pattern_label() {
  if (pattern_label) {
    lv_label_set_text_fmt(pattern_label, "Pattern: %s",
                          kPatternNames[gCurrentPatternNumber]);
  }
}

static void update_brightness_label() {
  if (brightness_label) {
    lv_label_set_text_fmt(brightness_label, "Brightness: %u", gBrightness);
  }
}

static void update_day_bar_segments(lv_obj_t *seg0, lv_obj_t *seg1,
                                    lv_obj_t *seg2, lv_obj_t *seg3) {
  if (!seg0 || !seg1 || !seg2 || !seg3) {
    return;
  }

  lv_obj_t *segs[4] = {seg0, seg1, seg2, seg3};
  for (auto *seg : segs) {
    lv_obj_add_flag(seg, LV_OBJ_FLAG_HIDDEN);
  }

  constexpr int kDayMinutes = 24 * 60;
  constexpr int kBarW = 180;
  constexpr int kBarH = 8;

  struct Interval {
    int s;
    int e;
  };
  Interval intervals[4];
  int count = 0;

  auto add_interval = [&](int s, int e) {
    if (e <= s || count >= 4)
      return;
    intervals[count++] = {s, e};
  };

  auto add_range = [&](bool enabled, int start_min, int end_min) {
    if (!enabled || start_min == end_min)
      return;
    if (start_min < end_min) {
      add_interval(start_min, end_min);
    } else {
      add_interval(start_min, kDayMinutes);
      add_interval(0, end_min);
    }
  };

  add_range(gSchedule1Enabled, gSchedule1StartMinutes, gSchedule1EndMinutes);
  add_range(gSchedule2Enabled, gSchedule2StartMinutes, gSchedule2EndMinutes);
  if (count == 0)
    return;

  for (int i = 0; i < count - 1; i++) {
    for (int j = i + 1; j < count; j++) {
      if (intervals[j].s < intervals[i].s) {
        Interval t = intervals[i];
        intervals[i] = intervals[j];
        intervals[j] = t;
      }
    }
  }

  Interval merged[4];
  int merged_count = 0;
  for (int i = 0; i < count; i++) {
    if (merged_count == 0 || intervals[i].s > merged[merged_count - 1].e) {
      if (merged_count < 4)
        merged[merged_count++] = intervals[i];
    } else {
      merged[merged_count - 1].e =
          max(merged[merged_count - 1].e, intervals[i].e);
    }
  }

  for (int i = 0; i < merged_count && i < 4; i++) {
    int x = (merged[i].s * kBarW) / kDayMinutes;
    int w = (merged[i].e * kBarW) / kDayMinutes - x;
    if (w < 2)
      w = 2;
    lv_obj_set_size(segs[i], w, kBarH);
    lv_obj_set_pos(segs[i], x, 0);
    lv_obj_remove_flag(segs[i], LV_OBJ_FLAG_HIDDEN);
  }
}

static void update_day_bar_and_marker(lv_obj_t *track, lv_obj_t *marker,
                                      lv_obj_t *seg0, lv_obj_t *seg1,
                                      lv_obj_t *seg2, lv_obj_t *seg3) {
  update_day_bar_segments(seg0, seg1, seg2, seg3);
  if (!track || !marker)
    return;
  constexpr int kDayMinutes = 24 * 60;
  constexpr int kBarW = 180;
  const int x = (now_minutes() * kBarW) / kDayMinutes;
  const int track_x = lv_obj_get_x(track);
  const int track_y = lv_obj_get_y(track);
  lv_obj_set_pos(marker, track_x + x - 1, track_y - 2);
}

static void update_schedule_day_bar() {
  update_day_bar_and_marker(
      schedule_summary_day_bar_track, schedule_summary_day_bar_marker,
      schedule_summary_day_bar_seg_0, schedule_summary_day_bar_seg_1,
      schedule_summary_day_bar_seg_2, schedule_summary_day_bar_seg_3);
}

static void update_main_day_bar() {
  update_day_bar_and_marker(main_day_bar_track, main_day_bar_marker,
                            main_day_bar_seg_0, main_day_bar_seg_1,
                            main_day_bar_seg_2, main_day_bar_seg_3);
}

static void update_schedule_labels() {
  char r1_start_buf[16];
  char r1_end_buf[16];
  char r2_start_buf[16];
  char r2_end_buf[16];
  format_time_12h(gSchedule1StartMinutes / 60, gSchedule1StartMinutes % 60,
                  r1_start_buf, sizeof(r1_start_buf));
  format_time_12h(gSchedule1EndMinutes / 60, gSchedule1EndMinutes % 60,
                  r1_end_buf, sizeof(r1_end_buf));
  format_time_12h(gSchedule2StartMinutes / 60, gSchedule2StartMinutes % 60,
                  r2_start_buf, sizeof(r2_start_buf));
  format_time_12h(gSchedule2EndMinutes / 60, gSchedule2EndMinutes % 60,
                  r2_end_buf, sizeof(r2_end_buf));
  const bool editing_r1 = (gScheduleEditTarget == SCHEDULE_EDIT_RANGE1);
  if (schedule_edit_title_label) {
    lv_label_set_text(schedule_edit_title_label,
                      editing_r1 ? "Edit Morning" : "Edit Evening");
  }
  if (schedule_range1_start_time_label) {
    lv_label_set_text(schedule_range1_start_time_label,
                      editing_r1 ? r1_start_buf : r2_start_buf);
  }
  if (schedule_range1_end_time_label) {
    lv_label_set_text(schedule_range1_end_time_label,
                      editing_r1 ? r1_end_buf : r2_end_buf);
  }

  if (schedule_summary_r1_label) {
    lv_label_set_text(schedule_summary_r1_label, "Morning");
    lv_obj_set_style_text_color(schedule_summary_r1_label,
                                gSchedule1Enabled ? lv_color_hex(0xFFFFFF)
                                                  : lv_color_hex(0x777777),
                                LV_PART_MAIN);
  }
  if (schedule_summary_r2_label) {
    lv_label_set_text(schedule_summary_r2_label, "Evening");
    lv_obj_set_style_text_color(schedule_summary_r2_label,
                                gSchedule2Enabled ? lv_color_hex(0xFFFFFF)
                                                  : lv_color_hex(0x777777),
                                LV_PART_MAIN);
  }
  if (schedule_summary_r1_time_label) {
    lv_label_set_text_fmt(schedule_summary_r1_time_label, "%s - %s",
                          r1_start_buf, r1_end_buf);
    lv_obj_set_style_text_color(schedule_summary_r1_time_label,
                                gSchedule1Enabled ? lv_color_hex(0xFFFFFF)
                                                  : lv_color_hex(0x777777),
                                LV_PART_MAIN);
  }
  if (schedule_summary_r2_time_label) {
    lv_label_set_text_fmt(schedule_summary_r2_time_label, "%s - %s",
                          r2_start_buf, r2_end_buf);
    lv_obj_set_style_text_color(schedule_summary_r2_time_label,
                                gSchedule2Enabled ? lv_color_hex(0xFFFFFF)
                                                  : lv_color_hex(0x777777),
                                LV_PART_MAIN);
  }

  update_schedule_day_bar();

  if (schedule_edit_range1_enable_switch) {
    const bool enabled = editing_r1 ? gSchedule1Enabled : gSchedule2Enabled;
    if (enabled) {
      lv_obj_add_state(schedule_edit_range1_enable_switch, LV_STATE_CHECKED);
    } else {
      lv_obj_remove_state(schedule_edit_range1_enable_switch, LV_STATE_CHECKED);
    }
  }

  update_main_day_bar();
  update_time_label();
}

static int sanitize_minutes_of_day(int v, int fallback) {
  if (v < 0 || v >= (24 * 60))
    return fallback;
  return v;
}

static uint16_t sanitize_led_count(int v) {
  if (v < 1)
    return 1;
  if (v > MAX_LEDS)
    return MAX_LEDS;
  return static_cast<uint16_t>(v);
}

static void apply_led_count(uint16_t count) {
  gLedCount = sanitize_led_count(count);
  // Immediately clear full strip so removed tail pixels are not left lit.
  turn_leds_off();
}

static void save_schedule_config() {
  if (!gSchedulePrefsReady)
    return;
  gPrefs.putUShort("s1_start", static_cast<uint16_t>(gSchedule1StartMinutes));
  gPrefs.putUShort("s1_end", static_cast<uint16_t>(gSchedule1EndMinutes));
  gPrefs.putUShort("s2_start", static_cast<uint16_t>(gSchedule2StartMinutes));
  gPrefs.putUShort("s2_end", static_cast<uint16_t>(gSchedule2EndMinutes));
  gPrefs.putBool("s1_en", gSchedule1Enabled);
  gPrefs.putBool("s2_en", gSchedule2Enabled);
  gPrefs.putUShort("led_count", gLedCount);
}

static void load_schedule_config() {
  gSchedule1StartMinutes = sanitize_minutes_of_day(
      static_cast<int>(gPrefs.getUShort(
          "s1_start", static_cast<uint16_t>(gSchedule1StartMinutes))),
      gSchedule1StartMinutes);
  gSchedule1EndMinutes = sanitize_minutes_of_day(
      static_cast<int>(gPrefs.getUShort(
          "s1_end", static_cast<uint16_t>(gSchedule1EndMinutes))),
      gSchedule1EndMinutes);
  gSchedule2StartMinutes = sanitize_minutes_of_day(
      static_cast<int>(gPrefs.getUShort(
          "s2_start", static_cast<uint16_t>(gSchedule2StartMinutes))),
      gSchedule2StartMinutes);
  gSchedule2EndMinutes = sanitize_minutes_of_day(
      static_cast<int>(gPrefs.getUShort(
          "s2_end", static_cast<uint16_t>(gSchedule2EndMinutes))),
      gSchedule2EndMinutes);
  gSchedule1Enabled = gPrefs.getBool("s1_en", gSchedule1Enabled);
  gSchedule2Enabled = gPrefs.getBool("s2_en", gSchedule2Enabled);
  gLedCount = sanitize_led_count(
      static_cast<int>(gPrefs.getUShort("led_count", gLedCount)));
}

static void apply_clock_dialog_to_target() {
  const int h = lv_spinbox_get_value(clock_hour_spin);
  const int m = lv_spinbox_get_value(clock_min_spin);
  if (gClockTarget == CLOCK_TARGET_NOW) {
    gClockHour = h;
    gClockMinute = m;
    gClockCarryMs = 0;
    update_time_label();
  } else if (gClockTarget == CLOCK_TARGET_RANGE1_START) {
    gSchedule1StartMinutes = (h * 60) + m;
    save_schedule_config();
    update_schedule_labels();
  } else if (gClockTarget == CLOCK_TARGET_RANGE1_END) {
    gSchedule1EndMinutes = (h * 60) + m;
    save_schedule_config();
    update_schedule_labels();
  } else if (gClockTarget == CLOCK_TARGET_RANGE2_START) {
    gSchedule2StartMinutes = (h * 60) + m;
    save_schedule_config();
    update_schedule_labels();
  } else {
    gSchedule2EndMinutes = (h * 60) + m;
    save_schedule_config();
    update_schedule_labels();
  }
}

static void set_active_clock_field(lv_obj_t *spin) {
  clock_active_spin = spin;

  if (clock_hour_spin) {
    const bool active = (clock_hour_spin == spin);
    lv_obj_set_style_border_width(clock_hour_spin, active ? 3 : 1,
                                  LV_PART_MAIN);
    lv_obj_set_style_border_color(
        clock_hour_spin,
        active ? lv_color_hex(0x2E7DFF) : lv_color_hex(0x666666), LV_PART_MAIN);
  }
  if (clock_min_spin) {
    const bool active = (clock_min_spin == spin);
    lv_obj_set_style_border_width(clock_min_spin, active ? 3 : 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(
        clock_min_spin,
        active ? lv_color_hex(0x2E7DFF) : lv_color_hex(0x666666), LV_PART_MAIN);
  }
}

static void clock_field_select_event_cb(lv_event_t *e) {
  if (lv_event_get_code(e) != LV_EVENT_CLICKED)
    return;
  lv_obj_t *spin = lv_event_get_target_obj(e);
  set_active_clock_field(spin);
}

static void clock_step_active_field(int step) {
  if (!clock_active_spin)
    return;

  const bool is_hour = (clock_active_spin == clock_hour_spin);
  const int min_v = 0;
  const int max_v = is_hour ? 23 : 59;
  int v = lv_spinbox_get_value(clock_active_spin);

  v += step;
  if (v > max_v)
    v = min_v;
  if (v < min_v)
    v = max_v;
  lv_spinbox_set_value(clock_active_spin, v);
}

static void open_clock_dialog(ClockTarget target) {
  gClockTarget = target;

  if (clock_title_label) {
    if (target == CLOCK_TARGET_NOW) {
      lv_label_set_text(clock_title_label, "Set Current Time");
    } else if (target == CLOCK_TARGET_RANGE1_START) {
      lv_label_set_text(clock_title_label, "Set Morning Start");
    } else if (target == CLOCK_TARGET_RANGE1_END) {
      lv_label_set_text(clock_title_label, "Set Morning End");
    } else if (target == CLOCK_TARGET_RANGE2_START) {
      lv_label_set_text(clock_title_label, "Set Evening Start");
    } else if (target == CLOCK_TARGET_RANGE2_END) {
      lv_label_set_text(clock_title_label, "Set Evening End");
    }
  }

  int h = gClockHour;
  int m = gClockMinute;
  if (target == CLOCK_TARGET_RANGE1_START) {
    h = gSchedule1StartMinutes / 60;
    m = gSchedule1StartMinutes % 60;
  } else if (target == CLOCK_TARGET_RANGE1_END) {
    h = gSchedule1EndMinutes / 60;
    m = gSchedule1EndMinutes % 60;
  } else if (target == CLOCK_TARGET_RANGE2_START) {
    h = gSchedule2StartMinutes / 60;
    m = gSchedule2StartMinutes % 60;
  } else if (target == CLOCK_TARGET_RANGE2_END) {
    h = gSchedule2EndMinutes / 60;
    m = gSchedule2EndMinutes % 60;
  }
  lv_spinbox_set_value(clock_hour_spin, h);
  lv_spinbox_set_value(clock_min_spin, m);
  set_active_clock_field(clock_hour_spin);
  lv_obj_remove_flag(clock_dialog, LV_OBJ_FLAG_HIDDEN);
}

static void close_clock_dialog() {
  lv_obj_add_flag(clock_dialog, LV_OBJ_FLAG_HIDDEN);
}

static void handle_back_navigation() {
  if (clock_dialog && !lv_obj_has_flag(clock_dialog, LV_OBJ_FLAG_HIDDEN)) {
    close_clock_dialog();
    return;
  }
  if (led_strip_page && !lv_obj_has_flag(led_strip_page, LV_OBJ_FLAG_HIDDEN)) {
    show_settings_page();
    return;
  }
  if (schedule_edit_page &&
      !lv_obj_has_flag(schedule_edit_page, LV_OBJ_FLAG_HIDDEN)) {
    show_schedule_page();
    return;
  }
  if (schedule_page && !lv_obj_has_flag(schedule_page, LV_OBJ_FLAG_HIDDEN)) {
    show_settings_page();
    return;
  }
  if (settings_page && !lv_obj_has_flag(settings_page, LV_OBJ_FLAG_HIDDEN)) {
    show_main_page();
  }
}

static void touchpad_input_read(lv_indev_t *drv, lv_indev_data_t *data) {
  static int16_t tp_x = TFT_X / 2;
  static int16_t tp_y = TFT_Y / 2;
  static bool pressed = false;

  LV_UNUSED(drv);

  if (touchpad.available()) {
    const uint8_t gesture = touchpad.gesture_id;
    if (gesture == GESTURE_SWIPE_RIGHT) {
      const uint32_t now = millis();
      if (now - gLastBackSwipeMs > 250U) {
        gLastBackSwipeMs = now;
        handle_back_navigation();
      }
    }

    int16_t x = static_cast<int16_t>(touchpad.x);
    int16_t y = static_cast<int16_t>(touchpad.y);

    if (TOUCH_SWAP_XY) {
      int16_t tmp = x;
      x = y;
      y = tmp;
    }
    if (TOUCH_FLIP_X)
      x = (TFT_X - 1) - x;
    if (TOUCH_FLIP_Y)
      y = (TFT_Y - 1) - y;

    tp_x = constrain(x, 0, TFT_X - 1);
    tp_y = constrain(y, 0, TFT_Y - 1);
    pressed = (touchpad.finger_num != 0);
  }

  data->point.x = tp_x;
  data->point.y = tp_y;
  data->state = pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
  gTouchPressed = pressed;
  if (pressed)
    gLastTouchMs = millis();
}

static void brightness_slider_event_cb(lv_event_t *e) {
  if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED)
    return;
  lv_obj_t *slider = lv_event_get_target_obj(e);
  gBrightness = static_cast<uint8_t>(lv_slider_get_value(slider));
  FastLED.setBrightness(gBrightness);
  if (!should_render_leds())
    FastLED.show();
  update_brightness_label();
}

static void run_switch_event_cb(lv_event_t *e) {
  if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED)
    return;
  lv_obj_t *sw = lv_event_get_target_obj(e);
  gLedRunning = lv_obj_has_state(sw, LV_STATE_CHECKED);
  if (!gLedRunning)
    turn_leds_off();
  update_time_label();
}

static void schedule_range1_enable_switch_event_cb(lv_event_t *e) {
  if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED)
    return;
  lv_obj_t *sw = lv_event_get_target_obj(e);
  const bool enabled = lv_obj_has_state(sw, LV_STATE_CHECKED);
  if (gScheduleEditTarget == SCHEDULE_EDIT_RANGE1) {
    gSchedule1Enabled = enabled;
  } else {
    gSchedule2Enabled = enabled;
  }
  if (!is_any_schedule_enabled()) {
    gPrevScheduleActive = false;
  } else {
    // Keep current power state; schedule applies on the next edge.
    gPrevScheduleActive = is_schedule_active_now();
  }
  save_schedule_config();
  update_schedule_labels();
}

static void show_main_page() {
  lv_obj_remove_flag(main_page, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(settings_page, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(schedule_page, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(schedule_edit_page, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(led_strip_page, LV_OBJ_FLAG_HIDDEN);
}

static void show_settings_page() {
  lv_obj_add_flag(main_page, LV_OBJ_FLAG_HIDDEN);
  lv_obj_remove_flag(settings_page, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(schedule_page, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(schedule_edit_page, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(led_strip_page, LV_OBJ_FLAG_HIDDEN);
}

static void show_schedule_page() {
  lv_obj_add_flag(main_page, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(settings_page, LV_OBJ_FLAG_HIDDEN);
  lv_obj_remove_flag(schedule_page, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(schedule_edit_page, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(led_strip_page, LV_OBJ_FLAG_HIDDEN);
  update_schedule_labels();
}

static void show_schedule_edit_page(ScheduleEditTarget target) {
  gScheduleEditTarget = target;
  lv_obj_add_flag(main_page, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(settings_page, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(schedule_page, LV_OBJ_FLAG_HIDDEN);
  lv_obj_remove_flag(schedule_edit_page, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(led_strip_page, LV_OBJ_FLAG_HIDDEN);
  update_schedule_labels();
}

static void show_led_strip_page() {
  lv_obj_add_flag(main_page, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(settings_page, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(schedule_page, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(schedule_edit_page, LV_OBJ_FLAG_HIDDEN);
  lv_obj_remove_flag(led_strip_page, LV_OBJ_FLAG_HIDDEN);
  if (led_count_spin)
    lv_spinbox_set_value(led_count_spin, gLedCount);
}

static void schedule_edit_r1_button_event_cb(lv_event_t *e) {
  if (lv_event_get_code(e) != LV_EVENT_CLICKED)
    return;
  show_schedule_edit_page(SCHEDULE_EDIT_RANGE1);
}

static void schedule_edit_r2_button_event_cb(lv_event_t *e) {
  if (lv_event_get_code(e) != LV_EVENT_CLICKED)
    return;
  show_schedule_edit_page(SCHEDULE_EDIT_RANGE2);
}

static void schedule_edit_back_button_event_cb(lv_event_t *e) {
  if (lv_event_get_code(e) != LV_EVENT_CLICKED)
    return;
  show_schedule_page();
}

static void schedule_summary_back_button_event_cb(lv_event_t *e) {
  if (lv_event_get_code(e) != LV_EVENT_CLICKED)
    return;
  show_settings_page();
}

static void schedule_button_event_cb(lv_event_t *e) {
  if (lv_event_get_code(e) != LV_EVENT_CLICKED)
    return;
  show_schedule_page();
}

static void settings_button_event_cb(lv_event_t *e) {
  if (lv_event_get_code(e) != LV_EVENT_CLICKED)
    return;
  show_settings_page();
}

static void settings_time_button_event_cb(lv_event_t *e) {
  if (lv_event_get_code(e) != LV_EVENT_CLICKED)
    return;
  open_clock_dialog(CLOCK_TARGET_NOW);
}

static void settings_back_button_event_cb(lv_event_t *e) {
  if (lv_event_get_code(e) != LV_EVENT_CLICKED)
    return;
  show_main_page();
}

static void led_strip_button_event_cb(lv_event_t *e) {
  if (lv_event_get_code(e) != LV_EVENT_CLICKED)
    return;
  show_led_strip_page();
}

static void led_strip_back_button_event_cb(lv_event_t *e) {
  if (lv_event_get_code(e) != LV_EVENT_CLICKED)
    return;
  show_settings_page();
}

static void led_strip_save_button_event_cb(lv_event_t *e) {
  if (lv_event_get_code(e) != LV_EVENT_CLICKED)
    return;
  if (!led_count_spin)
    return;
  apply_led_count(static_cast<uint16_t>(lv_spinbox_get_value(led_count_spin)));
  save_schedule_config();
  show_settings_page();
}

static void led_strip_inc_event_cb(lv_event_t *e) {
  const lv_event_code_t code = lv_event_get_code(e);
  if (code != LV_EVENT_CLICKED && code != LV_EVENT_LONG_PRESSED_REPEAT)
    return;
  if (!led_count_spin)
    return;
  int v = lv_spinbox_get_value(led_count_spin);
  if (v < MAX_LEDS)
    lv_spinbox_set_value(led_count_spin, v + 1);
}

static void led_strip_dec_event_cb(lv_event_t *e) {
  const lv_event_code_t code = lv_event_get_code(e);
  if (code != LV_EVENT_CLICKED && code != LV_EVENT_LONG_PRESSED_REPEAT)
    return;
  if (!led_count_spin)
    return;
  int v = lv_spinbox_get_value(led_count_spin);
  if (v > 1)
    lv_spinbox_set_value(led_count_spin, v - 1);
}

static void schedule_set_r1_start_button_event_cb(lv_event_t *e) {
  if (lv_event_get_code(e) != LV_EVENT_CLICKED)
    return;
  open_clock_dialog(gScheduleEditTarget == SCHEDULE_EDIT_RANGE1
                        ? CLOCK_TARGET_RANGE1_START
                        : CLOCK_TARGET_RANGE2_START);
}

static void schedule_set_r1_end_button_event_cb(lv_event_t *e) {
  if (lv_event_get_code(e) != LV_EVENT_CLICKED)
    return;
  open_clock_dialog(gScheduleEditTarget == SCHEDULE_EDIT_RANGE1
                        ? CLOCK_TARGET_RANGE1_END
                        : CLOCK_TARGET_RANGE2_END);
}

static void clock_ok_button_event_cb(lv_event_t *e) {
  if (lv_event_get_code(e) != LV_EVENT_CLICKED)
    return;
  apply_clock_dialog_to_target();
  close_clock_dialog();
  gPrevScheduleActive = is_schedule_active_now();
}

static void clock_cancel_button_event_cb(lv_event_t *e) {
  if (lv_event_get_code(e) != LV_EVENT_CLICKED)
    return;
  close_clock_dialog();
}

static void spin_inc_event_cb(lv_event_t *e) {
  if (lv_event_get_code(e) != LV_EVENT_CLICKED)
    return;
  clock_step_active_field(+1);
}

static void spin_dec_event_cb(lv_event_t *e) {
  if (lv_event_get_code(e) != LV_EVENT_CLICKED)
    return;
  clock_step_active_field(-1);
}

static void init_fastled() {
  FastLED.addLeds<LED_TYPE, LED_DATA_PIN, LED_COLOR_ORDER>(leds, MAX_LEDS)
      .setCorrection(TypicalLEDStrip);
  FastLED.setBrightness(gBrightness);
  turn_leds_off();
}

static void nextPattern() {
  gCurrentPatternNumber =
      (gCurrentPatternNumber + 1) % (sizeof(gPatterns) / sizeof(gPatterns[0]));
  update_pattern_label();
}

static void run_fastled() {
  if (!should_render_leds()) {
    if (gLedsOn)
      turn_leds_off();
    return;
  }

  static uint32_t last_frame_ms = 0;
  static uint32_t last_hue_ms = 0;
  static uint32_t last_pattern_ms = 0;
  const uint32_t now = millis();

  if (now - last_frame_ms >= (1000U / LED_FPS)) {
    gPatterns[gCurrentPatternNumber]();
    FastLED.show();
    gLedsOn = true;
    last_frame_ms = now;
  }

  if (now - last_hue_ms >= 20U) {
    gHue++;
    last_hue_ms = now;
  }

  if (now - last_pattern_ms >= PATTERN_SWITCH_MS) {
    nextPattern();
    last_pattern_ms = now;
  }
}

static void update_software_clock() {
  const uint32_t now = millis();
  if (gLastClockTickMs == 0) {
    gLastClockTickMs = now;
    return;
  }
  gClockCarryMs += (now - gLastClockTickMs);
  gLastClockTickMs = now;

  bool changed = false;
  while (gClockCarryMs >= 60000U) {
    gClockCarryMs -= 60000U;
    gClockMinute++;
    if (gClockMinute >= 60) {
      gClockMinute = 0;
      gClockHour = (gClockHour + 1) % 24;
    }
    changed = true;
  }
  if (changed)
    update_time_label();
}

static void rainbow() { fill_rainbow(leds, gLedCount, gHue, 7); }

static void rainbowWithGlitter() {
  rainbow();
  addGlitter(80);
}

static void addGlitter(fract8 chanceOfGlitter) {
  if (random8() < chanceOfGlitter)
    leds[random16(gLedCount)] += CRGB::White;
}

static void confetti() {
  fadeToBlackBy(leds, gLedCount, 10);
  int pos = random16(gLedCount);
  leds[pos] += CHSV(gHue + random8(64), 200, 255);
}

static void sinelon() {
  fadeToBlackBy(leds, gLedCount, 20);
  int pos = beatsin16(13, 0, gLedCount - 1);
  leds[pos] += CHSV(gHue, 255, 192);
}

static void bpm() {
  uint8_t BeatsPerMinute = 62;
  CRGBPalette16 palette = PartyColors_p;
  uint8_t beat = beatsin8(BeatsPerMinute, 64, 255);
  for (int i = 0; i < gLedCount; i++) {
    leds[i] = ColorFromPalette(palette, gHue + (i * 2), beat - gHue + (i * 10));
  }
}

static void juggle() {
  fadeToBlackBy(leds, gLedCount, 20);
  uint8_t dothue = 0;
  for (int i = 0; i < 8; i++) {
    leds[beatsin16(i + 7, 0, gLedCount - 1)] |= CHSV(dothue, 200, 255);
    dothue += 32;
  }
}

static lv_obj_t *make_button(lv_obj_t *parent, const char *txt,
                             lv_event_cb_t cb) {
  lv_obj_t *btn = lv_button_create(parent);
  lv_obj_t *lbl = lv_label_create(btn);
  lv_label_set_text(lbl, txt);
  lv_obj_center(lbl);
  lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, nullptr);
  return btn;
}

static lv_obj_t *make_button_ud(lv_obj_t *parent, const char *txt,
                                lv_event_cb_t cb, void *user_data) {
  lv_obj_t *btn = lv_button_create(parent);
  lv_obj_t *lbl = lv_label_create(btn);
  lv_label_set_text(lbl, txt);
  lv_obj_center(lbl);
  lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, user_data);
  return btn;
}

void setup() {
  Serial.begin(115200);
  Serial.println("boot");

  gSchedulePrefsReady = gPrefs.begin("lampu_cfg", false);
  if (gSchedulePrefsReady) {
    load_schedule_config();
  } else {
    Serial.println("warn: prefs unavailable, schedule not persisted");
  }

  analogWriteResolution(8);
  set_backlight(DISPLAY_BACKLIGHT_ON_LEVEL);

  tft.init();
  tft.setRotation(TFT_ROTATION);
  tft.setSwapBytes(true);
  tft.invertDisplay(true);
  tft.fillScreen(TFT_BLACK);

  touchI2C.setPins(TP_SDA, TP_SCL);
  touchpad.begin(mode_change);
  Serial.println(touchpad.version());

  lv_init();
  lv_tick_set_cb(millis);

  disp = lv_display_create(TFT_X, TFT_Y);
  lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565);
  lv_display_set_flush_cb(disp, disp_flush_cb);
  lv_display_set_buffers(disp, lv_buf_1, lv_buf_2, sizeof(lv_buf_1),
                         LV_DISPLAY_RENDER_MODE_PARTIAL);

  indev = lv_indev_create();
  lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
  lv_indev_set_read_cb(indev, touchpad_input_read);
  lv_indev_set_display(indev, disp);

  lv_obj_t *scr = lv_screen_active();
  lv_obj_set_style_bg_color(scr, lv_color_hex(0x000000), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);

  main_page = lv_obj_create(scr);
  lv_obj_remove_style_all(main_page);
  lv_obj_set_size(main_page, TFT_X, TFT_Y);
  lv_obj_set_style_bg_opa(main_page, LV_OPA_TRANSP, LV_PART_MAIN);

  settings_page = lv_obj_create(scr);
  lv_obj_remove_style_all(settings_page);
  lv_obj_set_size(settings_page, TFT_X, TFT_Y);
  lv_obj_set_style_bg_opa(settings_page, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_add_flag(settings_page, LV_OBJ_FLAG_HIDDEN);

  schedule_page = lv_obj_create(scr);
  lv_obj_remove_style_all(schedule_page);
  lv_obj_set_size(schedule_page, TFT_X, TFT_Y);
  lv_obj_set_style_bg_opa(schedule_page, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_add_flag(schedule_page, LV_OBJ_FLAG_HIDDEN);

  schedule_edit_page = lv_obj_create(scr);
  lv_obj_remove_style_all(schedule_edit_page);
  lv_obj_set_size(schedule_edit_page, TFT_X, TFT_Y);
  lv_obj_set_style_bg_opa(schedule_edit_page, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_add_flag(schedule_edit_page, LV_OBJ_FLAG_HIDDEN);

  led_strip_page = lv_obj_create(scr);
  lv_obj_remove_style_all(led_strip_page);
  lv_obj_set_size(led_strip_page, TFT_X, TFT_Y);
  lv_obj_set_style_bg_opa(led_strip_page, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_add_flag(led_strip_page, LV_OBJ_FLAG_HIDDEN);

  // Main page widgets
  time_label = lv_label_create(main_page);
  lv_obj_set_style_text_color(time_label, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
  lv_obj_align(time_label, LV_ALIGN_TOP_MID, 0, 6);
  update_time_label();

  remaining_label = lv_label_create(main_page);
  lv_obj_set_style_text_color(remaining_label, lv_color_hex(0xC9F99D),
                              LV_PART_MAIN);
  lv_obj_align(remaining_label, LV_ALIGN_TOP_MID, 0, 28);
  lv_obj_add_flag(remaining_label, LV_OBJ_FLAG_HIDDEN);

  main_day_bar_track = lv_obj_create(main_page);
  lv_obj_remove_style_all(main_day_bar_track);
  lv_obj_set_size(main_day_bar_track, 180, 8);
  lv_obj_align(main_day_bar_track, LV_ALIGN_TOP_MID, 0, 32);
  lv_obj_set_style_bg_color(main_day_bar_track, lv_color_hex(0x4A4A4A),
                            LV_PART_MAIN);
  lv_obj_set_style_bg_opa(main_day_bar_track, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_radius(main_day_bar_track, 4, LV_PART_MAIN);

  main_day_bar_seg_0 = lv_obj_create(main_day_bar_track);
  main_day_bar_seg_1 = lv_obj_create(main_day_bar_track);
  main_day_bar_seg_2 = lv_obj_create(main_day_bar_track);
  main_day_bar_seg_3 = lv_obj_create(main_day_bar_track);
  lv_obj_t *main_bar_segs[4] = {main_day_bar_seg_0, main_day_bar_seg_1,
                                main_day_bar_seg_2, main_day_bar_seg_3};
  for (auto *seg : main_bar_segs) {
    lv_obj_remove_style_all(seg);
    lv_obj_set_style_bg_color(seg, lv_color_hex(0x4CD964), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(seg, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(seg, 4, LV_PART_MAIN);
  }

  main_day_bar_marker = lv_obj_create(main_page);
  lv_obj_remove_style_all(main_day_bar_marker);
  lv_obj_set_size(main_day_bar_marker, 2, 12);
  lv_obj_set_style_bg_color(main_day_bar_marker, lv_color_hex(0xFFFFFF),
                            LV_PART_MAIN);
  lv_obj_set_style_bg_opa(main_day_bar_marker, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_radius(main_day_bar_marker, 1, LV_PART_MAIN);
  update_main_day_bar();

  lv_obj_t *settings_btn =
      make_button(main_page, "Settings", settings_button_event_cb);
  lv_obj_set_size(settings_btn, 120, 32);
  lv_obj_align(settings_btn, LV_ALIGN_BOTTOM_MID, 0, -12);

  pattern_label = lv_label_create(main_page);
  lv_obj_set_style_text_color(pattern_label, lv_color_hex(0xFFFFFF),
                              LV_PART_MAIN);
  lv_obj_align(pattern_label, LV_ALIGN_TOP_MID, 0, 66);
  update_pattern_label();

  brightness_slider = lv_slider_create(main_page);
  lv_slider_set_range(brightness_slider, 1, 255);
  lv_slider_set_value(brightness_slider, gBrightness, LV_ANIM_OFF);
  lv_obj_set_size(brightness_slider, 180, 16);
  lv_obj_align_to(brightness_slider, pattern_label, LV_ALIGN_OUT_BOTTOM_MID, 0,
                  20);
  lv_obj_add_event_cb(brightness_slider, brightness_slider_event_cb,
                      LV_EVENT_VALUE_CHANGED, nullptr);

  brightness_label = lv_label_create(main_page);
  lv_obj_set_style_text_color(brightness_label, lv_color_hex(0xFFFFFF),
                              LV_PART_MAIN);
  lv_obj_set_width(brightness_label, TFT_X);
  lv_obj_set_style_text_align(brightness_label, LV_TEXT_ALIGN_CENTER,
                              LV_PART_MAIN);
  lv_obj_align_to(brightness_label, brightness_slider, LV_ALIGN_OUT_BOTTOM_MID,
                  0, 16);
  update_brightness_label();

  run_switch = lv_switch_create(main_page);
  if (gLedRunning)
    lv_obj_add_state(run_switch, LV_STATE_CHECKED);
  lv_obj_align_to(run_switch, brightness_label, LV_ALIGN_OUT_BOTTOM_MID, 26,
                  20);
  lv_obj_add_event_cb(run_switch, run_switch_event_cb, LV_EVENT_VALUE_CHANGED,
                      nullptr);

  run_switch_label = lv_label_create(main_page);
  lv_obj_set_style_text_color(run_switch_label, lv_color_hex(0xFFFFFF),
                              LV_PART_MAIN);
  lv_label_set_text(run_switch_label, "Power");
  lv_obj_align_to(run_switch_label, run_switch, LV_ALIGN_OUT_LEFT_MID, -10, 0);

  // Settings page widgets
  lv_obj_t *settings_title = lv_label_create(settings_page);
  lv_obj_set_style_text_color(settings_title, lv_color_hex(0xFFFFFF),
                              LV_PART_MAIN);
  lv_label_set_text(settings_title, "Settings");
  lv_obj_align(settings_title, LV_ALIGN_TOP_MID, 0, 8);

  lv_obj_t *settings_back_btn =
      make_button(settings_page, "Back", settings_back_button_event_cb);
  lv_obj_set_size(settings_back_btn, 90, 34);
  lv_obj_align(settings_back_btn, LV_ALIGN_BOTTOM_MID, 0, -12);

  lv_obj_t *led_strip_btn =
      make_button(settings_page, "LED Strip", led_strip_button_event_cb);
  lv_obj_set_size(led_strip_btn, 180, 40);
  lv_obj_align(led_strip_btn, LV_ALIGN_TOP_MID, 0, 56);

  lv_obj_t *time_btn =
      make_button(settings_page, "Time", settings_time_button_event_cb);
  lv_obj_set_size(time_btn, 180, 40);
  lv_obj_align(time_btn, LV_ALIGN_TOP_MID, 0, 108);

  lv_obj_t *schedule_btn =
      make_button(settings_page, "Schedule", schedule_button_event_cb);
  lv_obj_set_size(schedule_btn, 180, 40);
  lv_obj_align(schedule_btn, LV_ALIGN_TOP_MID, 0, 160);

  // LED strip page widgets
  lv_obj_t *led_strip_title = lv_label_create(led_strip_page);
  lv_obj_set_style_text_color(led_strip_title, lv_color_hex(0xFFFFFF),
                              LV_PART_MAIN);
  lv_label_set_text(led_strip_title, "LED Strip");
  lv_obj_align(led_strip_title, LV_ALIGN_TOP_MID, 0, 8);

  lv_obj_t *led_count_label = lv_label_create(led_strip_page);
  lv_obj_set_style_text_color(led_count_label, lv_color_hex(0xFFFFFF),
                              LV_PART_MAIN);
  lv_label_set_text(led_count_label, "LED Count");
  lv_obj_align(led_count_label, LV_ALIGN_TOP_MID, 0, 48);

  led_count_spin = lv_spinbox_create(led_strip_page);
  lv_spinbox_set_range(led_count_spin, 1, MAX_LEDS);
  lv_spinbox_set_digit_format(led_count_spin, 3, 0);
  lv_spinbox_set_value(led_count_spin, gLedCount);
  lv_obj_set_size(led_count_spin, 96, 48);
  lv_obj_align(led_count_spin, LV_ALIGN_TOP_MID, 0, 76);
  lv_obj_set_style_bg_color(led_count_spin, lv_color_hex(0x1B1B1B),
                            LV_PART_MAIN);
  lv_obj_set_style_text_color(led_count_spin, lv_color_hex(0xFFFFFF),
                              LV_PART_MAIN);
  lv_obj_set_style_text_align(led_count_spin, LV_TEXT_ALIGN_CENTER,
                              LV_PART_MAIN);

  lv_obj_t *led_inc_btn =
      make_button(led_strip_page, "+", led_strip_inc_event_cb);
  lv_obj_set_size(led_inc_btn, 70, 40);
  lv_obj_align(led_inc_btn, LV_ALIGN_TOP_MID, 48, 136);
  lv_obj_add_event_cb(led_inc_btn, led_strip_inc_event_cb,
                      LV_EVENT_LONG_PRESSED_REPEAT, nullptr);

  lv_obj_t *led_dec_btn =
      make_button(led_strip_page, "-", led_strip_dec_event_cb);
  lv_obj_set_size(led_dec_btn, 70, 40);
  lv_obj_align(led_dec_btn, LV_ALIGN_TOP_MID, -48, 136);
  lv_obj_add_event_cb(led_dec_btn, led_strip_dec_event_cb,
                      LV_EVENT_LONG_PRESSED_REPEAT, nullptr);

  lv_obj_t *led_save_btn =
      make_button(led_strip_page, "Save", led_strip_save_button_event_cb);
  lv_obj_set_size(led_save_btn, 90, 34);
  lv_obj_align(led_save_btn, LV_ALIGN_BOTTOM_RIGHT, -18, -12);

  lv_obj_t *led_back_btn =
      make_button(led_strip_page, "Back", led_strip_back_button_event_cb);
  lv_obj_set_size(led_back_btn, 90, 34);
  lv_obj_align(led_back_btn, LV_ALIGN_BOTTOM_LEFT, 18, -12);

  // Schedule summary page widgets
  lv_obj_t *sched_title = lv_label_create(schedule_page);
  lv_obj_set_style_text_color(sched_title, lv_color_hex(0xFFFFFF),
                              LV_PART_MAIN);
  lv_label_set_text(sched_title, "Schedule");
  lv_obj_align(sched_title, LV_ALIGN_TOP_MID, 0, 8);

  lv_obj_t *summary_back_btn =
      make_button(schedule_page, "Back", schedule_summary_back_button_event_cb);
  lv_obj_set_size(summary_back_btn, 90, 34);
  lv_obj_align(summary_back_btn, LV_ALIGN_BOTTOM_MID, 0, -12);

  schedule_summary_r1_label = lv_label_create(schedule_page);
  lv_obj_set_style_text_color(schedule_summary_r1_label, lv_color_hex(0xFFFFFF),
                              LV_PART_MAIN);
  lv_obj_align(schedule_summary_r1_label, LV_ALIGN_TOP_LEFT, 16, 62);

  schedule_summary_r1_time_label = lv_label_create(schedule_page);
  lv_obj_set_style_text_color(schedule_summary_r1_time_label,
                              lv_color_hex(0xFFFFFF), LV_PART_MAIN);
  lv_obj_align(schedule_summary_r1_time_label, LV_ALIGN_TOP_LEFT, 16, 82);

  lv_obj_t *edit_r1_btn =
      make_button(schedule_page, "Edit", schedule_edit_r1_button_event_cb);
  lv_obj_set_size(edit_r1_btn, 64, 30);
  lv_obj_align(edit_r1_btn, LV_ALIGN_TOP_RIGHT, -12, 74);

  schedule_summary_r2_label = lv_label_create(schedule_page);
  lv_obj_set_style_text_color(schedule_summary_r2_label, lv_color_hex(0xFFFFFF),
                              LV_PART_MAIN);
  lv_obj_align(schedule_summary_r2_label, LV_ALIGN_TOP_LEFT, 16, 138);

  schedule_summary_r2_time_label = lv_label_create(schedule_page);
  lv_obj_set_style_text_color(schedule_summary_r2_time_label,
                              lv_color_hex(0xFFFFFF), LV_PART_MAIN);
  lv_obj_align(schedule_summary_r2_time_label, LV_ALIGN_TOP_LEFT, 16, 158);

  lv_obj_t *edit_r2_btn =
      make_button(schedule_page, "Edit", schedule_edit_r2_button_event_cb);
  lv_obj_set_size(edit_r2_btn, 64, 30);
  lv_obj_align(edit_r2_btn, LV_ALIGN_TOP_RIGHT, -12, 150);

  schedule_summary_day_bar_track = lv_obj_create(schedule_page);
  lv_obj_remove_style_all(schedule_summary_day_bar_track);
  lv_obj_set_size(schedule_summary_day_bar_track, 180, 8);
  lv_obj_align(schedule_summary_day_bar_track, LV_ALIGN_TOP_MID, 0, 32);
  lv_obj_set_style_bg_color(schedule_summary_day_bar_track,
                            lv_color_hex(0x4A4A4A), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(schedule_summary_day_bar_track, LV_OPA_COVER,
                          LV_PART_MAIN);
  lv_obj_set_style_radius(schedule_summary_day_bar_track, 4, LV_PART_MAIN);

  schedule_summary_day_bar_seg_0 =
      lv_obj_create(schedule_summary_day_bar_track);
  schedule_summary_day_bar_seg_1 =
      lv_obj_create(schedule_summary_day_bar_track);
  schedule_summary_day_bar_seg_2 =
      lv_obj_create(schedule_summary_day_bar_track);
  schedule_summary_day_bar_seg_3 =
      lv_obj_create(schedule_summary_day_bar_track);
  lv_obj_t *bar_segs[4] = {
      schedule_summary_day_bar_seg_0, schedule_summary_day_bar_seg_1,
      schedule_summary_day_bar_seg_2, schedule_summary_day_bar_seg_3};
  for (auto *seg : bar_segs) {
    lv_obj_remove_style_all(seg);
    lv_obj_set_style_bg_color(seg, lv_color_hex(0x4CD964), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(seg, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(seg, 4, LV_PART_MAIN);
  }

  schedule_summary_day_bar_marker = lv_obj_create(schedule_page);
  lv_obj_remove_style_all(schedule_summary_day_bar_marker);
  lv_obj_set_size(schedule_summary_day_bar_marker, 2, 12);
  lv_obj_set_style_bg_color(schedule_summary_day_bar_marker,
                            lv_color_hex(0xFFFFFF), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(schedule_summary_day_bar_marker, LV_OPA_COVER,
                          LV_PART_MAIN);
  lv_obj_set_style_radius(schedule_summary_day_bar_marker, 1, LV_PART_MAIN);

  // Range edit page widgets
  schedule_edit_title_label = lv_label_create(schedule_edit_page);
  lv_obj_set_style_text_color(schedule_edit_title_label, lv_color_hex(0xFFFFFF),
                              LV_PART_MAIN);
  lv_label_set_text(schedule_edit_title_label, "Edit Morning");
  lv_obj_align(schedule_edit_title_label, LV_ALIGN_TOP_MID, 0, 8);

  lv_obj_t *edit_back_btn = make_button(schedule_edit_page, "Back",
                                        schedule_edit_back_button_event_cb);
  lv_obj_set_size(edit_back_btn, 90, 34);
  lv_obj_align(edit_back_btn, LV_ALIGN_BOTTOM_MID, 0, -12);

  lv_obj_t *range_enable_label = lv_label_create(schedule_edit_page);
  lv_obj_set_style_text_color(range_enable_label, lv_color_hex(0xFFFFFF),
                              LV_PART_MAIN);
  lv_label_set_text(range_enable_label, "Enable");
  lv_obj_align(range_enable_label, LV_ALIGN_TOP_LEFT, 20, 48);

  schedule_edit_range1_enable_switch = lv_switch_create(schedule_edit_page);
  lv_obj_add_state(schedule_edit_range1_enable_switch, LV_STATE_CHECKED);
  lv_obj_align(schedule_edit_range1_enable_switch, LV_ALIGN_TOP_RIGHT, -20, 42);
  lv_obj_add_event_cb(schedule_edit_range1_enable_switch,
                      schedule_range1_enable_switch_event_cb,
                      LV_EVENT_VALUE_CHANGED, nullptr);

  schedule_range1_start_btn = make_button(
      schedule_edit_page, "Start", schedule_set_r1_start_button_event_cb);
  lv_obj_set_size(schedule_range1_start_btn, 96, 34);
  lv_obj_align(schedule_range1_start_btn, LV_ALIGN_TOP_LEFT, 14, 92);

  schedule_range1_end_btn = make_button(schedule_edit_page, "End",
                                        schedule_set_r1_end_button_event_cb);
  lv_obj_set_size(schedule_range1_end_btn, 96, 34);
  lv_obj_align(schedule_range1_end_btn, LV_ALIGN_TOP_RIGHT, -14, 92);

  schedule_range1_start_time_label = lv_label_create(schedule_edit_page);
  lv_obj_set_style_text_color(schedule_range1_start_time_label,
                              lv_color_hex(0xFFFFFF), LV_PART_MAIN);
  lv_obj_align_to(schedule_range1_start_time_label, schedule_range1_start_btn,
                  LV_ALIGN_OUT_BOTTOM_MID, 0, 6);

  schedule_range1_end_time_label = lv_label_create(schedule_edit_page);
  lv_obj_set_style_text_color(schedule_range1_end_time_label,
                              lv_color_hex(0xFFFFFF), LV_PART_MAIN);
  lv_obj_align_to(schedule_range1_end_time_label, schedule_range1_end_btn,
                  LV_ALIGN_OUT_BOTTOM_MID, 0, 6);

  update_schedule_labels();

  // Clock dialog
  clock_dialog = lv_obj_create(scr);
  lv_obj_set_size(clock_dialog, TFT_X, TFT_Y);
  lv_obj_center(clock_dialog);
  lv_obj_set_style_bg_color(clock_dialog, lv_color_hex(0x101010), LV_PART_MAIN);
  lv_obj_set_style_border_width(clock_dialog, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(clock_dialog, 12, LV_PART_MAIN);
  lv_obj_set_scrollbar_mode(clock_dialog, LV_SCROLLBAR_MODE_OFF);
  lv_obj_add_flag(clock_dialog, LV_OBJ_FLAG_HIDDEN);

  clock_title_label = lv_label_create(clock_dialog);
  lv_obj_set_style_text_color(clock_title_label, lv_color_hex(0xFFFFFF),
                              LV_PART_MAIN);
  lv_label_set_text(clock_title_label, "Set Current Time");
  lv_obj_align(clock_title_label, LV_ALIGN_TOP_MID, 0, 6);

  lv_obj_t *hour_lbl = lv_label_create(clock_dialog);
  lv_obj_set_style_text_color(hour_lbl, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
  lv_label_set_text(hour_lbl, "Hour");
  lv_obj_align(hour_lbl, LV_ALIGN_TOP_LEFT, 24, 30);

  lv_obj_t *min_lbl = lv_label_create(clock_dialog);
  lv_obj_set_style_text_color(min_lbl, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
  lv_label_set_text(min_lbl, "Minute");
  lv_obj_align(min_lbl, LV_ALIGN_TOP_RIGHT, -44, 30);

  clock_hour_spin = lv_spinbox_create(clock_dialog);
  lv_spinbox_set_range(clock_hour_spin, 0, 23);
  lv_spinbox_set_digit_format(clock_hour_spin, 2, 0);
  lv_obj_set_size(clock_hour_spin, 78, 46);
  lv_obj_align(clock_hour_spin, LV_ALIGN_TOP_LEFT, 20, 60);
  lv_obj_add_event_cb(clock_hour_spin, clock_field_select_event_cb,
                      LV_EVENT_CLICKED, nullptr);
  lv_obj_set_style_bg_color(clock_hour_spin, lv_color_hex(0x1B1B1B),
                            LV_PART_MAIN);
  lv_obj_set_style_text_color(clock_hour_spin, lv_color_hex(0xFFFFFF),
                              LV_PART_MAIN);

  clock_min_spin = lv_spinbox_create(clock_dialog);
  lv_spinbox_set_range(clock_min_spin, 0, 59);
  lv_spinbox_set_digit_format(clock_min_spin, 2, 0);
  lv_obj_set_size(clock_min_spin, 78, 46);
  lv_obj_align(clock_min_spin, LV_ALIGN_TOP_RIGHT, -20, 60);
  lv_obj_add_event_cb(clock_min_spin, clock_field_select_event_cb,
                      LV_EVENT_CLICKED, nullptr);
  lv_obj_set_style_bg_color(clock_min_spin, lv_color_hex(0x1B1B1B),
                            LV_PART_MAIN);
  lv_obj_set_style_text_color(clock_min_spin, lv_color_hex(0xFFFFFF),
                              LV_PART_MAIN);

  lv_obj_t *shared_up = make_button(clock_dialog, "+", spin_inc_event_cb);
  lv_obj_set_size(shared_up, 90, 40);
  lv_obj_align(shared_up, LV_ALIGN_TOP_MID, 52, 124);

  lv_obj_t *shared_down = make_button(clock_dialog, "-", spin_dec_event_cb);
  lv_obj_set_size(shared_down, 90, 40);
  lv_obj_align_to(shared_down, shared_up, LV_ALIGN_OUT_LEFT_MID, -14, 0);

  lv_obj_t *ok_btn = make_button(clock_dialog, "OK", clock_ok_button_event_cb);
  lv_obj_set_size(ok_btn, 78, 40);
  lv_obj_align(ok_btn, LV_ALIGN_BOTTOM_RIGHT, -12, -12);

  lv_obj_t *cancel_btn =
      make_button(clock_dialog, "Cancel", clock_cancel_button_event_cb);
  lv_obj_set_size(cancel_btn, 78, 40);
  lv_obj_align(cancel_btn, LV_ALIGN_BOTTOM_LEFT, 12, -12);

  init_fastled();
  gPrevScheduleActive = is_schedule_active_now();
  gLastTouchMs = millis();
}

void loop() {
  lv_timer_handler();
  update_software_clock();

  if (is_any_schedule_enabled()) {
    const bool schedule_active = is_schedule_active_now();
    if (gLedRunning && gPrevScheduleActive && !schedule_active) {
      gLedRunning = false;
      if (run_switch)
        lv_obj_remove_state(run_switch, LV_STATE_CHECKED);
      turn_leds_off();
      update_time_label();
    } else if (!gLedRunning && !gPrevScheduleActive && schedule_active) {
      gLedRunning = true;
      if (run_switch)
        lv_obj_add_state(run_switch, LV_STATE_CHECKED);
      update_time_label();
    }
    gPrevScheduleActive = schedule_active;
  } else {
    gPrevScheduleActive = false;
  }

  const uint32_t now = millis();
  if ((now - gLastTouchMs) >= DISPLAY_SLEEP_TIMEOUT_MS) {
    sleep_display();
  } else if (gTouchPressed || ((now - gLastTouchMs) < 200U)) {
    wake_display();
  }

  const bool ui_priority = gTouchPressed || ((now - gLastTouchMs) < 200U);
  if (!ui_priority) {
    run_fastled();
  }

  delay(5);
}
