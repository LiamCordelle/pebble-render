// Host-side "kernel" for the PebbleOS applib graphics stack.
//
// Compiled with the firmware's own headers. It replaces the parts of the OS the
// graphics code calls into (resources, system fonts, app state, logging) and
// renders one frame the way the firmware does for a full-screen app window:
// window background fill, then the canvas layer's update proc with the default
// GContext drawing state.
//
// Usage (normally driven by render.py):
//   render_bin FW_DIR APP_PACK DESIGN_INDEX OUT.raw [options]
// FW_DIR holds system_resources.pbpack and builtin_<id>.bin (see setup_fw.py).
// OUT.raw receives the 200x228 GColor8 framebuffer, one byte per pixel.
// Options set the scene (see host_options.h): --epoch N, --12h, --battery N, --charging,
// --plugged, --no-phone, --steps N, --hr N, --run-ms N, --no-tick, --message K=V;...,
// --detail (lab projects), -v.

#include "applib/applib_resource_private.h"
#include "applib/fonts/fonts.h"
#include "applib/fonts/fonts_private.h"
#include "applib/graphics/framebuffer.h"
#include "applib/graphics/graphics.h"
#include "applib/graphics/gtypes.h"
#include "applib/graphics/text.h"
#include "applib/graphics/text_render.h"
#include "applib/graphics/text_resources.h"
#include "applib/pbl_std/locale.h"
#include "kernel/pebble_tasks.h"
#include "pbl/drivers/rtc.h"
#include "pbl/util/heap.h"
#include "util/time/time.h"
#include "process_state/app_state/app_state.h"
#include "resource/resource.h"
#include "resource/resource_ids.auto.h"
#include "syscall/syscall.h"

#include <dirent.h>
#include <limits.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "font_table.h"
#include "host_options.h"

// ---------------------------------------------------------------------------
// Resource packs: [manifest | table | data], as on flash and in the PBW.

typedef struct {
  uint8_t *bytes;
  size_t size;
  uint32_t count;
  uint32_t table_entries;  // 768 for the system store, 256 for apps
} Pack;

typedef struct {
  uint32_t id;
  uint32_t offset;
  uint32_t length;
  uint32_t crc;
} PackEntry;

static Pack s_system_pack;
static Pack s_app_pack;

// Resources compiled into the firmware image (e.g. FONT_FALLBACK_INTERNAL).
#define MAX_BUILTINS 64
static struct {
  uint32_t id;
  uint8_t *data;
  size_t size;
} s_builtins[MAX_BUILTINS];
static int s_num_builtins;

#define APP_RESOURCE_BANK 1

static uint8_t *prv_read_file(const char *path, size_t *size_out) {
  FILE *f = fopen(path, "rb");
  if (!f) {
    return NULL;
  }
  fseek(f, 0, SEEK_END);
  long size = ftell(f);
  fseek(f, 0, SEEK_SET);
  uint8_t *data = malloc(size > 0 ? size : 1);
  if (size > 0 && fread(data, 1, size, f) != (size_t)size) {
    free(data);
    fclose(f);
    return NULL;
  }
  fclose(f);
  *size_out = size;
  return data;
}

static bool prv_load_pack(Pack *pack, const char *path, uint32_t table_entries) {
  pack->bytes = prv_read_file(path, &pack->size);
  if (!pack->bytes || pack->size < 12) {
    return false;
  }
  memcpy(&pack->count, pack->bytes, sizeof(uint32_t));
  pack->table_entries = table_entries;
  return pack->count <= table_entries;
}

static const PackEntry *prv_pack_entry(const Pack *pack, uint32_t id) {
  if (!pack->bytes || id == 0) {
    return NULL;
  }
  for (uint32_t i = 0; i < pack->count; i++) {
    const PackEntry *e = (const PackEntry *)(pack->bytes + 12 + 16 * i);
    if (e->id == id) {
      return e;
    }
  }
  return NULL;
}

static int prv_load_builtins(const char *fw_dir) {
  DIR *dir = opendir(fw_dir);
  if (!dir) {
    return 0;
  }
  struct dirent *entry;
  while ((entry = readdir(dir)) != NULL && s_num_builtins < MAX_BUILTINS) {
    unsigned id;
    char tail[8];
    if (sscanf(entry->d_name, "builtin_%u.%7s", &id, tail) != 2 || strcmp(tail, "bin") != 0) {
      continue;
    }
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/%s", fw_dir, entry->d_name);
    size_t size;
    uint8_t *data = prv_read_file(path, &size);
    if (data) {
      s_builtins[s_num_builtins].id = id;
      s_builtins[s_num_builtins].data = data;
      s_builtins[s_num_builtins].size = size;
      s_num_builtins++;
    }
  }
  closedir(dir);
  return s_num_builtins;
}

