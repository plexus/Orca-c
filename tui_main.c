#include "bank.h"
#include "base.h"
#include "field.h"
#include "gbuffer.h"
#include "mark.h"
#include "sim.h"
#include <getopt.h>
#include <locale.h>
#include <ncurses.h>

#define SOKOL_IMPL
#include "sokol_time.h"
#undef SOKOL_IMPL

#define AND_CTRL(c) ((c)&037)

static void usage() {
  // clang-format off
  fprintf(stderr,
      "Usage: orca [options] [file]\n\n"
      "Options:\n"
      "    --margins <number> Set cosmetic margins.\n"
      "                       Default: 2\n"
      "    -h or --help       Print this message and exit.\n"
      );
  // clang-format on
}

typedef enum {
  Tui_input_mode_normal = 0,
  Tui_input_mode_append = 1,
  Tui_input_mode_piano = 2,
} Tui_input_mode;

typedef enum {
  C_natural,
  C_black,
  C_red,
  C_green,
  C_yellow,
  C_blue,
  C_magenta,
  C_cyan,
  C_white,
} Color_name;

enum {
  Colors_count = C_white + 1,
};

enum {
  Cdef_normal = COLOR_PAIR(1),
};

typedef enum {
  A_normal = A_NORMAL,
  A_bold = A_BOLD,
  A_dim = A_DIM,
  A_standout = A_STANDOUT,
  A_reverse = A_REVERSE,
} Term_attr;

ORCA_FORCE_INLINE
int fg_bg(Color_name fg, Color_name bg) {
  return COLOR_PAIR(1 + fg * Colors_count + bg);
}

typedef enum {
  Glyph_class_unknown,
  Glyph_class_grid,
  Glyph_class_comment,
  Glyph_class_uppercase,
  Glyph_class_lowercase,
  Glyph_class_movement,
  Glyph_class_numeric,
  Glyph_class_bang,
} Glyph_class;

static Glyph_class glyph_class_of(Glyph glyph) {
  if (glyph == '.')
    return Glyph_class_grid;
  if (glyph >= '0' && glyph <= '9')
    return Glyph_class_numeric;
  switch (glyph) {
  case 'N':
  case 'n':
  case 'E':
  case 'e':
  case 'S':
  case 's':
  case 'W':
  case 'w':
  case 'Z':
  case 'z':
    return Glyph_class_movement;
  case '!':
  case ':':
    return Glyph_class_lowercase;
  case '*':
    return Glyph_class_bang;
  case '#':
    return Glyph_class_comment;
  }
  if (glyph >= 'A' && glyph <= 'Z')
    return Glyph_class_uppercase;
  if (glyph >= 'a' && glyph <= 'z')
    return Glyph_class_lowercase;
  return Glyph_class_unknown;
}

static int term_attrs_of_cell(Glyph g, Mark m) {
  Glyph_class gclass = glyph_class_of(g);
  int attr = A_normal;
  switch (gclass) {
  case Glyph_class_unknown:
    attr = A_bold | fg_bg(C_red, C_natural);
    break;
  case Glyph_class_grid:
    attr = A_bold | fg_bg(C_black, C_natural);
    break;
  case Glyph_class_comment:
    attr = A_dim | Cdef_normal;
    break;
  case Glyph_class_uppercase:
    attr = A_normal | fg_bg(C_black, C_cyan);
    break;
  case Glyph_class_lowercase:
  case Glyph_class_movement:
  case Glyph_class_numeric:
    attr = A_bold | Cdef_normal;
    break;
  case Glyph_class_bang:
    attr = A_bold | Cdef_normal;
    break;
  }
  if (gclass != Glyph_class_comment) {
    if ((m & (Mark_flag_lock | Mark_flag_input)) ==
        (Mark_flag_lock | Mark_flag_input)) {
      attr = A_normal | Cdef_normal;
    } else if (m & Mark_flag_lock) {
      attr = A_dim | Cdef_normal;
    }
  }
  if (m & Mark_flag_output) {
    attr = A_reverse;
  }
  if (m & Mark_flag_haste_input) {
    attr = A_bold | fg_bg(C_cyan, C_natural);
  }
  return attr;
}

typedef struct {
  Usz y;
  Usz x;
} Tui_cursor;

void tui_cursor_init(Tui_cursor* tc) {
  tc->y = 0;
  tc->x = 0;
}

