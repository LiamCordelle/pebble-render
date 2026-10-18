// Kitchen sink: one watchface that exercises the parts of the SDK a renderer is most likely to
// get wrong, used to check pebble-render against the emulator pixel for pixel.
//
// Top to bottom: system fonts (Gothic, Bitham, LECO, Roboto), text alignment and ellipsis
// overflow, a custom TTF, a PNG in two bitmap layers (plain and set-compositing), filled and
// outlined anti-aliased paths, nested layers with clipping and offset bounds, and a status
// line showing the clock style, battery, Bluetooth and step count the watch reports.

#include <pebble.h>

static Window *s_window;
static TextLayer *s_time_layer;
static TextLayer *s_overflow_layer;
static TextLayer *s_right_layer;
static TextLayer *s_status_layer;
static Layer *s_canvas;
static Layer *s_clip_parent;
static Layer *s_clip_child;
static BitmapLayer *s_bitmap_layer;
static BitmapLayer *s_bitmap_set_layer;
static GBitmap *s_tile;
static GFont s_pixel_font;
static GPath *s_star;

static char s_time_text[8];
static char s_status_text[48];

static const GPathInfo STAR_INFO = {
  .num_points = 10,
  .points = (GPoint[]){ {0, -14}, {4, -5}, {13, -4}, {6, 2}, {8, 12},
                        {0, 7}, {-8, 12}, {-6, 2}, {-13, -4}, {-4, -5} },
};

static void update_time(void) {
  time_t now = time(NULL);
  struct tm *t = localtime(&now);
  strftime(s_time_text, sizeof(s_time_text), clock_is_24h_style() ? "%H:%M" : "%I:%M", t);
  text_layer_set_text(s_time_layer, s_time_text);

  BatteryChargeState battery = battery_state_service_peek();
  snprintf(s_status_text, sizeof(s_status_text), "%s %d%%%s %s %d",
           clock_is_24h_style() ? "24H" : "12H", battery.charge_percent,
           battery.is_charging ? "+" : "",
           connection_service_peek_pebble_app_connection() ? "BT" : "--",
           (int)health_service_sum_today(HealthMetricStepCount));
  text_layer_set_text(s_status_layer, s_status_text);
}

static void tick_handler(struct tm *tick_time, TimeUnits units_changed) {
  update_time();
}

static void canvas_update_proc(Layer *layer, GContext *ctx) {
  GRect bounds = layer_get_bounds(layer);

  // Custom TTF.
  graphics_context_set_text_color(ctx, GColorChromeYellow);
  graphics_draw_text(ctx, "PIXEL 16", s_pixel_font, GRect(4, 0, 120, 20),
                     GTextOverflowModeWordWrap, GTextAlignmentLeft, NULL);

  // Anti-aliased filled and outlined paths, a thick line and circles.
  graphics_context_set_antialiased(ctx, true);
  gpath_move_to(s_star, GPoint(150, 12));
  graphics_context_set_fill_color(ctx, GColorRed);
  gpath_draw_filled(ctx, s_star);
  graphics_context_set_stroke_color(ctx, GColorWhite);
  graphics_context_set_stroke_width(ctx, 1);
  gpath_draw_outline(ctx, s_star);
  graphics_context_set_stroke_color(ctx, GColorCyan);
  graphics_context_set_stroke_width(ctx, 3);
  graphics_draw_line(ctx, GPoint(4, 26), GPoint(bounds.size.w - 40, 34));
  graphics_context_set_fill_color(ctx, GColorJaegerGreen);
  graphics_fill_circle(ctx, GPoint(184, 12), 9);
  graphics_draw_circle(ctx, GPoint(184, 12), 12);
  graphics_draw_arc(ctx, GRect(166, 22, 30, 30), GOvalScaleModeFitCircle, 0,
                    TRIG_MAX_ANGLE * 3 / 4);
}

static void clip_child_update_proc(Layer *layer, GContext *ctx) {
  // The child sits partly outside its parent, and its bounds are scrolled by (-10, -6):
  // the parent must clip it, and drawing must start at the offset origin.
  graphics_context_set_fill_color(ctx, GColorVividCerulean);
  graphics_fill_rect(ctx, GRect(0, 0, 60, 30), 6, GCornersAll);
  graphics_context_set_text_color(ctx, GColorBlack);
  graphics_draw_text(ctx, "CLIP", fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD),
                     GRect(10, 4, 50, 22), GTextOverflowModeFill, GTextAlignmentLeft, NULL);
}

static void clip_parent_update_proc(Layer *layer, GContext *ctx) {
  graphics_context_set_stroke_color(ctx, GColorMelon);
  graphics_draw_rect(ctx, layer_get_bounds(layer));
}

