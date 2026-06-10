// Hyundai Bluelink - Pebble Time 2 (Emery) - C SDK
// src/c/bluelink.c
//
// Windows:
//   menu_window     - MenuLayer: 5 base + 2 EV-only actions + Settings
//   settings_window - toggles: EV features, 12V battery, °F/°C
//   status_window   - TextLayer grid (rows shown/hidden per settings)
//   confirm_window  - confirmation before destructive commands
//   temp_window     - temperature picker before Remote Start
//   feedback_window - full-screen success/error, auto-pops after 2.5s
//
// Persist keys (survive app restart, no phone needed):
//   PERSIST_EV_ENABLED    0  bool: show EV rows + charge commands
//   PERSIST_12V_ENABLED   1  bool: show 12V battery row
//   PERSIST_USE_FAHRENHEIT 2 bool: temp picker in °F vs °C
//
// AppMessage keys (must match Android PebbleMessageReceiver.kt):
//   KEY_CMD          0
//   KEY_RESULT       1
//   KEY_MSG          2
//   KEY_LOCKED       3
//   KEY_TEMP         4
//   KEY_RANGE        5
//   KEY_CHARGING     6
//   KEY_CHARGE_TIME  7
//   KEY_TWELVE_SOC   8
//   KEY_CLIMATE_TEMP 9

#include <pebble.h>

// ── AppMessage keys ───────────────────────────────────────────────────────────
#define KEY_CMD          0
#define KEY_RESULT       1
#define KEY_MSG          2
#define KEY_LOCKED       3
#define KEY_TEMP         4
#define KEY_RANGE        5
#define KEY_CHARGING     6
#define KEY_CHARGE_TIME  7
#define KEY_TWELVE_SOC   8
#define KEY_CLIMATE_TEMP 9

// ── Persist keys ──────────────────────────────────────────────────────────────
#define PERSIST_EV_ENABLED      0
#define PERSIST_12V_ENABLED     1
#define PERSIST_USE_FAHRENHEIT  2
#define PERSIST_USE_MILES       3

// ── Menu ──────────────────────────────────────────────────────────────────────
// All possible items; EV-only ones are filtered at draw/select time
#define IDX_STATUS        0
#define IDX_LOCK          1
#define IDX_UNLOCK        2
#define IDX_START         3
#define IDX_STOP          4
#define IDX_CHARGE_START  5
#define IDX_CHARGE_STOP   6
#define IDX_SETTINGS      7
#define NUM_ALL_ITEMS     8

typedef struct {
  const char *title;
  const char *subtitle;
  const char *cmd;
  bool        confirm;
  bool        needs_temp;
  bool        ev_only;     // hidden when EV features disabled
} MenuItem;

static const MenuItem ALL_ITEMS[NUM_ALL_ITEMS] = {
  { "Vehicle Status",  "Refresh status",        NULL,          false, false, false },
  { "Lock",            "Lock all doors",         "lock",        true,  false, false },
  { "Unlock",          "Unlock all doors",       "unlock",      true,  false, false },
  { "Remote Start",    "Start engine/climate",   "start",       true,  true,  false },
  { "Remote Stop",     "Stop engine",            "stop",        true,  false, false },
  { "Start Charging",  "Begin charging",         "chargestart", true,  false, true  },
  { "Stop Charging",   "Stop charging",          "chargestop",  true,  false, true  },
  { "Settings",        "EV, 12V, units",         NULL,          false, false, false },
};

// ── Settings state (loaded from persist on init) ──────────────────────────────
static bool s_ev_enabled     = true;
static bool s_12v_enabled    = true;
static bool s_use_fahrenheit = true;
static bool s_use_miles      = true;

// ── App state ─────────────────────────────────────────────────────────────────
static int  s_pending_item_idx  = -1;
static bool s_status_loaded     = false;
static bool s_locked            = true;
static int  s_range             = 0;
static bool s_charging          = false;
static int  s_charge_time_mins  = 0;
static int  s_twelve_soc        = 0;
static int  s_climate_temp_f    = 70;

// ── Visible menu helpers ──────────────────────────────────────────────────────
// Returns how many items are currently visible given settings
static int visible_item_count(void) {
  int count = 0;
  for (int i = 0; i < NUM_ALL_ITEMS; i++) {
    if (!ALL_ITEMS[i].ev_only || s_ev_enabled) count++;
  }
  return count;
}

// Maps a visible row index to an ALL_ITEMS index
static int visible_to_all_idx(int visible_row) {
  int count = 0;
  for (int i = 0; i < NUM_ALL_ITEMS; i++) {
    if (!ALL_ITEMS[i].ev_only || s_ev_enabled) {
      if (count == visible_row) return i;
      count++;
    }
  }
  return 0;
}

// ── Forward declarations ──────────────────────────────────────────────────────
static void status_window_push(void);
static void settings_window_push(void);
static void confirm_window_push(int all_idx);
static void temp_window_push(int all_idx);
static void feedback_window_push(bool success, const char *msg);
static void send_command(const char *cmd);
static void send_start_with_temp(int temp_f);
static void request_status(void);
static void status_update_display(void);

