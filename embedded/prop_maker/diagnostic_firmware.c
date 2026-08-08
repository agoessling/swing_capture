#include <stdbool.h>
#include <stdio.h>

#include "embedded/prop_maker/board.h"
#include "hardware/gpio.h"
#include "pico/stdio.h"
#include "pico/time.h"

int main(void) {
  stdio_init_all();

  gpio_init(PICO_DEFAULT_LED_PIN);
  gpio_set_dir(PICO_DEFAULT_LED_PIN, (bool)GPIO_OUT);

  bool led_on = false;
  unsigned long heartbeat = 0;
  while (true) {
    led_on = (bool)!led_on;
    gpio_put(PICO_DEFAULT_LED_PIN, led_on);
    printf("swing_capture prop-maker diagnostic heartbeat=%lu\n", heartbeat++);
    sleep_ms(500);
  }
}
