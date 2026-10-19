/**
 * clock_ui.c - 16x2 LCD clock/date screen with a KY-040 controlled menu
 *
 * Uses YOUR DS3231 driver: DS3231.h with
 *     Date_time_t { second, minute, hour, day, date, month, year }
 *     void rtc_read_time(Date_time_t *dt);
 *     void rtc_write_time(Date_time_t *dt);
 * Year = 0..99 (2000..2099). dt->day = day of week (1 = Monday ... 7 = Sunday).
 * The RTC always keeps 24h time; the 12h (AM/PM) / 24h choice only affects the
 * display and the Set time screen.
 */
#include "clock_ui.h"
#include "DS3231.h"
#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------------------
 * LCD adapter: uses the I2C_LCD library (lcd_gotoxy / lcd_puts).
 * If your LCD header has another file name, change the #include below.
 * ---------------------------------------------------------------------- */
#include "i2c_lcd.h"
#define UI_LCD_SETCURSOR(col, row)  lcd_gotoxy(s_lcd, (col), (row))
#define UI_LCD_PRINT(str)           lcd_puts(s_lcd, (str))
/* ---------------------------------------------------------------------- */

#define UI_READ_PERIOD_MS    200u    /* how often HOME reads the RTC */
#define UI_BLINK_PERIOD_MS   400u    /* blink period of the edited field */

typedef enum {
    UI_HOME = 0,
    UI_MENU,
    UI_EDIT_TIME,
    UI_EDIT_DATE
} UI_State;

#define UI_DEFAULT_12H       1       /* 1 = start in 12h AM/PM mode, 0 = 24h */

#define MENU_SET_TIME   0u
#define MENU_SET_DATE   1u
#define MENU_FORMAT     2u
#define MENU_BACK       3u
#define MENU_COUNT      4u