// =============================================================================
// FEEDBACK WINDOW
// =============================================================================
static Window    *s_feedback_window;
static TextLayer *s_feedback_icon_layer;
static TextLayer *s_feedback_msg_layer;
static TextLayer *s_feedback_hint_layer;
static AppTimer  *s_feedback_timer;

static void feedback_timer_callback(void *ctx) {
  s_feedback_timer = NULL;
  window_stack_pop(true);
}

static void feedback_window_load(Window *window) {
  Layer *root = window_get_root_layer(window);
  GRect bounds = layer_get_bounds(root);

  s_feedback_icon_layer = text_layer_create(GRect(0, 50, bounds.size.w, 60));
  text_layer_set_font(s_feedback_icon_layer,
                      fonts_get_system_font(FONT_KEY_BITHAM_42_BOLD));
  text_layer_set_text_alignment(s_feedback_icon_layer, GTextAlignmentCenter);
  text_layer_set_background_color(s_feedback_icon_layer, GColorClear);
  layer_add_child(root, text_layer_get_layer(s_feedback_icon_layer));

  s_feedback_msg_layer = text_layer_create(
      GRect(8, 118, bounds.size.w - 16, 60));
  text_layer_set_font(s_feedback_msg_layer,
                      fonts_get_system_font(FONT_KEY_GOTHIC_24_BOLD));
  text_layer_set_text_alignment(s_feedback_msg_layer, GTextAlignmentCenter);
  text_layer_set_background_color(s_feedback_msg_layer, GColorClear);
  layer_add_child(root, text_layer_get_layer(s_feedback_msg_layer));

  s_feedback_hint_layer = text_layer_create(
      GRect(0, bounds.size.h - 22, bounds.size.w, 18));
  text_layer_set_font(s_feedback_hint_layer,
                      fonts_get_system_font(FONT_KEY_GOTHIC_14));
  text_layer_set_text_alignment(s_feedback_hint_layer, GTextAlignmentCenter);
  text_layer_set_text(s_feedback_hint_layer, "Returns automatically...");
  text_layer_set_background_color(s_feedback_hint_layer, GColorClear);
  layer_add_child(root, text_layer_get_layer(s_feedback_hint_layer));
}

static void feedback_window_appear(Window *window) {
  s_feedback_timer = app_timer_register(3000, feedback_timer_callback, NULL);
}

static void feedback_window_unload(Window *window) {
  if (s_feedback_timer) {
    app_timer_cancel(s_feedback_timer);
    s_feedback_timer = NULL;
  }
  text_layer_destroy(s_feedback_icon_layer);
  text_layer_destroy(s_feedback_msg_layer);
  text_layer_destroy(s_feedback_hint_layer);
  window_destroy(window);
  s_feedback_window = NULL;
}

static void feedback_window_push(bool success, const char *msg) {
  if (success) {
    static const uint32_t segs[] = { 80, 80, 80 };
    VibePattern vp = { .durations = segs, .num_segments = 3 };
    vibes_enqueue_custom_pattern(vp);
  } else {
    static const uint32_t segs[] = { 200, 100, 200 };
    VibePattern vp = { .durations = segs, .num_segments = 3 };
    vibes_enqueue_custom_pattern(vp);
  }

  s_feedback_window = window_create();
  window_set_background_color(s_feedback_window,
      success ? GColorMediumSpringGreen : GColorRed);
  window_set_window_handlers(s_feedback_window, (WindowHandlers){
    .load   = feedback_window_load,
    .appear = feedback_window_appear,
    .unload = feedback_window_unload,
  });
  window_stack_push(s_feedback_window, true);

  text_layer_set_text(s_feedback_icon_layer, success ? "OK" : "ERR");
  text_layer_set_text_color(s_feedback_icon_layer, GColorWhite);
  text_layer_set_text(s_feedback_msg_layer, msg);
  text_layer_set_text_color(s_feedback_msg_layer, GColorWhite);
  text_layer_set_text_color(s_feedback_hint_layer, GColorWhite);
}

// =============================================================================
// SETTINGS WINDOW
// Three toggle rows; SELECT flips the value and redraws
// =============================================================================
static Window    *s_settings_window;
static MenuLayer *s_settings_layer;

#define NUM_SETTINGS 4

typedef struct {
  const char *title;
  bool       *value;
} SettingItem;

static SettingItem SETTINGS[NUM_SETTINGS] = {
  { "EV Features",                     &s_ev_enabled     },
  { "12V Battery",                     &s_12v_enabled    },
  { "Fahrenheit (off for C)",          &s_use_fahrenheit },
  { "Miles (off for Km)",              &s_use_miles      },
};

static uint16_t settings_get_num_sections(MenuLayer *ml, void *ctx) { return 1; }

static uint16_t settings_get_num_rows(MenuLayer *ml, uint16_t section,
                                      void *ctx) {
  return NUM_SETTINGS;
}

