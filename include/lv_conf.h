#ifndef LV_CONF_H
#define LV_CONF_H

/*
 * Project-local LVGL config for ESP32-C3 + ST7789 (240x280).
 * Keep this minimal and let LVGL defaults fill the rest.
 */

#define LV_COLOR_DEPTH 16
#define LV_USE_OS LV_OS_NONE

#define LV_USE_STDLIB_MALLOC LV_STDLIB_BUILTIN
#define LV_USE_STDLIB_STRING LV_STDLIB_BUILTIN
#define LV_USE_STDLIB_SPRINTF LV_STDLIB_BUILTIN
#define LV_MEM_SIZE (64U * 1024U)

#define LV_DEF_REFR_PERIOD 16
#define LV_DPI_DEF 130

#define LV_USE_LOG 0
#define LV_USE_ASSERT_NULL 1
#define LV_USE_ASSERT_MALLOC 1

#define LV_DRAW_SW_DRAW_UNIT_CNT 1
#define LV_DRAW_SW_SUPPORT_RGB565 1
#define LV_DRAW_SW_SUPPORT_RGB565_SWAPPED 0

#endif /* LV_CONF_H */