void tui_cursor_move_relative(Tui_cursor* tc, Usz field_h, Usz field_w,
                              Isz delta_y, Isz delta_x) {
  Isz y0 = (Isz)tc->y + delta_y;
  Isz x0 = (Isz)tc->x + delta_x;
  if (y0 >= (Isz)field_h)
    y0 = (Isz)field_h - 1;
  if (y0 < 0)
    y0 = 0;
  if (x0 >= (Isz)field_w)
    x0 = (Isz)field_w - 1;
  if (x0 < 0)
    x0 = 0;
  tc->y = (Usz)y0;
  tc->x = (Usz)x0;
}

void tdraw_tui_cursor(WINDOW* win, int win_h, int win_w, Glyph const* gbuffer,
                      Usz field_h, Usz field_w, Usz ruler_spacing_y,
                      Usz ruler_spacing_x, Usz cursor_y, Usz cursor_x,
                      Tui_input_mode input_mode, bool is_playing) {
  (void)ruler_spacing_y;
  (void)ruler_spacing_x;
  (void)input_mode;
  if (cursor_y >= field_h || cursor_x >= field_w || (int)cursor_y >= win_h ||
      (int)cursor_x >= win_w)
    return;
  Glyph beneath = gbuffer[cursor_y * field_w + cursor_x];
  char displayed;
  if (beneath == '.') {
    displayed = is_playing ? '@' : '~';
  } else {
    displayed = beneath;
  }
  chtype ch =
      (chtype)(displayed | (A_reverse | A_bold | fg_bg(C_yellow, C_natural)));
  wmove(win, (int)cursor_y, (int)cursor_x);
  waddchnstr(win, &ch, 1);
}

typedef struct Undo_node {
  Field field;
  Usz tick_num;
  struct Undo_node* prev;
  struct Undo_node* next;
} Undo_node;

typedef struct {
  Undo_node* first;
  Undo_node* last;
  Usz count;
} Undo_history;

void undo_history_init(Undo_history* hist) {
  hist->first = NULL;
  hist->last = NULL;
  hist->count = 0;
}
void undo_history_deinit(Undo_history* hist) {
  Undo_node* a = hist->first;
  while (a) {
    Undo_node* b = a->next;
    field_deinit(&a->field);
    free(a);
    a = b;
  }
}

enum { Undo_history_max = 500 };

void undo_history_push(Undo_history* hist, Field* field, Usz tick_num) {
  Undo_node* new_node;
  if (hist->count == Undo_history_max) {
    new_node = hist->first;
    if (new_node == hist->last) {
      hist->first = NULL;
      hist->last = NULL;
    } else {
      hist->first = new_node->next;
      hist->first->prev = NULL;
    }
  } else {
    new_node = malloc(sizeof(Undo_node));
    ++hist->count;
    field_init(&new_node->field);
  }
  field_copy(field, &new_node->field);
  new_node->tick_num = tick_num;
  if (hist->last) {
    hist->last->next = new_node;
    new_node->prev = hist->last;
  } else {
    hist->first = new_node;
    hist->last = new_node;
    new_node->prev = NULL;
  }
  new_node->next = NULL;
  hist->last = new_node;
}

void undo_history_pop(Undo_history* hist, Field* out_field, Usz* out_tick_num) {
  Undo_node* last = hist->last;
  if (!last)
    return;
  field_copy(&last->field, out_field);
  *out_tick_num = last->tick_num;
  if (hist->first == last) {
    hist->first = NULL;
    hist->last = NULL;
  } else {
    Undo_node* new_last = last->prev;
    new_last->next = NULL;
    hist->last = new_last;
  }
  field_deinit(&last->field);
  free(last);
  --hist->count;
}

Usz undo_history_count(Undo_history* hist) { return hist->count; }

