// Host "OS" for ordinary Pebble apps: the window stack, the event loop, app timers and the
// system services a watchface calls, all deterministic. Compiled with the firmware headers,
// next to the firmware's own window, layer and text layer code, so the frame is drawn by
// the same code the watch runs (window_render -> layer_render_tree).
//
// The scene (clock, battery, phone, health) comes from g_host (host_options.h). Services an
// app may call but that can't change a screenshot (vibes, light, accelerometer, compass,
// touch) are accepted and ignored. Anything else falls through to build.py's weak stubs,
// which stop the render with a message naming the call.

#include "host_options.h"

#include "applib/accel_service.h"
#include "applib/app.h"
#include "applib/app_focus_service.h"
#include "applib/app_light.h"
#include "applib/app_message/app_message.h"
#include "applib/app_timer.h"
#include "applib/battery_state_service.h"
#include "applib/compass_service.h"
#include "applib/connection_service.h"
#include "applib/graphics/graphics.h"
#include "applib/health_service.h"
#include "applib/persist.h"
#include "applib/tick_timer_service.h"
#include "applib/touch_service.h"
#include "applib/ui/app_window_stack.h"
#include "applib/ui/vibes.h"
#include "applib/ui/window.h"
#include "applib/ui/window_private.h"
#include "applib/unobstructed_area_service.h"
#include "applib/unobstructed_area_service_private.h"
#include "applib/ui/click.h"
#include "applib/ui/scroll_layer.h"
#include "applib/ui/status_bar_layer.h"
#include "applib/ui/window_manager.h"
#include "applib/ui/window_stack.h"
#include "applib/ui/window_stack_private.h"
#include "kernel/kernel_applib_state.h"
#include "kernel/ui/kernel_ui.h"
#include "process_management/app_install_types.h"
#include "shell/system_theme.h"
#include "process_state/app_state/app_state.h"
#include "syscall/syscall.h"
#include "system/status_codes.h"
#include "util/dict.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

// ---------------------------------------------------------------------------
// Window stack. Pushing a window puts it on screen, which runs its load and appear
// handlers, as the window manager does on the watch.

#define MAX_WINDOWS 16
static Window *s_windows[MAX_WINDOWS];
static int s_num_windows;
static bool s_rendered;

void app_window_stack_push(Window *window, bool animated) {
  if (!window || s_num_windows >= MAX_WINDOWS) {
    return;
  }
  if (s_num_windows > 0) {
    window_set_on_screen(s_windows[s_num_windows - 1], false, true);
  }
  s_windows[s_num_windows++] = window;
  window_set_on_screen(window, true, true);
}

Window *app_window_stack_get_top_window(void) {
  return s_num_windows ? s_windows[s_num_windows - 1] : NULL;
}

Window *app_window_stack_pop(bool animated) {
  if (!s_num_windows) {
    return NULL;
  }
  Window *top = s_windows[--s_num_windows];
  window_set_on_screen(top, false, true);
  window_unload(top);
  if (s_num_windows) {
    window_set_on_screen(s_windows[s_num_windows - 1], true, true);
  }
  return top;
}

void app_window_stack_pop_all(const bool animated) {
  while (s_num_windows) {
    app_window_stack_pop(animated);
  }
}

bool app_window_stack_remove(Window *window, bool animated) {
  for (int i = 0; i < s_num_windows; i++) {
    if (s_windows[i] == window) {
      if (i == s_num_windows - 1) {
        app_window_stack_pop(animated);
        return true;
      }
      memmove(&s_windows[i], &s_windows[i + 1], (s_num_windows - i - 1) * sizeof(Window *));
      s_num_windows--;
      window_set_on_screen(window, false, true);
      window_unload(window);
      return true;
    }
  }
  return false;
}

bool app_window_stack_contains_window(Window *window) {
  for (int i = 0; i < s_num_windows; i++) {
    if (s_windows[i] == window) {
      return true;
    }
  }
  return false;
}

