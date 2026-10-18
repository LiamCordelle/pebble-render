// App side of the host renderer for ordinary Pebble apps, compiled like the app's own
// sources (SDK pebble.h, emery defines). It runs the app's real main(), which build.py
// renames to pebble_app_main. Rendering happens inside app_event_loop() (os_host.c), after
// init() has pushed a window and before deinit() tears it down.

#include <pebble.h>

extern int pebble_app_main(void);
extern bool host_app_rendered(void);

int hostgfx_app_design_count(void) {
  return 1;
}

const char *hostgfx_app_design_name(int index) {
  return "app";
}

void hostgfx_app_load(int index) {
}

int hostgfx_app_draw(GContext *ctx, GRect bounds, int index, int detail) {
  if (detail) {
    return -1;
  }
  pebble_app_main();
  return host_app_rendered() ? 0 : 1;
}

void hostgfx_app_unload(int index) {
}
