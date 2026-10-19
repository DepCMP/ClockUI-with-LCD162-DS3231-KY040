/**
 * clock_ui.h - 16x2 LCD clock/date screen with a KY-040 controlled menu
 *
 * Screens:
 *   HOME       : date + weekday, time (updates by itself)
 *   MENU       : Set time / Set date / Back
 *   EDIT TIME  : hour -> minute -> second
 *   EDIT DATE  : day -> month -> year
 *
 * Controls:
 *   Click      : HOME -> MENU, select a menu item, next field, save on last field
 *   Rotate     : change menu item or the blinking field value
 *   Long press : go back to HOME (cancels editing without saving)
 */
#ifndef CLOCK_UI_H
#define CLOCK_UI_H

#ifdef __cplusplus
extern "C" {
#endif

#include "ky040.h"
#include "i2c_lcd.h"

/* Call once, after lcd_init(), DS3231_Init() and KY040_Init() are done */
void ClockUI_Init(KY040_Handle *enc, I2C_LCD_HandleTypeDef *lcd);

/* Call as often as possible from the main loop (non-blocking) */
void ClockUI_Process(void);

#ifdef __cplusplus
}
#endif

#endif /* CLOCK_UI_H */