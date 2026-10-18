// A watchapp that tours the SDK's UI components, for checking pebble-render against the
// emulator with button presses: a menu under a status bar opens an action bar with a
// counter, a scroll layer, a property animation, a number window, an action menu and a
// simple menu. There's no phone-side JavaScript, so only the buttons change what's shown.

#include <pebble.h>

static GBitmap *s_icon_plus;
static GBitmap *s_icon_reset;
static GBitmap *s_icon_minus;

// ---------------------------------------------------------------------------
// Action bar: up and down change a counter, holding select resets it.

static Window *s_counter_window;
static ActionBarLayer *s_action_bar;
static TextLayer *s_count_layer;
static TextLayer *s_count_label;
static int s_count = 3;
static char s_count_text[8];

static void prv_show_count(void) {
  snprintf(s_count_text, sizeof(s_count_text), "%d", s_count);
  text_layer_set_text(s_count_layer, s_count_text);
}

static void prv_counter_up(ClickRecognizerRef recognizer, void *context) {
  s_count++;
  prv_show_count();
}

static void prv_counter_down(ClickRecognizerRef recognizer, void *context) {
  s_count--;
  prv_show_count();
}

static void prv_counter_reset(ClickRecognizerRef recognizer, void *context) {
  s_count = 0;
  prv_show_count();
}

static void prv_counter_click_config(void *context) {
  window_single_repeating_click_subscribe(BUTTON_ID_UP, 150, prv_counter_up);
  window_single_click_subscribe(BUTTON_ID_DOWN, prv_counter_down);
  window_long_click_subscribe(BUTTON_ID_SELECT, 700, prv_counter_reset, NULL);
}

static void prv_counter_load(Window *window) {
  Layer *root = window_get_root_layer(window);
  GRect bounds = layer_get_bounds(root);
  const int width = bounds.size.w - ACTION_BAR_WIDTH;

  s_count_label = text_layer_create(GRect(0, 40, width, 30));
  text_layer_set_text(s_count_label, "Laps");
  text_layer_set_font(s_count_label, fonts_get_system_font(FONT_KEY_GOTHIC_24_BOLD));
  text_layer_set_text_alignment(s_count_label, GTextAlignmentCenter);
  text_layer_set_background_color(s_count_label, GColorClear);
  layer_add_child(root, text_layer_get_layer(s_count_label));

  s_count_layer = text_layer_create(GRect(0, 72, width, 60));
  text_layer_set_font(s_count_layer, fonts_get_system_font(FONT_KEY_BITHAM_42_BOLD));
  text_layer_set_text_alignment(s_count_layer, GTextAlignmentCenter);
  text_layer_set_background_color(s_count_layer, GColorClear);
  layer_add_child(root, text_layer_get_layer(s_count_layer));
  prv_show_count();

  s_action_bar = action_bar_layer_create();
  action_bar_layer_set_background_color(s_action_bar, GColorMintGreen);
  action_bar_layer_set_icon(s_action_bar, BUTTON_ID_UP, s_icon_plus);
  action_bar_layer_set_icon(s_action_bar, BUTTON_ID_SELECT, s_icon_reset);
  action_bar_layer_set_icon(s_action_bar, BUTTON_ID_DOWN, s_icon_minus);
  action_bar_layer_set_click_config_provider(s_action_bar, prv_counter_click_config);
  action_bar_layer_add_to_window(s_action_bar, window);
}

static void prv_counter_unload(Window *window) {
  action_bar_layer_destroy(s_action_bar);
  text_layer_destroy(s_count_layer);
  text_layer_destroy(s_count_label);
}

// ---------------------------------------------------------------------------
// Scroll layer: up and down scroll a long text.

static Window *s_scroll_window;
static ScrollLayer *s_scroll_layer;
static TextLayer *s_scroll_text;

static const char *SCROLL_TEXT =
    "Scroll layers move their content with up and down, a page at a time, and animate "
    "between positions. This text is long enough to need scrolling on the Pebble Time 2.\n\n"
    "Content indicators show which way there is more to read. Press down to scroll to the "
    "end, and up to come back to the start. Back returns to the menu.";

static void prv_scroll_load(Window *window) {
  Layer *root = window_get_root_layer(window);
  GRect bounds = layer_get_bounds(root);
  s_scroll_layer = scroll_layer_create(bounds);
  scroll_layer_set_click_config_onto_window(s_scroll_layer, window);

  s_scroll_text = text_layer_create(GRect(6, 0, bounds.size.w - 12, 2000));
  text_layer_set_text(s_scroll_text, SCROLL_TEXT);
  text_layer_set_font(s_scroll_text, fonts_get_system_font(FONT_KEY_GOTHIC_24));
  GSize size = text_layer_get_content_size(s_scroll_text);
  text_layer_set_size(s_scroll_text, GSize(bounds.size.w - 12, size.h + 8));
  scroll_layer_set_content_size(s_scroll_layer, GSize(bounds.size.w, size.h + 12));
  scroll_layer_add_child(s_scroll_layer, text_layer_get_layer(s_scroll_text));
  layer_add_child(root, scroll_layer_get_layer(s_scroll_layer));
}