void tdraw_hud(WINDOW* win, int win_y, int win_x, int height, int width,
               const char* filename, Usz field_h, Usz field_w,
               Usz ruler_spacing_y, Usz ruler_spacing_x, Usz tick_num,
               Tui_cursor* const tui_cursor, Tui_input_mode input_mode) {
  (void)height;
  (void)width;
  wmove(win, win_y, win_x);
  wprintw(win, "%dx%d\t%d/%d\t%df\t120\t-------", (int)field_w, (int)field_h,
          (int)ruler_spacing_x, (int)ruler_spacing_y, (int)tick_num);
  wclrtoeol(win);
  wmove(win, win_y + 1, win_x);
  wprintw(win, "%d,%d\t1:1\tcell\t", (int)tui_cursor->x, (int)tui_cursor->y);
  switch (input_mode) {
  case Tui_input_mode_normal:
    wattrset(win, A_normal);
    wprintw(win, "insert");
    break;
  case Tui_input_mode_append:
    wattrset(win, A_bold);
    wprintw(win, "append");
    break;
  case Tui_input_mode_piano:
    wattrset(win, A_reverse);
    wprintw(win, "trigger");
    break;
  }
  wattrset(win, A_normal);
  wprintw(win, "\t%s", filename);
  wclrtoeol(win);
}

void tdraw_field(WINDOW* win, int term_h, int term_w, int pos_y, int pos_x,
                 Glyph const* gbuffer, Mark const* mbuffer, Usz field_h,
                 Usz field_w, Usz ruler_spacing_y, Usz ruler_spacing_x) {
  enum { Bufcount = 4096 };
  (void)term_h;
  (void)term_w;
  if (field_w > Bufcount)
    return;
  if (pos_y >= term_h || pos_x >= term_w)
    return;
  Usz num_y = (Usz)term_h - (Usz)pos_y;
  Usz num_x = (Usz)term_w - (Usz)pos_x;
  if (field_h < num_y)
    num_y = field_h;
  if (field_w < num_x)
    num_x = field_w;
  chtype buffer[Bufcount];
  bool use_rulers = ruler_spacing_y != 0 && ruler_spacing_x != 0;
  for (Usz y = 0; y < num_y; ++y) {
    Glyph const* gline = gbuffer + y * field_w;
    Mark const* mline = mbuffer + y * field_w;
    bool use_y_ruler = use_rulers && y % ruler_spacing_y == 0;
    for (Usz x = 0; x < num_x; ++x) {
      Glyph g = gline[x];
      Mark m = mline[x];
      int attrs = term_attrs_of_cell(g, m);
      if (g == '.') {
        if (use_y_ruler && x % ruler_spacing_x == 0)
          g = '+';
      }
      buffer[x] = (chtype)((int)g | attrs);
    }
    wmove(win, pos_y + (int)y, pos_x);
    waddchnstr(win, buffer, (int)num_x);
    // Trying to clear to eol with 0 chars remaining on line will clear whole
    // line from start
    if (pos_x + (int)num_x != term_w) {
      wmove(win, pos_y + (int)y, pos_x + (int)num_x);
      wclrtoeol(win);
    }
  }
}

void tui_cursor_confine(Tui_cursor* tc, Usz height, Usz width) {
  if (height == 0 || width == 0)
    return;
  if (tc->y >= height)
    tc->y = height - 1;
  if (tc->x >= width)
    tc->x = width - 1;
}

void tdraw_oevent_list(WINDOW* win, Oevent_list const* oevent_list) {
  (void)win;
  (void)oevent_list;
  wmove(win, 0, 0);
  int win_h, win_w;
  getmaxyx(win, win_h, win_w);
  (void)win_w;
  wprintw(win, "Count: %d", (int)oevent_list->count);
  for (Usz i = 0, num_events = oevent_list->count; i < num_events; ++i) {
    int cury = getcury(win);
    if (cury + 1 >= win_h)
      return;
    wmove(win, cury + 1, 0);
    Oevent const* ev = oevent_list->buffer + i;
    Oevent_types evt = ev->oevent_type;
    switch (evt) {
    case Oevent_type_midi: {
      Oevent_midi const* em = (Oevent_midi const*)ev;
      wprintw(win,
              "MIDI\tchannel %d\toctave %d\tnote %d\tvelocity %d\tlength %d",
              (int)em->channel, (int)em->octave, (int)em->note,
              (int)em->velocity, (int)em->bar_divisor);
      break;
    }
    }
  }
}

void tui_resize_grid(Field* field, Markmap_reusable* markmap, Usz new_height,
                     Usz new_width, Usz tick_num, Field* scratch_field,
                     Undo_history* undo_hist, Tui_cursor* tui_cursor,
                     bool* needs_remarking) {
  assert(new_height > 0 && new_width > 0);
  undo_history_push(undo_hist, field, tick_num);
  field_copy(field, scratch_field);
  field_resize_raw(field, new_height, new_width);
  // junky copies until i write a smarter thing
  memset(field->buffer, '.', new_height * new_width * sizeof(Glyph));
  gbuffer_copy_subrect(scratch_field->buffer, field->buffer,
                       scratch_field->height, scratch_field->width,
                       field->height, field->width, 0, 0, 0, 0,
                       scratch_field->height, scratch_field->width);
  tui_cursor_confine(tui_cursor, new_height, new_width);
  markmap_reusable_ensure_size(markmap, new_height, new_width);
  *needs_remarking = true;
}

