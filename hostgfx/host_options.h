// The scene a render reproduces: clock, device state and sensor values. fw_host.c fills it
// from the command line (render.py passes the flags); os_host.c serves it to the app.
#pragma once

#include <stdbool.h>
#include <stdint.h>

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
  uint32_t run_ms;       // app timers fire for this long before the frame is captured
  bool tick;             // call the tick handler once at start, as the first tick would
  const char *messages;  // AppMessage to deliver at start: "key=value;key=value" (numeric keys)
  bool verbose;
} HostOptions;

extern HostOptions g_host;

// os_host.c: milliseconds of simulated time elapsed (timers advance the clock too).
uint32_t host_elapsed_ms(void);
// os_host.c: whether the app reached app_event_loop() with a window on screen.
bool host_app_rendered(void);
