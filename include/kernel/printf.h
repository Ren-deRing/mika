#pragma once

#include <stdarg.h>
#include <stdint.h>

#include <lib/stb_sprintf.h>

typedef char* (*kprint_callback_t)(const char* buf, void* user, int len);

void set_output_sink(kprint_callback_t new_sink);

void kputc(char c);
void vprintf(const char* format, va_list args);
void printf(const char* format, ...);
void dprintf(const char* format, ...);
int snprintf(char* buf, size_t size, const char* fmt, ...);
int vsnprintf(char* buf, size_t size, const char* fmt, va_list args);