static int16_t settings_get_cell_height(MenuLayer *ml, MenuIndex *idx,
                                        void *ctx) {
  return 44;
}

static int16_t settings_get_header_height(MenuLayer *ml, uint16_t section,
                                          void *ctx) {
  return 36;
}

static void settings_draw_header(GContext *ctx, const Layer *cell_layer,
                                 uint16_t section, void *cb_ctx) {
  GRect bounds = layer_get_bounds(cell_layer);
  graphics_context_set_fill_color(ctx, GColorDarkGray);
  graphics_fill_rect(ctx, bounds, 0, GCornerNone);
  graphics_context_set_text_color(ctx, GColorWhite);
  graphics_draw_text(ctx, "Settings",
                     fonts_get_system_font(FONT_KEY_GOTHIC_24_BOLD),
                     GRect(0, 4, bounds.size.w, 28),
                     GTextOverflowModeTrailingEllipsis,
                     GTextAlignmentCenter, NULL);
}

static void settings_draw_row(GContext *ctx, const Layer *cell_layer,
                              MenuIndex *idx, void *cb_ctx) {
  const SettingItem *item = &SETTINGS[idx->row];
  bool highlighted = menu_cell_layer_is_highlighted(cell_layer);
  GRect bounds = layer_get_bounds(cell_layer);

  graphics_context_set_fill_color(ctx,
      highlighted ? GColorOxfordBlue : GColorDarkGray);
  graphics_fill_rect(ctx, bounds, 0, GCornerNone);

  // Title
  graphics_context_set_text_color(ctx, GColorWhite);
  graphics_draw_text(ctx, item->title,
                     fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD),
                     GRect(8, 4, bounds.size.w - 52, 22),
                     GTextOverflowModeTrailingEllipsis,
                     GTextAlignmentLeft, NULL);

  // ON / OFF badge on the right
  bool on = *(item->value);
  GColor badge_bg = on ? GColorMediumSpringGreen : GColorDarkGray;
  graphics_context_set_fill_color(ctx, badge_bg);
  graphics_fill_rect(ctx,
      GRect(bounds.size.w - 44, 10, 36, 22), 4, GCornersAll);
  graphics_context_set_text_color(ctx, GColorWhite);
  graphics_draw_text(ctx, on ? "ON" : "OFF",
                     fonts_get_system_font(FONT_KEY_GOTHIC_14_BOLD),
                     GRect(bounds.size.w - 44, 11, 36, 18),
                     GTextOverflowModeTrailingEllipsis,
                     GTextAlignmentCenter, NULL);
}

static void settings_select_callback(MenuLayer *ml, MenuIndex *idx,
                                     void *ctx) {
  SettingItem *item = &SETTINGS[idx->row];
  *(item->value) = !(*(item->value));

  // Persist the new value
  persist_write_bool(PERSIST_EV_ENABLED,     s_ev_enabled);
  persist_write_bool(PERSIST_12V_ENABLED,    s_12v_enabled);
  persist_write_bool(PERSIST_USE_FAHRENHEIT, s_use_fahrenheit);
  persist_write_bool(PERSIST_USE_MILES,      s_use_miles);

  vibes_short_pulse();
  menu_layer_reload_data(ml);
}

static void settings_window_load(Window *window) {
  Layer *root = window_get_root_layer(window);
  GRect bounds = layer_get_bounds(root);

  s_settings_layer = menu_layer_create(bounds);
  menu_layer_set_normal_colors(s_settings_layer,    GColorDarkGray, GColorWhite);
  menu_layer_set_highlight_colors(s_settings_layer, GColorCobaltBlue,      GColorWhite);
  menu_layer_set_callbacks(s_settings_layer, NULL, (MenuLayerCallbacks){
    .get_num_sections  = settings_get_num_sections,
    .get_num_rows      = settings_get_num_rows,
    .get_cell_height   = settings_get_cell_height,
    .get_header_height = settings_get_header_height,
    .draw_header       = settings_draw_header,
    .draw_row          = settings_draw_row,
    .select_click      = settings_select_callback,
  });
  menu_layer_set_click_config_onto_window(s_settings_layer, window);
  layer_add_child(root, menu_layer_get_layer(s_settings_layer));
}

static void settings_window_unload(Window *window) {
  menu_layer_destroy(s_settings_layer);
  window_destroy(window);
  s_settings_window = NULL;
  // Reload main menu so EV rows appear/disappear immediately on return
  if (s_settings_window) status_update_display();
}

static void settings_window_push(void) {
  s_settings_window = window_create();
  window_set_background_color(s_settings_window, GColorBlack);
  window_set_window_handlers(s_settings_window, (WindowHandlers){
    .load   = settings_window_load,
    .unload = settings_window_unload,
  });
  window_stack_push(s_settings_window, true);
}

// =============================================================================
// TEMPERATURE PICKER WINDOW
// =============================================================================
static Window    *s_temp_window;
static TextLayer *s_temp_title_layer;
static TextLayer *s_temp_value_layer;
static TextLayer *s_temp_hint_layer;
static char       s_temp_value_buf[16];