uint32_t app_window_stack_count(void) {
  return s_num_windows;
}

// ---------------------------------------------------------------------------
// What the firmware's window and layer code asks of the rest of the OS. One app, one
// full-screen window at a time: no transitions, no status bar, no obstructions.

static Layer *s_layer_tree_stack[LAYER_TREE_STACK_SIZE];

Layer **app_state_get_layer_tree_stack(void) {
  return s_layer_tree_stack;
}

Layer **kernel_applib_get_layer_tree_stack(void) {
  return s_layer_tree_stack;
}

GContext *graphics_context_get_current_context(void) {
  return app_state_get_graphics_context();
}

Window *window_manager_get_top_window(void) {
  return app_window_stack_get_top_window();
}

ClickManager *window_manager_get_window_click_manager(Window *window) {
  return NULL;
}

void app_click_config_setup_with_window(ClickManager *click_manager, struct Window *window) {
}

bool window_stack_is_animating_with_fixed_status_bar(WindowStack *window_stack) {
  return false;
}

bool window_transition_context_has_legacy_window_to(WindowStack *stack, Window *window) {
  return false;
}

bool layer_is_status_bar_layer(Layer *layer) {
  return false;
}

bool scroll_layer_is_instance(const Layer *layer) {
  return false;
}

// Only reached for legacy 2.x apps, which this renderer doesn't build.
void status_bar_layer_render(GContext *ctx, const GRect *bounds, StatusBarLayerConfig *config) {
}

static UnobstructedAreaState s_unobstructed_area_state;

UnobstructedAreaState *app_state_get_unobstructed_area_state(void) {
  return &s_unobstructed_area_state;
}

void unobstructed_area_service_get_area(UnobstructedAreaState *state, GRect *area) {
  *area = (GRect){ .size = { PBL_DISPLAY_WIDTH, PBL_DISPLAY_HEIGHT } };
}

// A TextLayer with no font gets the platform's default size (Large on emery).
PreferredContentSize system_theme_get_default_content_size_for_runtime_platform(void) {
  return PreferredContentSizeDefault;
}

AppInstallId sys_process_manager_get_current_process_id(void) {
  return 0;
}

bool app_install_id_from_app_db(AppInstallId id) {
  return false;
}

// ---------------------------------------------------------------------------
// App timers on a simulated clock. app_event_loop() runs them for g_host.run_ms before
// capturing the frame, so animated faces can be caught mid-animation.

struct AppTimer {
  bool active;
  bool repeating;
  uint32_t due_ms;
  uint32_t period_ms;
  AppTimerCallback callback;
  void *data;
};

#define MAX_TIMERS 64
static struct AppTimer s_timers[MAX_TIMERS];
static uint32_t s_now_ms;

uint32_t host_elapsed_ms(void) {
  return s_now_ms;
}

AppTimer *app_timer_register_repeatable(uint32_t timeout_ms, AppTimerCallback callback,
                                        void *callback_data, bool repeating) {
  for (int i = 0; i < MAX_TIMERS; i++) {
    if (!s_timers[i].active) {
      s_timers[i] = (struct AppTimer){
        .active = true,
        .repeating = repeating,
        .due_ms = s_now_ms + timeout_ms,
        .period_ms = timeout_ms ? timeout_ms : 1,
        .callback = callback,
        .data = callback_data,
      };
      return &s_timers[i];
    }
  }
  fprintf(stderr, "render: more than %d app timers at once\n", MAX_TIMERS);
  return NULL;
}

AppTimer *app_timer_register(uint32_t timeout_ms, AppTimerCallback callback,
                             void *callback_data) {
  return app_timer_register_repeatable(timeout_ms, callback, callback_data, false);
}

bool app_timer_reschedule(AppTimer *timer, uint32_t new_timeout_ms) {
  if (!timer || !timer->active) {
    return false;
  }
  timer->due_ms = s_now_ms + new_timeout_ms;
  return true;
}

