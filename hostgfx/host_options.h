// The scene a render reproduces: clock, device state, sensor values and what happens after
// launch. fw_host.c fills it from the command line (render.py passes the flags); os_host.c
// serves it to the app.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define HOST_MAX_ACTIONS 256

typedef struct {
  long epoch;            // seconds since 1970; render.py runs the renderer with TZ=UTC,
                         // so this is also the wall-clock time the watch shows
  bool clock_24h;        // clock_is_24h_style()
  int battery_pct;       // battery_state_service_peek()
  bool charging;
  bool plugged;
  bool phone_connected;  // connection_service_peek_pebble_app_connection()
  int steps;             // health_service_sum_today(HealthMetricStepCount)
  int heart_rate;        // health_service_peek_current_value(HealthMetricHeartRateBPM)
  bool tick;             // deliver the first tick at launch, as the watch does within a second
  // What happens after launch, in order:
  //   "press:BUTTON:HOLD_MS"   button down, HOLD_MS of time, button up (0 back .. 3 down)
  //   "wait:MS"                MS of time: timers, animations and ticks run
  //   "message:KEY=VALUE;..."  an AppMessage from the phone (numeric keys)
  const char *actions[HOST_MAX_ACTIONS];
  int num_actions;
  uint32_t run_ms;          // time that passes after the actions, before the frame is captured
  const char *persist_in;   // persistent storage at launch (format: see os_host.c)
  const char *persist_out;  // where to write persistent storage after the run
  const char *frames_path;  // with frame_ms: the screen is appended here every frame_ms
  uint32_t frame_ms;
  bool verbose;
} HostOptions;

extern HostOptions g_host;

// os_host.c: milliseconds of simulated time elapsed (timers advance the clock too).
uint32_t host_elapsed_ms(void);
// os_host.c: whether the app reached app_event_loop() with a window on screen.
bool host_app_rendered(void);
