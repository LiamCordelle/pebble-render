// App side of the host renderer for Watchface Lab projects (src/c/lab_designs.h):
// compiled like the project's own sources (SDK pebble.h, emery defines, LAB_SCENARIO), in
// place of lab_main.c. Mirrors lab_main.c: same design table, data refresh, load and draw
// order. Ordinary Pebble apps use app_glue.c instead.

#include "lab.h"
#include "lab_designs.h"

#define LAB_EXTERN(sym) extern const LabDesign sym;
LAB_DESIGNS(LAB_EXTERN)
#define LAB_POINTER(sym) &sym,
static const LabDesign *const s_designs[] = { LAB_DESIGNS(LAB_POINTER) };
#define DESIGN_COUNT ((int)(sizeof(s_designs) / sizeof(s_designs[0])))

static LabData s_data;

int hostgfx_app_design_count(void) {
  return DESIGN_COUNT;
}

const char *hostgfx_app_design_name(int index) {
  return s_designs[index]->name;
}

void hostgfx_app_load(int index) {
  // lab_main.c init(): load_design() then lab_data_refresh().
  if (s_designs[index]->load) {
    s_designs[index]->load();
  }
  lab_data_refresh(&s_data);
}

int hostgfx_app_draw(GContext *ctx, GRect bounds, int index, int detail) {
  // lab_main.c canvas_update_proc() (the design-name pill is only drawn after a tap).
  graphics_context_set_antialiased(ctx, true);
  if (detail) {
#if defined(LAB_HAS_DETAIL)
    if (!s_designs[index]->draw_detail) {
      return -1;
    }
    s_designs[index]->draw_detail(ctx, bounds, &s_data);
    return 0;
#else
    return -1;
#endif
  }
  s_designs[index]->draw(ctx, bounds, &s_data);
  return 0;
}

void hostgfx_app_unload(int index) {
  if (s_designs[index]->unload) {
    s_designs[index]->unload();
  }
  lab_release_shared();
}