void app_timer_cancel(AppTimer *timer) {
  if (timer) {
    timer->active = false;
  }
}

void *app_timer_get_data(AppTimer *timer) {
  return timer ? timer->data : NULL;
}

static void prv_run_timers(uint32_t until_ms) {
  for (int fired = 0; fired < 200000; fired++) {
    struct AppTimer *next = NULL;
    for (int i = 0; i < MAX_TIMERS; i++) {
      struct AppTimer *t = &s_timers[i];
      if (t->active && t->due_ms <= until_ms && (!next || t->due_ms < next->due_ms)) {
        next = t;
      }
    }
    if (!next) {
      break;
    }
    s_now_ms = next->due_ms;
    AppTimerCallback callback = next->callback;
    void *data = next->data;
    if (next->repeating) {
      next->due_ms += next->period_ms;
    } else {
      next->active = false;
    }
    callback(data);
  }
  s_now_ms = until_ms;
}

// ---------------------------------------------------------------------------
// Event services. Subscriptions are remembered; the scene's values are served on peek.

static TickHandler s_tick_handler;
static TimeUnits s_tick_units;

void tick_timer_service_subscribe(TimeUnits tick_units, TickHandler handler) {
  s_tick_handler = handler;
  s_tick_units = tick_units;
}

void tick_timer_service_unsubscribe(void) {
  s_tick_handler = NULL;
}

BatteryChargeState battery_state_service_peek(void) {
  return (BatteryChargeState){
    .charge_percent = (uint8_t)g_host.battery_pct,
    .is_charging = g_host.charging,
    .is_plugged = g_host.plugged,
  };
}

void battery_state_service_subscribe(BatteryStateHandler handler) {
}

void battery_state_service_unsubscribe(void) {
}

bool connection_service_peek_pebble_app_connection(void) {
  return g_host.phone_connected;
}

bool connection_service_peek_pebblekit_connection(void) {
  return g_host.phone_connected;
}

void connection_service_subscribe(ConnectionHandlers conn_handlers) {
}

void connection_service_unsubscribe(void) {
}

void bluetooth_connection_service_subscribe(ConnectionHandler handler) {
}

void bluetooth_connection_service_unsubscribe(void) {
}

static AppFocusHandlers s_focus_handlers;

void app_focus_service_subscribe_handlers(AppFocusHandlers handlers) {
  s_focus_handlers = handlers;
}

void app_focus_service_subscribe(AppFocusHandler handler) {
  s_focus_handlers = (AppFocusHandlers){ .did_focus = handler };
}

void app_focus_service_unsubscribe(void) {
  s_focus_handlers = (AppFocusHandlers){};
}

void accel_tap_service_subscribe(AccelTapHandler handler) {
}

void accel_tap_service_unsubscribe(void) {
}

void accel_data_service_subscribe(uint32_t samples_per_update, AccelDataHandler handler) {
}

void accel_data_service_unsubscribe(void) {
}

int accel_service_peek(AccelData *data) {
  if (data) {
    *data = (AccelData){ .z = -1000 };
  }
  return 0;
}

int accel_service_set_sampling_rate(AccelSamplingRate rate) {
  return 0;
}

int accel_service_set_samples_per_update(uint32_t num_samples) {
  return 0;
}

void compass_service_subscribe(CompassHeadingHandler handler) {
}

void compass_service_unsubscribe(void) {
}

int compass_service_set_heading_filter(CompassHeading filter) {
  return 0;
}

int compass_service_peek(CompassHeadingData *data) {
  if (data) {
    *data = (CompassHeadingData){ .compass_status = CompassStatusUnavailable };
  }
  return -1;
}

void touch_service_subscribe(TouchServiceHandler handler, void *context) {
}

void touch_service_unsubscribe(void) {
}