// Returns a pointer to the resource bytes and its length, or NULL.
static const uint8_t *prv_resource(ResAppNum app_num, uint32_t id, size_t *length) {
  if (app_num == SYSTEM_APP) {
    for (int i = 0; i < s_num_builtins; i++) {
      if (s_builtins[i].id == id) {
        *length = s_builtins[i].size;
        return s_builtins[i].data;
      }
    }
    const PackEntry *e = prv_pack_entry(&s_system_pack, id);
    if (!e) {
      return NULL;
    }
    *length = e->length;
    return s_system_pack.bytes + 12 + 16 * s_system_pack.table_entries + e->offset;
  }
  const PackEntry *e = prv_pack_entry(&s_app_pack, id);
  if (!e) {
    return NULL;
  }
  *length = e->length;
  return s_app_pack.bytes + 12 + 16 * s_app_pack.table_entries + e->offset;
}

ResAppNum sys_get_current_resource_num(void) {
  return APP_RESOURCE_BANK;
}

bool sys_resource_is_valid(ResAppNum app_num, uint32_t resource_id) {
  size_t length;
  return prv_resource(app_num, resource_id, &length) != NULL;
}

size_t sys_resource_size(ResAppNum app_num, uint32_t handle) {
  size_t length = 0;
  return prv_resource(app_num, handle, &length) ? length : 0;
}

size_t sys_resource_load_range(ResAppNum app_num, uint32_t id, uint32_t start_bytes,
                               uint8_t *buffer, size_t num_bytes) {
  size_t length = 0;
  const uint8_t *data = prv_resource(app_num, id, &length);
  if (!data || start_bytes >= length) {
    return 0;
  }
  size_t n = num_bytes;
  if (n > length - start_bytes) {
    n = length - start_bytes;
  }
  memcpy(buffer, data + start_bytes, n);
  return n;
}

// QEMU's resources live in SPI flash, which is not memory mapped: always copy.
const uint8_t *sys_resource_read_only_bytes(ResAppNum app_num, uint32_t resource_id,
                                            size_t *num_bytes_out) {
  return NULL;
}

bool sys_resource_bytes_are_readonly(void *bytes) {
  return false;
}

bool resource_storage_builtin_bytes_are_readonly(const void *bytes) {
  return false;
}

bool resource_storage_flash_bytes_are_readonly(const void *bytes) {
  return false;
}

uint32_t sys_resource_get_and_cache(ResAppNum app_num, uint32_t resource_id) {
  return sys_resource_is_valid(app_num, resource_id) ? resource_id : 0;
}

uint32_t resource_get_and_cache(ResAppNum app_num, uint32_t resource_id) {
  return sys_resource_get_and_cache(app_num, resource_id);
}

ResourceCallbackHandle resource_watch(ResAppNum app_num, uint32_t resource_id,
                                      ResourceChangedCallback callback, void *data) {
  return NULL;
}

void resource_unwatch(ResourceCallbackHandle cb_handle) {
}

// ---------------------------------------------------------------------------
// System fonts (mirrors resource/system_resource.c, with the firmware's table).

#define NUM_HOST_FONTS (sizeof(s_host_font_keys) / sizeof(s_host_font_keys[0]))
static FontInfo s_system_fonts[NUM_HOST_FONTS + 1];

GFont sys_font_get_system_font(const char *font_key) {
  if (font_key == NULL) {
    FontInfo *fallback = &s_system_fonts[NUM_HOST_FONTS];
    if (!fallback->loaded) {
      if (!text_resources_init_font(SYSTEM_APP, RESOURCE_ID_FONT_FALLBACK_INTERNAL, 0,
                                    fallback)) {
        fprintf(stderr, "render: fallback font missing\n");
        exit(3);
      }
    }
    return fallback;
  }
  for (size_t i = 0; i < NUM_HOST_FONTS; i++) {
    if (strcmp(font_key, s_host_font_keys[i].key_name) == 0) {
      FontInfo *info = &s_system_fonts[i];
      if (!info->loaded &&
          !text_resources_init_font(SYSTEM_APP, s_host_font_keys[i].resource_id,
                                    s_host_font_keys[i].extension_id, info)) {
        return NULL;
      }
      return info;
    }
  }
  return NULL;
}

void sys_font_reload_font(FontInfo *fontinfo) {
  text_resources_init_font(fontinfo->base.app_num, fontinfo->base.resource_id,
                           fontinfo->extension.resource_id, fontinfo);
}

