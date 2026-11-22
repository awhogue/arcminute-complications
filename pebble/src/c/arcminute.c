#include <pebble.h>
#include "../../build/include/message_keys.auto.h"

// PDC (Pebble Draw Command) structures
typedef struct
{
  int16_t x;
  int16_t y;
} __attribute__((packed)) Point;

typedef struct
{
  uint8_t type;
  uint8_t flags;
  uint8_t stroke_color;
  uint8_t stroke_width;
  uint8_t fill_color;
  uint16_t path_open_radius;
  uint16_t num_points;
  Point points[];
} __attribute__((packed)) PebbleDrawCommand;

typedef struct
{
  uint16_t num_commands;
  PebbleDrawCommand commands[];
} __attribute__((packed)) PebbleDrawCommandList;

typedef struct
{
  uint16_t width;
  uint16_t height;
} __attribute__((packed)) ViewBox;

typedef struct
{
  uint8_t version;
  uint8_t reserved;
  ViewBox view_box;
  PebbleDrawCommandList command_list;
} __attribute__((packed)) PebbleDrawCommandImage;

typedef struct
{
  char magic[4];
  uint32_t image_size;
  PebbleDrawCommandImage image;
} __attribute__((packed)) PebbleDrawCommandImageFile;

// Function to create a simple PDC image in memory
PebbleDrawCommandImageFile *create_pdc_image(uint16_t width, uint16_t height, uint16_t num_commands)
{
  size_t total_size = sizeof(PebbleDrawCommandImageFile) + (num_commands * sizeof(PebbleDrawCommand));
  PebbleDrawCommandImageFile *pdc = malloc(total_size);
  if (!pdc)
    return NULL;

  memcpy(pdc->magic, "PDCI", 4);
  pdc->image_size = total_size - 8;
  pdc->image.version = 1;
  pdc->image.reserved = 0;
  pdc->image.view_box.width = width;
  pdc->image.view_box.height = height;
  pdc->image.command_list.num_commands = num_commands;

  return pdc;
}

// Function to add points to a PDC draw command
bool add_points_to_command(PebbleDrawCommand *command, const Point *points, uint16_t num_points)
{
  if (!command || !points || num_points == 0)
    return false;

  command->num_points = num_points;
  memcpy(command->points, points, num_points * sizeof(Point));

  return true;
}

static Window *s_main_window;
static Layer *s_face_layer;
static Layer *s_hand_layer;
static TextLayer *s_battery_layer;
static bool debug = false;
static float s_scale = 1.0f;
static float s_mark_scale = 1.0f;
static float s_stroke_scale = 1.0f;
static int16_t s_orbit_inset = 150;
static GColor s_background_color;
static GColor s_face_color;
static GColor s_hand_color;
static bool s_thick_hand = false;
static bool s_face_clear = true;
static int s_dial_style = 0;
static GFont s_large_numeral_font;

// ---- Complications ----
#define COMP_FLANK_DEG 38
#define COMP_FLANK_R 100
#define PERSIST_COMP_MODE 11
#define PERSIST_TEMP_UNIT 12
#define PERSIST_TEMP_TENTHS_C 13
#define PERSIST_TEMP_TIME 14
#define COMP_SHOW_MS 5000
#define WEATHER_MAX_AGE_S (30 * 60)

static Layer *s_comp_layer;
static AppTimer *s_comp_timer;
static bool s_comp_visible = false;
static int s_comp_mode = 0;   // 0 = show on shake/tap, 1 = always
static int s_temp_unit = 0;   // 0 = Fahrenheit, 1 = Celsius
static bool s_temp_known = false;
static int s_temp_tenths_c = 0;
static time_t s_temp_time = 0;

static GColor background_color() {
  return s_background_color;
}

// Scale a gabbro-reference stroke/dot width down for smaller screens, never below 1px.
static uint8_t scaled_stroke(int16_t gabbro_px) {
  int16_t w = (int16_t)(gabbro_px * s_stroke_scale + 0.5f);
  return w < 1 ? 1 : (uint8_t)w;
}

static GColor face_color() {
  return s_face_clear ? s_background_color : s_face_color;
}