// Convert stored °F to °C for display
static int fahrenheit_to_celsius(int f) { return (f - 32) * 5 / 9; }

static void temp_update_display(void) {
  if (s_use_fahrenheit) {
    snprintf(s_temp_value_buf, sizeof(s_temp_value_buf),
             "%d\u00B0F", s_climate_temp_f);
  } else {
    snprintf(s_temp_value_buf, sizeof(s_temp_value_buf),
             "%d\u00B0C", fahrenheit_to_celsius(s_climate_temp_f));
  }
  text_layer_set_text(s_temp_value_layer, s_temp_value_buf);
}

static void temp_up_click(ClickRecognizerRef rec, void *ctx) {
  if (s_climate_temp_f < 90) { s_climate_temp_f++; temp_update_display(); }
}

static void temp_down_click(ClickRecognizerRef rec, void *ctx) {
  if (s_climate_temp_f > 60) { s_climate_temp_f--; temp_update_display(); }
}

static void temp_select_click(ClickRecognizerRef rec, void *ctx) {
  window_stack_pop(true);
  send_start_with_temp(s_climate_temp_f);
}

static void temp_click_config(void *ctx) {
  window_single_click_subscribe(BUTTON_ID_UP,     temp_up_click);
  window_single_click_subscribe(BUTTON_ID_DOWN,   temp_down_click);
  window_single_click_subscribe(BUTTON_ID_SELECT, temp_select_click);
  window_single_repeating_click_subscribe(BUTTON_ID_UP,   200, temp_up_click);
  window_single_repeating_click_subscribe(BUTTON_ID_DOWN, 200, temp_down_click);
}

static void temp_window_load(Window *window) {
  Layer *root = window_get_root_layer(window);
  GRect bounds = layer_get_bounds(root);

  s_temp_title_layer = text_layer_create(GRect(0, 0, bounds.size.w, 36));
  text_layer_set_font(s_temp_title_layer,
                      fonts_get_system_font(FONT_KEY_GOTHIC_24_BOLD));
  text_layer_set_text_alignment(s_temp_title_layer, GTextAlignmentCenter);
  text_layer_set_background_color(s_temp_title_layer, GColorCobaltBlue);
  text_layer_set_text_color(s_temp_title_layer, GColorWhite);
  text_layer_set_text(s_temp_title_layer, "Set Temp");
  layer_add_child(root, text_layer_get_layer(s_temp_title_layer));

  s_temp_value_layer = text_layer_create(GRect(0, 70, bounds.size.w, 60));
  text_layer_set_font(s_temp_value_layer,
                      fonts_get_system_font(FONT_KEY_BITHAM_42_BOLD));
  text_layer_set_text_alignment(s_temp_value_layer, GTextAlignmentCenter);
  text_layer_set_background_color(s_temp_value_layer, GColorClear);
  text_layer_set_text_color(s_temp_value_layer, GColorWhite);
  layer_add_child(root, text_layer_get_layer(s_temp_value_layer));

  s_temp_hint_layer = text_layer_create(
      GRect(0, bounds.size.h - 38, bounds.size.w, 36));
  text_layer_set_font(s_temp_hint_layer,
                      fonts_get_system_font(FONT_KEY_GOTHIC_14));
  text_layer_set_text_alignment(s_temp_hint_layer, GTextAlignmentCenter);
  text_layer_set_text_color(s_temp_hint_layer, GColorLightGray);
  text_layer_set_background_color(s_temp_hint_layer, GColorClear);
  text_layer_set_text(s_temp_hint_layer, "UP/DOWN: adjust\nSELECT: start");
  layer_add_child(root, text_layer_get_layer(s_temp_hint_layer));

  temp_update_display();
  window_set_click_config_provider(window, temp_click_config);
}

static void temp_window_unload(Window *window) {
  text_layer_destroy(s_temp_title_layer);
  text_layer_destroy(s_temp_value_layer);
  text_layer_destroy(s_temp_hint_layer);
  window_destroy(window);
  s_temp_window = NULL;
}

static void temp_window_push(int all_idx) {
  s_pending_item_idx = all_idx;
  s_temp_window = window_create();
  window_set_background_color(s_temp_window, GColorBlack);
  window_set_window_handlers(s_temp_window, (WindowHandlers){
    .load   = temp_window_load,
    .unload = temp_window_unload,
  });
  window_stack_push(s_temp_window, true);
}

// =============================================================================
// CONFIRM WINDOW
// =============================================================================
static Window    *s_confirm_window;
static TextLayer *s_confirm_title_layer;
static TextLayer *s_confirm_body_layer;
static TextLayer *s_confirm_hint_layer;
static char       s_confirm_body_buf[64];

static void confirm_select_click(ClickRecognizerRef rec, void *ctx) {
  if (s_pending_item_idx < 0) return;
  send_command(ALL_ITEMS[s_pending_item_idx].cmd);
  window_stack_pop(true);
}