static void prv_scroll_unload(Window *window) {
  text_layer_destroy(s_scroll_text);
  scroll_layer_destroy(s_scroll_layer);
}

// ---------------------------------------------------------------------------
// Property animation: a ball slides across when the window appears.

static Window *s_animation_window;
static Layer *s_ball_layer;

static void prv_ball_update(Layer *layer, GContext *ctx) {
  GRect bounds = layer_get_bounds(layer);
  graphics_context_set_fill_color(ctx, GColorOrange);
  graphics_fill_circle(ctx, grect_center_point(&bounds), bounds.size.w / 2 - 1);
}

static void prv_animation_load(Window *window) {
  s_ball_layer = layer_create(GRect(10, 94, 40, 40));
  layer_set_update_proc(s_ball_layer, prv_ball_update);
  layer_add_child(window_get_root_layer(window), s_ball_layer);
}

static void prv_animation_appear(Window *window) {
  GRect from = GRect(10, 94, 40, 40);
  GRect to = GRect(150, 94, 40, 40);
  layer_set_frame(s_ball_layer, from);
  PropertyAnimation *slide = property_animation_create_layer_frame(s_ball_layer, &from, &to);
  Animation *animation = property_animation_get_animation(slide);
  animation_set_duration(animation, 1000);
  animation_set_curve(animation, AnimationCurveEaseInOut);
  animation_schedule(animation);
}

static void prv_animation_unload(Window *window) {
  layer_destroy(s_ball_layer);
}

// ---------------------------------------------------------------------------
// Number window, action menu and simple menu.

static NumberWindow *s_number_window;

static void prv_number_selected(NumberWindow *number_window, void *context) {
  window_stack_pop(true);
}

static ActionMenuLevel *s_action_root;

static void prv_action_performed(ActionMenu *action_menu, const ActionMenuItem *action,
                                 void *context) {
}

static void prv_open_action_menu(void) {
  if (!s_action_root) {
    s_action_root = action_menu_level_create(4);
    action_menu_level_add_action(s_action_root, "Start", prv_action_performed, NULL);
    action_menu_level_add_action(s_action_root, "Pause", prv_action_performed, NULL);
    ActionMenuLevel *pace = action_menu_level_create(3);
    action_menu_level_add_action(pace, "Easy", prv_action_performed, NULL);
    action_menu_level_add_action(pace, "Tempo", prv_action_performed, NULL);
    action_menu_level_add_action(pace, "Race", prv_action_performed, NULL);
    action_menu_level_add_child(s_action_root, pace, "Pace");
    action_menu_level_add_action(s_action_root, "Discard", prv_action_performed, NULL);
  }
  ActionMenuConfig config = {
    .root_level = s_action_root,
    .colors = { .background = GColorChromeYellow, .foreground = GColorBlack },
    .align = ActionMenuAlignCenter,
  };
  action_menu_open(&config);
}

static Window *s_simple_window;
static SimpleMenuLayer *s_simple_menu;
static SimpleMenuItem s_simple_items[3];
static SimpleMenuSection s_simple_section;

static void prv_simple_load(Window *window) {
  s_simple_items[0] = (SimpleMenuItem){ .title = "Add", .subtitle = "Plus icon",
                                        .icon = s_icon_plus };
  s_simple_items[1] = (SimpleMenuItem){ .title = "Reset", .subtitle = "Circle icon",
                                        .icon = s_icon_reset };
  s_simple_items[2] = (SimpleMenuItem){ .title = "Remove", .subtitle = "Minus icon",
                                        .icon = s_icon_minus };
  s_simple_section = (SimpleMenuSection){ .title = "Simple menu", .items = s_simple_items,
                                          .num_items = 3 };
  Layer *root = window_get_root_layer(window);
  s_simple_menu = simple_menu_layer_create(layer_get_bounds(root), window, &s_simple_section, 1,
                                           NULL);
  layer_add_child(root, simple_menu_layer_get_layer(s_simple_menu));
}

static void prv_simple_unload(Window *window) {
  simple_menu_layer_destroy(s_simple_menu);
}

// ---------------------------------------------------------------------------
// The main menu, under a status bar.

static Window *s_main_window;
static StatusBarLayer *s_status_bar;
static MenuLayer *s_menu_layer;

static const char *s_titles[2][3] = {
  { "Action bar", "Scroll", "Animation" },
  { "Number window", "Action menu", "Simple menu" },
};
static const char *s_subtitles[2][3] = {
  { "Counter", "Long text", "Sliding ball" },
  { "Pick a value", "Levels", "With icons" },
};

static uint16_t prv_num_sections(MenuLayer *menu_layer, void *context) {
  return 2;
}

static uint16_t prv_num_rows(MenuLayer *menu_layer, uint16_t section, void *context) {
  return 3;
}