static bool color_is_light(GColor c) {
#if defined(PBL_COLOR)
  return ((int)c.r + (int)c.g + (int)c.b) >= 5;
#else
  return gcolor_equal(c, GColorWhite);
#endif
}

static bool face_is_light() {
  return color_is_light(face_color());
}

static GColor face_text_color() {
  return face_is_light() ? GColorBlack : GColorWhite;
}

static GColor face_minor_tick_color() {
  return face_is_light() ? GColorDarkGray : GColorLightGray;
}

static GColor hand_color() {
#if defined(PBL_COLOR)
  return s_hand_color;
#else
  return face_is_light() ? GColorBlack : GColorWhite;
#endif
}
// static int font_size_index = 1; // 0=large, 1=medium, 2=small, 3=xsmall (commented out)

static void inbox_received_callback(DictionaryIterator *iterator, void *context);
static void inbox_dropped_callback(AppMessageResult reason, void *context);
static void outbox_failed_callback(DictionaryIterator *iterator, AppMessageResult reason, void *context);
static void outbox_sent_callback(DictionaryIterator *iterator, void *context);
// Font size setting commented out - not working correctly
// static GFont get_font_for_index(int index);

// static GFont get_font_for_index(int index)
// {
//   switch (index)
//   {
//   case 0:
//     return fonts_get_system_font(FONT_KEY_BITHAM_34_MEDIUM_NUMBERS);
//   case 1:
//     return fonts_get_system_font(FONT_KEY_GOTHIC_28_BOLD);
//   case 2:
//     return fonts_get_system_font(FONT_KEY_GOTHIC_28_BOLD);
//   case 3:
//     return fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD);
//   default:
//     return fonts_get_system_font(FONT_KEY_GOTHIC_28_BOLD);
//   }
// }

static void update_time()
{
  // Get a tm structure
  time_t temp = time(NULL);
  struct tm *tick_time = localtime(&temp);

  // Write the current hours and minutes into a buffer
  static char s_buffer[8];
  strftime(s_buffer, sizeof(s_buffer), clock_is_24h_style() ? "%H:%M" : "%I:%M", tick_time);
}
static void battery_handler(BatteryChargeState charge_state)
{
  static char s_battery_buffer[16];

  if (charge_state.is_charging)
  {
    snprintf(s_battery_buffer, sizeof(s_battery_buffer), "%d", charge_state.charge_percent);
  }
  else
  {
    snprintf(s_battery_buffer, sizeof(s_battery_buffer), "%d", charge_state.charge_percent);
  }
  text_layer_set_text(s_battery_layer, s_battery_buffer);
}

static float current_hand_angle()
{
  time_t temp = time(NULL);
  struct tm *t = localtime(&temp);
  if (debug)
    return 12 * t->tm_sec;
  return 30 * ((float)(t->tm_hour % 12) + ((float)t->tm_min / 60));
}

static void update_frame_location()
{
  time_t temp = time(NULL);
  struct tm *tick_time = localtime(&temp);
  int bhour = tick_time->tm_hour;
  int bmin = tick_time->tm_min;
  float angle = 30 * ((float)(bhour % 12) + ((float)bmin / 60));
  if (debug)
    angle = 12 * tick_time->tm_sec;

  GRect frame = layer_get_frame(s_face_layer);
  GRect frame2 = layer_get_frame(s_hand_layer);

  GPoint origin = gpoint_from_polar(grect_inset(frame2, GEdgeInsets(-s_orbit_inset)), GOvalScaleModeFitCircle, DEG_TO_TRIGANGLE(angle + 180));
  frame.origin = origin;
  frame.origin.x -= frame.size.w / 2;
  frame.origin.y -= frame.size.h / 2;

  layer_set_frame(s_face_layer, frame);
}

static void tick_handler(struct tm *tick_time, TimeUnits units_changed)
{
  update_time();
  layer_mark_dirty(s_hand_layer);
  update_frame_location();
  layer_mark_dirty(s_comp_layer);
}