// ---------------------------------------------------------------------------
// App state.

static FrameBuffer s_framebuffer;
static GContext s_ctx;
static TextRenderState s_text_render_state;
static bool s_perimeter_debug;
static struct tm s_gmtime_tm;
static struct tm s_localtime_tm;
static char s_localtime_zone[TZ_LEN];
static uint32_t s_rand_state[16];

GContext *app_state_get_graphics_context(void) {
  return &s_ctx;
}

FrameBuffer *app_state_get_framebuffer(void) {
  return &s_framebuffer;
}

TextRenderState *app_state_get_text_render_state(void) {
  return &s_text_render_state;
}

bool app_state_get_text_perimeter_debugging_enabled(void) {
  return s_perimeter_debug;
}

void app_state_set_text_perimeter_debugging_enabled(bool enabled) {
  s_perimeter_debug = enabled;
}

Heap *app_state_get_heap(void) {
  return NULL;
}

GBitmap *app_state_legacy2_get_2bit_framebuffer(void) {
  return NULL;
}

struct tm *app_state_get_gmtime_tm(void) {
  return &s_gmtime_tm;
}

struct tm *app_state_get_localtime_tm(void) {
  return &s_localtime_tm;
}

char *app_state_get_localtime_zone(void) {
  return s_localtime_zone;
}

void *app_state_get_rand_ptr(void) {
  return s_rand_state;
}

LocaleInfo *app_state_get_locale_info(void) {
  static LocaleInfo s_locale;
  if (!s_locale.app_locale_time[0]) {
    locale_init_app_locale(&s_locale);
  }
  return &s_locale;
}

PebbleTask pebble_task_get_current(void) {
  return PebbleTask_App;
}

// The app heap. The watch's heap allocator never changes pixels, so use the host's.
void *task_malloc(size_t bytes) {
  return malloc(bytes);
}

void *task_calloc(size_t count, size_t size) {
  return calloc(count, size);
}

void task_free(void *ptr) {
  free(ptr);
}

void *task_realloc(void *ptr, size_t size) {
  return realloc(ptr, size);
}

bool process_manager_compiled_with_legacy2_sdk(void) {
  return false;
}

// ---------------------------------------------------------------------------
// Time, i18n and other kernel services reached through pbl_std/strftime. The lab's
// demo/stress/empty scenarios use fixed epochs, so "now" only matters for the live
// scenario, which the host renderer does not reproduce.

#define HOST_DEFAULT_EPOCH 1792228140  // Sat 17 Oct 2026 09:09 UTC (the lab demo time)

HostOptions g_host = {
  .epoch = HOST_DEFAULT_EPOCH,
  .clock_24h = true,
  .battery_pct = 80,
  .phone_connected = true,
  .tick = true,
};

// The clock starts at --epoch and moves on as app timers run (os_host.c).
time_t sys_get_time(void) {
  return g_host.epoch + host_elapsed_ms() / 1000;
}

void sys_get_time_ms(time_t *t, uint16_t *out_ms) {
  *t = sys_get_time();
  *out_ms = host_elapsed_ms() % 1000;
}

time_t rtc_get_time(void) {
  return sys_get_time();
}

RtcTicks rtc_get_ticks(void) {
  return (RtcTicks)host_elapsed_ms() * RTC_TICKS_HZ / 1000;
}

struct tm *sys_gmtime_r(const time_t *timep, struct tm *result) {
  return gmtime_r(timep, result);
}

struct tm *sys_localtime_r(const time_t *timep, struct tm *result) {
  return localtime_r(timep, result);
}

// The watch's zone is whatever --tz-info set (render.py --tz), held by util/time/time.c.
void sys_copy_timezone_abbr(char *timezone_abbr, time_t time) {
  time_get_timezone_abbr(timezone_abbr, time);
}

const char *i18n_get(const char *string, const void *owner) {
  return string;
}

void i18n_free(const char *string, const void *owner) {
}

// No language pack on the emulator: the system locale is the firmware default.
void sys_i18n_get_locale(char *buf) {
  strncpy(buf, "en_US", ISO_LOCALE_LENGTH);
}

size_t sys_i18n_get_length(const char *string) {
  return strlen(string);
}

void sys_i18n_get_with_buffer(const char *string, char *buffer, size_t length) {
  strncpy(buffer, string, length);
  if (length) {
    buffer[length - 1] = '\0';
  }
}

bool heap_is_allocated(Heap *const heap, void *ptr) {
  return false;
}

