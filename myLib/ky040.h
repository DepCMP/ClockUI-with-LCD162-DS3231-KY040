/**
 * ky040.h - KY-040 rotary encoder library for STM32 HAL (menu navigation)
 *
 * Features:
 *  - Quadrature decoding with a lookup table (tolerates contact bounce)
 *  - One event per detent: CW / CCW
 *  - Push button with debounce: CLICK (short press) / LONG_PRESS
 *  - Event queue, so the main loop can read events at its own pace
 *
 * Usage:
 *  1. Configure CLK, DT, SW as GPIO_Input with Pull-up (no EXTI needed).
 *  2. Call KY040_Init() once.
 *  3. Call KY040_Tick() every 1 ms (e.g. from HAL_SYSTICK_Callback).
 *  4. In the main loop, call KY040_GetEvent() and react to the event.
 */
#ifndef KY040_H
#define KY040_H

#ifdef __cplusplus
extern "C" {
#endif

#include "main.h"
#include <stdint.h>

/* ---------------- Configuration (override with -D or edit here) ---------------- */

/* Quadrature transitions per detent. KY-040 is usually 4; try 2 if one click
 * of the knob needs two detents to register. */
#ifndef KY040_STEPS_PER_DETENT
#define KY040_STEPS_PER_DETENT  4
#endif

/* Set to 1 if turning clockwise reports CCW. */
#ifndef KY040_REVERSE
#define KY040_REVERSE           0
#endif

/* Button debounce time in ms (Tick period = 1 ms). Max 255. */
#ifndef KY040_DEBOUNCE_MS
#define KY040_DEBOUNCE_MS       20
#endif

/* Hold time in ms that counts as a long press. */
#ifndef KY040_LONG_PRESS_MS
#define KY040_LONG_PRESS_MS     800
#endif

/* Event queue size. Must be a power of 2 and at most 128. */
#ifndef KY040_QUEUE_SIZE
#define KY040_QUEUE_SIZE        16
#endif

/* ---------------- Types ---------------- */

typedef enum {
    KY040_EVT_NONE = 0,
    KY040_EVT_CW,           /* one detent clockwise            */
    KY040_EVT_CCW,          /* one detent counter-clockwise    */
    KY040_EVT_CLICK,        /* short press, sent on release    */
    KY040_EVT_LONG_PRESS    /* held longer than LONG_PRESS_MS  */
} KY040_Event;

typedef struct {
    GPIO_TypeDef *clk_port;  uint16_t clk_pin;
    GPIO_TypeDef *dt_port;   uint16_t dt_pin;
    GPIO_TypeDef *sw_port;   uint16_t sw_pin;

    /* internal state - do not touch */
    uint8_t  prev_ab;
    int8_t   acc;
    int32_t  position;

    uint8_t  sw_raw_last;
    uint8_t  sw_stable;      /* 1 = pressed */
    uint8_t  sw_cnt;
    uint16_t press_ms;
    uint8_t  long_sent;

    volatile uint8_t     q_head;
    volatile uint8_t     q_tail;
    volatile KY040_Event queue[KY040_QUEUE_SIZE];
} KY040_Handle;

/* ---------------- API ---------------- */

/* Initialize the handle. Pins must already be configured as inputs with pull-up. */
void        KY040_Init(KY040_Handle *h,
                       GPIO_TypeDef *clk_port, uint16_t clk_pin,
                       GPIO_TypeDef *dt_port,  uint16_t dt_pin,
                       GPIO_TypeDef *sw_port,  uint16_t sw_pin);

/* Call every 1 ms. Safe to call from an ISR. Does nothing before KY040_Init(). */
void        KY040_Tick(KY040_Handle *h);

/* Pop one event from the queue. Returns KY040_EVT_NONE if empty. */
KY040_Event KY040_GetEvent(KY040_Handle *h);

/* Total detents counted since Init (CW = +1, CCW = -1). */
int32_t     KY040_GetPosition(const KY040_Handle *h);

/* Clear all pending events. */
void        KY040_FlushEvents(KY040_Handle *h);

#ifdef __cplusplus
}
#endif

#endif /* KY040_H */