static Usz adjust_humanized_snapped(Usz ruler, Usz in, Isz delta_rulers) {
  // slightly more confusing because desired grid sizes are +1 (e.g. ruler of
  // length 8 wants to snap to 25 and 33, not 24 and 32). also this math is
  // sloppy.
  assert(ruler > 0);
  if (in == 0) {
    return delta_rulers > 0 ? ruler * (Usz)delta_rulers : 1;
  }
  // could overflow if inputs are big
  if (delta_rulers < 0)
    in += ruler - 1;
  Isz n = ((Isz)in - 1) / (Isz)ruler + delta_rulers;
  if (n < 0)
    n = 0;
  return ruler * (Usz)n + 1;
}

// Resizes by number of ruler divisions, and snaps size to closest division in
// a way a human would expect. Adds +1 to the output, so grid resulting size is
// 1 unit longer than the actual ruler length.
void tui_resize_grid_snap_ruler(Field* field, Markmap_reusable* markmap,
                                Usz ruler_y, Usz ruler_x, Isz delta_h,
                                Isz delta_w, Usz tick_num, Field* scratch_field,
                                Undo_history* undo_hist, Tui_cursor* tui_cursor,
                                bool* needs_remarking) {
  assert(ruler_y > 0);
  assert(ruler_x > 0);
  Usz field_h = field->height;
  Usz field_w = field->width;
  assert(field_h > 0);
  assert(field_w > 0);
  if (ruler_y == 0 || ruler_x == 0 || field_h == 0 || field_w == 0)
    return;
  Usz new_field_h = field_h;
  Usz new_field_w = field_w;
  if (delta_h != 0)
    new_field_h = adjust_humanized_snapped(ruler_y, field_h, delta_h);
  if (delta_w != 0)
    new_field_w = adjust_humanized_snapped(ruler_x, field_w, delta_w);
  if (new_field_h == field_h && new_field_w == field_w)
    return;
  tui_resize_grid(field, markmap, new_field_h, new_field_w, tick_num,
                  scratch_field, undo_hist, tui_cursor, needs_remarking);
}

typedef struct {
  Field field;
  Field scratch_field;
  Markmap_reusable markmap_r;
  Bank bank;
  Undo_history undo_hist;
  Oevent_list oevent_list;
  Oevent_list scratch_oevent_list;
  Tui_cursor tui_cursor;
  Piano_bits piano_bits;
  Usz tick_num;
  Usz ruler_spacing_y, ruler_spacing_x;
  Tui_input_mode input_mode;
  Usz bpm;
  double accum_secs;
  bool needs_remarking;
  bool is_draw_dirty;
  bool is_playing;
  bool draw_event_list;
} App_state;

void app_init(App_state* a) {
  field_init(&a->field);
  field_init(&a->scratch_field);
  markmap_reusable_init(&a->markmap_r);
  bank_init(&a->bank);
  undo_history_init(&a->undo_hist);
  tui_cursor_init(&a->tui_cursor);
  oevent_list_init(&a->oevent_list);
  oevent_list_init(&a->scratch_oevent_list);
  a->piano_bits = ORCA_PIANO_BITS_NONE;
  a->tick_num = 0;
  a->ruler_spacing_y = 8;
  a->ruler_spacing_x = 8;
  a->input_mode = Tui_input_mode_normal;
  a->bpm = 120;
  a->accum_secs = 0.0;
  a->needs_remarking = true;
  a->is_draw_dirty = false;
  a->is_playing = false;
  a->draw_event_list = false;
}

void app_deinit(App_state* a) {
  field_deinit(&a->field);
  field_deinit(&a->scratch_field);
  markmap_reusable_deinit(&a->markmap_r);
  bank_deinit(&a->bank);
  undo_history_deinit(&a->undo_hist);
  oevent_list_deinit(&a->oevent_list);
  oevent_list_deinit(&a->scratch_oevent_list);
}