static void my_hand_draw(Layer *layer, GContext *ctx)
{
  // GRect bounds = layer_get_bounds(layer);
  GRect face_frame = layer_get_frame(s_face_layer);

  time_t temp = time(NULL);
  struct tm *tick_time = localtime(&temp);
  int bhour = tick_time->tm_hour;
  int bmin = tick_time->tm_min;

  float angle = 30 * ((float)(bhour % 12) + ((float)bmin / 60));
  if (debug)
    angle = 12 * tick_time->tm_sec;

  GPoint center = GPoint(face_frame.origin.x + face_frame.size.w / 2, face_frame.origin.y + face_frame.size.h / 2);
  GPoint end_point = gpoint_from_polar(face_frame, GOvalScaleModeFitCircle, DEG_TO_TRIGANGLE(angle));

  uint8_t stroke_width =
#if PBL_DISPLAY_WIDTH == 260
      9;
#elif PBL_DISPLAY_WIDTH == 200
      6;
#else
      5;
#endif

  if (s_thick_hand)
    stroke_width += 1;

  graphics_context_set_stroke_color(ctx, hand_color());
  graphics_context_set_stroke_width(ctx, stroke_width);
  graphics_draw_line(ctx, center, end_point);
}

static void my_face_draw(Layer *layer, GContext *ctx)
{
  GRect bounds = layer_get_bounds(layer);
  const int16_t half_h = bounds.size.h / 2;
  const bool large_numerals = s_dial_style == 1;
  const int16_t hour_inset = large_numerals
      ? (int16_t)(40 * s_mark_scale)
      : (int16_t)(20 * s_scale);
  const int16_t ascender = large_numerals
#if PBL_DISPLAY_WIDTH >= 200
      ? 24
#else
      ? 17
#endif
      : (int16_t)(8 * s_scale);

  graphics_context_set_antialiased(ctx, false);

  // Fill the face disk (omitted when face is transparent), 4px beyond the marker ring
  if (!s_face_clear) {
    graphics_context_set_fill_color(ctx, face_color());
    graphics_fill_circle(ctx, GPoint(half_h, half_h), bounds.size.w / 2 + 8);
  }

  graphics_context_set_stroke_color(ctx, face_text_color());
  graphics_context_set_stroke_width(ctx, 2);
  graphics_context_set_text_color(ctx, face_text_color());

  // In 24h mode each tick shows the absolute hour (0-23) nearest the current
  // time, so the visible arc reads 13-23 in the afternoon and combos across
  // midnight (...22, 23, 0, 1...) as the dial rolls over. Midnight is 0, not
  // 24, matching how a 24h clock is actually written (per ericmigi, #4).
  const bool h24 = clock_is_24h_style();
  time_t face_now = time(NULL);
  struct tm *face_tm = localtime(&face_now);
  const float cur_hour = face_tm->tm_hour + face_tm->tm_min / 60.0f;

  for (int i = 0; i < 12; i++)
  {
    int angle = DEG_TO_TRIGANGLE(i * 30);

    int label;
    if (h24) {
      float laps = (cur_hour - i) / 12.0f;
      int nearest = i + 12 * (int)(laps + (laps >= 0 ? 0.5f : -0.5f));
      label = ((nearest % 24) + 24) % 24;
    } else {
      label = i == 0 ? 12 : i;
    }

    static char buf[] = "000";
    snprintf(buf, sizeof(buf), "%d", label);
    GFont number_font = large_numerals
        ? s_large_numeral_font
        : fonts_get_system_font(
#if PBL_DISPLAY_WIDTH == 260
              FONT_KEY_BITHAM_42_LIGHT
#else
              FONT_KEY_BITHAM_34_MEDIUM_NUMBERS
#endif
          );
    GRect meas_rect = GRect(0, 0, bounds.size.w, bounds.size.h);
    GSize size = graphics_text_layout_get_content_size(buf, number_font, meas_rect,
                                                       GTextOverflowModeFill, GTextAlignmentCenter);

    // Font metric dead-space above the visual digit area
    const int16_t box_top_offset = large_numerals ? (size.h * 27 / 100) : ascender;
    const int16_t box_h = size.h - box_top_offset;

    // Radial unit vector (outward from center, toward tick)
    int32_t rdx = sin_lookup(angle);
    int32_t rdy = -cos_lookup(angle);
    int32_t abs_rdx = rdx < 0 ? -rdx : rdx;
    int32_t abs_rdy = rdy < 0 ? -rdy : rdy;

    // Distance from box center to nearest box edge along the radial direction
    int32_t hw = size.w / 2;
    int32_t hh = box_h / 2;
    int32_t t_x = abs_rdx > 0 ? (hw * TRIG_MAX_RATIO / abs_rdx) : INT16_MAX;
    int32_t t_y = abs_rdy > 0 ? (hh * TRIG_MAX_RATIO / abs_rdy) : INT16_MAX;
    int32_t d_edge = t_x < t_y ? t_x : t_y;

    const int16_t gap = hour_inset / 2;
    GPoint tick_inner = gpoint_from_polar(grect_crop(bounds, hour_inset), GOvalScaleModeFitCircle, angle);
    GPoint box_center = GPoint(
        tick_inner.x - (int16_t)((gap + d_edge) * rdx / TRIG_MAX_RATIO),
        tick_inner.y - (int16_t)((gap + d_edge) * rdy / TRIG_MAX_RATIO));

    GRect text_rect = GRect(
        box_center.x - size.w / 2,
        box_center.y - box_h / 2 - box_top_offset,
        size.w, size.h);

    graphics_draw_text(ctx, buf, number_font,
#if PBL_DISPLAY_WIDTH == 260
                       text_rect,
#else
                       large_numerals ? text_rect : grect_inset(text_rect, GEdgeInsets4(-8, 0, 0, 0)),
#endif
                       GTextOverflowModeFill, GTextAlignmentCenter, NULL);

#ifdef DEBUG_BOXES
    graphics_context_set_stroke_width(ctx, 1);
    graphics_draw_rect(ctx, GRect(box_center.x - size.w / 2, box_center.y - box_h / 2, size.w, box_h));
#endif

    graphics_context_set_stroke_color(ctx, face_text_color());
    graphics_context_set_stroke_width(ctx, large_numerals ? scaled_stroke(6)
#if PBL_DISPLAY_WIDTH == 260
                                                          : 3);
#else
                                                          : 2);
#endif
    graphics_draw_line(ctx,
                       gpoint_from_polar(grect_crop(bounds, hour_inset), GOvalScaleModeFitCircle, angle),
                       gpoint_from_polar(bounds, GOvalScaleModeFitCircle, angle));

    for (int j = 1; j < 12; j++)
    {
      GColor line_color = face_minor_tick_color();

      angle += DEG_TO_TRIGANGLE(2.5);
      graphics_context_set_stroke_color(ctx, line_color);
      if (j % 3 == 0) {
        const bool half_hour = j == 6;
        const int16_t tick_inset = half_hour ? hour_inset : hour_inset / 2;
        graphics_context_set_stroke_width(ctx, large_numerals ? scaled_stroke(4) : 2);
        graphics_draw_line(ctx,
                           gpoint_from_polar(grect_crop(bounds, tick_inset), GOvalScaleModeFitCircle, angle),
                           gpoint_from_polar(bounds, GOvalScaleModeFitCircle, angle));
      } else {
        graphics_context_set_fill_color(ctx, line_color);
        GPoint dot = gpoint_from_polar(bounds, GOvalScaleModeFitCircle, angle);
        graphics_fill_circle(ctx, dot, large_numerals ? scaled_stroke(3) : 2);
      }
    }
  }
}

