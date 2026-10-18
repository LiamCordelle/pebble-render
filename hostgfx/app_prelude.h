// Force-included before every app source file in the host renderer build.
//
// The app is compiled against the SDK's emery pebble.h. On the watch, each SDK
// function is a jump-table slot that lands on a firmware implementation, sometimes
// under another name (snprintf -> pbl_snprintf, graphics_fill_rect ->
// graphics_fill_round_rect_by_value, malloc -> task_malloc...). sdk_rename.h maps
// the names the same way, so app calls reach the same firmware code.
#pragma once

#include <locale.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gen/sdk_rename.h"

struct tm;
// libc entry points whose firmware implementations have other names; the host
// headers above declared the original names before the renames took effect.
int pbl_snprintf(char *str, size_t n, const char *format, ...);
void *pbl_memcpy(void *destination, const void *source, size_t num);
size_t pbl_strftime(char *s, size_t maxsize, const char *format, const struct tm *tim_p);
struct tm *pbl_override_localtime(const long *timep);
struct tm *pbl_override_gmtime(const long *timep);
long pbl_override_mktime(struct tm *tb);
long pbl_override_time(long *tloc);
double pbl_override_difftime(long end, long beginning);
void *task_malloc(size_t bytes);
void *task_calloc(size_t count, size_t size);
void task_free(void *ptr);
void *task_realloc(void *ptr, size_t size);
char *pbl_setlocale(int category, const char *locale);
