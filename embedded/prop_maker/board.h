/*
 * Board configuration for the Adafruit Feather RP2040 Prop-Maker.
 *
 * Pin assignments and flash devices are cross-checked against Adafruit's
 * product guide and CircuitPython board definition.
 */

#ifndef SWING_CAPTURE_EMBEDDED_PROP_MAKER_BOARD_H_
#define SWING_CAPTURE_EMBEDDED_PROP_MAKER_BOARD_H_

#define ADAFRUIT_FEATHER_RP2040_PROP_MAKER

// Feather UART pins.
#define PICO_DEFAULT_UART 0
#define PICO_DEFAULT_UART_TX_PIN 0
#define PICO_DEFAULT_UART_RX_PIN 1

// Onboard indicators.
#define PICO_DEFAULT_LED_PIN 13
#define PICO_DEFAULT_WS2812_PIN 4

// STEMMA QT connector.
#define PICO_DEFAULT_I2C 1
#define PICO_DEFAULT_I2C_SDA_PIN 2
#define PICO_DEFAULT_I2C_SCL_PIN 3

// Feather SPI pins.
#define PICO_DEFAULT_SPI 1
#define PICO_DEFAULT_SPI_SCK_PIN 14
#define PICO_DEFAULT_SPI_TX_PIN 15
#define PICO_DEFAULT_SPI_RX_PIN 8

// Prop-Maker peripherals. External power remains off until firmware drives
// GPIO23 high.
#define PROP_MAKER_BOOT_BUTTON_PIN 7
#define PROP_MAKER_I2S_DATA_PIN 16
#define PROP_MAKER_I2S_BIT_CLOCK_PIN 17
#define PROP_MAKER_I2S_WORD_SELECT_PIN 18
#define PROP_MAKER_EXTERNAL_BUTTON_PIN 19
#define PROP_MAKER_SERVO_PIN 20
#define PROP_MAKER_EXTERNAL_NEOPIXEL_PIN 21
#define PROP_MAKER_ACCELEROMETER_INTERRUPT_PIN 22
#define PROP_MAKER_EXTERNAL_POWER_PIN 23

// The board uses an 8 MiB GD25Q64C or W25Q64JV-compatible QSPI device.
#define PICO_BOOT_STAGE2_CHOOSE_W25Q080 1
#define PICO_FLASH_SIZE_BYTES (8 * 1024 * 1024)
#define PICO_FLASH_SPI_CLKDIV 2

#define PICO_RP2040_B0_SUPPORTED 1

#endif  // SWING_CAPTURE_EMBEDDED_PROP_MAKER_BOARD_H_