uint32_t *worker_state_get_rand_ptr(void) {
  return NULL;
}

struct tm *worker_state_get_gmtime_tm(void) {
  return &s_gmtime_tm;
}

struct tm *worker_state_get_localtime_tm(void) {
  return &s_localtime_tm;
}

char *worker_state_get_localtime_zone(void) {
  return s_localtime_zone;
}

// ---------------------------------------------------------------------------
// Logging and asserts.

static bool s_verbose;

void pbl_log_vargs(uint8_t log_level, const char *src_filename, int src_line_number,
                   const char *fmt, va_list args) {
  if (s_verbose) {
    fprintf(stderr, "[fw %s:%d] ", src_filename, src_line_number);
    vfprintf(stderr, fmt, args);
    fputc('\n', stderr);
  }
}

void pbl_log(uint8_t log_level, const char *src_filename, int src_line_number,
             const char *fmt, ...) {
  va_list args;
  va_start(args, fmt);
  pbl_log_vargs(log_level, src_filename, src_line_number, fmt, args);
  va_end(args);
}

void app_log_vargs(uint8_t log_level, const char *src_filename, int src_line_number,
                   const char *fmt, va_list args) {
  fprintf(stderr, "[app %s:%d] ", src_filename, src_line_number);
  vfprintf(stderr, fmt, args);
  fputc('\n', stderr);
}

void app_log(uint8_t log_level, const char *src_filename, int src_line_number,
             const char *fmt, ...) {
  va_list args;
  va_start(args, fmt);
  app_log_vargs(log_level, src_filename, src_line_number, fmt, args);
  va_end(args);
}

void passert_failed(const char *filename, int line_number, const char *message, ...) {
  fprintf(stderr, "render: firmware assert at %s:%d", filename, line_number);
  if (message) {
    va_list ap;
    va_start(ap, message);
    fputs(": ", stderr);
    vfprintf(stderr, message, ap);
    va_end(ap);
  }
  fputc('\n', stderr);
  exit(4);
}

void passert_failed_no_message(const char *filename, int line_number) {
  passert_failed(filename, line_number, NULL);
}

void passert_failed_no_message_with_lr(const char *filename, int line_number, uint32_t lr) {
  passert_failed(filename, line_number, NULL);
}

void passert_failed_hashed(uint32_t packed_loghash, ...) {
  fprintf(stderr, "render: firmware assert (hashed 0x%x)\n", packed_loghash);
  exit(4);
}

void passert_failed_hashed_no_message(uint32_t packed_loghash) {
  passert_failed_hashed(packed_loghash);
}

void passert_failed_hashed_no_message_with_lr(uint32_t packed_loghash, uint32_t lr) {
  passert_failed_hashed(packed_loghash);
}

void util_assertion_failed(const char *filename, int line) {
  passert_failed(filename, line, NULL);
}

void wtf(void) {
  fprintf(stderr, "render: firmware wtf()\n");
  exit(4);
}

// ---------------------------------------------------------------------------
// Frame rendering. The app side (app_glue.c) provides these.

extern int hostgfx_app_design_count(void);
extern const char *hostgfx_app_design_name(int index);
extern void hostgfx_app_load(int index);
extern int hostgfx_app_draw(GContext *ctx, GRect bounds, int index, int detail);
extern void hostgfx_app_unload(int index);