// ---------------------------------------------------------------------------
// Complications: date, steps and temperature, positioned relative to the hand.
// The hand always lies on the line through the screen centre (the dial centre
// orbits the screen centre directly opposite the hand), so every label can be
// placed in polar coordinates around the screen centre using only the hand angle.
// ---------------------------------------------------------------------------

static void format_steps(char *buf, size_t len)
{
#if defined(PBL_HEALTH)
  HealthServiceAccessibilityMask mask = health_service_metric_accessible(HealthMetricStepCount, time_start_of_today(), time(NULL));
  if (mask & HealthServiceAccessibilityMaskAvailable)
  {
    int steps = (int)health_service_sum_today(HealthMetricStepCount);
    if (steps < 1000)
    {
      snprintf(buf, len, "%d", steps);
    }
    else
    {
      int tenths = steps / 100; // truncate: 1449 -> 14 -> "1.4k"
      snprintf(buf, len, "%d.%dk", tenths / 10, tenths % 10);
    }
    return;
  }
#endif
  snprintf(buf, len, "--");
}

static void format_date(char *buf, size_t len)
{
  time_t now = time(NULL);
  struct tm *t = localtime(&now);
  char wday[8];
  strftime(wday, sizeof(wday), "%a", t);
  snprintf(buf, len, "%s\n%d", wday, t->tm_mday);
}

