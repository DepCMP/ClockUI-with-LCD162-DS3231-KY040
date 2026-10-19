/**
 * ky040.c - KY-040 rotary encoder library for STM32 HAL
 */
#include "ky040.h"
#include <string.h>

#define Q_MASK  (KY040_QUEUE_SIZE - 1)

/* Quadrature transition table, index = (previous_AB << 2) | current_AB,
 * where AB = (CLK << 1) | DT.
 * +1 / -1 = one valid step, 0 = no change or invalid (bounce) transition. */
static const int8_t s_step_table[16] = {
     0, -1,  1,  0,
     1,  0,  0, -1,
    -1,  0,  0,  1,
     0,  1, -1,  0
};

static void push_event(KY040_Handle *h, KY040_Event e)
{
    uint8_t next = (uint8_t)((h->q_head + 1u) & Q_MASK);
    if (next == h->q_tail) {
        return;                 /* queue full: drop the newest event */
    }
    h->queue[h->q_head] = e;
    h->q_head = next;
}

static uint8_t read_ab(const KY040_Handle *h)
{
    uint8_t a = (HAL_GPIO_ReadPin(h->clk_port, h->clk_pin) == GPIO_PIN_SET) ? 1u : 0u;
    uint8_t b = (HAL_GPIO_ReadPin(h->dt_port,  h->dt_pin)  == GPIO_PIN_SET) ? 1u : 0u;
    return (uint8_t)((a << 1) | b);
}

static uint8_t read_sw_pressed(const KY040_Handle *h)
{
    /* The button pulls the pin to GND when pressed */
    return (HAL_GPIO_ReadPin(h->sw_port, h->sw_pin) == GPIO_PIN_RESET) ? 1u : 0u;
}

void KY040_Init(KY040_Handle *h,
                GPIO_TypeDef *clk_port, uint16_t clk_pin,
                GPIO_TypeDef *dt_port,  uint16_t dt_pin,
                GPIO_TypeDef *sw_port,  uint16_t sw_pin)
{
    memset(h, 0, sizeof(*h));

    /* Pins are stored last: KY040_Tick() ignores the handle while clk_port is NULL,
     * so a SysTick interrupt in the middle of Init is harmless. */
    h->clk_pin = clk_pin;
    h->dt_port = dt_port;  h->dt_pin = dt_pin;
    h->sw_port = sw_port;  h->sw_pin = sw_pin;

    h->prev_ab      = read_ab(h);
    h->sw_stable    = read_sw_pressed(h);
    h->sw_raw_last  = h->sw_stable;

    h->clk_port = clk_port;     /* enables KY040_Tick() */
}

void KY040_Tick(KY040_Handle *h)
{
    if (h->clk_port == NULL) {
        return;                 /* not initialized yet */
    }

    /* ---- Rotation ---- */
    uint8_t cur = read_ab(h);
    if (cur != h->prev_ab) {
        int8_t d = s_step_table[(h->prev_ab << 2) | cur];
        h->prev_ab = cur;
#if KY040_REVERSE
        d = (int8_t)(-d);
#endif
        h->acc = (int8_t)(h->acc + d);

        if (h->acc >= KY040_STEPS_PER_DETENT) {
            h->acc = 0;
            h->position++;
            push_event(h, KY040_EVT_CW);
        } else if (h->acc <= -KY040_STEPS_PER_DETENT) {
            h->acc = 0;
            h->position--;
            push_event(h, KY040_EVT_CCW);
        }

        /* Both lines high = detent rest position: drop leftover partial steps */
        if (cur == 3u) {
            h->acc = 0;
        }
    }

    /* ---- Button (debounce + short/long press) ---- */
    uint8_t raw = read_sw_pressed(h);
    if (raw == h->sw_raw_last) {
        if (h->sw_cnt < KY040_DEBOUNCE_MS) {
            h->sw_cnt++;
        }
        if (h->sw_cnt >= KY040_DEBOUNCE_MS && raw != h->sw_stable) {
            h->sw_stable = raw;
            if (raw) {                          /* just pressed */
                h->press_ms  = 0;
                h->long_sent = 0;
            } else {                            /* just released */
                if (!h->long_sent) {
                    push_event(h, KY040_EVT_CLICK);
                }
            }
        }
    } else {
        h->sw_raw_last = raw;
        h->sw_cnt = 0;
    }

    if (h->sw_stable && !h->long_sent) {
        if (++h->press_ms >= KY040_LONG_PRESS_MS) {
            h->long_sent = 1;
            push_event(h, KY040_EVT_LONG_PRESS);
        }
    }
}

KY040_Event KY040_GetEvent(KY040_Handle *h)
{
    if (h->q_head == h->q_tail) {
        return KY040_EVT_NONE;
    }
    KY040_Event e = h->queue[h->q_tail];
    h->q_tail = (uint8_t)((h->q_tail + 1u) & Q_MASK);
    return e;
}

int32_t KY040_GetPosition(const KY040_Handle *h)
{
    return h->position;
}

void KY040_FlushEvents(KY040_Handle *h)
{
    h->q_tail = h->q_head;
}                                                                                                                                                                     