static const char *const k_dow_names[8] = { "???", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun" };

static KY040_Handle        *s_enc;
static I2C_LCD_HandleTypeDef *s_lcd;
static UI_State      s_state;
static Date_time_t   s_now;
static uint8_t       s_menu_idx;
static uint8_t       s_field;        /* 0..2: field being edited */
static uint8_t       s_ed[4];        /* values being edited (time: h,m,s,pm / date: d,m,y) */
static uint8_t       s_fmt12 = UI_DEFAULT_12H;   /* 1 = 12h AM/PM, 0 = 24h */
static uint8_t       s_day_wish;     /* day chosen by the user, kept when month/year change */
static uint8_t       s_blink_on;     /* 1 = field visible */
static uint8_t       s_dirty;        /* 1 = screen must be redrawn */
static uint32_t      s_t_read;
static uint32_t      s_t_blink;

/* ---------------------------- calendar helpers ------------------------- */

static uint8_t days_in_month(uint8_t month, uint8_t year)
{
    static const uint8_t dim[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    uint16_t y = (uint16_t)(2000u + year);

    if (month < 1 || month > 12) {
        return 31;
    }
    if (month == 2) {
        uint8_t leap = (uint8_t)(((y % 4u) == 0u && (y % 100u) != 0u) || (y % 400u) == 0u);
        return leap ? 29 : 28;
    }
    return dim[month - 1];
}

/* Sakamoto's algorithm. Returns 1 = Monday ... 7 = Sunday */
static uint8_t calc_dow(uint8_t date, uint8_t month, uint8_t year)
{
    static const uint8_t t[12] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
    int y = 2000 + year;
    int d;

    if (month < 1 || month > 12) {
        return 1;
    }
    if (month < 3) {
        y -= 1;
    }
    d = (y + y / 4 - y / 100 + y / 400 + t[month - 1] + date) % 7;   /* 0 = Sunday */
    return (uint8_t)((d == 0) ? 7 : d);
}

/* 24h -> 12h: 0 -> 12 AM, 12 -> 12 PM, 13 -> 1 PM */
static void to_12h(uint8_t h24, uint8_t *h12, uint8_t *pm)
{
    uint8_t h = (uint8_t)(h24 % 12u);
    *pm  = (uint8_t)(h24 >= 12u);
    *h12 = h ? h : 12;
}

/* 12h -> 24h: 12 AM -> 0, 12 PM -> 12, 1 PM -> 13 */
static uint8_t to_24h(uint8_t h12, uint8_t pm)
{
    return (uint8_t)((h12 % 12u) + (pm ? 12u : 0u));
}

static uint8_t nfields(void)
{
    return (uint8_t)((s_state == UI_EDIT_TIME && s_fmt12) ? 4 : 3);
}

static uint8_t same_time(const Date_time_t *a, const Date_time_t *b)
{
    return (uint8_t)(a->second == b->second && a->minute == b->minute &&
                     a->hour   == b->hour   && a->date   == b->date   &&
                     a->month  == b->month  && a->year   == b->year);
}

/* ---------------------------- helpers ---------------------------------- */

static void print_line(uint8_t row, const char *s)
{
    char buf[17];
    snprintf(buf, sizeof(buf), "%-16.16s", s);       /* pad with spaces: no clear needed */
    UI_LCD_SETCURSOR(0, row);
    UI_LCD_PRINT(buf);
}

static uint8_t wrap(uint8_t v, int dir, uint8_t lo, uint8_t hi)
{
    int n = (int)v + dir;
    if (n > (int)hi) n = lo;
    else if (n < (int)lo) n = hi;
    return (uint8_t)n;
}

/* Day = the user's chosen day, limited to the days of the current month/year.
 * Example: 29 Feb 2024 -> change year -> 28 Feb 2025 -> back to 2028 -> 29 Feb again. */
static void apply_day_wish(void)
{
    uint8_t max = days_in_month(s_ed[1], s_ed[2]);
    s_ed[0] = (s_day_wish > max) ? max : s_day_wish;
}

static void edit_change(int dir)
{
    if (s_state == UI_EDIT_TIME) {
        if (s_fmt12) {
            switch (s_field) {
            case 0:  s_ed[0] = wrap(s_ed[0], dir, 1, 12); break;      /* hour 1..12 */
            case 1:
            case 2:  s_ed[s_field] = wrap(s_ed[s_field], dir, 0, 59); break;
            default: s_ed[3] ^= 1u; break;                            /* AM <-> PM */
            }
        } else {
            static const uint8_t max[3] = { 23, 59, 59 };
            s_ed[s_field] = wrap(s_ed[s_field], dir, 0, max[s_field]);
        }
    } else {                                          /* UI_EDIT_DATE */
        switch (s_field) {
        case 0:
            s_ed[0] = wrap(s_ed[0], dir, 1, days_in_month(s_ed[1], s_ed[2]));
            s_day_wish = s_ed[0];
            break;
        case 1: s_ed[1] = wrap(s_ed[1], dir, 1, 12); apply_day_wish(); break;
        default: s_ed[2] = wrap(s_ed[2], dir, 0, 99); apply_day_wish(); break;
        }
    }
}

static void go_home(void)
{
    s_state  = UI_HOME;
    s_t_read = HAL_GetTick() - UI_READ_PERIOD_MS;     /* force an immediate RTC read */
    s_dirty  = 1;
}

static void enter_edit(UI_State st)
{
    rtc_read_time(&s_now);                            /* fresh values as starting point */
    s_state = st;
    s_field = 0;
    if (st == UI_EDIT_TIME) {
        if (s_fmt12) {
            to_12h(s_now.hour, &s_ed[0], &s_ed[3]);
        } else {
            s_ed[0] = s_now.hour;
        }
        s_ed[1] = s_now.minute;  s_ed[2] = s_now.second;
    } else {
        s_ed[0] = s_now.date;  s_ed[1] = s_now.month;  s_ed[2] = s_now.year;
        if (s_ed[0] < 1)  s_ed[0] = 1;
        if (s_ed[1] < 1 || s_ed[1] > 12) s_ed[1] = 1;
        s_day_wish = s_ed[0];
        apply_day_wish();
    }
    s_blink_on = 1;
    s_t_blink  = HAL_GetTick();
}

static void save_and_exit(void)
{
    Date_time_t dt;

    /* rtc_write_time() writes all 7 registers, so read the current values first
     * and only change the fields being edited (the other half stays untouched). */
    rtc_read_time(&dt);
    if (s_state == UI_EDIT_TIME) {
        dt.hour   = s_fmt12 ? to_24h(s_ed[0], s_ed[3]) : s_ed[0];
        dt.minute = s_ed[1];
        dt.second = s_ed[2];
    } else {
        dt.date  = s_ed[0];
        dt.month = s_ed[1];
        dt.year  = s_ed[2];
        dt.day   = calc_dow(s_ed[0], s_ed[1], s_ed[2]);
    }
    rtc_write_time(&dt);
    go_home();
}

/* ---------------------------- events ----------------------------------- */

static void handle_event(KY040_Event e)
{
    s_blink_on = 1;                                   /* show digits while user is acting */
    s_t_blink  = HAL_GetTick();

    switch (s_state) {
    case UI_HOME:
        if (e == KY040_EVT_CLICK) {
            s_state    = UI_MENU;
            s_menu_idx = 0;
        }
        break;

    case UI_MENU:
        if (e == KY040_EVT_CW) {
            s_menu_idx = (uint8_t)((s_menu_idx + 1u) % MENU_COUNT);
        } else if (e == KY040_EVT_CCW) {
            s_menu_idx = (uint8_t)((s_menu_idx + MENU_COUNT - 1u) % MENU_COUNT);
        } else if (e == KY040_EVT_CLICK) {
            if (s_menu_idx == MENU_SET_TIME)      enter_edit(UI_EDIT_TIME);
            else if (s_menu_idx == MENU_SET_DATE) enter_edit(UI_EDIT_DATE);
            else if (s_menu_idx == MENU_FORMAT)   s_fmt12 = (uint8_t)!s_fmt12;   /* stay in menu */
            else                                  go_home();
        } else if (e == KY040_EVT_LONG_PRESS) {
            go_home();
        }
        break;

    case UI_EDIT_TIME:
    case UI_EDIT_DATE:
        if (e == KY040_EVT_CW) {
            edit_change(+1);
        } else if (e == KY040_EVT_CCW) {
            edit_change(-1);
        } else if (e == KY040_EVT_CLICK) {
            if (++s_field >= nfields()) {
                save_and_exit();
            }
        } else if (e == KY040_EVT_LONG_PRESS) {
            go_home();                                /* cancel, nothing saved */
        }
        break;
    }
    s_dirty = 1;
}

/* ---------------------------- drawing ---------------------------------- */

static void draw(void)
{
    char l0[24], l1[24];

    switch (s_state) {
    case UI_HOME:
        snprintf(l0, sizeof(l0), "%s %02u/%02u/20%02u",
                 k_dow_names[calc_dow(s_now.date, s_now.month, s_now.year)],
                 (unsigned)(s_now.date % 100u), (unsigned)(s_now.month % 100u),
                 (unsigned)(s_now.year % 100u));
        if (s_fmt12) {
            uint8_t h12, pm;
            to_12h(s_now.hour, &h12, &pm);
            snprintf(l1, sizeof(l1), "Time %02u:%02u:%02u %s",
                     (unsigned)(h12 % 100u), (unsigned)(s_now.minute % 100u),
                     (unsigned)(s_now.second % 100u), pm ? "PM" : "AM");
        } else {
            snprintf(l1, sizeof(l1), "Time %02u:%02u:%02u",
                     (unsigned)(s_now.hour % 100u), (unsigned)(s_now.minute % 100u),
                     (unsigned)(s_now.second % 100u));
        }
        print_line(0, l0);
        print_line(1, l1);
        break;

    case UI_MENU:
        print_line(0, "-- MENU --");
        switch (s_menu_idx) {
        case MENU_SET_TIME: snprintf(l1, sizeof(l1), "> Set time"); break;
        case MENU_SET_DATE: snprintf(l1, sizeof(l1), "> Set date"); break;
        case MENU_FORMAT:   snprintf(l1, sizeof(l1), "> Format: %s", s_fmt12 ? "12h" : "24h"); break;
        default:            snprintf(l1, sizeof(l1), "> Back"); break;
        }
        print_line(1, l1);
        break;

    case UI_EDIT_TIME:
    case UI_EDIT_DATE: {
        char f[3][5];
        for (uint8_t i = 0; i < 3; i++) {
            if (i == s_field && !s_blink_on) {
                snprintf(f[i], sizeof(f[i]), (s_state == UI_EDIT_DATE && i == 2) ? "    " : "  ");
            } else if (s_state == UI_EDIT_DATE && i == 2) {
                snprintf(f[i], sizeof(f[i]), "20%02u", (unsigned)(s_ed[i] % 100u));
            } else {
                snprintf(f[i], sizeof(f[i]), "%02u", (unsigned)(s_ed[i] % 100u));
            }
        }
        if (s_state == UI_EDIT_TIME) {
            print_line(0, "Set time");
            if (s_fmt12) {
                const char *ap = (s_field == 3 && !s_blink_on) ? "  " : (s_ed[3] ? "PM" : "AM");
                snprintf(l1, sizeof(l1), "  %s:%s:%s %s", f[0], f[1], f[2], ap);
            } else {
                snprintf(l1, sizeof(l1), "  %s:%s:%s", f[0], f[1], f[2]);
            }
        } else {
            print_line(0, "Set date");
            snprintf(l1, sizeof(l1), "  %s/%s/%s", f[0], f[1], f[2]);
        }
        print_line(1, l1);
        break;
    }
    }
}

/* ---------------------------- public API ------------------------------- */

void ClockUI_Init(KY040_Handle *enc, I2C_LCD_HandleTypeDef *lcd)
{
    s_enc = enc;
    s_lcd = lcd;
    KY040_FlushEvents(s_enc);
    memset(&s_now, 0, sizeof(s_now));
    rtc_read_time(&s_now);
    s_blink_on = 1;
    go_home();
}

void ClockUI_Process(void)
{
    uint32_t t = HAL_GetTick();
    KY040_Event e;

    while ((e = KY040_GetEvent(s_enc)) != KY040_EVT_NONE) {
        handle_event(e);
    }

    if (s_state == UI_HOME) {
        if ((t - s_t_read) >= UI_READ_PERIOD_MS) {
            Date_time_t n;
            s_t_read = t;
            rtc_read_time(&n);
            if (!same_time(&n, &s_now)) {
                s_now   = n;
                s_dirty = 1;
            }
        }
    } else if (s_state == UI_EDIT_TIME || s_state == UI_EDIT_DATE) {
        if ((t - s_t_blink) >= UI_BLINK_PERIOD_MS) {
            s_t_blink  = t;
            s_blink_on = (uint8_t)!s_blink_on;
            s_dirty    = 1;
        }
    }

    if (s_dirty) {
        s_dirty = 0;
        draw();
    }
}