bool app_is_draw_dirty(App_state* a) {
  return a->is_draw_dirty || a->needs_remarking;
}

double app_secs_to_deadline(App_state const* a) {
  if (a->is_playing) {
    double secs_span = 60.0 / (double)a->bpm / 4.0;
    double rem = secs_span - a->accum_secs;
    if (rem < 0.0)
      rem = 0.0;
    return rem;
  } else {
    return 1.0;
  }
}

void app_apply_delta_secs(App_state* a, double secs) {
  if (a->is_playing) {
    a->accum_secs += secs;
  }
}

void app_do_stuff(App_state* a) {
  double secs_span = 60.0 / (double)a->bpm / 4.0;
  while (a->accum_secs > secs_span) {
    a->accum_secs -= secs_span;
    undo_history_push(&a->undo_hist, &a->field, a->tick_num);
    orca_run(a->field.buffer, a->markmap_r.buffer, a->field.height,
             a->field.width, a->tick_num, &a->bank, &a->oevent_list,
             a->piano_bits);
    ++a->tick_num;
    a->piano_bits = ORCA_PIANO_BITS_NONE;
    a->needs_remarking = true;
    a->is_draw_dirty = true;
  }
}

static double ms_to_sec(double ms) {
  return ms / 1000.0;
}

void app_force_draw_dirty(App_state* a) { a->is_draw_dirty = true; }

void app_draw(App_state* a, WINDOW* win) {
  // We can predictavely step the next simulation tick and then use the
  // resulting markmap buffer for better UI visualization. If we don't do
  // this, after loading a fresh file or after the user performs some edit
  // (or even after a regular simulation step), the new glyph buffer won't
  // have had phase 0 of the simulation run, which means the ports and other
  // flags won't be set on the markmap buffer, so the colors for disabled
  // cells, ports, etc. won't be set.
  //
  // We can just perform a simulation step using the current state, keep the
  // markmap buffer that it produces, then roll back the glyph buffer to
  // where it was before. This should produce results similar to having
  // specialized UI code that looks at each glyph and figures out the ports,
  // etc.
  if (a->needs_remarking) {
    field_resize_raw_if_necessary(&a->scratch_field, a->field.height,
                                  a->field.width);
    field_copy(&a->field, &a->scratch_field);
    markmap_reusable_ensure_size(&a->markmap_r, a->field.height,
                                 a->field.width);
    orca_run(a->scratch_field.buffer, a->markmap_r.buffer, a->field.height,
             a->field.width, a->tick_num, &a->bank, &a->scratch_oevent_list,
             a->piano_bits);
    a->needs_remarking = false;
  }
  int win_h, win_w;
  getmaxyx(win, win_h, win_w);
  tdraw_field(win, win_h, win_w, 0, 0, a->field.buffer, a->markmap_r.buffer,
              a->field.height, a->field.width, a->ruler_spacing_y,
              a->ruler_spacing_x);
  for (int y = a->field.height; y < win_h - 1; ++y) {
    wmove(win, y, 0);
    wclrtoeol(win);
  }
  tdraw_tui_cursor(win, win_h, win_w, a->field.buffer, a->field.height,
                   a->field.width, a->ruler_spacing_y, a->ruler_spacing_x,
                   a->tui_cursor.y, a->tui_cursor.x, a->input_mode,
                   a->is_playing);
  if (win_h > 3) {
    tdraw_hud(win, win_h - 2, 0, 2, win_w, "noname", a->field.height,
              a->field.width, a->ruler_spacing_y, a->ruler_spacing_x,
              a->tick_num, &a->tui_cursor, a->input_mode);
  }
  if (a->draw_event_list) {
    tdraw_oevent_list(win, &a->oevent_list);
  }
  a->is_draw_dirty = false;
  wrefresh(win);
}

void app_move_cursor_relative(App_state* a, Isz delta_y, Isz delta_x) {
  tui_cursor_move_relative(&a->tui_cursor, a->field.height, a->field.width,
                           delta_y, delta_x);
  a->is_draw_dirty = true;
}

