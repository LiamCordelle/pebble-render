// Host "OS" for ordinary Pebble apps: the window stack, the event loop, app timers, button
// presses and the system services an app calls, all deterministic and on a simulated clock.
// Compiled with the firmware headers, next to the firmware's own window, layer, click and
// animation code, so the frame is drawn by the same code the watch runs
// (window_render -> layer_render_tree).
//
// The scene (clock, battery, phone, health) comes from g_host (host_options.h), and so do the
// actions after launch (button presses, waits, AppMessages). Services an app may call but that
// can't change a screenshot (vibes, light, accelerometer, compass, touch) are accepted and
// ignored. Anything else falls through to build.py's weak stubs, which stop the render with a
// message naming the call.

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
#include "applib/event_service_client.h"
#include "applib/graphics/graphics.h"
#include "applib/health_service.h"
#include "applib/legacy2/ui/animation_legacy2.h"
#include "applib/legacy2/ui/animation_private_legacy2.h"
#include "applib/legacy2/ui/menu_layer_legacy2.h"
#include "applib/legacy2/ui/property_animation_legacy2.h"
#include "applib/persist.h"
#include "applib/tick_timer_service.h"
#include "applib/touch_service.h"
#include "applib/ui/animation_private.h"
#include "applib/ui/app_window_click_glue.h"
#include "applib/ui/app_window_stack.h"
#include "applib/ui/click.h"
#include "applib/ui/click_internal.h"
#include "applib/ui/content_indicator_private.h"
#include "applib/ui/vibes.h"
#include "applib/ui/window.h"
#include "applib/ui/window_manager.h"
#include "applib/ui/window_private.h"
#include "applib/ui/window_stack.h"
#include "applib/ui/window_stack_private.h"
#include "applib/unobstructed_area_service.h"
#include "applib/unobstructed_area_service_private.h"
#include "kernel/events.h"
#include "kernel/kernel_applib_state.h"
#include "kernel/ui/kernel_ui.h"
#include "pbl/services/animation_service.h"
#include "pbl/services/clock.h"
#include "process_management/app_install_types.h"
#include "process_management/process_manager.h"
#include "process_state/app_state/app_state.h"
#include "shell/system_theme.h"
#include "syscall/syscall.h"
#include "system/status_codes.h"
#include "util/dict.h"
#include "util/time/time.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static void prv_upkeep(void);

// ---------------------------------------------------------------------------
// Window stack. Pushing a window puts it on screen, which runs its load and appear
// handlers, as the window manager does on the watch. Windows change without transitions.

#define MAX_WINDOWS 16
static Window *s_windows[MAX_WINDOWS];
static int s_num_windows;
static bool s_rendered;

static ClickManager *prv_click_manager(void);

// The window that comes to the top gets the buttons: after its appear handler, the app's
// click manager is cleared and the window's click config provider runs
// (window_transition_context_appear()).
static void prv_window_came_to_top(Window *window) {
  window_set_on_screen(window, true, true);
  app_click_config_setup_with_window(prv_click_manager(), window);
}