static void format_temp(char *buf, size_t len)
{
  if (!s_temp_known)
  {
    snprintf(buf, len, "--°");
    return;
  }
  int v10 = s_temp_unit == 1 ? s_temp_tenths_c : (s_temp_tenths_c * 9 / 5 + 320);
  int v = (v10 + (v10 >= 0 ? 5 : -5)) / 10;
  snprintf(buf, len, "%d°", v);
}

static void request_weather()
{
  DictionaryIterator *iter;
  if (app_message_outbox_begin(&iter) != APP_MSG_OK)
    return;
  dict_write_uint8(iter, MESSAGE_KEY_WEATHER_REQUEST, 1);
  app_message_outbox_send();
}

static void draw_label(GContext *ctx, GRect bounds, float angle_deg, int16_t radius,
                       const char *text, GFont font, GColor bg)
{
  while (angle_deg < 0)
    angle_deg += 360;
  while (angle_deg >= 360)
    angle_deg -= 360;

  GRect ring = grect_inset(bounds, GEdgeInsets(bounds.size.w / 2 - radius));
  GPoint c = gpoint_from_polar(ring, GOvalScaleModeFitCircle, DEG_TO_TRIGANGLE(angle_deg));

  // Split on '\n' into up to two lines, drawn explicitly.
  char lines[2][16];
  int nlines = 1;
  const char *nl = strchr(text, '\n');
  if (nl)
  {
    size_t l0 = (size_t)(nl - text);
    if (l0 >= sizeof(lines[0])) l0 = sizeof(lines[0]) - 1;
    memcpy(lines[0], text, l0);
    lines[0][l0] = 0;
    strncpy(lines[1], nl + 1, sizeof(lines[1]) - 1);
    lines[1][sizeof(lines[1]) - 1] = 0;
    nlines = 2;
  }
  else
  {
    strncpy(lines[0], text, sizeof(lines[0]) - 1);
    lines[0][sizeof(lines[0]) - 1] = 0;
  }

  const int16_t top_pad = 8; // Gothic metric dead-space above the glyphs
  const int16_t pad = 3;
  int16_t text_w = 0, line_h = 0;
  for (int i = 0; i < nlines; i++)
  {
    GSize sz = graphics_text_layout_get_content_size(lines[i], font, GRect(0, 0, bounds.size.w, 40),
                                                     GTextOverflowModeFill, GTextAlignmentCenter);
    if (sz.w > text_w) text_w = sz.w;
    line_h = sz.h;
  }
  const int16_t pitch = line_h - top_pad + 3;
  GRect box = GRect(c.x - text_w / 2 - pad, c.y - (nlines * pitch) / 2 - 2, text_w + 2 * pad, nlines * pitch + 4);

  // keep the label on screen
  if (box.origin.x < 0) box.origin.x = 0;
  if (box.origin.y < 0) box.origin.y = 0;
  if (box.origin.x + box.size.w > bounds.size.w) box.origin.x = bounds.size.w - box.size.w;
  if (box.origin.y + box.size.h > bounds.size.h) box.origin.y = bounds.size.h - box.size.h;

  // keep the farthest corner inside the round screen: nudge the box toward the centre
  {
    const int16_t cx = bounds.size.w / 2, cy = bounds.size.h / 2;
    const int16_t max_r = cx - 3;
    for (int i = 0; i < 12; i++)
    {
      int16_t fx = box.origin.x + box.size.w / 2 < cx ? box.origin.x : box.origin.x + box.size.w;
      int16_t fy = box.origin.y + box.size.h / 2 < cy ? box.origin.y : box.origin.y + box.size.h;
      int32_t d2 = (fx - cx) * (fx - cx) + (fy - cy) * (fy - cy);
      if (d2 <= (int32_t)max_r * max_r)
        break;
      box.origin.x += (box.origin.x + box.size.w / 2 < cx) ? 1 : -1;
      box.origin.y += (box.origin.y + box.size.h / 2 < cy) ? 1 : -1;
    }
  }

  graphics_context_set_fill_color(ctx, bg);
  graphics_fill_rect(ctx, box, 4, GCornersAll);
  graphics_context_set_text_color(ctx, color_is_light(bg) ? GColorBlack : GColorWhite);
  for (int i = 0; i < nlines; i++)
  {
    graphics_draw_text(ctx, lines[i], font,
                       GRect(box.origin.x, box.origin.y - top_pad + 2 + i * pitch, box.size.w, line_h),
                       GTextOverflowModeFill, GTextAlignmentCenter, NULL);
  }
}