static void confirm_click_config(void *ctx) {
  window_single_click_subscribe(BUTTON_ID_SELECT, confirm_select_click);
}

static void confirm_window_load(Window *window) {
  Layer *root = window_get_root_layer(window);
  GRect bounds = layer_get_bounds(root);

  s_confirm_title_layer = text_layer_create(GRect(0, 0, bounds.size.w, 36));
  text_layer_set_font(s_confirm_title_layer,
                      fonts_get_system_font(FONT_KEY_GOTHIC_24_BOLD));
  text_layer_set_text_alignment(s_confirm_title_layer, GTextAlignmentCenter);
  text_layer_set_background_color(s_confirm_title_layer, GColorCobaltBlue);
  text_layer_set_text_color(s_confirm_title_layer, GColorWhite);
  text_layer_set_text(s_confirm_title_layer, "Confirm");
  layer_add_child(root, text_layer_get_layer(s_confirm_title_layer));

  s_confirm_body_layer = text_layer_create(
      GRect(8, 50, bounds.size.w - 16, 120));
  text_layer_set_font(s_confirm_body_layer,
                      fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD));
  text_layer_set_text_alignment(s_confirm_body_layer, GTextAlignmentCenter);
  text_layer_set_overflow_mode(s_confirm_body_layer, GTextOverflowModeWordWrap);
  text_layer_set_background_color(s_confirm_body_layer, GColorClear);
  layer_add_child(root, text_layer_get_layer(s_confirm_body_layer));

  s_confirm_hint_layer = text_layer_create(
      GRect(0, bounds.size.h - 20, bounds.size.w, 18));
  text_layer_set_font(s_confirm_hint_layer,
                      fonts_get_system_font(FONT_KEY_GOTHIC_14));
  text_layer_set_text_alignment(s_confirm_hint_layer, GTextAlignmentCenter);
  text_layer_set_text(s_confirm_hint_layer, "BACK to cancel");
  text_layer_set_background_color(s_confirm_hint_layer, GColorClear);
  layer_add_child(root, text_layer_get_layer(s_confirm_hint_layer));

  if (s_pending_item_idx >= 0) {
    snprintf(s_confirm_body_buf, sizeof(s_confirm_body_buf),
             "%s\n\nPress SELECT\nto confirm",
             ALL_ITEMS[s_pending_item_idx].title);
    text_layer_set_text(s_confirm_body_layer, s_confirm_body_buf);
  }

  window_set_click_config_provider(window, confirm_click_config);
}

static void confirm_window_unload(Window *window) {
  text_layer_destroy(s_confirm_title_layer);
  text_layer_destroy(s_confirm_body_layer);
  text_layer_destroy(s_confirm_hint_layer);
  window_destroy(window);
  s_confirm_window = NULL;
}

static void confirm_window_push(int all_idx) {
  s_pending_item_idx = all_idx;
  s_confirm_window = window_create();
  window_set_background_color(s_confirm_window, GColorBlack);
  window_set_window_handlers(s_confirm_window, (WindowHandlers){
    .load   = confirm_window_load,
    .unload = confirm_window_unload,
  });
  window_stack_push(s_confirm_window, true);
}

// =============================================================================
// STATUS WINDOW
// Rows shown/hidden according to settings
// =============================================================================
static Window    *s_status_window;
static TextLayer *s_status_title_layer;
static TextLayer *s_status_loading_layer;
static TextLayer *s_status_locked_label;
static TextLayer *s_status_locked_val;
static TextLayer *s_status_range_label;
static TextLayer *s_status_range_val;
static TextLayer *s_status_charging_label;
static TextLayer *s_status_charging_val;
static TextLayer *s_status_chargetime_label;
static TextLayer *s_status_chargetime_val;
static TextLayer *s_status_twelve_label;
static TextLayer *s_status_twelve_val;

static char s_locked_buf[16];
static char s_range_buf[16];
static char s_charging_buf[16];
static char s_chargetime_buf[20];
static char s_twelve_buf[12];

