#pragma once
#include "esp_err.h"
#include "esp_log.h"
#define ESP_RETURN_ON_ERROR(x, tag, fmt, ...) do { esp_err_t _e=(x); if(_e!=ESP_OK){ return _e; } } while(0)
#define ESP_RETURN_ON_FALSE(a, err, tag, fmt, ...) do { if(!(a)){ return (err); } } while(0)
