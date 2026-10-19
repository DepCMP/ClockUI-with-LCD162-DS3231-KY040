#ifndef DS3231_H
#define DS3231_H


#include "stdint.h"
#include "main.h"
//extern I2C_HandleTypeDef hi2c2; //day la cach 1
static I2C_HandleTypeDef* ds_i2c2; //cach 2
typedef struct
{
	uint8_t second;
	uint8_t minute;
	uint8_t hour;
	uint8_t day;
	uint8_t date;
	uint8_t month;
	uint8_t year;
}Date_time_t;
#define RTC_ADDR (0x68<<1)
#define ADDR_RES 0x00
void rtc_write_time(Date_time_t* dt);
void rtc_read_time(Date_time_t* dt);
uint8_t rtc_read_temp(Date_time_t* dt);
void DS3231_Init(I2C_HandleTypeDef* i2c2); //cach 2

#endif //DS3231_H