static void window_load(Window *window) {
  Layer *root = window_get_root_layer(window);
  window_set_background_color(window, GColorOxfordBlue);

  s_time_layer = text_layer_create(GRect(0, 2, 200, 46));
  text_layer_set_background_color(s_time_layer, GColorClear);
  text_layer_set_text_color(s_time_layer, GColorWhite);
  text_layer_set_font(s_time_layer, fonts_get_system_font(FONT_KEY_LECO_42_NUMBERS));
  text_layer_set_text_alignment(s_time_layer, GTextAlignmentCenter);
  layer_add_child(root, text_layer_get_layer(s_time_layer));

  s_overflow_layer = text_layer_create(GRect(4, 50, 120, 22));
  text_layer_set_background_color(s_overflow_layer, GColorWhite);
  text_layer_set_text_color(s_overflow_layer, GColorBlack);
  text_layer_set_font(s_overflow_layer, fonts_get_system_font(FONT_KEY_GOTHIC_18));
  text_layer_set_overflow_mode(s_overflow_layer, GTextOverflowModeTrailingEllipsis);
  text_layer_set_text(s_overflow_layer, "This sentence is far too long to fit");
  layer_add_child(root, text_layer_get_layer(s_overflow_layer));

  s_right_layer = text_layer_create(GRect(128, 50, 68, 22));
  text_layer_set_background_color(s_right_layer, GColorClear);
  text_layer_set_text_color(s_right_layer, GColorPictonBlue);
  text_layer_set_font(s_right_layer, fonts_get_system_font(FONT_KEY_ROBOTO_CONDENSED_21));
  text_layer_set_text_alignment(s_right_layer, GTextAlignmentRight);
  text_layer_set_text(s_right_layer, "Right");
  layer_add_child(root, text_layer_get_layer(s_right_layer));

  s_canvas = layer_create(GRect(0, 76, 200, 56));
  layer_set_update_proc(s_canvas, canvas_update_proc);
  layer_add_child(root, s_canvas);

  s_tile = gbitmap_create_with_resource(RESOURCE_ID_TILE);
  s_bitmap_layer = bitmap_layer_create(GRect(4, 136, 40, 40));
  bitmap_layer_set_bitmap(s_bitmap_layer, s_tile);
  layer_add_child(root, bitmap_layer_get_layer(s_bitmap_layer));
  s_bitmap_set_layer = bitmap_layer_create(GRect(48, 136, 40, 40));
  bitmap_layer_set_bitmap(s_bitmap_set_layer, s_tile);
  bitmap_layer_set_compositing_mode(s_bitmap_set_layer, GCompOpSet);
  bitmap_layer_set_background_color(s_bitmap_set_layer, GColorLightGray);
  layer_add_child(root, bitmap_layer_get_layer(s_bitmap_set_layer));

  s_clip_parent = layer_create(GRect(100, 140, 92, 34));
  layer_set_update_proc(s_clip_parent, clip_parent_update_proc);
  layer_add_child(root, s_clip_parent);
  s_clip_child = layer_create(GRect(46, 8, 60, 30));
  layer_set_bounds(s_clip_child, GRect(-10, -6, 60, 30));
  layer_set_update_proc(s_clip_child, clip_child_update_proc);
  layer_add_child(s_clip_parent, s_clip_child);

  s_status_layer = text_layer_create(GRect(0, 182, 200, 40));
  text_layer_set_background_color(s_status_layer, GColorBlack);
  text_layer_set_text_color(s_status_layer, GColorMintGreen);
  text_layer_set_font(s_status_layer, fonts_get_system_font(FONT_KEY_BITHAM_30_BLACK));
  text_layer_set_text_alignment(s_status_layer, GTextAlignmentCenter);
  layer_add_child(root, text_layer_get_layer(s_status_layer));

  update_time();
}

static void window_unload(Window *window) {
  text_layer_destroy(s_time_layer);
  text_layer_destroy(s_overflow_layer);
  text_layer_destroy(s_right_layer);
  text_layer_destroy(s_status_layer);
  bitmap_layer_destroy(s_bitmap_layer);
  bitmap_layer_destroy(s_bitmap_set_layer);
  gbitmap_destroy(s_tile);
  layer_destroy(s_clip_child);
  layer_destroy(s_clip_parent);
  layer_destroy(s_canvas);
}

static void init(void) {
  s_pixel_font = fonts_load_custom_font(resource_get_handle(RESOURCE_ID_FONT_PIXEL_16));
  s_star = gpath_create(&STAR_INFO);
  s_window = window_create();
  window_set_window_handlers(s_window, (WindowHandlers){
    .load = window_load,
    .unload = window_unload,
  });
  window_stack_push(s_window, true);
  tick_timer_service_subscribe(MINUTE_UNIT, tick_handler);
}

static void deinit(void) {
  tick_timer_service_unsubscribe();
  window_destroy(s_window);
  gpath_destroy(s_star);
  fonts_unload_custom_font(s_pixel_font);
}

int main(void) {
  init();
  app_event_loop();
  deinit();
}