static void comp_draw(Layer *layer, GContext *ctx)
{
  GRect bounds = layer_get_bounds(layer);
  GFont font = fonts_get_system_font(FONT_KEY_GOTHIC_28_BOLD);
  float theta = current_hand_angle();

  // The hand tip sits (dial radius - orbit radius) from the screen centre.
  GRect face_frame = layer_get_frame(s_face_layer);
  int16_t tip_r = face_frame.size.w / 2 - (bounds.size.w / 2 + s_orbit_inset);
  int16_t half = bounds.size.w / 2;
  int16_t temp_r = (tip_r + half) / 2;

  char date[24], steps[16], temp[16];
  format_date(date, sizeof(date));
  format_steps(steps, sizeof(steps));
  format_temp(temp, sizeof(temp));

  // Temp: beyond the hand tip, outside the dial.
  draw_label(ctx, bounds, theta, temp_r, temp, font, background_color());
  // Date / steps: flanking the empty wedge opposite the hand, inside the dial.
  draw_label(ctx, bounds, theta + 180 - COMP_FLANK_DEG, COMP_FLANK_R, date, font, face_color());
  draw_label(ctx, bounds, theta + 180 + COMP_FLANK_DEG, COMP_FLANK_R, steps, font, face_color());
}

static void comp_update_visibility()
{
  layer_set_hidden(s_comp_layer, !(s_comp_mode == 1 || s_comp_visible));
}

static void comp_hide_cb(void *data)
{
  s_comp_timer = NULL;
  s_comp_visible = false;
  comp_update_visibility();
}

static void tap_handler(AccelAxisType axis, int32_t direction)
{
  if (s_comp_mode == 1)
    return;
  if (s_comp_timer)
    app_timer_cancel(s_comp_timer);
  s_comp_timer = app_timer_register(COMP_SHOW_MS, comp_hide_cb, NULL);
  s_comp_visible = true;
  layer_mark_dirty(s_comp_layer);
  comp_update_visibility();
  if (!s_temp_known || time(NULL) - s_temp_time > WEATHER_MAX_AGE_S)
    request_weather();
}

static void comp_apply_mode()
{
  accel_tap_service_unsubscribe();
  if (s_comp_mode == 0)
    accel_tap_service_subscribe(tap_handler);
  comp_update_visibility();
  layer_mark_dirty(s_comp_layer);
}

static void main_window_load(Window *window)
{
  Layer *window_layer = window_get_root_layer(window);
  GRect bounds = layer_get_bounds(window_layer);

  int16_t screen_size = bounds.size.w < bounds.size.h ? bounds.size.w : bounds.size.h;
  s_scale = 180.0f / (float)screen_size;
  // Marks and stroke widths scale proportionally with the screen; gabbro (260px) is the reference platform.
  s_mark_scale = 180.0f * (float)screen_size / (260.0f * 260.0f);
  s_stroke_scale = (float)screen_size / 260.0f;
  s_orbit_inset = (int16_t)(PBL_IF_ROUND_ELSE(130.0f, 150.0f) * (float)screen_size / 180.0f);

  s_face_layer = layer_create(GRect(0, 0, bounds.size.h * 3, bounds.size.h * 3));
  layer_set_update_proc(s_face_layer, my_face_draw);
  layer_set_clips(s_face_layer, false);

  s_hand_layer = layer_create(bounds);
  layer_set_update_proc(s_hand_layer, my_hand_draw);

  // Create the TextLayer with specific bounds
  s_battery_layer = text_layer_create(
      GRect(0, PBL_IF_ROUND_ELSE(58, 52), bounds.size.w, 50));

  // Improve the layout to be more like a watchface
  // text_layer_set_background_color(s_battery_layer, GColorClear);
  // text_layer_set_text_color(s_battery_layer, GColorDarkGray);
  // text_layer_set_text(s_battery_layer, "50");
  // text_layer_set_font(s_battery_layer, fonts_get_system_font(FONT_KEY_GOTHIC_28_BOLD));
  // text_layer_set_text_alignment(s_battery_layer, GTextAlignmentCenter);

  s_comp_layer = layer_create(bounds);
  layer_set_update_proc(s_comp_layer, comp_draw);

  update_frame_location();

  // Face below, hand on top so the hand renders over the dial and marks
  layer_add_child(window_layer, s_face_layer);
  layer_add_child(window_layer, s_hand_layer);
  layer_add_child(window_layer, s_comp_layer);
  comp_apply_mode();
}