void app_adjust_rulers_relative(App_state* a, Isz delta_y, Isz delta_x) {
  Isz new_y = (Isz)a->ruler_spacing_y + delta_y;
  Isz new_x = (Isz)a->ruler_spacing_x + delta_x;
  if (new_y < 4)
    new_y = 4;
  else if (new_y > 16)
    new_y = 16;
  if (new_x < 4)
    new_x = 4;
  else if (new_x > 16)
    new_x = 16;
  if ((Usz)new_y == a->ruler_spacing_y && (Usz)new_x == a->ruler_spacing_x)
    return;
  a->ruler_spacing_y = (Usz)new_y;
  a->ruler_spacing_x = (Usz)new_x;
}

void app_resize_grid_relative(App_state* a, Isz delta_y, Isz delta_x) {
  tui_resize_grid_snap_ruler(&a->field, &a->markmap_r, a->ruler_spacing_y,
                             a->ruler_spacing_x, delta_y, delta_x, a->tick_num,
                             &a->scratch_field, &a->undo_hist, &a->tui_cursor,
                             &a->needs_remarking);
  a->is_draw_dirty = true;
}

void app_write_character(App_state* a, char c) {
  undo_history_push(&a->undo_hist, &a->field, a->tick_num);
  gbuffer_poke(a->field.buffer, a->field.height, a->field.width,
               a->tui_cursor.y, a->tui_cursor.x, c);
  // Indicate we want the next simulation step to be run predictavely,
  // so that we can use the reulsting mark buffer for UI visualization.
  // This is "expensive", so it could be skipped for non-interactive
  // input in situations where max throughput is necessary.
  a->needs_remarking = true;
  if (a->input_mode == Tui_input_mode_append) {
    tui_cursor_move_relative(&a->tui_cursor, a->field.height, a->field.width, 0,
                             1);
  }
  a->is_draw_dirty = true;
}

void app_add_piano_bits_for_character(App_state* a, char c) {
  Piano_bits added_bits = piano_bits_of((Glyph)c);
  a->piano_bits |= added_bits;
}

void app_input_character(App_state* a, char c) {
  bool ok = c >= '!' && c <= '~';
  if (!ok)
    return;
  switch (a->input_mode) {
  case Tui_input_mode_normal:
  case Tui_input_mode_append:
    app_write_character(a, c);
    break;
  case Tui_input_mode_piano:
    app_add_piano_bits_for_character(a, c);
    break;
  }
}

typedef enum {
  App_input_cmd_undo,
  App_input_cmd_toggle_append_mode,
  App_input_cmd_toggle_piano_mode,
  App_input_cmd_step_forward,
  App_input_cmd_toggle_show_event_list,
  App_input_cmd_toggle_play_pause,
} App_input_cmd;

void app_input_cmd(App_state* a, App_input_cmd ev) {
  switch (ev) {
  case App_input_cmd_undo:
    if (undo_history_count(&a->undo_hist) > 0) {
      undo_history_pop(&a->undo_hist, &a->field, &a->tick_num);
      a->needs_remarking = true;
      a->is_draw_dirty = true;
    }
    break;
  case App_input_cmd_toggle_append_mode:
    if (a->input_mode == Tui_input_mode_append) {
      a->input_mode = Tui_input_mode_normal;
    } else {
      a->input_mode = Tui_input_mode_append;
    }
    a->is_draw_dirty = true;
    break;
  case App_input_cmd_toggle_piano_mode:
    if (a->input_mode == Tui_input_mode_piano) {
      a->input_mode = Tui_input_mode_normal;
    } else {
      a->input_mode = Tui_input_mode_piano;
    }
    a->is_draw_dirty = true;
    break;
  case App_input_cmd_step_forward:
    undo_history_push(&a->undo_hist, &a->field, a->tick_num);
    orca_run(a->field.buffer, a->markmap_r.buffer, a->field.height,
             a->field.width, a->tick_num, &a->bank, &a->oevent_list,
             a->piano_bits);
    ++a->tick_num;
    a->piano_bits = ORCA_PIANO_BITS_NONE;
    a->needs_remarking = true;
    a->is_draw_dirty = true;
    break;
  case App_input_cmd_toggle_play_pause:
    if (a->is_playing) {
      a->is_playing = false;
      // nodelay(stdscr, FALSE);
    } else {
      a->is_playing = true;
      // nodelay(stdscr, TRUE);
    }
    a->is_draw_dirty = true;
    break;
  case App_input_cmd_toggle_show_event_list:
    a->draw_event_list = !a->draw_event_list;
    a->is_draw_dirty = true;
    break;
  }
}

enum { Argopt_margins = UCHAR_MAX + 1 };

