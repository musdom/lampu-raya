/* lvgl + fastled controller on P169H002-CTP display */

#include <Arduino.h>
#include <SPI.h>
#include <TFT_eSPI.h>
#include <Wire.h>
#include <FastLED.h>
#include "cst816t.h"
#include "lvgl.h"

// display
#define TFT_X 240
#define TFT_Y 280
#define TFT_ROTATION 0

// touch
#define TP_SDA 4
#define TP_SCL 5
#define TP_RST 3
#define TP_IRQ 2

// FastLED
#define LED_DATA_PIN 20
#define LED_TYPE WS2811
#define LED_COLOR_ORDER GRB
#define NUM_LEDS 300
#define LED_FPS 120
#define PATTERN_SWITCH_MS 10000

// Touch orientation tuning.
#define TOUCH_SWAP_XY 0
#define TOUCH_FLIP_X 0
#define TOUCH_FLIP_Y 0

enum ClockTarget { CLOCK_TARGET_NOW = 0, CLOCK_TARGET_START = 1, CLOCK_TARGET_END = 2 };

TwoWire touchI2C = TwoWire(0);
cst816t touchpad(touchI2C, TP_RST, TP_IRQ);
TFT_eSPI tft = TFT_eSPI();
CRGB leds[NUM_LEDS];

static lv_display_t *disp = nullptr;
static lv_indev_t *indev = nullptr;
static uint16_t lv_buf_1[TFT_X * 20];
static uint16_t lv_buf_2[TFT_X * 20];

static lv_obj_t *main_page = nullptr;
static lv_obj_t *schedule_page = nullptr;
static lv_obj_t *clock_dialog = nullptr;
static lv_obj_t *clock_title_label = nullptr;
static lv_obj_t *clock_hour_spin = nullptr;
static lv_obj_t *clock_min_spin = nullptr;
static lv_obj_t *clock_active_spin = nullptr;

static lv_obj_t *time_label = nullptr;
static lv_obj_t *remaining_label = nullptr;
static lv_obj_t *pattern_label = nullptr;
static lv_obj_t *brightness_label = nullptr;
static lv_obj_t *brightness_slider = nullptr;
static lv_obj_t *run_switch = nullptr;
static lv_obj_t *run_switch_label = nullptr;
static lv_obj_t *schedule_enable_switch = nullptr;
static lv_obj_t *schedule_enable_label = nullptr;
static lv_obj_t *schedule_start_label = nullptr;
static lv_obj_t *schedule_end_label = nullptr;

static uint8_t gCurrentPatternNumber = 0;
static uint8_t gHue = 0;
static uint8_t gBrightness = 96;
static bool gLedRunning = true;
static bool gLedsOn = false;
static bool gTouchPressed = false;
static uint32_t gLastTouchMs = 0;

static int gClockHour = 19;
static int gClockMinute = 0;
static int gScheduleStartMinutes = 19 * 60;
static int gScheduleEndMinutes = 23 * 60;
static uint32_t gClockCarryMs = 0;
static uint32_t gLastClockTickMs = 0;
static ClockTarget gClockTarget = CLOCK_TARGET_NOW;
static bool gPrevScheduleActive = false;
static bool gScheduleEnabled = true;

// Forward declarations for FastLED patterns
static void rainbow();
static void rainbowWithGlitter();
static void confetti();
static void sinelon();
static void juggle();
static void bpm();
static void addGlitter(fract8 chanceOfGlitter);
static void nextPattern();

typedef void (*SimplePatternList[])();
SimplePatternList gPatterns = {
    rainbow, rainbowWithGlitter, confetti, sinelon, juggle, bpm};
static const char *kPatternNames[] = {"Rainbow", "Rainbow + Glitter",
                                      "Confetti", "Sinelon",
                                      "Juggle",  "BPM"};

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
  fill_solid(leds, NUM_LEDS, CRGB::Black);
  FastLED.show();
  gLedsOn = false;
}

static int now_minutes() { return (gClockHour * 60) + gClockMinute; }

static bool is_schedule_active_now() {
  const int now = now_minutes();
  if (gScheduleStartMinutes <= gScheduleEndMinutes) {
    return (now >= gScheduleStartMinutes) && (now < gScheduleEndMinutes);
  }
  return (now >= gScheduleStartMinutes) || (now < gScheduleEndMinutes);
}