static void inbox_received_callback(DictionaryIterator *iterator, void *context)
{
  Tuple *bg_tuple = dict_find(iterator, MESSAGE_KEY_BG_HEX);
  if (bg_tuple)
  {
    int32_t hex = bg_tuple->value->int32;
    s_background_color = GColorFromHEX(hex);
    persist_write_int(5, hex);
  }

  Tuple *face_hex_tuple = dict_find(iterator, MESSAGE_KEY_FACE_HEX);
  if (face_hex_tuple)
  {
    int32_t hex = face_hex_tuple->value->int32;
    s_face_color = GColorFromHEX(hex);
    persist_write_int(6, hex);
  }

  Tuple *hand_hex_tuple = dict_find(iterator, MESSAGE_KEY_HAND_HEX);
  if (hand_hex_tuple)
  {
    int32_t hex = hand_hex_tuple->value->int32;
    s_hand_color = GColorFromHEX(hex);
    persist_write_int(7, hex);
  }

  Tuple *dial_style_tuple = dict_find(iterator, MESSAGE_KEY_DIAL_STYLE);
  if (dial_style_tuple)
  {
    s_dial_style = dial_style_tuple->value->int32;
    persist_write_int(9, s_dial_style);
  }

  Tuple *face_clear_tuple = dict_find(iterator, MESSAGE_KEY_FACE_CLEAR);
  if (face_clear_tuple)
  {
    s_face_clear = face_clear_tuple->value->int32 != 0;
    persist_write_bool(8, s_face_clear);
  }

  Tuple *thick_hand_tuple = dict_find(iterator, MESSAGE_KEY_THICK_HAND);
  if (thick_hand_tuple)
  {
    s_thick_hand = thick_hand_tuple->value->int32 != 0;
    persist_write_bool(10, s_thick_hand);
  }

  Tuple *comp_mode_tuple = dict_find(iterator, MESSAGE_KEY_COMP_MODE);
  if (comp_mode_tuple)
  {
    s_comp_mode = comp_mode_tuple->value->int32;
    persist_write_int(PERSIST_COMP_MODE, s_comp_mode);
    comp_apply_mode();
  }

  Tuple *temp_unit_tuple = dict_find(iterator, MESSAGE_KEY_TEMP_UNIT);
  if (temp_unit_tuple)
  {
    s_temp_unit = temp_unit_tuple->value->int32;
    persist_write_int(PERSIST_TEMP_UNIT, s_temp_unit);
  }

  Tuple *temp_value_tuple = dict_find(iterator, MESSAGE_KEY_TEMP_VALUE);
  if (temp_value_tuple)
  {
    s_temp_tenths_c = temp_value_tuple->value->int32;
    s_temp_known = true;
    s_temp_time = time(NULL);
    persist_write_int(PERSIST_TEMP_TENTHS_C, s_temp_tenths_c);
    persist_write_int(PERSIST_TEMP_TIME, (int32_t)s_temp_time);
  }

  window_set_background_color(s_main_window, background_color());
  layer_mark_dirty(s_face_layer);
  layer_mark_dirty(s_comp_layer);
  layer_mark_dirty(s_hand_layer);

  // Font size setting commented out - not working correctly
  // Tuple *font_size_tuple = dict_find(iterator, MESSAGE_KEY_FONT_SIZE);
  // if (font_size_tuple)
  // {
  //   font_size_index = font_size_tuple->value->int32;
  //   if (font_size_index < 0 || font_size_index > 3)
  //   {
  //     font_size_index = 1; // default to medium
  //   }
  //   persist_write_int(1, font_size_index);
  //   APP_LOG(APP_LOG_LEVEL_INFO, "Font size changed to index: %d", font_size_index);
  //   layer_mark_dirty(s_face_layer);
  // }
}