static void status_update_display(void) {
  if (!s_status_window) return;

  // Show/hide EV rows based on setting
  layer_set_hidden(text_layer_get_layer(s_status_range_label),
                   !s_ev_enabled);
  layer_set_hidden(text_layer_get_layer(s_status_range_val),
                   !s_ev_enabled);
  layer_set_hidden(text_layer_get_layer(s_status_charging_label),
                   !s_ev_enabled);
  layer_set_hidden(text_layer_get_layer(s_status_charging_val),
                   !s_ev_enabled);
  layer_set_hidden(text_layer_get_layer(s_status_chargetime_label),
                   !s_ev_enabled);
  layer_set_hidden(text_layer_get_layer(s_status_chargetime_val),
                   !s_ev_enabled);

  // Show/hide 12V row based on setting
  layer_set_hidden(text_layer_get_layer(s_status_twelve_label),
                   !s_12v_enabled);
  layer_set_hidden(text_layer_get_layer(s_status_twelve_val),
                   !s_12v_enabled);

  if (!s_status_loaded) {
    text_layer_set_text(s_status_loading_layer, "Fetching status...");
    layer_set_hidden(text_layer_get_layer(s_status_loading_layer), false);
    layer_set_hidden(text_layer_get_layer(s_status_locked_val),    true);
    return;
  }

  layer_set_hidden(text_layer_get_layer(s_status_loading_layer), true);
  layer_set_hidden(text_layer_get_layer(s_status_locked_val),    false);

  snprintf(s_locked_buf,   sizeof(s_locked_buf),
           s_locked ? "Locked" : "Unlocked");
  if (s_use_miles) {
    snprintf(s_range_buf, sizeof(s_range_buf), "%d mi", s_range);
  } else {
    // Convert miles to km (API always returns miles for US region)
    snprintf(s_range_buf, sizeof(s_range_buf), "%d km",
             (int)(s_range * 1.60934f));
  }
  snprintf(s_charging_buf, sizeof(s_charging_buf),
           s_charging ? "Yes" : "No");

  if (s_charging && s_charge_time_mins > 0) {
    int hrs  = s_charge_time_mins / 60;
    int mins = s_charge_time_mins % 60;
    if (hrs > 0)
      snprintf(s_chargetime_buf, sizeof(s_chargetime_buf), "%dh %dm", hrs, mins);
    else
      snprintf(s_chargetime_buf, sizeof(s_chargetime_buf), "%d min", mins);
  } else {
    snprintf(s_chargetime_buf, sizeof(s_chargetime_buf), "--");
  }

  snprintf(s_twelve_buf, sizeof(s_twelve_buf), "%d%%", s_twelve_soc);

  text_layer_set_text(s_status_locked_val,     s_locked_buf);
  text_layer_set_text_color(s_status_locked_val,
      s_locked ? GColorMediumSpringGreen : GColorRed);
  text_layer_set_text(s_status_range_val,      s_range_buf);
  text_layer_set_text(s_status_charging_val,   s_charging_buf);
  text_layer_set_text(s_status_chargetime_val, s_chargetime_buf);
  text_layer_set_text(s_status_twelve_val,     s_twelve_buf);
}

static void make_status_row(Layer *root, int y, const char *label_str,
                             TextLayer **out_label, TextLayer **out_value) {
  int w = layer_get_bounds(root).size.w;

  *out_label = text_layer_create(GRect(8, y, 90, 20));
  text_layer_set_font(*out_label, fonts_get_system_font(FONT_KEY_GOTHIC_14));
  text_layer_set_text_color(*out_label, GColorLightGray);
  text_layer_set_background_color(*out_label, GColorClear);
  text_layer_set_text(*out_label, label_str);
  layer_add_child(root, text_layer_get_layer(*out_label));

  *out_value = text_layer_create(GRect(w / 2, y, w / 2 - 8, 20));
  text_layer_set_font(*out_value,
                      fonts_get_system_font(FONT_KEY_GOTHIC_14_BOLD));
  text_layer_set_text_alignment(*out_value, GTextAlignmentRight);
  text_layer_set_text_color(*out_value, GColorWhite);
  text_layer_set_background_color(*out_value, GColorClear);
  text_layer_set_text(*out_value, "--");
  layer_add_child(root, text_layer_get_layer(*out_value));
}

static void status_window_load(Window *window) {
  Layer *root = window_get_root_layer(window);
  GRect bounds = layer_get_bounds(root);

  s_status_title_layer = text_layer_create(GRect(0, 0, bounds.size.w, 36));
  text_layer_set_font(s_status_title_layer,
                      fonts_get_system_font(FONT_KEY_GOTHIC_24_BOLD));
  text_layer_set_text_alignment(s_status_title_layer, GTextAlignmentCenter);
  text_layer_set_background_color(s_status_title_layer, GColorCobaltBlue);
  text_layer_set_text_color(s_status_title_layer, GColorWhite);
  text_layer_set_text(s_status_title_layer, "Status");
  layer_add_child(root, text_layer_get_layer(s_status_title_layer));

  s_status_loading_layer = text_layer_create(GRect(0, 90, bounds.size.w, 28));
  text_layer_set_font(s_status_loading_layer,
                      fonts_get_system_font(FONT_KEY_GOTHIC_18));
  text_layer_set_text_alignment(s_status_loading_layer, GTextAlignmentCenter);
  text_layer_set_background_color(s_status_loading_layer, GColorClear);
  text_layer_set_text_color(s_status_loading_layer, GColorLightGray);
  layer_add_child(root, text_layer_get_layer(s_status_loading_layer));

  // All rows always created; visibility controlled in status_update_display()
  make_status_row(root,  40, "Doors",
                  &s_status_locked_label,     &s_status_locked_val);
  make_status_row(root,  62, "Range",
                  &s_status_range_label,      &s_status_range_val);
  make_status_row(root,  84, "Charging",
                  &s_status_charging_label,   &s_status_charging_val);
  make_status_row(root, 106, "Time left",
                  &s_status_chargetime_label, &s_status_chargetime_val);
  make_status_row(root, 128, "12V Bat",
                  &s_status_twelve_label,     &s_status_twelve_val);

  status_update_display();
  request_status();
}