int main(int argc, char** argv) {
  static struct option tui_options[] = {
      {"margins", required_argument, 0, Argopt_margins},
      {"help", no_argument, 0, 'h'},
      {NULL, 0, NULL, 0}};
  char* input_file = NULL;
  int margin_thickness = 2;
  for (;;) {
    int c = getopt_long(argc, argv, "h", tui_options, NULL);
    if (c == -1)
      break;
    switch (c) {
    case 'h':
      usage();
      return 0;
    case Argopt_margins:
      margin_thickness = atoi(optarg);
      if (margin_thickness == 0 && strcmp(optarg, "0")) {
        fprintf(stderr,
                "Bad margins argument %s.\n"
                "Must be 0 or positive integer.\n",
                optarg);
        return 1;
      }
      break;
    case '?':
      usage();
      return 1;
    }
  }

  if (margin_thickness < 0) {
    fprintf(stderr, "Margins must be >= 0.\n");
    usage();
    return 1;
  }

  if (optind == argc - 1) {
    input_file = argv[optind];
  } else if (optind < argc - 1) {
    fprintf(stderr, "Expected only 1 file argument.\n");
    return 1;
  }

  App_state app_state;
  app_init(&app_state);

  if (input_file) {
    Field_load_error fle = field_load_file(input_file, &app_state.field);
    if (fle != Field_load_error_ok) {
      char const* errstr = "Unknown";
      switch (fle) {
      case Field_load_error_ok:
        break;
      case Field_load_error_cant_open_file:
        errstr = "Unable to open file";
        break;
      case Field_load_error_too_many_columns:
        errstr = "Grid file has too many columns";
        break;
      case Field_load_error_too_many_rows:
        errstr = "Grid file has too many rows";
        break;
      case Field_load_error_no_rows_read:
        errstr = "Grid file has no rows";
        break;
      case Field_load_error_not_a_rectangle:
        errstr = "Grid file is not a rectangle";
        break;
      }
      fprintf(stderr, "File load error: %s.\n", errstr);
      app_deinit(&app_state);
      return 1;
    }
  } else {
    input_file = "unnamed";
    field_init_fill(&app_state.field, 25, 57, '.');
  }
  // Set up timer lib
  stm_setup();

  // Enable UTF-8 by explicitly initializing our locale before initializing
  // ncurses.
  setlocale(LC_ALL, "");
  // Initialize ncurses
  initscr();
  // Allow ncurses to control newline translation. Fine to use with any modern
  // terminal, and will let ncurses run faster.
  nonl();
  // Set interrupt keys (interrupt, break, quit...) to not flush. Helps keep
  // ncurses state consistent, at the cost of less responsive terminal
  // interrupt. (This will rarely happen.)
  intrflush(stdscr, FALSE);
  // Receive keyboard input immediately, and receive shift, control, etc. as
  // separate events, instead of combined with individual characters.
  // raw();
  // Don't echo keyboard input
  noecho();
  // Also receive arrow keys, etc.
  keypad(stdscr, TRUE);
  // Hide the terminal cursor
  curs_set(0);
  // Don't block on calls like getch() -- have it ERR immediately if the user
  // hasn't typed anything. That way we can mix other timers in our code,
  // instead of being a slave only to terminal input.
  // nodelay(stdscr, TRUE);
  // Enable color
  start_color();
  use_default_colors();

  for (int ifg = 0; ifg < Colors_count; ++ifg) {
    for (int ibg = 0; ibg < Colors_count; ++ibg) {
      int res = init_pair((short int)(1 + ifg * Colors_count + ibg),
                          (short int)(ifg - 1), (short int)(ibg - 1));
      if (res == ERR) {
        endwin();
        fprintf(stderr, "Error initializing color\n");
        exit(1);
      }
    }
  }

  WINDOW* cont_win = NULL;
  int key = KEY_RESIZE;
  wtimeout(stdscr, 0);
  U64 last_time = 0;
  // double accum_time = 0.0;

  for (;;) {
    switch (key) {
    case ERR: {
      U64 diff = stm_laptime(&last_time);
      app_apply_delta_secs(&app_state, stm_sec(diff));
      app_do_stuff(&app_state);
      if (app_is_draw_dirty(&app_state)) {
        app_draw(&app_state, cont_win);
      }
      diff = stm_laptime(&last_time);
      app_apply_delta_secs(&app_state, stm_sec(diff));
      double secs_to_d = app_secs_to_deadline(&app_state);
      // fprintf(stderr, "to deadline: %f\n", secs_to_d);
      if (secs_to_d < ms_to_sec(0.5)) {
        wtimeout(stdscr, 0);
      } else if (secs_to_d < ms_to_sec(3.0)) {
        wtimeout(stdscr, 1);
      } else if (secs_to_d < ms_to_sec(10.0)) {
        wtimeout(stdscr, 1);
      } else if (secs_to_d < ms_to_sec(50.0)) {
        wtimeout(stdscr, 10);
      } else {
        wtimeout(stdscr, 10);
      }
      //struct timespec ts;
      //ts.tv_sec = 0;
      //// ts.tv_nsec = 1000 * 1000 * 1;
      //ts.tv_nsec = 1;
      //int ret = nanosleep(&ts, NULL);
      //if (ret) {
      //  fprintf(stderr, "interrupted sleep: %d\n", ret);
      //}
    } break;
    case KEY_RESIZE: {
      int term_height = getmaxy(stdscr);
      int term_width = getmaxx(stdscr);
      assert(term_height >= 0 && term_width >= 0);
      int content_y = 0;
      int content_x = 0;
      int content_h = term_height;
      int content_w = term_width;
      int margins_2 = margin_thickness * 2;
      if (margin_thickness > 0 && term_height > margins_2 &&
          term_width > margins_2) {
        content_y += margin_thickness;
        content_x += margin_thickness;
        content_h -= margins_2;
        content_w -= margins_2;
      }
      if (cont_win == NULL || getmaxy(cont_win) != content_h ||
          getmaxx(cont_win) != content_w) {
        if (cont_win) {
          delwin(cont_win);
        }
        wclear(stdscr);
        cont_win = derwin(stdscr, content_h, content_w, content_y, content_x);
        app_force_draw_dirty(&app_state);
      }
    } break;
    case AND_CTRL('q'):
      goto quit;
    case KEY_UP:
    case AND_CTRL('k'):
      app_move_cursor_relative(&app_state, -1, 0);
      break;
    case AND_CTRL('j'):
    case KEY_DOWN:
      app_move_cursor_relative(&app_state, 1, 0);
      break;
    case KEY_BACKSPACE:
    case AND_CTRL('h'):
    case KEY_LEFT:
      app_move_cursor_relative(&app_state, 0, -1);
      break;
    case AND_CTRL('l'):
    case KEY_RIGHT:
      app_move_cursor_relative(&app_state, 0, 1);
      break;
    case AND_CTRL('u'):
      app_input_cmd(&app_state, App_input_cmd_undo);
      break;
    case '[':
      app_adjust_rulers_relative(&app_state, 0, -1);
      break;
    case ']':
      app_adjust_rulers_relative(&app_state, 0, 1);
      break;
    case '{':
      app_adjust_rulers_relative(&app_state, -1, 0);
      break;
    case '}':
      app_adjust_rulers_relative(&app_state, 1, 0);
      break;
    case '(':
      app_resize_grid_relative(&app_state, 0, -1);
      break;
    case ')':
      app_resize_grid_relative(&app_state, 0, 1);
      break;
    case '_':
      app_resize_grid_relative(&app_state, -1, 0);
      break;
    case '+':
      app_resize_grid_relative(&app_state, 1, 0);
      break;
    case '\r':
    case KEY_ENTER:
      app_input_cmd(&app_state, App_input_cmd_toggle_append_mode);
      break;
    case '/':
      app_input_cmd(&app_state, App_input_cmd_toggle_piano_mode);
      break;
    case AND_CTRL('f'): {
      app_input_cmd(&app_state, App_input_cmd_step_forward);
    } break;
    case AND_CTRL('e'):
      app_input_cmd(&app_state, App_input_cmd_toggle_show_event_list);
      break;
    case ' ':
      app_input_cmd(&app_state, App_input_cmd_toggle_play_pause);
      break;
    default:
      if (key >= '!' && key <= '~') {
        app_input_character(&app_state, (char)key);
      }
      break;
#if 0
      else {
        fprintf(stderr, "Unknown key number: %d\n", key);
      }
#endif
      break;
    }
    key = wgetch(stdscr);
  }
quit:
  if (cont_win) {
    delwin(cont_win);
  }
  endwin();
  app_deinit(&app_state);
  return 0;
}