void app_window_stack_push(Window *window, bool animated) {
  if (!window || s_num_windows >= MAX_WINDOWS) {
    return;
  }
  if (s_num_windows > 0) {
    window_set_on_screen(s_windows[s_num_windows - 1], false, true);
  }
  s_windows[s_num_windows++] = window;
  prv_window_came_to_top(window);
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
    prv_window_came_to_top(s_windows[s_num_windows - 1]);
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

// The firmware's window stack calls, which action menus and progress windows use. There's
// one stack, and it never animates.
static WindowStack s_window_stack;

WindowStack *app_state_get_window_stack(void) {
  return &s_window_stack;
}

void window_stack_push(WindowStack *window_stack, Window *window, bool animated) {
  app_window_stack_push(window, animated);
}

bool window_stack_remove(Window *window, bool animated) {
  return app_window_stack_remove(window, animated);
}

// Puts the window under the top one, so it shows when the top window goes.
void window_stack_insert_next(WindowStack *window_stack, Window *window) {
  if (!window || s_num_windows >= MAX_WINDOWS) {
    return;
  }
  if (s_num_windows == 0) {
    app_window_stack_push(window, false);
    return;
  }
  s_windows[s_num_windows] = s_windows[s_num_windows - 1];
  s_windows[s_num_windows - 1] = window;
  s_num_windows++;
}

// ---------------------------------------------------------------------------
// What the firmware's window and layer code asks of the rest of the OS. One app, one
// full-screen window at a time: no transitions, no modals, no obstructions.

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

bool window_stack_is_animating_with_fixed_status_bar(WindowStack *window_stack) {
  return false;
}

bool window_transition_context_has_legacy_window_to(WindowStack *stack, Window *window) {
  return false;
}

static UnobstructedAreaState s_unobstructed_area_state;

UnobstructedAreaState *app_state_get_unobstructed_area_state(void) {
  return &s_unobstructed_area_state;
}

void unobstructed_area_service_get_area(UnobstructedAreaState *state, GRect *area) {
  *area = (GRect){ .size = { PBL_DISPLAY_WIDTH, PBL_DISPLAY_HEIGHT } };
}

// The Text Size setting at its default (Large on emery), which menus and status bars use.
PreferredContentSize system_theme_get_content_size(void) {
  return system_theme_convert_host_content_size_to_runtime_platform(PreferredContentSizeDefault);
}

PlatformType process_manager_current_platform(void) {
  return PlatformTypeEmery;
}

AppInstallId sys_process_manager_get_current_process_id(void) {
  return 0;
}

bool app_install_id_from_app_db(AppInstallId id) {
  return false;
}

// The scroll arrows that scroll and menu layers share, kept per app.
ContentIndicatorsBuffer *content_indicator_get_current_buffer(void) {
  static ContentIndicatorsBuffer s_buffer;
  static bool s_initialized;
  if (!s_initialized) {
    s_initialized = true;
    content_indicator_init_buffer(&s_buffer);
  }
  return &s_buffer;
}

// The status bar's clock: "13:05", or "1:05 PM" (clock_format_time() in services/clock).
void clock_copy_time_string(char *buffer, uint8_t size) {
  if (!buffer || size == 0) {
    return;
  }
  const time_t now = sys_get_time();
  struct tm local;
  localtime_r(&now, &local);
  const bool is_24h = clock_is_24h_style();
  const char *format = is_24h ? "%u:%02u" : (local.tm_hour < 12 ? "%u:%02u AM" : "%u:%02u PM");
  snprintf(buffer, size, format, time_util_get_num_hours(local.tm_hour, is_24h), local.tm_min);
}

void dbgserial_putstr_fmt(char *str, unsigned int size, const char *fmt, ...) {
}

void vPortEnterCritical(void) {
}

void vPortExitCritical(void) {
}

// Only reached by apps built with SDK 2, which the renderer doesn't build.
static void prv_legacy2(const char *name) {
  fprintf(stderr, "render: %s() is for SDK 2 apps, which the host renderer doesn't support\n",
          name);
  exit(5);
}

void animation_legacy2_private_init_scheduler(AnimationLegacy2Scheduler *scheduler) {
  prv_legacy2(__func__);
}

AnimationCurveFunction animation_legacy2_get_custom_curve(struct AnimationLegacy2 *animation) {
  prv_legacy2(__func__);
  return NULL;
}

void menu_layer_legacy2_init(MenuLayer *menu_layer, const GRect *frame) {
  prv_legacy2(__func__);
}

void property_animation_legacy2_init(struct PropertyAnimationLegacy2 *property_animation,
      const struct PropertyAnimationLegacy2Implementation *implementation, void *subject,
      void *from_value, void *to_value) {
  prv_legacy2(__func__);
}

void property_animation_legacy2_init_layer_frame(
    struct PropertyAnimationLegacy2 *property_animation, struct Layer *layer, GRect *from_frame,
    GRect *to_frame) {
  prv_legacy2(__func__);
}

// ---------------------------------------------------------------------------
// Buttons. A press goes where applib/app.c sends it: Back leaves the top window unless the
// window handles Back itself; every other press goes to the firmware's click recognizers,
// which turn downs and ups into single, repeating, multi and long clicks with app timers.

static ClickManager s_click_manager;

static ClickManager *prv_click_manager(void) {
  static bool s_initialized;
  if (!s_initialized) {
    s_initialized = true;
    click_manager_init(&s_click_manager);
  }
  return &s_click_manager;
}

ClickManager *app_state_get_click_manager(void) {
  return prv_click_manager();
}

ClickManager *window_manager_get_window_click_manager(Window *window) {
  return prv_click_manager();
}

static void prv_button_down(ButtonId button) {
  Window *top = app_window_stack_get_top_window();
  if (!top) {
    return;
  }
  if (button == BUTTON_ID_BACK && !top->overrides_back_button) {
    app_window_stack_pop(true);
    return;
  }
  click_recognizer_handle_button_down(&prv_click_manager()->recognizers[button]);
}

static void prv_button_up(ButtonId button) {
  click_recognizer_handle_button_up(&prv_click_manager()->recognizers[button]);
}

// ---------------------------------------------------------------------------
// App timers on a simulated clock. Time only moves when the actions or --run-ms say so.

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

static struct AppTimer *prv_next_timer(uint32_t until_ms) {
  struct AppTimer *next = NULL;
  for (int i = 0; i < MAX_TIMERS; i++) {
    struct AppTimer *t = &s_timers[i];
    if (t->active && t->due_ms <= until_ms && (!next || t->due_ms < next->due_ms)) {
      next = t;
    }
  }
  return next;
}

// ---------------------------------------------------------------------------
// The Animation framework (the firmware's animation.c) asks the animation service for a
// timer to its next frame; here that's an app timer on the simulated clock.

static AnimationState s_animation_state;
static AppTimer *s_animation_timer;

AnimationState *app_state_get_animation_state(void) {
  static bool s_initialized;
  if (!s_initialized) {
    s_initialized = true;
    animation_private_state_init(&s_animation_state);
  }
  return &s_animation_state;
}

AnimationState *kernel_applib_get_animation_state(void) {
  return app_state_get_animation_state();
}

static void prv_animation_timer_fired(void *data) {
  s_animation_timer = NULL;
  animation_private_timer_callback(app_state_get_animation_state());
}

void animation_service_timer_schedule(uint32_t ms) {
  if (s_animation_timer) {
    app_timer_reschedule(s_animation_timer, ms);
  } else {
    s_animation_timer = app_timer_register(ms, prv_animation_timer_fired, NULL);
  }
}

void animation_service_timer_event_received(void) {
}

// ---------------------------------------------------------------------------
// Ticks. The kernel sends a tick event every second; the tick timer service passes it on
// when a unit the app subscribed to changed, exactly as do_handle() in
// applib/tick_timer_service.c does (the first tick after subscribing always goes through,
// with no units changed). Firmware UI code, such as the status bar, subscribes to the same
// events through the event service.

static struct {
  TickHandler handler;
  TimeUnits units;
  bool first_tick;
  bool last_is_24h;
  struct tm last_time;
} s_tick;

void tick_timer_service_subscribe(TimeUnits tick_units, TickHandler handler) {
  s_tick.handler = handler;
  s_tick.units = tick_units;
  s_tick.first_tick = true;
}

void tick_timer_service_unsubscribe(void) {
  s_tick.handler = NULL;
}

static void prv_tick_timer_service_handle(time_t now) {
  if (!s_tick.handler) {
    return;
  }
  struct tm local;
  localtime_r(&now, &local);
  TimeUnits units_changed = 0;
  if (!s_tick.first_tick) {
    if (s_tick.last_time.tm_sec != local.tm_sec) {
      units_changed |= SECOND_UNIT;
    }
    if (s_tick.last_time.tm_min != local.tm_min) {
      units_changed |= MINUTE_UNIT;
    }
    if (s_tick.last_time.tm_hour != local.tm_hour) {
      units_changed |= HOUR_UNIT;
    }
    if (s_tick.last_time.tm_mday != local.tm_mday) {
      units_changed |= DAY_UNIT;
    }
    if (s_tick.last_time.tm_mon != local.tm_mon) {
      units_changed |= MONTH_UNIT;
    }
    if (s_tick.last_time.tm_year != local.tm_year) {
      units_changed |= YEAR_UNIT;
    }
  }
  const bool is_24h = clock_is_24h_style();
  const bool format_changed = (is_24h != s_tick.last_is_24h);
  s_tick.last_is_24h = is_24h;
  if ((s_tick.units & units_changed) || s_tick.first_tick || format_changed) {
    s_tick.handler(&local, units_changed);
  }
  s_tick.last_time = local;
  s_tick.first_tick = false;
}

#define MAX_EVENT_CLIENTS 16
static EventServiceInfo *s_event_clients[MAX_EVENT_CLIENTS];

void event_service_client_subscribe(EventServiceInfo *handler) {
  int free_slot = -1;
  for (int i = 0; i < MAX_EVENT_CLIENTS; i++) {
    if (s_event_clients[i] == handler) {
      return;
    }
    if (!s_event_clients[i] && free_slot < 0) {
      free_slot = i;
    }
  }
  if (free_slot >= 0) {
    s_event_clients[free_slot] = handler;
  }
}

void event_service_client_unsubscribe(EventServiceInfo *handler) {
  for (int i = 0; i < MAX_EVENT_CLIENTS; i++) {
    if (s_event_clients[i] == handler) {
      s_event_clients[i] = NULL;
    }
  }
}

static bool prv_wants_ticks(void) {
  if (s_tick.handler) {
    return true;
  }
  for (int i = 0; i < MAX_EVENT_CLIENTS; i++) {
    if (s_event_clients[i] && s_event_clients[i]->type == PEBBLE_TICK_EVENT) {
      return true;
    }
  }
  return false;
}

static void prv_put_tick_event(void) {
  const time_t now = sys_get_time();
  prv_tick_timer_service_handle(now);
  PebbleEvent event = { .type = PEBBLE_TICK_EVENT, .clock_tick = { .tick_time = now } };
  for (int i = 0; i < MAX_EVENT_CLIENTS; i++) {
    EventServiceInfo *client = s_event_clients[i];
    if (client && client->type == PEBBLE_TICK_EVENT) {
      client->handler(&event, client->context);
    }
  }
}

// ---------------------------------------------------------------------------
// Event services. Subscriptions are remembered; the scene's values are served on peek.

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

bool health_service_set_heart_rate_sample_period(uint16_t interval_sec) {
  return true;
}

bool health_service_events_subscribe(HealthEventHandler handler, void *context) {
  return true;
}

bool health_service_events_unsubscribe(void) {
  return true;
}

// ---------------------------------------------------------------------------
// Persistent storage: an in-memory store that starts empty, like a fresh install, unless
// render.py seeds it (--persist, --load-persist). It can be saved after the run
// (--save-persist). Both files hold one "KEY HEX" line per value ("-" for no bytes).

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

static void prv_persist_load(void) {
  static bool s_loaded;
  if (s_loaded) {
    return;
  }
  s_loaded = true;
  if (!g_host.persist_in) {
    return;
  }
  FILE *f = fopen(g_host.persist_in, "r");
  if (!f) {
    fprintf(stderr, "render: cannot read %s\n", g_host.persist_in);
    exit(2);
  }
  unsigned long key;
  char hex[2 * PERSIST_DATA_MAX_LENGTH + 8];
  while (fscanf(f, "%lu %519s", &key, hex) == 2) {
    uint8_t data[PERSIST_DATA_MAX_LENGTH];
    size_t n = 0;
    for (const char *p = hex; p[0] && p[1] && n < sizeof(data); p += 2) {
      unsigned byte;
      if (sscanf(p, "%2x", &byte) == 1) {
        data[n++] = (uint8_t)byte;
      }
    }
    prv_persist_write((uint32_t)key, data, n);
  }
  fclose(f);
}

// Called once the app has exited, so values saved in deinit() are included.
void host_persist_save(void) {
  if (!g_host.persist_out) {
    return;
  }
  FILE *f = fopen(g_host.persist_out, "w");
  if (!f) {
    fprintf(stderr, "render: cannot write %s\n", g_host.persist_out);
    return;
  }
  for (int i = 0; i < MAX_PERSIST; i++) {
    if (!s_persist[i].used) {
      continue;
    }
    fprintf(f, "%lu ", (unsigned long)s_persist[i].key);
    if (!s_persist[i].size) {
      fputc('-', f);
    }
    for (size_t b = 0; b < s_persist[i].size; b++) {
      fprintf(f, "%02x", s_persist[i].data[b]);
    }
    fputc('\n', f);
  }
  fclose(f);
}

size_t persist_get_max_size(void) {
  return 4096;
}

bool persist_exists(const uint32_t key) {
  prv_persist_load();
  return prv_persist_find(key) >= 0;
}

int persist_get_size(const uint32_t key) {
  prv_persist_load();
  int i = prv_persist_find(key);
  return i < 0 ? E_DOES_NOT_EXIST : (int)s_persist[i].size;
}

int persist_read_data(const uint32_t key, void *buffer, const size_t buffer_size) {
  prv_persist_load();
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
  prv_persist_load();
  int n = prv_persist_write(key, &value, sizeof(value));
  return n < 0 ? n : S_SUCCESS;
}

status_t persist_write_int(const uint32_t key, const int32_t value) {
  prv_persist_load();
  int n = prv_persist_write(key, &value, sizeof(value));
  return n < 0 ? n : S_SUCCESS;
}

int persist_write_data(const uint32_t key, const void *data, const size_t size) {
  prv_persist_load();
  return prv_persist_write(key, data, size);
}

int persist_write_string(const uint32_t key, const char *cstring) {
  prv_persist_load();
  return prv_persist_write(key, cstring, strlen(cstring) + 1);
}

status_t persist_delete(const uint32_t key) {
  prv_persist_load();
  int i = prv_persist_find(key);
  if (i < 0) {
    return E_DOES_NOT_EXIST;
  }
  s_persist[i].used = false;
  return S_SUCCESS;
}

// ---------------------------------------------------------------------------
// AppMessage. There's no phone app: outgoing messages are acknowledged after a short round
// trip when the phone is connected, and fail when it isn't. Incoming messages come from the
// "message:" actions, as if the phone had sent them.

#define OUTBOX_ROUND_TRIP_MS 100

static AppMessageInboxReceived s_inbox_received;
static AppMessageInboxDropped s_inbox_dropped;
static AppMessageOutboxSent s_outbox_sent;
static AppMessageOutboxFailed s_outbox_failed;
static void *s_app_message_context;
static bool s_app_message_open;
static uint8_t s_outbox[1024];
static DictionaryIterator s_outbox_iter;
static bool s_outbox_pending;

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
  if (s_outbox_pending) {
    return APP_MSG_BUSY;
  }
  dict_write_begin(&s_outbox_iter, s_outbox, sizeof(s_outbox));
  *iterator = &s_outbox_iter;
  return APP_MSG_OK;
}