static void status_window_unload(Window *window) {
  text_layer_destroy(s_status_title_layer);
  text_layer_destroy(s_status_loading_layer);
  text_layer_destroy(s_status_locked_label);
  text_layer_destroy(s_status_locked_val);
  text_layer_destroy(s_status_range_label);
  text_layer_destroy(s_status_range_val);
  text_layer_destroy(s_status_charging_label);
  text_layer_destroy(s_status_charging_val);
  text_layer_destroy(s_status_chargetime_label);
  text_layer_destroy(s_status_chargetime_val);
  text_layer_destroy(s_status_twelve_label);
  text_layer_destroy(s_status_twelve_val);
  window_destroy(window);
  s_status_window = NULL;
}

static void status_window_push(void) {
  s_status_loaded = false;
  s_status_window = window_create();
  window_set_background_color(s_status_window, GColorBlack);
  window_set_window_handlers(s_status_window, (WindowHandlers){
    .load   = status_window_load,
    .unload = status_window_unload,
  });
  window_stack_push(s_status_window, true);
}

// =============================================================================
// MAIN MENU WINDOW
// =============================================================================
static Window    *s_menu_window;
static MenuLayer *s_menu_layer;

static uint16_t menu_get_num_sections(MenuLayer *ml, void *ctx) { return 1; }

static uint16_t menu_get_num_rows(MenuLayer *ml, uint16_t section, void *ctx) {
  return (uint16_t)visible_item_count();
}

static int16_t menu_get_cell_height(MenuLayer *ml, MenuIndex *idx, void *ctx) {
  return 44;
}

static int16_t menu_get_header_height(MenuLayer *ml, uint16_t section,
                                      void *ctx) {
  return 36;
}

static void menu_draw_header(GContext *ctx, const Layer *cell_layer,
                             uint16_t section, void *cb_ctx) {
  GRect bounds = layer_get_bounds(cell_layer);
  graphics_context_set_fill_color(ctx, GColorCobaltBlue);
  graphics_fill_rect(ctx, bounds, 0, GCornerNone);
  graphics_context_set_text_color(ctx, GColorWhite);
  graphics_draw_text(ctx, "BlueLink",
                     fonts_get_system_font(FONT_KEY_GOTHIC_24_BOLD),
                     GRect(0, 4, bounds.size.w, 28),
                     GTextOverflowModeTrailingEllipsis,
                     GTextAlignmentCenter, NULL);
}

static void menu_draw_row(GContext *ctx, const Layer *cell_layer,
                          MenuIndex *idx, void *cb_ctx) {
  int all_idx = visible_to_all_idx(idx->row);
  const MenuItem *item = &ALL_ITEMS[all_idx];
  bool highlighted = menu_cell_layer_is_highlighted(cell_layer);
  GRect bounds = layer_get_bounds(cell_layer);

  graphics_context_set_fill_color(ctx,
      highlighted ? GColorOxfordBlue : GColorDarkGray);
  graphics_fill_rect(ctx, bounds, 0, GCornerNone);

  graphics_context_set_text_color(ctx, GColorWhite);
  graphics_draw_text(ctx, item->title,
                     fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD),
                     GRect(8, 4, bounds.size.w - 20, 22),
                     GTextOverflowModeTrailingEllipsis,
                     GTextAlignmentLeft, NULL);

  graphics_context_set_text_color(ctx,
      highlighted ? GColorWhite : GColorLightGray);
  graphics_draw_text(ctx, item->subtitle,
                     fonts_get_system_font(FONT_KEY_GOTHIC_14),
                     GRect(8, 24, bounds.size.w - 20, 18),
                     GTextOverflowModeTrailingEllipsis,
                     GTextAlignmentLeft, NULL);
}

static void menu_select_callback(MenuLayer *ml, MenuIndex *idx, void *ctx) {
  int all_idx = visible_to_all_idx(idx->row);
  vibes_short_pulse();

  if (all_idx == IDX_STATUS) {
    status_window_push();
  } else if (all_idx == IDX_SETTINGS) {
    settings_window_push();
  } else if (ALL_ITEMS[all_idx].needs_temp) {
    temp_window_push(all_idx);
  } else if (ALL_ITEMS[all_idx].confirm) {
    confirm_window_push(all_idx);
  } else {
    send_command(ALL_ITEMS[all_idx].cmd);
  }
}

static void menu_window_load(Window *window) {
  Layer *root = window_get_root_layer(window);
  GRect bounds = layer_get_bounds(root);

  s_menu_layer = menu_layer_create(bounds);
  menu_layer_set_normal_colors(s_menu_layer,    GColorDarkGray, GColorWhite);
  menu_layer_set_highlight_colors(s_menu_layer, GColorFolly,      GColorWhite);
  menu_layer_set_callbacks(s_menu_layer, NULL, (MenuLayerCallbacks){
    .get_num_sections  = menu_get_num_sections,
    .get_num_rows      = menu_get_num_rows,
    .get_cell_height   = menu_get_cell_height,
    .get_header_height = menu_get_header_height,
    .draw_header       = menu_draw_header,
    .draw_row          = menu_draw_row,
    .select_click      = menu_select_callback,
  });
  menu_layer_set_click_config_onto_window(s_menu_layer, window);
  layer_add_child(root, menu_layer_get_layer(s_menu_layer));
}