static bool should_render_leds() {
  return gLedRunning;
}

static int minutes_until_next_daily_time(int target_minutes) {
  const int now = now_minutes();
  int diff = target_minutes - now;
  if (diff <= 0) diff += 24 * 60;
  return diff;
}

static void format_time_12h(int hour24, int minute, char *out, size_t out_len) {
  int hour12 = hour24 % 12;
  if (hour12 == 0) hour12 = 12;
  const char *ampm = (hour24 < 12) ? "AM" : "PM";
  snprintf(out, out_len, "%d:%02d %s", hour12, minute, ampm);
}

static void update_time_label() {
  if (!time_label) return;
  char buf[24];
  format_time_12h(gClockHour, gClockMinute, buf, sizeof(buf));
  lv_label_set_text_fmt(time_label, "Lampu Raya  %s", buf);

  if (!remaining_label) return;
  lv_obj_clear_flag(remaining_label, LV_OBJ_FLAG_HIDDEN);
  if (!gScheduleEnabled) {
    if (gLedRunning) {
      lv_obj_set_style_text_color(remaining_label, lv_color_hex(0xC9F99D), LV_PART_MAIN);
      lv_label_set_text(remaining_label, "On");
    } else {
      lv_obj_set_style_text_color(remaining_label, lv_color_hex(0xFF4D4F), LV_PART_MAIN);
      lv_label_set_text(remaining_label, "Off");
    }
    return;
  }

  if (gLedRunning) {
    const int mins_left_on = minutes_until_next_daily_time(gScheduleEndMinutes);
    lv_obj_set_style_text_color(remaining_label, lv_color_hex(0xC9F99D), LV_PART_MAIN);
    const int h = mins_left_on / 60;
    const int m = mins_left_on % 60;
    if (h > 0) {
      lv_label_set_text_fmt(remaining_label, "On left: %dh %02dm", h, m);
    } else {
      lv_label_set_text_fmt(remaining_label, "On left: %dm", m);
    }
  } else {
    const int mins_left_off = minutes_until_next_daily_time(gScheduleStartMinutes);
    lv_obj_set_style_text_color(remaining_label, lv_color_hex(0xFF4D4F), LV_PART_MAIN);
    const int h = mins_left_off / 60;
    const int m = mins_left_off % 60;
    if (h > 0) {
      lv_label_set_text_fmt(remaining_label, "Off left: %dh %02dm", h, m);
    } else {
      lv_label_set_text_fmt(remaining_label, "Off left: %dm", m);
    }
  }
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

static void update_schedule_labels() {
  char start_buf[16];
  char end_buf[16];
  format_time_12h(gScheduleStartMinutes / 60, gScheduleStartMinutes % 60,
                  start_buf, sizeof(start_buf));
  format_time_12h(gScheduleEndMinutes / 60, gScheduleEndMinutes % 60,
                  end_buf, sizeof(end_buf));
  if (schedule_start_label) lv_label_set_text_fmt(schedule_start_label, "Start: %s", start_buf);
  if (schedule_end_label) lv_label_set_text_fmt(schedule_end_label, "End:   %s", end_buf);
  update_time_label();
}

static void apply_clock_dialog_to_target() {
  const int h = lv_spinbox_get_value(clock_hour_spin);
  const int m = lv_spinbox_get_value(clock_min_spin);
  if (gClockTarget == CLOCK_TARGET_NOW) {
    gClockHour = h;
    gClockMinute = m;
    gClockCarryMs = 0;
    update_time_label();
  } else if (gClockTarget == CLOCK_TARGET_START) {
    gScheduleStartMinutes = (h * 60) + m;
    update_schedule_labels();
  } else {
    gScheduleEndMinutes = (h * 60) + m;
    update_schedule_labels();
  }
}

static void set_active_clock_field(lv_obj_t *spin) {
  clock_active_spin = spin;

  if (clock_hour_spin) {
    const bool active = (clock_hour_spin == spin);
    lv_obj_set_style_border_width(clock_hour_spin, active ? 3 : 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(clock_hour_spin,
                                  active ? lv_color_hex(0x2E7DFF) : lv_color_hex(0x666666),
                                  LV_PART_MAIN);
  }
  if (clock_min_spin) {
    const bool active = (clock_min_spin == spin);
    lv_obj_set_style_border_width(clock_min_spin, active ? 3 : 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(clock_min_spin,
                                  active ? lv_color_hex(0x2E7DFF) : lv_color_hex(0x666666),
                                  LV_PART_MAIN);
  }
}

static void clock_field_select_event_cb(lv_event_t *e) {
  if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
  lv_obj_t *spin = lv_event_get_target_obj(e);
  set_active_clock_field(spin);
}

static void clock_step_active_field(int step) {
  if (!clock_active_spin) return;

  const bool is_hour = (clock_active_spin == clock_hour_spin);
  const int min_v = 0;
  const int max_v = is_hour ? 23 : 59;
  int v = lv_spinbox_get_value(clock_active_spin);

  v += step;
  if (v > max_v) v = min_v;
  if (v < min_v) v = max_v;
  lv_spinbox_set_value(clock_active_spin, v);
}

static void open_clock_dialog(ClockTarget target) {
  gClockTarget = target;

  if (clock_title_label) {
    if (target == CLOCK_TARGET_NOW) {
      lv_label_set_text(clock_title_label, "Set Current Time");
    } else if (target == CLOCK_TARGET_START) {
      lv_label_set_text(clock_title_label, "Set Start Time");
    } else {
      lv_label_set_text(clock_title_label, "Set End Time");
    }
  }

  int h = gClockHour;
  int m = gClockMinute;
  if (target == CLOCK_TARGET_START) {
    h = gScheduleStartMinutes / 60;
    m = gScheduleStartMinutes % 60;
  } else if (target == CLOCK_TARGET_END) {
    h = gScheduleEndMinutes / 60;
    m = gScheduleEndMinutes % 60;
  }
  lv_spinbox_set_value(clock_hour_spin, h);
  lv_spinbox_set_value(clock_min_spin, m);
  set_active_clock_field(clock_hour_spin);
  lv_obj_remove_flag(clock_dialog, LV_OBJ_FLAG_HIDDEN);
}

static void close_clock_dialog() { lv_obj_add_flag(clock_dialog, LV_OBJ_FLAG_HIDDEN); }

static void touchpad_input_read(lv_indev_t *drv, lv_indev_data_t *data) {
  static int16_t tp_x = TFT_X / 2;
  static int16_t tp_y = TFT_Y / 2;
  static bool pressed = false;

  LV_UNUSED(drv);

  if (touchpad.available()) {
    int16_t x = static_cast<int16_t>(touchpad.x);
    int16_t y = static_cast<int16_t>(touchpad.y);

    if (TOUCH_SWAP_XY) {
      int16_t tmp = x;
      x = y;
      y = tmp;
    }
    if (TOUCH_FLIP_X) x = (TFT_X - 1) - x;
    if (TOUCH_FLIP_Y) y = (TFT_Y - 1) - y;

    tp_x = constrain(x, 0, TFT_X - 1);
    tp_y = constrain(y, 0, TFT_Y - 1);
    pressed = (touchpad.finger_num != 0);
  }

  data->point.x = tp_x;
  data->point.y = tp_y;
  data->state = pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
  gTouchPressed = pressed;
  if (pressed) gLastTouchMs = millis();
}

static void brightness_slider_event_cb(lv_event_t *e) {
  if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
  lv_obj_t *slider = lv_event_get_target_obj(e);
  gBrightness = static_cast<uint8_t>(lv_slider_get_value(slider));
  FastLED.setBrightness(gBrightness);
  if (!should_render_leds()) FastLED.show();
  update_brightness_label();
}

static void run_switch_event_cb(lv_event_t *e) {
  if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
  lv_obj_t *sw = lv_event_get_target_obj(e);
  gLedRunning = lv_obj_has_state(sw, LV_STATE_CHECKED);
  if (!gLedRunning) turn_leds_off();
  update_time_label();
}

static void schedule_enable_switch_event_cb(lv_event_t *e) {
  if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
  lv_obj_t *sw = lv_event_get_target_obj(e);
  gScheduleEnabled = lv_obj_has_state(sw, LV_STATE_CHECKED);

  if (!gScheduleEnabled) {
    gPrevScheduleActive = false;
    update_time_label();
    return;
  }

  // When enabling schedule, keep current manual power state.
  // Schedule will apply on the next start/end edge.
  gPrevScheduleActive = is_schedule_active_now();
  update_time_label();
}

static void show_main_page() {
  lv_obj_remove_flag(main_page, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(schedule_page, LV_OBJ_FLAG_HIDDEN);
}

static void show_schedule_page() {
  lv_obj_add_flag(main_page, LV_OBJ_FLAG_HIDDEN);
  lv_obj_remove_flag(schedule_page, LV_OBJ_FLAG_HIDDEN);
}

static void clock_button_event_cb(lv_event_t *e) {
  if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
  open_clock_dialog(CLOCK_TARGET_NOW);
}

static void schedule_button_event_cb(lv_event_t *e) {
  if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
  show_schedule_page();
}

static void schedule_back_button_event_cb(lv_event_t *e) {
  if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
  show_main_page();
}

static void schedule_set_start_button_event_cb(lv_event_t *e) {
  if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
  open_clock_dialog(CLOCK_TARGET_START);
}

static void schedule_set_end_button_event_cb(lv_event_t *e) {
  if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
  open_clock_dialog(CLOCK_TARGET_END);
}

static void clock_ok_button_event_cb(lv_event_t *e) {
  if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
  apply_clock_dialog_to_target();
  close_clock_dialog();
}

static void clock_cancel_button_event_cb(lv_event_t *e) {
  if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
  close_clock_dialog();
}

static void spin_inc_event_cb(lv_event_t *e) {
  if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
  clock_step_active_field(+1);
}

static void spin_dec_event_cb(lv_event_t *e) {
  if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
  clock_step_active_field(-1);
}

static void init_fastled() {
  FastLED.addLeds<LED_TYPE, LED_DATA_PIN, LED_COLOR_ORDER>(leds, NUM_LEDS)
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
    if (gLedsOn) turn_leds_off();
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
  if (changed) update_time_label();
}

static void rainbow() { fill_rainbow(leds, NUM_LEDS, gHue, 7); }

static void rainbowWithGlitter() {
  rainbow();
  addGlitter(80);
}

static void addGlitter(fract8 chanceOfGlitter) {
  if (random8() < chanceOfGlitter) leds[random16(NUM_LEDS)] += CRGB::White;
}

static void confetti() {
  fadeToBlackBy(leds, NUM_LEDS, 10);
  int pos = random16(NUM_LEDS);
  leds[pos] += CHSV(gHue + random8(64), 200, 255);
}

static void sinelon() {
  fadeToBlackBy(leds, NUM_LEDS, 20);
  int pos = beatsin16(13, 0, NUM_LEDS - 1);
  leds[pos] += CHSV(gHue, 255, 192);
}

static void bpm() {
  uint8_t BeatsPerMinute = 62;
  CRGBPalette16 palette = PartyColors_p;
  uint8_t beat = beatsin8(BeatsPerMinute, 64, 255);
  for (int i = 0; i < NUM_LEDS; i++) {
    leds[i] = ColorFromPalette(palette, gHue + (i * 2), beat - gHue + (i * 10));
  }
}

static void juggle() {
  fadeToBlackBy(leds, NUM_LEDS, 20);
  uint8_t dothue = 0;
  for (int i = 0; i < 8; i++) {
    leds[beatsin16(i + 7, 0, NUM_LEDS - 1)] |= CHSV(dothue, 200, 255);
    dothue += 32;
  }
}

static lv_obj_t *make_button(lv_obj_t *parent, const char *txt, lv_event_cb_t cb) {
  lv_obj_t *btn = lv_button_create(parent);
  lv_obj_t *lbl = lv_label_create(btn);
  lv_label_set_text(lbl, txt);
  lv_obj_center(lbl);
  lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, nullptr);
  return btn;
}

static lv_obj_t *make_button_ud(lv_obj_t *parent, const char *txt, lv_event_cb_t cb,
                                void *user_data) {
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

  analogWriteResolution(8);
  analogWrite(TFT_BL, 127);

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

  schedule_page = lv_obj_create(scr);
  lv_obj_remove_style_all(schedule_page);
  lv_obj_set_size(schedule_page, TFT_X, TFT_Y);
  lv_obj_set_style_bg_opa(schedule_page, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_add_flag(schedule_page, LV_OBJ_FLAG_HIDDEN);

  // Main page widgets
  time_label = lv_label_create(main_page);
  lv_obj_set_style_text_color(time_label, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
  lv_obj_align(time_label, LV_ALIGN_TOP_MID, 0, 6);
  update_time_label();

  remaining_label = lv_label_create(main_page);
  lv_obj_set_style_text_color(remaining_label, lv_color_hex(0xC9F99D), LV_PART_MAIN);
  lv_obj_align(remaining_label, LV_ALIGN_TOP_MID, 0, 28);
  lv_obj_add_flag(remaining_label, LV_OBJ_FLAG_HIDDEN);
  update_time_label();

  lv_obj_t *clock_btn = make_button(main_page, "Clock", clock_button_event_cb);
  lv_obj_set_size(clock_btn, 90, 32);
  lv_obj_align(clock_btn, LV_ALIGN_BOTTOM_LEFT, 12, -12);

  lv_obj_t *schedule_btn = make_button(main_page, "Schedule", schedule_button_event_cb);
  lv_obj_set_size(schedule_btn, 110, 32);
  lv_obj_align(schedule_btn, LV_ALIGN_BOTTOM_RIGHT, -12, -12);

  pattern_label = lv_label_create(main_page);
  lv_obj_set_style_text_color(pattern_label, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
  lv_obj_align(pattern_label, LV_ALIGN_TOP_MID, 0, 60);
  update_pattern_label();

  brightness_slider = lv_slider_create(main_page);
  lv_slider_set_range(brightness_slider, 1, 255);
  lv_slider_set_value(brightness_slider, gBrightness, LV_ANIM_OFF);
  lv_obj_set_size(brightness_slider, 180, 16);
  lv_obj_align_to(brightness_slider, pattern_label, LV_ALIGN_OUT_BOTTOM_MID, 0, 20);
  lv_obj_add_event_cb(brightness_slider, brightness_slider_event_cb,
                      LV_EVENT_VALUE_CHANGED, nullptr);

  brightness_label = lv_label_create(main_page);
  lv_obj_set_style_text_color(brightness_label, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
  lv_obj_set_width(brightness_label, TFT_X);
  lv_obj_set_style_text_align(brightness_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
  lv_obj_align_to(brightness_label, brightness_slider, LV_ALIGN_OUT_BOTTOM_MID, 0, 16);
  update_brightness_label();

  run_switch = lv_switch_create(main_page);
  if (gLedRunning) lv_obj_add_state(run_switch, LV_STATE_CHECKED);
  lv_obj_align_to(run_switch, brightness_label, LV_ALIGN_OUT_BOTTOM_MID, 26, 20);
  lv_obj_add_event_cb(run_switch, run_switch_event_cb, LV_EVENT_VALUE_CHANGED, nullptr);

  run_switch_label = lv_label_create(main_page);
  lv_obj_set_style_text_color(run_switch_label, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
  lv_label_set_text(run_switch_label, "Power");
  lv_obj_align_to(run_switch_label, run_switch, LV_ALIGN_OUT_LEFT_MID, -10, 0);

  // Schedule page widgets
  lv_obj_t *sched_title = lv_label_create(schedule_page);
  lv_obj_set_style_text_color(sched_title, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
  lv_label_set_text(sched_title, "Schedule");
  lv_obj_align(sched_title, LV_ALIGN_TOP_MID, 0, 8);

  schedule_enable_label = lv_label_create(schedule_page);
  lv_obj_set_style_text_color(schedule_enable_label, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
  lv_label_set_text(schedule_enable_label, "Enable");
  lv_obj_align(schedule_enable_label, LV_ALIGN_TOP_LEFT, 20, 42);

  schedule_enable_switch = lv_switch_create(schedule_page);
  lv_obj_add_state(schedule_enable_switch, LV_STATE_CHECKED);
  lv_obj_align(schedule_enable_switch, LV_ALIGN_TOP_RIGHT, -20, 36);
  lv_obj_add_event_cb(schedule_enable_switch, schedule_enable_switch_event_cb,
                      LV_EVENT_VALUE_CHANGED, nullptr);

  schedule_start_label = lv_label_create(schedule_page);
  lv_obj_set_style_text_color(schedule_start_label, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
  lv_obj_align(schedule_start_label, LV_ALIGN_TOP_LEFT, 20, 88);

  schedule_end_label = lv_label_create(schedule_page);
  lv_obj_set_style_text_color(schedule_end_label, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
  lv_obj_align(schedule_end_label, LV_ALIGN_TOP_LEFT, 20, 128);
  update_schedule_labels();

  lv_obj_t *set_start_btn = make_button(schedule_page, "Set Start", schedule_set_start_button_event_cb);
  lv_obj_set_size(set_start_btn, 100, 36);
  lv_obj_align(set_start_btn, LV_ALIGN_TOP_RIGHT, -20, 80);

  lv_obj_t *set_end_btn = make_button(schedule_page, "Set End", schedule_set_end_button_event_cb);
  lv_obj_set_size(set_end_btn, 100, 36);
  lv_obj_align(set_end_btn, LV_ALIGN_TOP_RIGHT, -20, 120);

  lv_obj_t *back_btn = make_button(schedule_page, "Back", schedule_back_button_event_cb);
  lv_obj_set_size(back_btn, 100, 40);
  lv_obj_align(back_btn, LV_ALIGN_BOTTOM_MID, 0, -20);

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
  lv_obj_set_style_text_color(clock_title_label, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
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
  lv_obj_add_event_cb(clock_hour_spin, clock_field_select_event_cb, LV_EVENT_CLICKED, nullptr);
  lv_obj_set_style_bg_color(clock_hour_spin, lv_color_hex(0x1B1B1B), LV_PART_MAIN);
  lv_obj_set_style_text_color(clock_hour_spin, lv_color_hex(0xFFFFFF), LV_PART_MAIN);

  clock_min_spin = lv_spinbox_create(clock_dialog);
  lv_spinbox_set_range(clock_min_spin, 0, 59);
  lv_spinbox_set_digit_format(clock_min_spin, 2, 0);
  lv_obj_set_size(clock_min_spin, 78, 46);
  lv_obj_align(clock_min_spin, LV_ALIGN_TOP_RIGHT, -20, 60);
  lv_obj_add_event_cb(clock_min_spin, clock_field_select_event_cb, LV_EVENT_CLICKED, nullptr);
  lv_obj_set_style_bg_color(clock_min_spin, lv_color_hex(0x1B1B1B), LV_PART_MAIN);
  lv_obj_set_style_text_color(clock_min_spin, lv_color_hex(0xFFFFFF), LV_PART_MAIN);

  lv_obj_t *shared_up = make_button(clock_dialog, "+", spin_inc_event_cb);
  lv_obj_set_size(shared_up, 90, 40);
  lv_obj_align(shared_up, LV_ALIGN_TOP_MID, 52, 124);

  lv_obj_t *shared_down = make_button(clock_dialog, "-", spin_dec_event_cb);
  lv_obj_set_size(shared_down, 90, 40);
  lv_obj_align_to(shared_down, shared_up, LV_ALIGN_OUT_LEFT_MID, -14, 0);

  lv_obj_t *ok_btn = make_button(clock_dialog, "OK", clock_ok_button_event_cb);
  lv_obj_set_size(ok_btn, 78, 40);
  lv_obj_align(ok_btn, LV_ALIGN_BOTTOM_RIGHT, -12, -12);

  lv_obj_t *cancel_btn = make_button(clock_dialog, "Cancel", clock_cancel_button_event_cb);
  lv_obj_set_size(cancel_btn, 78, 40);
  lv_obj_align(cancel_btn, LV_ALIGN_BOTTOM_LEFT, 12, -12);

  init_fastled();
  gPrevScheduleActive = is_schedule_active_now();
}

void loop() {
  lv_timer_handler();
  update_software_clock();

  if (gScheduleEnabled) {
    const bool schedule_active = is_schedule_active_now();
    if (gLedRunning && gPrevScheduleActive && !schedule_active) {
      gLedRunning = false;
      if (run_switch) lv_obj_remove_state(run_switch, LV_STATE_CHECKED);
      turn_leds_off();
      update_time_label();
    } else if (!gLedRunning && !gPrevScheduleActive && schedule_active) {
      gLedRunning = true;
      if (run_switch) lv_obj_add_state(run_switch, LV_STATE_CHECKED);
      update_time_label();
    }
    gPrevScheduleActive = schedule_active;
  } else {
    gPrevScheduleActive = false;
  }

  const uint32_t now = millis();
  const bool ui_priority = gTouchPressed || ((now - gLastTouchMs) < 200U);
  if (!ui_priority) {
    run_fastled();
  }

  delay(5);
}