static int16_t prv_header_height(MenuLayer *menu_layer, uint16_t section, void *context) {
  return MENU_CELL_BASIC_HEADER_HEIGHT;
}

static void prv_draw_header(GContext *ctx, const Layer *cell, uint16_t section, void *context) {
  menu_cell_basic_header_draw(ctx, cell, section == 0 ? "Layers" : "System windows");
}

static void prv_draw_row(GContext *ctx, const Layer *cell, MenuIndex *index, void *context) {
  menu_cell_basic_draw(ctx, cell, s_titles[index->section][index->row],
                       s_subtitles[index->section][index->row], NULL);
}

static Window *prv_window(Window **window, WindowHandlers handlers) {
  if (!*window) {
    *window = window_create();
    window_set_window_handlers(*window, handlers);
  }
  return *window;
}

static void prv_select(MenuLayer *menu_layer, MenuIndex *index, void *context) {
  if (index->section == 0 && index->row == 0) {
    window_stack_push(prv_window(&s_counter_window, (WindowHandlers){
      .load = prv_counter_load, .unload = prv_counter_unload }), true);
  } else if (index->section == 0 && index->row == 1) {
    window_stack_push(prv_window(&s_scroll_window, (WindowHandlers){
      .load = prv_scroll_load, .unload = prv_scroll_unload }), true);
  } else if (index->section == 0) {
    window_stack_push(prv_window(&s_animation_window, (WindowHandlers){
      .load = prv_animation_load, .appear = prv_animation_appear,
      .unload = prv_animation_unload }), true);
  } else if (index->row == 0) {
    if (!s_number_window) {
      s_number_window = number_window_create("Laps", (NumberWindowCallbacks){
        .selected = prv_number_selected }, NULL);
      number_window_set_min(s_number_window, 0);
      number_window_set_max(s_number_window, 20);
      number_window_set_value(s_number_window, 5);
    }
    window_stack_push(number_window_get_window(s_number_window), true);
  } else if (index->row == 1) {
    prv_open_action_menu();
  } else {
    window_stack_push(prv_window(&s_simple_window, (WindowHandlers){
      .load = prv_simple_load, .unload = prv_simple_unload }), true);
  }
}

static void prv_main_load(Window *window) {
  Layer *root = window_get_root_layer(window);
  GRect bounds = layer_get_bounds(root);

  s_status_bar = status_bar_layer_create();
  status_bar_layer_set_colors(s_status_bar, GColorCobaltBlue, GColorWhite);
  status_bar_layer_set_separator_mode(s_status_bar, StatusBarLayerSeparatorModeDotted);
  layer_add_child(root, status_bar_layer_get_layer(s_status_bar));

  s_menu_layer = menu_layer_create(GRect(0, STATUS_BAR_LAYER_HEIGHT, bounds.size.w,
                                         bounds.size.h - STATUS_BAR_LAYER_HEIGHT));
  menu_layer_set_callbacks(s_menu_layer, NULL, (MenuLayerCallbacks){
    .get_num_sections = prv_num_sections,
    .get_num_rows = prv_num_rows,
    .get_header_height = prv_header_height,
    .draw_header = prv_draw_header,
    .draw_row = prv_draw_row,
    .select_click = prv_select,
  });
  menu_layer_set_highlight_colors(s_menu_layer, GColorCobaltBlue, GColorWhite);
  menu_layer_set_click_config_onto_window(s_menu_layer, window);
  layer_add_child(root, menu_layer_get_layer(s_menu_layer));
}

static void prv_main_unload(Window *window) {
  menu_layer_destroy(s_menu_layer);
  status_bar_layer_destroy(s_status_bar);
}

static void prv_init(void) {
  s_icon_plus = gbitmap_create_with_resource(RESOURCE_ID_ICON_PLUS);
  s_icon_reset = gbitmap_create_with_resource(RESOURCE_ID_ICON_RESET);
  s_icon_minus = gbitmap_create_with_resource(RESOURCE_ID_ICON_MINUS);
  s_main_window = window_create();
  window_set_window_handlers(s_main_window, (WindowHandlers){
    .load = prv_main_load, .unload = prv_main_unload });
  window_stack_push(s_main_window, true);
}

static void prv_deinit(void) {
  window_destroy(s_main_window);
  if (s_counter_window) {
    window_destroy(s_counter_window);
  }
  if (s_scroll_window) {
    window_destroy(s_scroll_window);
  }
  if (s_animation_window) {
    window_destroy(s_animation_window);
  }
  if (s_simple_window) {
    window_destroy(s_simple_window);
  }
  if (s_number_window) {
    number_window_destroy(s_number_window);
  }
  if (s_action_root) {
    action_menu_hierarchy_destroy(s_action_root, NULL, NULL);
  }
  gbitmap_destroy(s_icon_plus);
  gbitmap_destroy(s_icon_reset);
  gbitmap_destroy(s_icon_minus);
}

int main(void) {
  prv_init();
  app_event_loop();
  prv_deinit();
}