static void menu_window_unload(Window *window) {
  menu_layer_destroy(s_menu_layer);
}

// =============================================================================
// APPMESSAGE (phone bridge)
// =============================================================================
static void send_command(const char *cmd) {
  DictionaryIterator *out;
  AppMessageResult result = app_message_outbox_begin(&out);
  if (result != APP_MSG_OK) {
    APP_LOG(APP_LOG_LEVEL_ERROR, "outbox_begin failed: %d", (int)result);
    feedback_window_push(false, "Send failed");
    return;
  }
  dict_write_cstring(out, KEY_CMD, cmd);
  app_message_outbox_send();
}

static void send_start_with_temp(int temp_f) {
  DictionaryIterator *out;
  AppMessageResult result = app_message_outbox_begin(&out);
  if (result != APP_MSG_OK) {
    APP_LOG(APP_LOG_LEVEL_ERROR, "outbox_begin failed: %d", (int)result);
    feedback_window_push(false, "Send failed");
    return;
  }
  dict_write_cstring(out, KEY_CMD,          "start");
  dict_write_int(out,     KEY_CLIMATE_TEMP, &temp_f, sizeof(int), true);
  app_message_outbox_send();
}

static void request_status(void) {
  send_command("status");
}

static void inbox_received(DictionaryIterator *iter, void *ctx) {
  Tuple *locked_t     = dict_find(iter, KEY_LOCKED);
  Tuple *range_t      = dict_find(iter, KEY_RANGE);
  Tuple *charging_t   = dict_find(iter, KEY_CHARGING);
  Tuple *chargetime_t = dict_find(iter, KEY_CHARGE_TIME);
  Tuple *twelve_t     = dict_find(iter, KEY_TWELVE_SOC);

  if (locked_t) {
    s_locked           = (locked_t->value->int32 == 1);
    s_range            = range_t      ? (int)range_t->value->int32      : 0;
    s_charging         = charging_t   ? (charging_t->value->int32 == 1) : false;
    s_charge_time_mins = chargetime_t ? (int)chargetime_t->value->int32 : 0;
    s_twelve_soc       = twelve_t     ? (int)twelve_t->value->int32     : 0;
    s_status_loaded    = true;
    status_update_display();
    return;
  }

  Tuple *result_t = dict_find(iter, KEY_RESULT);
  Tuple *msg_t    = dict_find(iter, KEY_MSG);
  if (result_t) {
    bool ok = (strcmp(result_t->value->cstring, "ok") == 0);
    const char *msg = msg_t ? msg_t->value->cstring
                            : (ok ? "Done!" : "Failed");
    feedback_window_push(ok, msg);
  }
}

static void inbox_dropped(AppMessageResult reason, void *ctx) {
  APP_LOG(APP_LOG_LEVEL_WARNING, "Inbox dropped: %d", (int)reason);
}

static void outbox_failed(DictionaryIterator *iter, AppMessageResult reason,
                          void *ctx) {
  APP_LOG(APP_LOG_LEVEL_ERROR, "Outbox failed: %d", (int)reason);
  feedback_window_push(false, "No phone");
}

// =============================================================================
// APP LIFECYCLE
// =============================================================================
static void init(void) {
  // Load persisted settings (defaults: all on, Fahrenheit, miles)
  s_ev_enabled     = persist_exists(PERSIST_EV_ENABLED)
                     ? persist_read_bool(PERSIST_EV_ENABLED)     : true;
  s_12v_enabled    = persist_exists(PERSIST_12V_ENABLED)
                     ? persist_read_bool(PERSIST_12V_ENABLED)    : true;
  s_use_fahrenheit = persist_exists(PERSIST_USE_FAHRENHEIT)
                     ? persist_read_bool(PERSIST_USE_FAHRENHEIT) : true;
  s_use_miles      = persist_exists(PERSIST_USE_MILES)
                     ? persist_read_bool(PERSIST_USE_MILES)      : true;

  app_message_open(320, 320);
  app_message_register_inbox_received(inbox_received);
  app_message_register_inbox_dropped(inbox_dropped);
  app_message_register_outbox_failed(outbox_failed);

  s_menu_window = window_create();
  window_set_background_color(s_menu_window, GColorBlack);
  window_set_window_handlers(s_menu_window, (WindowHandlers){
    .load   = menu_window_load,
    .unload = menu_window_unload,
  });
  window_stack_push(s_menu_window, true);
}

static void deinit(void) {
  app_message_deregister_callbacks();
  window_destroy(s_menu_window);
}

int main(void) {
  init();
  app_event_loop();
  deinit();
}