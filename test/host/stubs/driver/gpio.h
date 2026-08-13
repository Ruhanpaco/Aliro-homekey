#pragma once
#include "soc/gpio_num.h"
/* ESP32: 0..39 exist, 34..39 are input-only */
#define GPIO_IS_VALID_GPIO(n) ((n) >= 0 && (n) < 40 && (n) != 20 && (n) != 24 && ((n) < 28 || (n) > 31))
#define GPIO_IS_VALID_OUTPUT_GPIO(n) (GPIO_IS_VALID_GPIO(n) && (n) < 34)
