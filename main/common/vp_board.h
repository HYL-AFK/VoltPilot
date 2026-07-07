#ifndef VP_BOARD_H
#define VP_BOARD_H

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "driver/uart.h"

/*
 * ESP32-S3 开发板板级引脚定义。
 * 后续模块统一引用这里的宏，避免在驱动和业务层散落硬编码 GPIO。
 */

/* 2.13 寸墨水屏，122(H) x 250(V)，排针丝印 SCL/SDA 但实际是 SPI。 */
#define VP_EPD_HOST SPI2_HOST

#define VP_EPD_PIN_SCK GPIO_NUM_12
#define VP_EPD_PIN_MOSI GPIO_NUM_11
#define VP_EPD_PIN_CS GPIO_NUM_13
#define VP_EPD_PIN_DC GPIO_NUM_21
#define VP_EPD_PIN_RST GPIO_NUM_38
#define VP_EPD_PIN_BUSY GPIO_NUM_39

#define VP_EPD_SPI_CLOCK_HZ (1 * 1000 * 1000)

/* BUSY 进入 ESP32-S3 前必须确认是 3.3V 安全电平。 */
#define VP_EPD_BUSY_ACTIVE_LEVEL 0
#define VP_EPD_BUSY_TIMEOUT_MS 30000

/* STC8H 挡位采集从机串口。 */
#define VP_STC_UART_PORT UART_NUM_1
#define VP_STC_UART_BAUD_RATE 115200
#define VP_STC_PIN_TX GPIO_NUM_17
#define VP_STC_PIN_RX GPIO_NUM_18
#define VP_STC_PIN_DE GPIO_NUM_10

/* BMS RS485 主站串口。 */
#define VP_BMS_UART_PORT UART_NUM_2
#define VP_BMS_UART_BAUD_RATE 9600
#define VP_BMS_PIN_TX GPIO_NUM_15
#define VP_BMS_PIN_RX GPIO_NUM_16
#define VP_BMS_PIN_DE GPIO_NUM_14

/* 输出电压反馈 ADC 采样。 */
#define VP_ADC_PIN_48V GPIO_NUM_1
#define VP_ADC_PIN_36V GPIO_NUM_2
#define VP_ADC_PIN_24V GPIO_NUM_4
#define VP_ADC_PIN_5V GPIO_NUM_5

/* 输出使能控制，上电默认必须保持关闭。 */
#define VP_OUT_PIN_EN_48V GPIO_NUM_6
#define VP_OUT_PIN_EN_36V GPIO_NUM_7
#define VP_OUT_PIN_EN_24V GPIO_NUM_8

/* 预留给 EEPROM/FRAM 的 I2C 总线。 */
#define VP_I2C_PORT 0
#define VP_I2C_PIN_SCL GPIO_NUM_35
#define VP_I2C_PIN_SDA GPIO_NUM_36
#define VP_I2C_CLOCK_HZ 400000

/* 人机接口：按键、蜂鸣器、系统指示灯。 */
#define VP_PIN_SCREEN_BUTTON GPIO_NUM_9
#define VP_PIN_BUZZER_PWM GPIO_NUM_40
#define VP_PIN_SYS_LED GPIO_NUM_41

/* 结合当前流程图预留的保守时序参数。 */
#define VP_INPUT_DEBOUNCE_MS 50
#define VP_OUTPUT_INTERLOCK_DELAY_MS 200
#define VP_STATE_SAVE_STABLE_MS 5000

#endif