bool touch_service_is_enabled(void) {
  return false;
}

void app_unobstructed_area_service_subscribe(UnobstructedAreaHandlers handlers, void *context) {
}

void app_unobstructed_area_service_unsubscribe(void) {
}

void vibes_short_pulse(void) {
}

void vibes_long_pulse(void) {
}

void vibes_double_pulse(void) {
}

void vibes_cancel(void) {
}

void vibes_enqueue_custom_pattern(VibePattern pattern) {
}

void app_light_enable_interaction(void) {
}

void app_light_enable(bool enable) {
}

bool clock_is_24h_style(void) {
  return g_host.clock_24h;
}

// ---------------------------------------------------------------------------
// Health: today's steps and the current heart rate come from the scene.

HealthServiceAccessibilityMask health_service_metric_accessible(HealthMetric metric,
                                                               time_t time_start,
                                                               time_t time_end) {
  return HealthServiceAccessibilityMaskAvailable;
}

HealthServiceAccessibilityMask health_service_metric_averaged_accessible(
    HealthMetric metric, time_t time_start, time_t time_end, HealthServiceTimeScope scope) {
  return HealthServiceAccessibilityMaskAvailable;
}

HealthServiceAccessibilityMask health_service_metric_aggregate_averaged_accessible(
    HealthMetric metric, time_t time_start, time_t time_end, HealthAggregation aggregation,
    HealthServiceTimeScope scope) {
  return HealthServiceAccessibilityMaskAvailable;
}

HealthServiceAccessibilityMask health_service_any_activity_accessible(
    HealthActivityMask activity_mask, time_t time_start, time_t time_end) {
  return HealthServiceAccessibilityMaskAvailable;
}

HealthActivityMask health_service_peek_current_activities(void) {
  return HealthActivityNone;
}

HealthValue health_service_sum_today(HealthMetric metric) {
  return metric == HealthMetricStepCount ? g_host.steps : 0;
}

HealthValue health_service_sum(HealthMetric metric, time_t time_start, time_t time_end) {
  return time_end > time_start ? health_service_sum_today(metric) : 0;
}

HealthValue health_service_sum_averaged(HealthMetric metric, time_t time_start,
                                        time_t time_end, HealthServiceTimeScope scope) {
  return 0;
}

HealthValue health_service_peek_current_value(HealthMetric metric) {
  return metric == HealthMetricHeartRateBPM ? g_host.heart_rate : 0;
}

HealthValue health_service_aggregate_averaged(HealthMetric metric, time_t time_start,
                                              time_t time_end, HealthAggregation aggregation,
                                              HealthServiceTimeScope scope) {
  return health_service_peek_current_value(metric);
}

bool health_service_events_subscribe(HealthEventHandler handler, void *context) {
  return true;
}

bool health_service_events_unsubscribe(void) {
  return true;
}

// ---------------------------------------------------------------------------
// Persistent storage: an in-memory store that starts empty, like a fresh install.

#define MAX_PERSIST 128
static struct {
  bool used;
  uint32_t key;
  size_t size;
  uint8_t data[PERSIST_DATA_MAX_LENGTH];
} s_persist[MAX_PERSIST];

static int prv_persist_find(uint32_t key) {
  for (int i = 0; i < MAX_PERSIST; i++) {
    if (s_persist[i].used && s_persist[i].key == key) {
      return i;
    }
  }
  return -1;
}

static int prv_persist_write(uint32_t key, const void *data, size_t size) {
  if (size > PERSIST_DATA_MAX_LENGTH) {
    size = PERSIST_DATA_MAX_LENGTH;
  }
  int i = prv_persist_find(key);
  for (int j = 0; i < 0 && j < MAX_PERSIST; j++) {
    if (!s_persist[j].used) {
      i = j;
    }
  }
  if (i < 0) {
    return E_OUT_OF_STORAGE;
  }
  s_persist[i].used = true;
  s_persist[i].key = key;
  s_persist[i].size = size;
  memcpy(s_persist[i].data, data, size);
  return (int)size;
}