static void inbox_dropped_callback(AppMessageResult reason, void *context)
{
  APP_LOG(APP_LOG_LEVEL_ERROR, "Message dropped!");
}

static void outbox_failed_callback(DictionaryIterator *iterator, AppMessageResult reason, void *context)
{
  APP_LOG(APP_LOG_LEVEL_ERROR, "Outbox send failed!");
}

static void outbox_sent_callback(DictionaryIterator *iterator, void *context)
{
  APP_LOG(APP_LOG_LEVEL_INFO, "Outbox send success!");
}

static void main_window_unload(Window *window)
{
  // Destroy TextLayer
  layer_destroy(s_comp_layer);
  layer_destroy(s_hand_layer);
  layer_destroy(s_face_layer);
}

static void init()
{
  s_background_color = GColorFromHEX(persist_exists(5) ? persist_read_int(5) : 0x000000);
  s_face_color = GColorFromHEX(persist_exists(6) ? persist_read_int(6) : 0x000000);
  s_hand_color = GColorFromHEX(persist_exists(7) ? persist_read_int(7) : 0xFF0000);
  s_face_clear = persist_exists(8) ? persist_read_bool(8) : true;
  s_dial_style = persist_exists(9) ? persist_read_int(9) : 1;
  s_thick_hand = persist_exists(10) ? persist_read_bool(10) : false;
  s_comp_mode = persist_exists(PERSIST_COMP_MODE) ? persist_read_int(PERSIST_COMP_MODE) : 0;
  s_temp_unit = persist_exists(PERSIST_TEMP_UNIT) ? persist_read_int(PERSIST_TEMP_UNIT) : 0;
  if (persist_exists(PERSIST_TEMP_TENTHS_C))
  {
    s_temp_known = true;
    s_temp_tenths_c = persist_read_int(PERSIST_TEMP_TENTHS_C);
    s_temp_time = persist_exists(PERSIST_TEMP_TIME) ? (time_t)persist_read_int(PERSIST_TEMP_TIME) : 0;
  }

  s_large_numeral_font = fonts_load_custom_font(resource_get_handle(
#if PBL_DISPLAY_WIDTH >= 200
      RESOURCE_ID_FONT_HELVETICA_95
#else
      RESOURCE_ID_FONT_HELVETICA_67
#endif
      ));
  // font_size_index = persist_exists(1) ? persist_read_int(1) : 1; // default to medium (commented out)

  // Create main Window element and assign to pointer
  s_main_window = window_create();
  window_set_background_color(s_main_window, background_color());

  // Set handlers to manage the elements inside the Window
  window_set_window_handlers(s_main_window, (WindowHandlers){
                                                .load = main_window_load,
                                                .unload = main_window_unload});

  // Show the Window on the watch, with animated=true
  window_stack_push(s_main_window, true);

  // Make sure the time is displayed from the start
  update_time();

  // Register with TickTimerService
  tick_timer_service_subscribe(MINUTE_UNIT, tick_handler);
  battery_state_service_subscribe(battery_handler);

  // Register callbacks for AppMessage
  app_message_register_inbox_received(inbox_received_callback);
  app_message_register_inbox_dropped(inbox_dropped_callback);
  app_message_register_outbox_failed(outbox_failed_callback);
  app_message_register_outbox_sent(outbox_sent_callback);

  // Open AppMessage with reasonable buffer sizes
  app_message_open(app_message_inbox_size_maximum(), app_message_outbox_size_maximum());
}

static void deinit()
{
  accel_tap_service_unsubscribe();
  if (s_comp_timer)
    app_timer_cancel(s_comp_timer);
  fonts_unload_custom_font(s_large_numeral_font);
  window_destroy(s_main_window);
}

int main(void)
{
  init();
  app_event_loop();
  deinit();
}