static void prv_outbox_round_trip_done(void *data) {
  s_outbox_pending = false;
  if (g_host.phone_connected) {
    if (s_outbox_sent) {
      s_outbox_sent(&s_outbox_iter, s_app_message_context);
    }
  } else if (s_outbox_failed) {
    s_outbox_failed(&s_outbox_iter, APP_MSG_NOT_CONNECTED, s_app_message_context);
  }
}

AppMessageResult app_message_outbox_send(void) {
  if (s_outbox_pending) {
    return APP_MSG_BUSY;
  }
  dict_write_end(&s_outbox_iter);
  s_outbox_pending = true;
  app_timer_register(OUTBOX_ROUND_TRIP_MS, prv_outbox_round_trip_done, NULL);
  return APP_MSG_OK;
}

// "10000=14;10001=Partly cloudy": each value is written as an int32 when it parses as one,
// otherwise as a string. render.py resolves messageKeys names to these numbers.
static void prv_deliver_message(const char *spec) {
  if (!s_inbox_received) {
    fprintf(stderr, "render: --message given, but the app registered no inbox handler\n");
    return;
  }
  static uint8_t buffer[2048];
  DictionaryIterator iter;
  dict_write_begin(&iter, buffer, sizeof(buffer));
  char items[1024];
  strncpy(items, spec, sizeof(items) - 1);
  items[sizeof(items) - 1] = '\0';
  for (char *item = strtok(items, ";"); item; item = strtok(NULL, ";")) {
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
// The event loop. After each event the top window is drawn if it asked to be, as
// event_loop_upkeep() in applib/app.c does, so the final frame is whatever the watch would
// be showing. With --gif, the screen is also sampled at a fixed rate of simulated time.

static FILE *s_frames;
static uint32_t s_next_frame_ms;

static void prv_render(Window *window) {
  GContext *ctx = app_state_get_graphics_context();
  window_render(window, ctx);
  if (ctx->lock) {
    graphics_release_frame_buffer(ctx, &ctx->dest_bitmap);
  }
  s_rendered = true;
}

static void prv_upkeep(void) {
  Window *top = app_window_stack_get_top_window();
  if (top && (top->is_render_scheduled || !s_rendered)) {
    prv_render(top);
  }
}

// Samples the screen at every frame time before `before_ms`.
static void prv_capture_frames(uint32_t before_ms) {
  while (s_frames && s_next_frame_ms < before_ms) {
    s_now_ms = s_next_frame_ms;
    prv_upkeep();
    const GBitmap *bitmap = &app_state_get_graphics_context()->dest_bitmap;
    for (int y = 0; y < PBL_DISPLAY_HEIGHT; y++) {
      fwrite((const uint8_t *)bitmap->addr + y * bitmap->row_size_bytes, 1, PBL_DISPLAY_WIDTH,
             s_frames);
    }
    s_next_frame_ms += g_host.frame_ms;
  }
}

// Lets `until_ms` of simulated time pass: timers fire when due, and the clock ticks at every
// whole second, as the watch's tick events do.
static void prv_run_until(uint32_t until_ms) {
  for (int fired = 0; fired < 1000000; fired++) {
    struct AppTimer *timer = prv_next_timer(until_ms);
    const uint32_t next_second = (s_now_ms / 1000 + 1) * 1000;
    const bool tick = prv_wants_ticks() && next_second <= until_ms &&
                      (!timer || next_second <= timer->due_ms);
    if (!tick && !timer) {
      break;
    }
    const uint32_t when = tick ? next_second : timer->due_ms;
    prv_capture_frames(when);
    s_now_ms = when;
    if (tick) {
      prv_put_tick_event();
    } else {
      AppTimerCallback callback = timer->callback;
      void *data = timer->data;
      if (timer->repeating) {
        timer->due_ms += timer->period_ms;
      } else {
        timer->active = false;
      }
      callback(data);
    }
    prv_upkeep();
  }
  prv_capture_frames(until_ms + 1);
  s_now_ms = until_ms;
}

static void prv_run_action(const char *action) {
  unsigned button, ms;
  if (sscanf(action, "press:%u:%u", &button, &ms) == 2 && button < NUM_BUTTONS) {
    const uint32_t release_ms = s_now_ms + ms;
    prv_button_down((ButtonId)button);
    prv_upkeep();
    // A timer due at the moment of release (a repeat or a long click) loses the race: the
    // button comes up first, as it does on the emulator.
    if (ms > 0) {
      prv_run_until(release_ms - 1);
    }
    s_now_ms = release_ms;
    prv_button_up((ButtonId)button);
    prv_upkeep();
  } else if (sscanf(action, "wait:%u", &ms) == 1) {
    prv_run_until(s_now_ms + ms);
  } else if (strncmp(action, "message:", 8) == 0) {
    prv_deliver_message(action + 8);
    prv_upkeep();
  } else {
    fprintf(stderr, "render: bad action %s\n", action);
    exit(2);
  }
}

bool host_app_rendered(void) {
  return s_rendered;
}

void app_event_loop(void) {
  if (g_host.frames_path && g_host.frame_ms) {
    s_frames = fopen(g_host.frames_path, "wb");
  }
  // Focus arrives once the launch transition ends.
  if (s_focus_handlers.will_focus) {
    s_focus_handlers.will_focus(true);
  }
  if (s_focus_handlers.did_focus) {
    s_focus_handlers.did_focus(true);
  }
  // The first tick comes within a second of launch on the watch; a face that only updates
  // in its tick handler would otherwise show its initial state.
  if (g_host.tick) {
    prv_put_tick_event();
  }
  prv_upkeep();
  prv_capture_frames(1);

  for (int i = 0; i < g_host.num_actions; i++) {
    prv_run_action(g_host.actions[i]);
    if (!app_window_stack_count()) {
      fprintf(stderr, "render: the app closed at action %d (%s): its last window left the "
                      "screen\n", i + 1, g_host.actions[i]);
      exit(8);
    }
  }
  prv_run_until(s_now_ms + g_host.run_ms);
  prv_upkeep();
  if (s_frames) {
    fclose(s_frames);
    s_frames = NULL;
  }
}