size_t persist_get_max_size(void) {
  return 4096;
}

bool persist_exists(const uint32_t key) {
  return prv_persist_find(key) >= 0;
}

int persist_get_size(const uint32_t key) {
  int i = prv_persist_find(key);
  return i < 0 ? E_DOES_NOT_EXIST : (int)s_persist[i].size;
}

int persist_read_data(const uint32_t key, void *buffer, const size_t buffer_size) {
  int i = prv_persist_find(key);
  if (i < 0) {
    return E_DOES_NOT_EXIST;
  }
  size_t n = s_persist[i].size < buffer_size ? s_persist[i].size : buffer_size;
  memcpy(buffer, s_persist[i].data, n);
  return (int)n;
}

bool persist_read_bool(const uint32_t key) {
  bool value = false;
  persist_read_data(key, &value, sizeof(value));
  return value;
}

int32_t persist_read_int(const uint32_t key) {
  int32_t value = 0;
  persist_read_data(key, &value, sizeof(value));
  return value;
}

int persist_read_string(const uint32_t key, char *buffer, const size_t buffer_size) {
  int n = persist_read_data(key, buffer, buffer_size);
  if (n >= 0 && buffer_size) {
    buffer[(size_t)n < buffer_size ? (size_t)n : buffer_size - 1] = '\0';
  }
  return n;
}

status_t persist_write_bool(const uint32_t key, const bool value) {
  int n = prv_persist_write(key, &value, sizeof(value));
  return n < 0 ? n : S_SUCCESS;
}

status_t persist_write_int(const uint32_t key, const int32_t value) {
  int n = prv_persist_write(key, &value, sizeof(value));
  return n < 0 ? n : S_SUCCESS;
}

int persist_write_data(const uint32_t key, const void *data, const size_t size) {
  return prv_persist_write(key, data, size);
}

int persist_write_string(const uint32_t key, const char *cstring) {
  return prv_persist_write(key, cstring, strlen(cstring) + 1);
}

status_t persist_delete(const uint32_t key) {
  int i = prv_persist_find(key);
  if (i < 0) {
    return E_DOES_NOT_EXIST;
  }
  s_persist[i].used = false;
  return S_SUCCESS;
}

// ---------------------------------------------------------------------------
// AppMessage. Outgoing messages are accepted and dropped (there's no phone). With
// --message, one incoming message is delivered at start, as if the phone had replied.

static AppMessageInboxReceived s_inbox_received;
static AppMessageInboxDropped s_inbox_dropped;
static AppMessageOutboxSent s_outbox_sent;
static AppMessageOutboxFailed s_outbox_failed;
static void *s_app_message_context;
static bool s_app_message_open;
static uint8_t s_outbox[1024];
static DictionaryIterator s_outbox_iter;

AppMessageResult app_message_open(const uint32_t size_inbound, const uint32_t size_outbound) {
  s_app_message_open = true;
  return APP_MSG_OK;
}

void app_message_close(void) {
  s_app_message_open = false;
}

void *app_message_get_context(void) {
  return s_app_message_context;
}

void *app_message_set_context(void *context) {
  void *previous = s_app_message_context;
  s_app_message_context = context;
  return previous;
}

AppMessageInboxReceived app_message_register_inbox_received(AppMessageInboxReceived cb) {
  AppMessageInboxReceived previous = s_inbox_received;
  s_inbox_received = cb;
  return previous;
}

AppMessageInboxDropped app_message_register_inbox_dropped(AppMessageInboxDropped cb) {
  AppMessageInboxDropped previous = s_inbox_dropped;
  s_inbox_dropped = cb;
  return previous;
}

AppMessageOutboxSent app_message_register_outbox_sent(AppMessageOutboxSent cb) {
  AppMessageOutboxSent previous = s_outbox_sent;
  s_outbox_sent = cb;
  return previous;
}

