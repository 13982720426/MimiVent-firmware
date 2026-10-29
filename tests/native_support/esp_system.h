#pragma once
#include <cstdint>
inline void esp_fill_random(void* out,size_t n) { static uint8_t seed=1; auto p=(uint8_t*)out; for(size_t i=0;i<n;i++) p[i]=seed++; }