int main(int argc, char **argv) {
  if (argc < 5) {
    fprintf(stderr, "usage: %s FW_DIR APP_PACK DESIGN_INDEX OUT.raw [-v] [--detail]\n",
            argv[0]);
    return 2;
  }
  int detail = 0;
  for (int i = 5; i < argc; i++) {
    const char *arg = argv[i];
    const char *val = (i + 1 < argc) ? argv[i + 1] : NULL;
    if (strcmp(arg, "-v") == 0) {
      s_verbose = g_host.verbose = true;
    } else if (strcmp(arg, "--detail") == 0) {
      detail = 1;
    } else if (strcmp(arg, "--12h") == 0) {
      g_host.clock_24h = false;
    } else if (strcmp(arg, "--charging") == 0) {
      g_host.charging = g_host.plugged = true;
    } else if (strcmp(arg, "--plugged") == 0) {
      g_host.plugged = true;
    } else if (strcmp(arg, "--no-phone") == 0) {
      g_host.phone_connected = false;
    } else if (strcmp(arg, "--no-tick") == 0) {
      g_host.tick = false;
    } else if (val && strcmp(arg, "--epoch") == 0) {
      g_host.epoch = atol(val);
      i++;
    } else if (val && strcmp(arg, "--battery") == 0) {
      g_host.battery_pct = atoi(val);
      i++;
    } else if (val && strcmp(arg, "--steps") == 0) {
      g_host.steps = atoi(val);
      i++;
    } else if (val && strcmp(arg, "--hr") == 0) {
      g_host.heart_rate = atoi(val);
      i++;
    } else if (val && strcmp(arg, "--run-ms") == 0) {
      g_host.run_ms = (uint32_t)atol(val);
      i++;
    } else if (val && strcmp(arg, "--tz-info") == 0) {
      // "gmtoff,dst_start,dst_end,ABBR", as the phone sends it to the watch
      long gmtoff, dst_start, dst_end;
      char abbr[8] = "";
      if (sscanf(val, "%ld,%ld,%ld,%7s", &gmtoff, &dst_start, &dst_end, abbr) < 3) {
        fprintf(stderr, "render: bad --tz-info %s\n", val);
        return 2;
      }
      TimezoneInfo tz = { .tm_gmtoff = gmtoff, .dst_start = dst_start, .dst_end = dst_end };
      strncpy(tz.tm_zone, abbr, sizeof(tz.tm_zone));
      time_util_update_timezone(&tz);
      i++;
    } else if (val && strcmp(arg, "--message") == 0) {
      g_host.messages = val;
      i++;
    } else {
      fprintf(stderr, "render: unknown option %s\n", arg);
      return 2;
    }
  }
  char system_pack[PATH_MAX];
  snprintf(system_pack, sizeof(system_pack), "%s/system_resources.pbpack", argv[1]);
  if (!prv_load_pack(&s_system_pack, system_pack, 768)) {
    fprintf(stderr, "render: cannot read system pack %s\n", system_pack);
    return 2;
  }
  if (!prv_load_builtins(argv[1])) {
    fprintf(stderr, "render: no builtin_<id>.bin resources in %s\n", argv[1]);
    return 2;
  }
  if (!prv_load_pack(&s_app_pack, argv[2], 256)) {
    fprintf(stderr, "render: cannot read app pack %s\n", argv[2]);
    return 2;
  }

  const int count = hostgfx_app_design_count();
  int index = atoi(argv[3]);
  if (count <= 0) {
    fprintf(stderr, "render: no designs\n");
    return 2;
  }
  index = ((index % count) + count) % count;

  // app_state_init(): framebuffer, cleared to white, and the app GContext.
  const GSize fb_size = GSize(PBL_DISPLAY_WIDTH, PBL_DISPLAY_HEIGHT);
  framebuffer_init(&s_framebuffer, &fb_size);
  framebuffer_clear(&s_framebuffer);
  graphics_context_init(&s_ctx, &s_framebuffer, GContextInitializationMode_App);

  hostgfx_app_load(index);

  // window_render(): the root layer fills the window background (the lab shell
  // uses GColorBlack) and restores the drawing state, then the full-screen
  // canvas layer's update proc runs with that same state.
  const GRect bounds = GRect(0, 0, PBL_DISPLAY_WIDTH, PBL_DISPLAY_HEIGHT);
  const GDrawState root_state = graphics_context_get_drawing_state(&s_ctx);
  graphics_context_set_fill_color(&s_ctx, GColorBlack);
  graphics_fill_rect(&s_ctx, &bounds);
  graphics_context_set_drawing_state(&s_ctx, root_state);

  const int drawn = hostgfx_app_draw(&s_ctx, bounds, index, detail);
  if (drawn == -1) {
    fprintf(stderr, "render: this design has no detail view (LabDesign.draw_detail is NULL)\n");
    return 6;
  }
  if (drawn != 0) {
    fprintf(stderr, "render: the app never reached app_event_loop() with a window on screen; "
                    "push a window in init() before the event loop\n");
    return 7;
  }
  if (s_ctx.lock) {
    graphics_release_frame_buffer(&s_ctx, &s_ctx.dest_bitmap);
  }
  graphics_context_set_drawing_state(&s_ctx, root_state);

  FILE *out = fopen(argv[4], "wb");
  if (!out) {
    fprintf(stderr, "render: cannot write %s\n", argv[4]);
    return 2;
  }
  const GBitmap *bitmap = &s_ctx.dest_bitmap;
  for (int y = 0; y < PBL_DISPLAY_HEIGHT; y++) {
    fwrite((const uint8_t *)bitmap->addr + y * bitmap->row_size_bytes, 1, PBL_DISPLAY_WIDTH,
           out);
  }
  fclose(out);
  hostgfx_app_unload(index);
  printf("%s\n", hostgfx_app_design_name(index));
  return 0;
}