AppMessageOutboxFailed app_message_register_outbox_failed(AppMessageOutboxFailed cb) {
  AppMessageOutboxFailed previous = s_outbox_failed;
  s_outbox_failed = cb;
  return previous;
}

void app_message_deregister_callbacks(void) {
  s_inbox_received = NULL;
  s_inbox_dropped = NULL;
  s_outbox_sent = NULL;
  s_outbox_failed = NULL;
}

uint32_t app_message_inbox_size_maximum(void) {
  return 8200;
}

uint32_t app_message_outbox_size_maximum(void) {
  return sizeof(s_outbox);
}

AppMessageResult app_message_outbox_begin(DictionaryIterator **iterator) {
  if (!iterator) {
    return APP_MSG_INVALID_ARGS;
  }
  dict_write_begin(&s_outbox_iter, s_outbox, sizeof(s_outbox));
  *iterator = &s_outbox_iter;
  return APP_MSG_OK;
}

AppMessageResult app_message_outbox_send(void) {
  dict_write_end(&s_outbox_iter);
  return APP_MSG_OK;
}

// "10000=14;10001=Partly cloudy": each value is written as an int32 when it parses as one,
// otherwise as a string. render.py resolves MESSAGE_KEY names to these numbers.
static void prv_deliver_messages(void) {
  if (!g_host.messages || !*g_host.messages) {
    return;
  }
  if (!s_inbox_received) {
    fprintf(stderr, "render: --message given, but the app registered no inbox handler\n");
    return;
  }
  static uint8_t buffer[2048];
  DictionaryIterator iter;
  dict_write_begin(&iter, buffer, sizeof(buffer));
  char spec[1024];
  strncpy(spec, g_host.messages, sizeof(spec) - 1);
  spec[sizeof(spec) - 1] = '\0';
  for (char *item = strtok(spec, ";"); item; item = strtok(NULL, ";")) {
    char *eq = strchr(item, '=');
    if (!eq) {
      continue;
    }
    *eq = '\0';
    const uint32_t key = (uint32_t)strtoul(item, NULL, 10);
    const char *value = eq + 1;
    char *end;
    const long number = strtol(value, &end, 10);
    if (*value && *end == '\0') {
      dict_write_int32(&iter, key, (int32_t)number);
    } else {
      dict_write_cstring(&iter, key, value);
    }
  }
  const uint32_t size = dict_write_end(&iter);
  DictionaryIterator read_iter;
  dict_read_begin_from_buffer(&read_iter, buffer, size);
  s_inbox_received(&read_iter, s_app_message_context);
}

// ---------------------------------------------------------------------------
// The event loop: one pass of the events a watchface sees at launch, then the frame.

bool host_app_rendered(void) {
  return s_rendered;
}

void app_event_loop(void) {
  // Focus arrives once the launch transition ends.
  if (s_focus_handlers.will_focus) {
    s_focus_handlers.will_focus(true);
  }
  if (s_focus_handlers.did_focus) {
    s_focus_handlers.did_focus(true);
  }
  // The first tick: on the watch it comes at the next boundary of the subscribed unit, so
  // a face that only updates in its tick handler would otherwise show its initial state.
  if (s_tick_handler && g_host.tick) {
    time_t now = sys_get_time();
    struct tm tick_time;
    localtime_r(&now, &tick_time);
    s_tick_handler(&tick_time, s_tick_units);
  }
  prv_deliver_messages();
  prv_run_timers(g_host.run_ms);

  Window *top = app_window_stack_get_top_window();
  if (!top) {
    return;
  }
  GContext *ctx = app_state_get_graphics_context();
  window_render(top, ctx);
  if (ctx->lock) {
    graphics_release_frame_buffer(ctx, &ctx->dest_bitmap);
  }
  s_rendered = true;
}
