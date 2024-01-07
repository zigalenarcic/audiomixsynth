/*
 * main.c
 *
 * Implementation of 'audiomixsynth' - an audio creation software
 *
 * Initial date: 2023-12-26 21:33 UTC+1:00
 *
 * Author: Ziga Lenarcic
 *
 * Public domain.
 */

#include <sys/types.h>
#include <stdio.h>
#include <dirent.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <math.h>
#include <sys/stat.h>
#include <glob.h>
#include <getopt.h>
#include <err.h>
#include <stdbool.h>
#include <ctype.h>
#include <pthread.h>
#include <GL/gl.h>
#include <GLFW/glfw3.h>

#include "audio.h"
#include "utils.h"
#include "common.h"

#include "ft2build.h"
#include FT_FREETYPE_H

#define PROGNAME "audiomixsynth"
#define VERSION_MAJOR 0
#define VERSION_MINOR 0
#define VERSION_PATCH 1

static const struct option longopts[] =
{
  {"no-fork",         no_argument,    NULL,   'f'},
  {"help",            no_argument,    NULL,   'h'},
  {"local-file",      no_argument,    NULL,   'l'},
  {"version",         no_argument,    NULL,   'V'},
  {NULL,              0,              NULL,   0},
};

#define FONT_CHAR_WIDTH 7
#define FONT_CHAR_HEIGHT 14

Rack the_rack;

typedef struct CharDescription_
{
  int available;
  float tex_coord0_x;
  float tex_coord0_y;
  float tex_coord1_x;
  float tex_coord1_y;

  int width;
  int height;
  int top;
  int left;
  int advance;
} CharDescription;

typedef struct FontData_
{
  uint8_t *bitmap;
  int bitmap_width;
  int bitmap_height;
  CharDescription chars[128];
  int character_width;
  int character_height;
  int line_height;
  double font_size;
  GLuint texture_id;
} FontData;

FontData *fonts[12];

struct {
  char font_file[512];
  int font_size;
  double gui_scale;
  double line_spacing;
} settings = { .font_size = 10, .gui_scale = 1.0, .line_spacing = 1.0};

GLFWwindow *window;

int window_width;
int window_height;

Slider *slider_drag;

double rack_width(void)
{
  return 640.0;
}

double rack_height_unit(double units)
{
  /* rack width = 19", 1U = 1.75" tall */
  return units * (int)(rack_width() / 19.0 * 1.75);
}

char tooltip[128];

vec2 mpos;
vec2 mpos_left_down;

bool redisplay_needed = false;

FT_Library library;

void exit_program(int code)
{
  if (window)
    glfwDestroyWindow(window);
  glfwTerminate();
  exit(code);
}

double clamp(double val, double min, double max)
{
  if (val > max) return max;
  if (val < min) return min;
  return val;
}

vec2 make_vec2(double x, double y)
{
  return (vec2){x, y};
}

vec2 mul_vec2(vec2 in, double scale)
{
  return (vec2){in.x * scale, in.y * scale};
}

vec2 floor_vec2(vec2 in)
{
  return (vec2){floorf(in.x), floorf(in.y)};
}

rect make_rect(double x, double y, double w, double h)
{
  return (rect){x, y, w, h};
}

rect move_rect(rect in, vec2 off)
{
  return make_rect(in.x + off.x, in.y + off.y, in.w, in.h);
}

vec2 rect_midpoint(rect in)
{
  return make_vec2(in.x + 0.5 * in.w, in.y + 0.5 * in.h);
}

rect rect_grow(rect in, double amount_x, double amount_y)
{
  return make_rect(in.x - amount_x, in.y - amount_y, in.w + 2 * amount_x, in.h + 2 * amount_y);
}

bool inside_rect(rect in, vec2 pos)
{
  return (pos.x >= in.x) && (pos.y >= in.y) && (pos.x < (in.x + in.w)) && (pos.y < (in.y + in.h));
}

unsigned round_to_power_of_2(unsigned x)
{
  for (int i = 31; i >= 0; i--)
  {
    if (x == (1 << i))
      return 1 << i;

    if ((x & (1 << i)) != 0)
      return 1 << (i + 1);
  }

  return 0;
}

int load_bmp(const char *filename)
{
  FILE *f = fopen(filename, "rb");

  if (!f)
    return -1;


  fclose(f);
}

int get_font_file(char *font)
{
  FILE *f = fopen(font, "rb");

  if (f)
  {
    fclose(f);
    return 1;
  }

  char cmd[512];
  snprintf(cmd, sizeof(cmd), "fc-match --format=%%{file} \"%s\"", font);

  FILE *proc = popen(cmd, "r");

  int proc_fd = fileno(proc);
  /* wait for output */

  fd_set fd_s;
  FD_ZERO(&fd_s);
  FD_SET(proc_fd, &fd_s);

  struct timeval timeout;
  timeout.tv_sec = 0;
  timeout.tv_usec = 100 * 1000;

  int ret = select(proc_fd + 1, &fd_s, NULL, NULL, &timeout);

  if (ret > 0)
  {
    /* read the output */
    if (proc != NULL)
    {
      char tmp_out[1024];
      char *ptr = fgets(tmp_out, sizeof(tmp_out), proc);
      pclose(proc);

      if (ptr)
      {
        strcpy(font, ptr);
        return 1;
      }
    }
  }

  return 0;
}

void copy_bitmap(uint8_t *dst, int w_dst, int h_dst, int x, int y, const uint8_t *src, int w_src, int h_src, int pitch_src)
{
  for (int j = 0; j < h_src; j++)
  {
    if ((j + y) >= h_dst)
      break;

    int bytes_to_copy = w_src;
    if ((bytes_to_copy + x) > w_dst)
    {
      bytes_to_copy = w_dst - x; // clip
    }

    memcpy(&dst[(j + y) * w_dst + x], &src[j * pitch_src], bytes_to_copy);
  }
}

void copy_bitmap_1bit(uint8_t *dst, int w_dst, int h_dst, int x, int y, const uint8_t *src, int w_src, int h_src, int pitch_src)
{
  for (int j = 0; j < h_src; j++)
  {
    if ((j + y) >= h_dst)
      break;

    for (int i = 0; i < w_src; i++)
    {
      if ((x + i) >= w_dst)
        break;
      int byte = i / 8;
      int bit = 7 - (i % 8);
      dst[(j + y) * w_dst + x + i] = (src[j * pitch_src + byte] & (1U << bit)) ? 255 : 0;
    }
  }
}

int init_freetype(void)
{
  int error = FT_Init_FreeType(&library);
  if (error)
  {
    fprintf(stderr, "Failed to initialize freetype\n");
    exit(EXIT_FAILURE);
  }
  return 0;
}

int render_font_texture(int idx_font, const char *font_file, int font_size_px)
{
  FT_Face face;

  FILE *f = fopen(font_file, "rb");
  if (f == NULL)
  {
    fprintf(stderr, "Font file missing: \"%s\"\n", font_file);
    return -1;
  }
  else
  {
    fclose(f);
  }

  int error = FT_New_Face(library, font_file, 0,  &face);
  if (error == FT_Err_Unknown_File_Format)
  {
    fprintf(stderr, "Unknown font format: %s\n", font_file);
    return -1;
  }
  else if (error)
  {
    fprintf(stderr, "File not found: %s\n", font_file);
    return -1;
  }
  else
  {
    //printf("Font \"%s\" loaded\n", font_file);
  }

  //error = FT_Set_Pixel_Sizes(face, 0, font_size_px);
  error = FT_Set_Char_Size(face, 0, font_size_px * 64, 96, 96);
  if (error)
  {
    fprintf(stderr, "FT_Set_Char_Size error: %d\n", error);
    return -1;
  }

  int font_width = 0;
  int font_height = 0;

  /* get character size of X, H */
  const char *str = "XH";
  while (*str)
  {
    int glyph_index = FT_Get_Char_Index(face, *str);
    str++;
    if (glyph_index > 0)
    {
      FT_Load_Glyph(face, glyph_index, FT_LOAD_DEFAULT);
      error = FT_Render_Glyph(face->glyph, FT_RENDER_MODE_NORMAL);
      if (error)
      {
        fprintf(stderr, "FT_Render_Glyph: Glyph render error: %d\n", error);
      }

      FT_Bitmap *bmp = &face->glyph->bitmap;

      font_width = MAX(font_width, bmp->width);
      font_height = MAX(font_height, bmp->rows);
    }
  }

  if ((font_width <= 0) || (font_height <= 0))
  {
    fprintf(stderr, "Failed to determine font size\n");
    return -1;
  }

  /* now allocate a texture of a good size */

  FontData *font = calloc(sizeof(FontData), 1);

  font->font_size = font_size_px;
  font->character_width = font_width;
  font->character_height = font_height;
  font->line_height = face->size->metrics.height / 64 + 1; /* add 1 px of line height to make text more breathy */

  font->bitmap_width = round_to_power_of_2(16 * (font_width + 2));
  font->bitmap_height = round_to_power_of_2(6 * (font_height * 2));
  font->bitmap_width = MAX(font->bitmap_width, font->bitmap_height);
  font->bitmap_height = MAX(font->bitmap_width, font->bitmap_height);

  font->bitmap = calloc(sizeof(uint8_t), font->bitmap_width * font->bitmap_height);

  for (int i = 32; i < 128; i++)
  {
    int glyph_index = FT_Get_Char_Index(face, i);
    if (glyph_index > 0)
    {
      FT_Load_Glyph(face, glyph_index, FT_LOAD_DEFAULT);

      error = FT_Render_Glyph(face->glyph, FT_RENDER_MODE_NORMAL);
      if (error)
      {
        fprintf(stderr, "FT_Render_Glyph: Glyph render error: %d\n", error);
        continue;
      }

      FT_Bitmap *bmp = &face->glyph->bitmap;

      int w = bmp->width;
      int h = bmp->rows;
      int pitch = bmp->pitch;
      int left = face->glyph->bitmap_left;
      int top = face->glyph->bitmap_top;

      //printf("w %d h %d pitch %d left %d top %d\n", w, h, pitch, left, top);

      int col = i % 16;
      int row = i / 16;
      int dst_x = col * (font_width + 2) + left + 1;
      int dst_y = row * (font_height * 2) - top;


      font->chars[i].available = 1;
      font->chars[i].top = top;
      font->chars[i].left = left;
      font->chars[i].width = w;
      font->chars[i].height = h;
      font->chars[i].advance = face->glyph->advance.x / 64;

      const float pixel_x = 1.0f / font->bitmap_width;
      const float pixel_y = 1.0f / font->bitmap_height;

      font->chars[i].tex_coord0_x = pixel_x * dst_x;
      font->chars[i].tex_coord0_y = pixel_y * dst_y;
      font->chars[i].tex_coord1_x = pixel_x * (dst_x + w);
      font->chars[i].tex_coord1_y = pixel_y * (dst_y + h);

      if (bmp->pixel_mode == FT_PIXEL_MODE_GRAY)
        copy_bitmap(font->bitmap, font->bitmap_width, font->bitmap_height, dst_x, dst_y, bmp->buffer, w, h, pitch);
      else if (bmp->pixel_mode == FT_PIXEL_MODE_MONO)
      {
        // monochrome
        copy_bitmap_1bit(font->bitmap, font->bitmap_width, font->bitmap_height, dst_x, dst_y, bmp->buffer, w, h, pitch);
      }
      else
      {
        fprintf(stderr, "Unsupported pixel mode (not 8 bit or 1 bit)\n");
      }

    }
    else
    {
      //printf("no glyph for %d\n", i);
      continue;
    }
  }

  FT_Done_Face(face);
  fonts[idx_font] = font;

  return 0;
}

void print_usage(const char *exe)
{
  fprintf(stderr, "Usage: %s [OPTION]\n", exe);

  exit(1);
}

bool contains_uppercase(const char *str)
{
  while (*str)
  {
    if ((*str >= 'A') && (*str <= 'Z'))
      return true;

    str++;
  }

  return false;
}

enum {
  DIM_SCROLLBAR_THUMB_MIN_HEIGHT = 0,
  DIM_SCROLLBAR_THUMB_MARGIN,
  DIM_SCROLLBAR_WIDTH,
  DIM_SCROLL_AMOUNT,
  DIM_GUI_MARGIN,
  DIM_TEXT_HORIZONTAL_MARGIN,
  DIM_SCROLL_OVERLAP,
  DIM_KEYBOARD_KEY_WHITE_WIDTH,
  DIM_KEYBOARD_KEY_WHITE_HEIGHT,
};

int get_dimension(int dim)
{
  switch (dim)
  {
    case DIM_SCROLLBAR_THUMB_MIN_HEIGHT: return 5;
    case DIM_SCROLLBAR_THUMB_MARGIN: return 1;
    case DIM_SCROLLBAR_WIDTH: return 14;
    case DIM_SCROLL_AMOUNT: return 15;
    case DIM_GUI_MARGIN: return 8;
    case DIM_TEXT_HORIZONTAL_MARGIN: return 5;
    case DIM_SCROLL_OVERLAP: return 5;
    case DIM_KEYBOARD_KEY_WHITE_WIDTH: return 20;
    case DIM_KEYBOARD_KEY_WHITE_HEIGHT: return 100;
    default: return 10;
  }
}

rect get_rack_window(void)
{
  return make_rect(get_dimension(DIM_GUI_MARGIN), get_dimension(DIM_GUI_MARGIN),
      rack_width() + get_dimension(DIM_SCROLLBAR_WIDTH), window_height - 2 * get_dimension(DIM_GUI_MARGIN) - get_dimension(DIM_KEYBOARD_KEY_WHITE_HEIGHT));
}

int document_height(rect rack_window)
{
  return the_rack.total_height + rack_window.h - rack_height_unit(1);
}

void update_scrollbar(void)
{
  rect rack_window = get_rack_window();
  int doc_height = document_height(rack_window);
  int thumb_size_tmp = (double)rack_window.h / (doc_height - 1) * rack_window.h;

  the_rack.scrollbar.thumb_size = clamp(thumb_size_tmp, get_dimension(DIM_SCROLLBAR_THUMB_MIN_HEIGHT), rack_window.h);
  //the_rack.scrollbar.thumb_position = round((double)the_rack.scroll_position / (doc_height - 1) * rack_window.h);
  the_rack.scrollbar.thumb_position = round((double)the_rack.scroll_position / (doc_height - rack_window.h) * (rack_window.h - the_rack.scrollbar.thumb_size));
}

int scrollbar_thumb_position_to_scroll_position(int thumb_position)
{
  rect rack_window = get_rack_window();
  int doc_height = document_height(rack_window);
  int thumb_size_tmp = (double)rack_window.h / (doc_height - 1) * rack_window.h;

  the_rack.scrollbar.thumb_size = clamp(thumb_size_tmp, get_dimension(DIM_SCROLLBAR_THUMB_MIN_HEIGHT), rack_window.h);

  int scrollbar_height = rack_window.h;

  double percentage = (double)thumb_position / (scrollbar_height - the_rack.scrollbar.thumb_size);

  return percentage * (doc_height - rack_window.h);
}

void redisplay(void)
{
  redisplay_needed = true;
}

void window_refresh_func(GLFWwindow *window)
{
  redisplay_needed = true;
}

void window_size_func(GLFWwindow *window, int w, int h)
{
  window_width = w;
  window_height = h;

  glViewport(0, 0, window_width, window_height);

  glMatrixMode(GL_PROJECTION);
  glLoadIdentity(); /* must reset, further calls modify the matrix */

  glOrtho(0 /*left*/,
      window_width /*right*/,
      window_height /*bottom*/,
      0 /*top*/,
      -1 /*nearVal*/,
      1 /*farVal*/);

  glMatrixMode(GL_MODELVIEW);

  update_scrollbar();
}

int fitting_window_width(void)
{
  return rack_width() + 2 * get_dimension(DIM_GUI_MARGIN) + get_dimension(DIM_SCROLLBAR_WIDTH);
}

int fitting_window_height(void)
{
  return 1024;
}

float color_table[][3] = {
  {21.0f/255.0f, 21.0f/255.0f, 21.0f/255.0f},
  {253.0f/255.0f, 253.0f/255.0f, 232.0f/255.0f},
  {164.0f/255.0f, 212.0f/255.0f, 241.0f/255.0f},
  {255.0f/255.0f, 206.0f/255.0f, 121.0f/255.0f},
  {123.0f/255.0f, 123.0f/255.0f, 123.0f/255.0f},
  {38.0f/255.0f, 38.0f/255.0f, 38.0f/255.0f},
  {69.0f/255.0f, 69.0f/255.0f, 69.0f/255.0f},
  {84.0f/255.0f, 84.0f/255.0f, 84.0f/255.0f},
  {72.0f/255.0f, 21.0f/255.0f, 255.0f/255.0f},
  {235.0f/255.0f, 180.0f/255.0f, 112.0f/255.0f},
  {143.0f/255.0f, 191.0f/255.0f, 220.0f/255.0f},
  {255.0f/255.0f, 21.0f/255.0f, 21.0f/255.0f},
  {21.0f/255.0f, 21.0f/255.0f, 255.0f/255.0f},
  {21.0f/255.0f, 255.0f/255.0f, 21.0f/255.0f},
};

enum {
  COLOR_INDEX_BACKGROUND = 0,
  COLOR_INDEX_FOREGROUND,
  COLOR_INDEX_BOLD,
  COLOR_INDEX_ITALIC,
  COLOR_INDEX_DIM,
  COLOR_INDEX_SCROLLBAR_BACKGROUND,
  COLOR_INDEX_SCROLLBAR_THUMB,
  COLOR_INDEX_SCROLLBAR_THUMB_HOVER,
  COLOR_INDEX_LINK,
  COLOR_INDEX_GUI_1, /* amber */
  COLOR_INDEX_GUI_2, /* blue */
  COLOR_INDEX_ERROR, /* red-like */
  COLOR_INDEX_SEARCHES,
  COLOR_INDEX_SEARCH_SELECTED,
};

void set_color(int i)
{
  glColor3f(color_table[i][0], color_table[i][1], color_table[i][2]);
}

void draw_rectangle(int x, int y, int w, int h)
{
  glBegin(GL_TRIANGLE_STRIP);
  glVertex2i(x, y);
  glVertex2i(x + w, y);
  glVertex2i(x, y + h);
  glVertex2i(x + w, y + h);
  glEnd();
}

void draw_rectangle_outline(int x, int y, int w, int h)
{
  glTranslatef(0.5, 0.5, 0); /* fix missing pixel in the corner */
  w -= 1; /* to match normal quads */
  h -= 1;
  glBegin(GL_LINE_STRIP);
  glVertex2i(x, y);
  glVertex2i(x + w, y);
  glVertex2i(x + w, y + h);
  glVertex2i(x, y + h);
  glVertex2i(x, y);
  glEnd();
  glTranslatef(-0.5, -0.5, 0);
}

void draw_rect(rect in, bool outline)
{
  void (*ptr)(int, int, int, int) = outline ? &draw_rectangle_outline : &draw_rectangle;
  ptr(in.x, in.y, in.w, in.h);
}

/*
   !"#$%&'()*+,-./
0123456789:;<=>?
@ABCDEFGHIJKLMNO
PQRSTUVWXYZ[\]^_
`abcdefghijklmno
pqrstuvwxyz{|}~
*/

int put_char_gl(int idx_font, int x, int y, char c)
{
  int ret = 0;
  int w = FONT_CHAR_WIDTH;
  int h = FONT_CHAR_HEIGHT;

  FontData *font = fonts[idx_font];
  if (!font)
    return -1;

  glBindTexture(GL_TEXTURE_2D, font->texture_id);
  glEnable(GL_BLEND);
  glEnable(GL_TEXTURE_2D);

  if (c < 32)
  {
    // unknown character
    glDisable(GL_BLEND);
    draw_rectangle_outline(x + 1, y + 1, w - 2, h - 2);
    glEnable(GL_BLEND);
  }
  else
  {
    int idx = (int)c;
    if (font->chars[idx].available)
    {
      int w = font->chars[idx].width;
      int h = font->chars[idx].height;
      int x_start = x + font->chars[idx].left;
      int y_start = y - font->chars[idx].top + font->character_height + 2;

      glBegin(GL_QUADS);
      glTexCoord2f(font->chars[idx].tex_coord0_x, font->chars[idx].tex_coord0_y);
      glVertex2f(x_start, y_start);
      glTexCoord2f(font->chars[idx].tex_coord0_x, font->chars[idx].tex_coord1_y);
      glVertex2f(x_start, y_start + h);
      glTexCoord2f(font->chars[idx].tex_coord1_x, font->chars[idx].tex_coord1_y);
      glVertex2f(x_start + w, y_start + h);
      glTexCoord2f(font->chars[idx].tex_coord1_x, font->chars[idx].tex_coord0_y);
      glVertex2f(x_start + w, y_start);
      glEnd();

      ret = font->chars[idx].advance;
    }
    else
    {
      glDisable(GL_BLEND);
      draw_rectangle_outline(x + 1, y + 1, w - 2, h - 2);
      glEnable(GL_BLEND);
      ret = font->character_width;
    }
  }

  glDisable(GL_BLEND);

  return ret;
}

size_t draw_string(int idx_font, int x, int y, const char *str)
{
  size_t count = 0;
  while (*str)
  {
    count++;
    x += put_char_gl(idx_font, x, y, *str);
    str++;
  }

  return count;
}

void add_gl_texture_monochrome(GLuint *texture, int width, int height, void *data)
{
  glGenTextures(1, texture);
  glBindTexture(GL_TEXTURE_2D, *texture);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_ALPHA, width, height, 0, GL_ALPHA, GL_UNSIGNED_BYTE, data);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
}

void upload_font_textures(void)
{
  for (int i = 0; i < ARRAY_SIZE(fonts); i++)
  {
    FontData *font = fonts[i];
    if (font)
    {
      add_gl_texture_monochrome(&font->texture_id, font->bitmap_width, font->bitmap_height, font->bitmap);
    }
  }
}

int find_string(const char *search_term, const char *text)
{
  int search_len = strlen(search_term);
  int text_len = strlen(text);

  for (int i = 0; i < (text_len - search_len); i++)
  {
    bool match = true;
    for (int j = 0; j < search_len; j++)
    {
      if (search_term[j] != text[i + j])
      {
        match = false;
        break;
      }
    }

    if (match)
      return i;
  }

  return -1;
}

int compar_match_rev(const void *a, const void *b)
{
  return ((const int *)b)[1] - ((const int *)a)[1];
}

char keyboard_state[256];
double keyboard_display_offset;

bool black_keys[12] = {false, true, false, true, false, false, true, false, true, false, true, false };
double key_pos[12] = {0, 0.5, 1, 1.5, 2, 3, 3.5, 4, 4.5, 5, 5.5, 6};

rect get_keyboard_key_rect(int key)
{
  double x = (((key / 12) * 7) + key_pos[key % 12]) * get_dimension(DIM_KEYBOARD_KEY_WHITE_WIDTH);

  if (black_keys[key % 12])
  {
    double w = get_dimension(DIM_KEYBOARD_KEY_WHITE_WIDTH) * 0.6f;
    return make_rect(x - keyboard_display_offset + 0.5 * get_dimension(DIM_KEYBOARD_KEY_WHITE_WIDTH) - 0.5 * w, 0, w, get_dimension(DIM_KEYBOARD_KEY_WHITE_HEIGHT) * 0.6);
  }
  else
    return make_rect(x - keyboard_display_offset, 0, get_dimension(DIM_KEYBOARD_KEY_WHITE_WIDTH), get_dimension(DIM_KEYBOARD_KEY_WHITE_HEIGHT));
}

vec2 get_keyboard_screen_pos(void)
{
  return make_vec2(-keyboard_display_offset, window_height - get_dimension(DIM_KEYBOARD_KEY_WHITE_HEIGHT));
}

rect get_keyboard_screen_rect(void)
{
  return make_rect(0, window_height - get_dimension(DIM_KEYBOARD_KEY_WHITE_HEIGHT), window_width, get_dimension(DIM_KEYBOARD_KEY_WHITE_HEIGHT));
}

bool keyboard_keyboard_hit_test(vec2 pos, int *key)
{
  vec2 pos2 = floor_vec2(pos);
  for (int pass = 0; pass <= 1; pass++)
  {
    for (int i = 0; i < 128; i++)
    { 
      if ((int)black_keys[i % 12] ^ pass)
      {
        vec2 off = get_keyboard_screen_pos();

        rect rect = move_rect(get_keyboard_key_rect(i), off);

        if (inside_rect(rect, pos2))
        {
          *key = i;
          return true;
        }
      }
    }
  }

  return false;
}

int set_grey(float val)
{
  glColor3f(val, val, val);
  return 0;
}

double relative_to_absolute(double rel, double min, double max, int curve)
{
  return min + (max - min) * rel;
}

double absolute_to_relative(double absolute, double min, double max, int curve)
{
  return (absolute - min) / (max - min);
}

double slider_travel(Slider *s)
{
  return s->horizontal ? (s->pos.w - s->thumb_size.x) : (s->pos.h - s->thumb_size.y);
}

double slider_value_to_screen_pos(Slider *s, double value)
{
  double rel_pos = absolute_to_relative(value, s->min, s->max, s->curve);
  return rel_pos * (s->horizontal ? s->pos.w : s->pos.h);
}

double slider_screen_pos_to_value(Slider *s, double pos)
{
  double rel_pos = pos / (s->horizontal ? s->pos.w : s->pos.h);
  rel_pos = clamp(rel_pos, 0.0, 1.0);
  double val = relative_to_absolute(rel_pos, s->min, s->max, s->curve);
  return val;
}

rect slider_hitbox(Slider *s)
{
  double rel_pos = absolute_to_relative(s->value, s->min, s->max, s->curve);

  if (s->horizontal)
  {
    return move_rect(make_rect(s->pos.x + rel_pos * s->pos.w, s->pos.y + 0.5 * s->pos.h, s->thumb_size.x, s->thumb_size.y),
        mul_vec2(s->thumb_size, -0.5));
  }
  else
  {
    return move_rect(make_rect(s->pos.x + 0.5 * s->pos.w, s->pos.y + (1.0 - rel_pos) * s->pos.h, s->thumb_size.x, s->thumb_size.y),
        mul_vec2(s->thumb_size, -0.5));
  }
}

void draw_slider_generic(Slider *slider, vec2 off)
{
  set_grey(0.0);

  rect r = move_rect(slider->pos, off);
  draw_rect(r, false);

  set_grey(0.5);
  double rel_pos = absolute_to_relative(slider->value, slider->min, slider->max, slider->curve);

  if (slider->horizontal)
  {
    draw_rect(make_rect(r.x, r.y, r.w * rel_pos, r.h), false);
  }
  else
  {
    draw_rect(make_rect(r.x, r.y + r.h * (1.0 - rel_pos), r.w, r.h * rel_pos), false);
  }

  rect r2 = move_rect(slider_hitbox(slider), off);
  vec2 point = rect_midpoint(r2);
  set_grey(1.0);
  draw_rect(r2, false);
}

void draw_generic_instrument(Instrument *inst, bool back, vec2 off)
{
  if (back)
  {
    rect r = move_rect(make_rect(0, 0, rack_width(), inst->height), off);
    set_grey(0.2);
    draw_rect(r, false);
    set_grey(0.5);
    draw_rect(r, true);

    set_grey(0.4);
    draw_string(1, off.x + 10, off.y + 10, inst->name);

    if (inst->num_inputs > 0)
    {
      for (int i = 0; i < inst->num_inputs; i++)
      {
        rect r = move_rect(inst->inputs[i].pos, off);
        set_grey(1.0);
        draw_rect(r, true);
      }
    }

    for (int i = 0; i < inst->num_outputs; i++)
    {
      rect r = move_rect(inst->outputs[i].pos, off);
      set_grey(0.7);
      draw_rect(r, true);
    }
  }
  else
  {
    rect r = move_rect(make_rect(0, 0, rack_width(), inst->height), off);
    set_grey(0.3);
    draw_rect(r, false);
    set_grey(0.8);
    draw_rect(r, true);

    for (int i = 0; i < inst->slider_count; i++)
      draw_slider_generic(&inst->sliders[i], off);

    set_grey(0.6);
    draw_string(1, off.x + 10, off.y + 10, inst->name);
  }
}

void update_instrument(Instrument *inst)
{
  /* recalcuate */
}

extern int main_frames;

double *allocate_double(int n);
Connection *init_connection(Connection *conn, int index, bool is_input, rect pos, struct Instrument_ *inst)
{
  conn->index = index;
  conn->is_input = is_input;
  conn->pos = pos;
  conn->inst = inst;
  conn->target_inst = NULL;
  conn->target_connection = 0;
  conn->buffer = NULL;
}

Slider *init_slider(Slider *slider, const char *name, 
  double min, double max, double value, int curve, int steps, rect pos, vec2 thumb_size, bool horizontal, struct Instrument_ *inst)
{
  strcpy(slider->name, name);
  slider->min = min;
  slider->max = max;
  slider->value = value;
  slider->curve = curve;
  slider->steps = steps;
  slider->pos = pos;
  slider->thumb_size = thumb_size;
  slider->horizontal = horizontal;

  slider->value_start_drag = 0.0;
  slider->inst = inst;
}

Instrument *AllocateInstrument(void)
{
  Instrument *inst = (Instrument *)calloc(1, sizeof(Instrument));

  return inst;
}

Instrument *make_synth(void)
{
  Instrument *inst = AllocateInstrument();

  strcpy(inst->name, "Synth");
  strcpy(inst->user_name, "Synth");
  inst->height = rack_height_unit(4);
  inst->draw = &draw_generic_instrument;
  inst->process_audio = &process_audio_synth;

  inst->num_inputs = 0;

  inst->num_outputs = 2;
  init_connection(&inst->outputs[0], 0, false, (rect){510, 10, 10, 10}, inst);
  init_connection(&inst->outputs[1], 1, false, (rect){530, 10, 10, 10}, inst);

  inst->slider_count = 3;
  init_slider(&inst->sliders[0], "Frequency", 0.2, 10.0, 1.0, 0, 0, (rect){10, 50, 100, 10}, (vec2){10, 10}, true, inst);
  init_slider(&inst->sliders[1], "Volume", 0.0, 1.0, 0.2, 0, 0, (rect){10, 70, 100, 10}, (vec2){10, 10}, true, inst);
  init_slider(&inst->sliders[2], "Filter", 10.0, 10000.0, 5000.0, 0, 0, (rect){10, 90, 300, 10}, (vec2){10, 10}, true, inst);

  return inst;
}

Instrument *make_io_device(void)
{
  Instrument *inst = AllocateInstrument();

  strcpy(inst->name, "IO Device");
  strcpy(inst->user_name, "IO");
  inst->height = rack_height_unit(1);
  inst->draw = &draw_generic_instrument;
  inst->process_audio = &process_audio_io_device;

  inst->num_inputs = 2;
  init_connection(&inst->inputs[0], 0, true, (rect){10, 10, 10, 10}, inst);
  init_connection(&inst->inputs[1], 1, true, (rect){30, 10, 10, 10}, inst);

  inst->num_outputs = 0;

  inst->slider_count = 1;
  init_slider(&inst->sliders[0], "Volume", 0.0, 1.0, 0.8, 0, 0, (rect){200, 10, 100, 10}, (vec2){10, 10}, true, inst);

  return inst;
}

Instrument *make_chorus(void)
{
  Instrument *inst = AllocateInstrument();

  strcpy(inst->name, "Chorus");
  strcpy(inst->user_name, "Chorus");
  inst->height = rack_height_unit(1);
  inst->draw = &draw_generic_instrument;
  inst->process_audio = &process_audio_chorus;

  inst->num_inputs = 2;
  init_connection(&inst->inputs[0], 0, true, (rect){10, 10, 10, 10}, inst);
  init_connection(&inst->inputs[1], 1, true, (rect){30, 10, 10, 10}, inst);


  inst->num_outputs = 2;
  init_connection(&inst->outputs[0], 0, false, (rect){10, 30, 10, 10}, inst);
  init_connection(&inst->outputs[1], 1, false, (rect){30, 30, 10, 10}, inst);

  inst->slider_count = 3;
  init_slider(&inst->sliders[0], "Rate", 0.2, 10.0, 1.0, 0, 0, (rect){10, 40, 100, 10}, (vec2){10, 10}, true, inst);
  init_slider(&inst->sliders[1], "Depth", 0.2, 10.0, 1.0, 0, 0, (rect){150, 40, 100, 10}, (vec2){10, 10}, true, inst);
  init_slider(&inst->sliders[2], "Mix", 0.0, 1.0, 0.0, 0, 0, (rect){280, 40, 100, 10}, (vec2){10, 10}, true, inst);

  return inst;
}

void recalculate_rack_coordinates(void)
{
  Instrument *inst = the_rack.first;
  double height = 0;
  while (inst)
  {
    inst->rack_pos = make_rect(0, height, rack_width(), inst->height);
    height += inst->height;
    inst = inst->next;
  }

  the_rack.total_height = height;
}

void connect_audio(Instrument *inst1, int n_output, Instrument *inst2, int n_input)
{
  inst1->outputs[n_output].target_inst = inst2;
  inst1->outputs[n_output].target_connection = n_input;

  inst2->inputs[n_input].target_inst = inst1;
  inst2->inputs[n_input].target_connection = n_output;
}

void disconnect_audio(Instrument *inst1, int n_output)
{
  Instrument *inst2 = inst1->outputs[n_output].target_inst;
  int n_input = inst1->outputs[n_output].target_connection;

  if (inst2)
  {
    inst1->outputs[n_output].target_inst = NULL;
    inst1->outputs[n_output].target_connection = 0;

    inst2->inputs[n_input].target_inst = NULL;
    inst2->inputs[n_input].target_connection = 0;
  }
}

Instrument *add_to_rack(Instrument *inst, bool autoconnect)
{
  if (!the_rack.first)
  {
    the_rack.first = inst;
    return inst;
  }

  Instrument *last_inst = the_rack.first;
  while (last_inst->next)
    last_inst = last_inst->next;

  /* on last device */

  last_inst->next = inst;
  inst->prev = last_inst;

  if (autoconnect)
  {
    if (inst->num_inputs > 0 && inst->num_outputs > 0)
    {
      /* FX try to remove previous connections and insert the instrument */

      for (int i = 0; i < MIN(inst->num_inputs, last_inst->num_outputs); i++)
      {
        Instrument *inst_prev = last_inst->outputs[i].target_inst;
        int target_connection_prev = last_inst->outputs[i].target_connection;

        disconnect_audio(last_inst, i);
        connect_audio(last_inst, i, inst, i);
        connect_audio(inst, i, inst_prev, target_connection_prev);
      }
    }
    else if (inst->num_outputs > 0 && !the_rack.first->inputs[0].target_inst && !the_rack.first->inputs[1].target_inst)
    {
      for (int i = 0; i < MIN(2, inst->num_outputs); i++)
      {
        connect_audio(inst, i, the_rack.first, i);
      }
    }

  }

  return inst;
}

void init_rack(void)
{
  add_to_rack(make_io_device(), true);
  add_to_rack(make_synth(), true);
  add_to_rack(make_chorus(), true);

  recalculate_audio_graph();
  recalculate_rack_coordinates();
}

rect get_scrollbar_rect(rect window, Scrollbar *scrollbar)
{
  return move_rect(make_rect(0, 0, get_dimension(DIM_SCROLLBAR_WIDTH), window.h), make_vec2(window.x + window.w - get_dimension(DIM_SCROLLBAR_WIDTH) , window.y));
}

rect get_scrollbar_thumb_rect(rect window, Scrollbar *scrollbar)
{
  return move_rect(make_rect(get_dimension(DIM_SCROLLBAR_THUMB_MARGIN), scrollbar->thumb_position + get_dimension(DIM_SCROLLBAR_THUMB_MARGIN), get_dimension(DIM_SCROLLBAR_WIDTH) - 2 * get_dimension(DIM_SCROLLBAR_THUMB_MARGIN), scrollbar->thumb_size - 2 * get_dimension(DIM_SCROLLBAR_THUMB_MARGIN)), make_vec2(window.x + window.w - get_dimension(DIM_SCROLLBAR_WIDTH) , window.y));
}

double catenary_root_func(double a, double dx, double dy, double length)
{
double part_a = sqrt(length * length - dy * dy);
double part_b = 2.0 * a * sinh(dx / (2.0 * a));
    printf("part a %f %f \n", part_a, part_b);
  return  part_a - part_b;
}

double solve_catenary(double dx, double dy, double length)
{
  if (dx == 0)
    dx = 10;
  printf("solve_catenary dx %f dy %f len %f\n", dx, dy, length);
  int steps = 0;
  double min_a = 10.0;
  double max_a = 10000.0;

  double min_val = catenary_root_func(min_a, dx, dy, length);
  double max_val = catenary_root_func(max_a, dx, dy, length);
    printf("min %f max %f\n", min_val, max_val);
  if (min_val * max_val >= 0)
  {
    printf("Cant solve catenary\n");
    return -1;
  }

  while ((max_a - min_a) > 0.1)
  {
    double middle_a = (min_a + max_a) / 2;
    double middle_val = catenary_root_func(middle_a, dx, dy, length);
    printf("step %d: %f %f %f -> %f %f %f \n", steps, min_a, max_a, middle_a, min_val, max_val, middle_val);

    if (min_val * middle_val < 0)
    {
      max_a = middle_a;
      max_val = middle_val;
    }
    else
    {
      min_a = middle_a;
      min_val = middle_val;
    }

    steps++;
  }

  printf("After %d steps %f %f\n", steps, min_a, max_a);

  return (min_a + max_a) / 2;
}

double catenary_y(double x, double a)
{
  return a * cosh(x / a);
}

void draw_cable(vec2 start, vec2 end)
{
#if 1
  double dx = end.x - start.x;
  double dy = end.y - start.y;
  double carthesian_len = sqrt(dx * dx + dy * dy);
  double a = solve_catenary(dx, dy, carthesian_len * 1.5);

  printf("A %f\n", a);

          glColor3f(0.2, 0.8, 0.2);
          glBegin(GL_LINE_STRIP);
          glVertex2i(start.x, start.y);
          glVertex2i(end.x, end.y);
          glEnd();


          if (a > 0)
          {
            double x1 = a * acosh(start.y/a);
            double x2 = a * acosh(end.y/a);
            printf("x1 x2 %f %f\n", x1, x2);

            glColor3f(1.0, 0.8, 0.2);
            glBegin(GL_LINE_STRIP);
            for (double x = start.x; x <= end.x; x += 1)
            {
              double x_interp = x1 + (x2 - x1) * ((x - start.x) / (end.x - start.x));
              glVertex2d(x, start.y - catenary_y(x_interp, a));
            }
            glEnd();
          }
#else
          glColor3f(0.2, 0.8, 0.2);
          glBegin(GL_LINE_STRIP);
          glVertex2i(start.x, start.y);
          glVertex2i(end.x, end.y);
          glEnd();
#endif
}

void draw_rack(rect rack_window)
{
  /* draw only within this area */
  glScissor(rack_window.x, window_height - (rack_window.y + rack_window.h) /* 0, 0 at lower left corner */, rack_window.w, rack_window.h);
  glEnable(GL_SCISSOR_TEST);
  vec2 origin = make_vec2(rack_window.x, rack_window.y);
  {
    Instrument *inst = the_rack.first;
    while (inst)
    {
      vec2 screen_pos = make_vec2(origin.x + inst->rack_pos.x, origin.y + inst->rack_pos.y - the_rack.scroll_position);

      inst->draw(inst, the_rack.show_back, screen_pos);
      inst = inst->next;
    }
  }

  /* draw connections */
  if (the_rack.show_back)
  {
    Instrument *inst = the_rack.first;
    while (inst)
    {
      vec2 screen_pos = make_vec2(origin.x + inst->rack_pos.x, origin.y + inst->rack_pos.y - the_rack.scroll_position);

      /* draw connections */
      for (int i = 0; i < inst->num_outputs; i++)
      {
        Instrument *dst_inst = inst->outputs[i].target_inst;
        int target_index = inst->outputs[i].target_connection;
        if (dst_inst && (target_index >= 0 && target_index < dst_inst->num_inputs))
        {
          rect r_start = move_rect(inst->outputs[i].pos, screen_pos);
          vec2 start = rect_midpoint(r_start);
          vec2 screen_pos2 = make_vec2(origin.x + dst_inst->rack_pos.x, origin.y + dst_inst->rack_pos.y - the_rack.scroll_position);
          rect r_end = move_rect(dst_inst->inputs[target_index].pos, screen_pos2);
          vec2 end = rect_midpoint(r_end);

          //printf("Draw connection %f %f %f %f\n", start.x, start.y, end.x, end.y);
          glColor3f(0.2, 1.0, 0.2);
          draw_rect(r_start, true);
          glColor3f(0.2, 0.8, 0.2);
          draw_rect(r_end, true);
          if (start.x < end.x)
          draw_cable(start, end);
          else
          draw_cable(end, start);
        }
      }
      inst = inst->next;
    }
  }

  /* draw the scrollbar */
  set_color(COLOR_INDEX_SCROLLBAR_BACKGROUND);
  draw_rect(get_scrollbar_rect(rack_window, &the_rack.scrollbar), false);

  update_scrollbar();

  set_color(the_rack.scrollbar.thumb_hover ? COLOR_INDEX_SCROLLBAR_THUMB_HOVER : COLOR_INDEX_SCROLLBAR_THUMB);

  draw_rect(get_scrollbar_thumb_rect(rack_window, &the_rack.scrollbar), false);

  glDisable(GL_SCISSOR_TEST);
}

void draw_keyboard(void)
{
  /* draw keyboard */

  for (int pass = 0; pass <= 1; pass++)
  {
    for (int i = 0; i < 128; i++)
    { 
      if ((int)black_keys[i % 12] ^ pass)
        continue;

      vec2 off = get_keyboard_screen_pos();

      rect key_rect = move_rect(get_keyboard_key_rect(i), off);

      if (keyboard_state[i])
        black_keys[i % 12] ? set_grey(0.25f) : set_grey(0.75f);
      else
        black_keys[i % 12] ? set_grey(0.f) : set_grey(1.f);

      draw_rect(key_rect, false);

      black_keys[i % 12] ? set_grey(0.25f) : set_grey(0.f);

      draw_rect(key_rect, true);
    }
  }
}

void render(void)
{
  glClearColor(0.25f, 0.25f, 0.25f, 1.f);
  glClear(GL_COLOR_BUFFER_BIT);
  glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
  glBlendEquation(GL_FUNC_ADD);

  glMatrixMode(GL_MODELVIEW);
  glLoadIdentity();
  glDisable(GL_BLEND);

  glColor3f(1.0, 1.0, 1.0);

  rect rack_window = get_rack_window();
  draw_rect(rect_grow(rack_window, 0, 0), true);
  draw_rack(rack_window);

  draw_keyboard();

  if (tooltip[0] != '\0')
  {
    vec2 pos = make_vec2(mpos.x + 10, mpos.y + 10);

    set_grey(0.4);
    rect r = make_rect(pos.x, pos.y, strlen(tooltip) * 10, 25);
    draw_rect(r, false);
    set_grey(0.8);
    draw_string(0, pos.x + 2, pos.y + 2, tooltip);
  }

  glfwSwapBuffers(window);
}

void set_scroll_position(int new_scroll_position)
{
  rect rack_window = get_rack_window();
  int doc_height = document_height(rack_window);

  new_scroll_position = clamp(new_scroll_position, 0, (doc_height - rack_window.h) > 0 ? doc_height - rack_window.h : 0);

  if (new_scroll_position != the_rack.scroll_position)
  {
    the_rack.scroll_position = new_scroll_position;
    redisplay();
  }
}

int keyboard_key = -1;

void mouse_button_func(GLFWwindow *window, int button, int action, int mods)
{
  rect rack_window = get_rack_window();
  rect scrollbar_rect = get_scrollbar_rect(rack_window, &the_rack.scrollbar);
  rect thumb_rect = get_scrollbar_thumb_rect(rack_window, &the_rack.scrollbar);

  switch (button)
  {
    case GLFW_MOUSE_BUTTON_LEFT:
      if (action == GLFW_PRESS)
      {
        if (inside_rect(thumb_rect, mpos))
        {
          the_rack.scrollbar.dragging = true;
          mpos_left_down = mpos;
          the_rack.scrollbar.thumb_mouse_down_thumb_position = the_rack.scrollbar.thumb_position;
        }
        else if (inside_rect(scrollbar_rect, mpos))
        {
          // page up or down if clicked outside the thumb
          if ((mpos.y - scrollbar_rect.y) < the_rack.scrollbar.thumb_position)
          {
            set_scroll_position(the_rack.scroll_position - (rack_window.h - get_dimension(DIM_SCROLL_OVERLAP)));
          }
          else if ((mpos.y - scrollbar_rect.y) >= (the_rack.scrollbar.thumb_position + the_rack.scrollbar.thumb_size))
          {
            set_scroll_position(the_rack.scroll_position + rack_window.h - get_dimension(DIM_SCROLL_OVERLAP));
          }
        }
        else if (inside_rect(rack_window, mpos))
        {
          /* check if interacting with the synth */

          vec2 rack_mpos = make_vec2(mpos.x - rack_window.x, mpos.y - rack_window.y + the_rack.scroll_position);

          Instrument *inst = the_rack.first;
          double height = 0;
          while (inst)
          {
            if ((rack_mpos.y >= inst->rack_pos.y)
                && (rack_mpos.y < (inst->rack_pos.y + inst->rack_pos.h))
                && inside_rect(inst->rack_pos, rack_mpos))
            {
              /* check if synth handles the click */

          printf("synth click\n");
              vec2 synth_mpos = make_vec2(rack_mpos.x - inst->rack_pos.x, rack_mpos.y - inst->rack_pos.y);
              for (int i = 0; i < inst->slider_count; i++)
              {
                rect rslider = slider_hitbox(&inst->sliders[i]);
                if (inside_rect(rslider, synth_mpos))
                {
          printf("sliders click %d\n", i);
                  mpos_left_down = mpos;
                  slider_drag = &inst->sliders[i];
                  inst->sliders[i].value_start_drag = inst->sliders[i].value;

                  snprintf(tooltip, sizeof(tooltip), "%s: %f", inst->sliders[i].name, inst->sliders[i].value);
                  redisplay();
                  break;
                }
              }

              break;
            }
            height += inst->height;
            inst = inst->next;
          }
        }
        else
        {
          int key = -1;
          if (keyboard_keyboard_hit_test(mpos, &key))
          {
            if (keyboard_key >= 0)
            {
              keyboard_state[keyboard_key] = 0;
              keyboard_input(keyboard_key, 0, 0);
              keyboard_key = -1;
            }
            keyboard_state[key] = 1;
            keyboard_input(key, 1, 127);
            keyboard_key = key;
            redisplay();
          }
        }
      }
      else if (action == GLFW_RELEASE)
      {
        the_rack.scrollbar.dragging = false;
        if (slider_drag)
        {
          tooltip[0] = '\0';
          slider_drag = NULL;
          redisplay();
        }

        if (keyboard_key >= 0)
        {
          keyboard_state[keyboard_key] = 0;
          keyboard_input(keyboard_key, 0, 0);
          keyboard_key = -1;
          redisplay();
        }

      }
      break;
    case GLFW_MOUSE_BUTTON_RIGHT: // right click
      break;
  }
}

void mouse_pos_func(GLFWwindow *window, double x_d, double y_d)
{
  mpos.x = x_d;
  mpos.y = y_d;

  bool redisp = false;

  if (slider_drag)
  {
    double delta = slider_drag->horizontal ? mpos.x - mpos_left_down.x : mpos.y - mpos_left_down.y;
    double len = slider_travel(slider_drag);
    double start = slider_value_to_screen_pos(slider_drag, slider_drag->value_start_drag);
    slider_drag->value = slider_screen_pos_to_value(slider_drag, start + delta);
    update_instrument(slider_drag->inst);
    snprintf(tooltip, sizeof(tooltip), "%s: %f", slider_drag->name, slider_drag->value);
    redisplay();
  }
  else if (the_rack.scrollbar.dragging)
  {
    rect rack_window = get_rack_window();
    int new_thumb_position = clamp(the_rack.scrollbar.thumb_mouse_down_thumb_position + mpos.y - mpos_left_down.y, 0, rack_window.h - the_rack.scrollbar.thumb_size);
    int new_scroll_position = scrollbar_thumb_position_to_scroll_position(new_thumb_position);
    set_scroll_position(new_scroll_position);
  }
  else
  {
    rect rack_window = get_rack_window();
    rect thumb_rect = get_scrollbar_thumb_rect(rack_window, &the_rack.scrollbar);

    if (inside_rect(thumb_rect, mpos))
    {
      if (the_rack.scrollbar.thumb_hover == 0)
      {
        the_rack.scrollbar.thumb_hover = 1;
        redisp = true;
      }
    }
    else
    {
      if (the_rack.scrollbar.thumb_hover == 1)
      {
        the_rack.scrollbar.thumb_hover = 0;
        redisp = true;
      }
    }

  }

  if (redisp)
    redisplay();
}

void mouse_scroll_func(GLFWwindow *window, double xoffset, double yoffset)
{
  rect rack_window = get_rack_window();

  if (inside_rect(rack_window, mpos))
  {
    if (yoffset > 0.0)
    {
      set_scroll_position(the_rack.scroll_position - get_dimension(DIM_SCROLL_AMOUNT));
    }
    else if (yoffset < 0.0)
    {
      set_scroll_position(the_rack.scroll_position + get_dimension(DIM_SCROLL_AMOUNT));
    }
  }
  else if (inside_rect(get_keyboard_screen_rect(), mpos))
  {
    if (yoffset < 0.0)
      keyboard_display_offset += 0.2 * get_dimension(DIM_KEYBOARD_KEY_WHITE_WIDTH);
    if (yoffset > 0.0)
      keyboard_display_offset -= 0.2 * get_dimension(DIM_KEYBOARD_KEY_WHITE_WIDTH);

    keyboard_display_offset = clamp(keyboard_display_offset, 0.0, 8 * 7 * get_dimension(DIM_KEYBOARD_KEY_WHITE_WIDTH) - window_width);

    redisplay();
  }
}

void key_func(GLFWwindow *window, int key, int scancode, int action, int mods)
{
  const char *k;

  struct {
    int key;
    int note;
  } keys[] = {
    {GLFW_KEY_Z, 0},
    {GLFW_KEY_S, 1},
    {GLFW_KEY_X, 2},
    {GLFW_KEY_D, 3},
    {GLFW_KEY_C, 4},
    {GLFW_KEY_V, 5},
    {GLFW_KEY_G, 6},
    {GLFW_KEY_B, 7},
    {GLFW_KEY_H, 8},
    {GLFW_KEY_N, 9},
    {GLFW_KEY_J, 10},
    {GLFW_KEY_M, 11},
    {GLFW_KEY_COMMA, 12},
    {GLFW_KEY_L, 13},
    {GLFW_KEY_PERIOD, 14},
    {GLFW_KEY_SEMICOLON, 15},
    {GLFW_KEY_SLASH, 16},

    {GLFW_KEY_Q, 12},
    {GLFW_KEY_2, 13},
    {GLFW_KEY_W, 14},
    {GLFW_KEY_3, 15},
    {GLFW_KEY_E, 16},
    {GLFW_KEY_R, 17},
    {GLFW_KEY_5, 18},
    {GLFW_KEY_T, 19},
    {GLFW_KEY_6, 20},
    {GLFW_KEY_Y, 21},
    {GLFW_KEY_7, 22},
    {GLFW_KEY_U, 23},
    {GLFW_KEY_I, 24},
    {GLFW_KEY_9, 25},
    {GLFW_KEY_O, 26},
    {GLFW_KEY_0, 27},
    {GLFW_KEY_P, 28},
    {GLFW_KEY_LEFT_BRACKET, 29},
    {GLFW_KEY_EQUAL, 30},
    {GLFW_KEY_RIGHT_BRACKET, 31},
  };

  for (int i = 0; i < ARRAY_SIZE(keys); i++)
  {
    if (keys[i].key == key && (action == GLFW_PRESS  || action == GLFW_RELEASE))
    {
      int newstate = action == GLFW_RELEASE ? 0 : 1;
      int velocity = 64;
      int octave = 4;
      int note = octave * 12 + keys[i].note;
      if (keyboard_state[note] != newstate)
      {
        keyboard_input(note, newstate, velocity);
        keyboard_state[note] = newstate;
        redisplay();
      }

      return;
    }
  }

  if (action == GLFW_RELEASE) /* ignore key up */
  {
    return;
  }

  switch (key)
  {
    case GLFW_KEY_BACKSPACE:
    case GLFW_KEY_ESCAPE: /* escape */
      {
        /* clear all input */
        for (int i = 0; i < ARRAY_SIZE(keyboard_state); i++)
        {
          if (keyboard_state[i])
          {
            keyboard_input(i, 0, 64);
            keyboard_state[i] = 0;
            redisplay();
          }
        }
      }
      break;
    case GLFW_KEY_ENTER:
    case GLFW_KEY_KP_ENTER:
      /* clear search */
      redisplay();
      break;
    case GLFW_KEY_UP:
      set_scroll_position(the_rack.scroll_position - get_dimension(DIM_SCROLL_AMOUNT));
      break;
    case GLFW_KEY_DOWN:
      set_scroll_position(the_rack.scroll_position + get_dimension(DIM_SCROLL_AMOUNT));
      break;
    case GLFW_KEY_PAGE_UP:
      set_scroll_position(the_rack.scroll_position - 100*get_dimension(DIM_SCROLL_AMOUNT));
      break;
    case GLFW_KEY_PAGE_DOWN:
      set_scroll_position(the_rack.scroll_position + 100*get_dimension(DIM_SCROLL_AMOUNT));
      break;
    case GLFW_KEY_HOME:
      set_scroll_position(0);
      break;
    case GLFW_KEY_END:
      set_scroll_position(1000000000);
      break;
    case GLFW_KEY_SPACE:
      break;
    case GLFW_KEY_TAB:
      the_rack.show_back = !the_rack.show_back;
      redisplay();
      break;
    case GLFW_KEY_C: /* ctrl-c */
    case GLFW_KEY_D: /* ctrl-d */
      if (mods & GLFW_MOD_CONTROL)
        exit_program(EXIT_SUCCESS);
      break;
    default:
      k = glfwGetKeyName(key, scancode);
      if (k == NULL)
        break;

      if (!strcmp(k, "c") || !strcmp(k, "d"))
      {
        if (mods & GLFW_MOD_CONTROL)
          exit_program(EXIT_SUCCESS);
      }

      break;
  }
}

void char_func(GLFWwindow *window, unsigned int codepoint)
{

  if (codepoint < 0x80)
  {
  }

  if (codepoint >= 0x80)
    return;

  int key = codepoint & 0xff;
  switch (key)
  {
    case 'q':
    case 'Q':
      break;
    default:
      break;
  }
  if (codepoint < 0x80)
  {
    redisplay();
  }
}

int parse_line(char *line, char *name_out, char *value_out)
{
  /* eat beginning whitespace */
  while ((*line && ((*line == ' ') || (*line == '\t'))))
    line++;

  if (*line == '\0')
    return -1;

  if (*line == '\n')
    return -1; /* empty line */

  if (*line == '#') /* comment */
    return -2;

  char *value = strchr(line, ':');
  if (value == NULL)
    return -3;

  *value = 0;
  value++;

  char *space_pos = strchr(line, ' ');
  if (space_pos != NULL)
  {
    *space_pos = 0;
  }

  while ((*value && ((*value == ' ') || (*value == '\t'))))
    value++;

  if (*value == '\0')
    return -1;

  if (*value == '\n')
    return -1; /* empty line */

  /* trim whitespace on the end of value */
  char *end = value + strlen(value) - 1;

  while (*end && ((*end == '\n') || (*end == ' ')))
  {
    *end = 0;
    end--;
  }

  if (name_out)
  {
    strcpy(name_out, line);
  }

  if (value_out)
  {
    strcpy(value_out, value);
  }

  return 0;
}

void load_settings(void)
{
  char settings_filename[512];
  FILE *f = NULL;

  char *xdg_home = getenv("XDG_CONFIG_HOME");
  if (xdg_home)
  {
    snprintf(settings_filename, sizeof(settings_filename), "%s/audiomix/audiomixrc", xdg_home);
    f = fopen(settings_filename, "rb");
  }

  if (f == NULL)
  {
    char *home = getenv("HOME");
    snprintf(settings_filename, sizeof(settings_filename), "%s/.config/audiomix/audiomixrc", home);
    f = fopen(settings_filename, "rb");
  }

  if (f == NULL)
  {
    char *home = getenv("HOME");
    snprintf(settings_filename, sizeof(settings_filename), "%s/.audiomixrc", home);
    f = fopen(settings_filename, "rb");
  }

  if (f == NULL)
  {
    return;
  }

  char line[1024];

  while (!feof(f))
  {
    line[0] = 0;
    if (fgets(line, sizeof(line), f) != NULL)
    {
      char name[256];
      char value[256];
      if (parse_line(line, name, value) == 0)
      {
        if (strcmp(name, "font") == 0)
        {
          strcpy(settings.font_file, value);
        }
        else if (strcmp(name, "font_size") == 0)
        {
          settings.font_size = atoi(value);
        }
        else if (strcmp(name, "gui_scale") == 0)
        {
          char *end = NULL;
          double val = strtod(value, &end);
          if ((end == NULL) || (end == value))
          {
            fprintf(stderr, "Failed to read value: \"%s\" from config file.\n", value);
          }
          else
          {
            settings.gui_scale = val;
          }
        }
      }
    }
  }

  fclose(f);
}

void glfw_error_callback(int error, const char* desc)
{
  fprintf(stderr, "GLFW error: %s\n", desc);
}

int main(int argc, char *argv[])
{
  char window_title[2048];
  char tmp_filename[1024];
  const char *filename = NULL;
  int no_fork = 0;
  int local_file = 0;
  int ch;

  load_settings();

  const char *first_arg = NULL;
  const char *second_arg = NULL;

  while ((ch = getopt_long(argc, argv, "fhlV", longopts, NULL)) != -1)
  {
    switch (ch)
    {
      case 'f':
        no_fork = 1;
        break;
      case 'h':
        print_usage(argv[0]);
        exit(EXIT_SUCCESS);
      case 'l':
        local_file = 1;
        break;
      case 'V':
        printf("audiomix %d.%d.%d\n", VERSION_MAJOR, VERSION_MINOR, VERSION_PATCH);
        exit(EXIT_SUCCESS);
      default:
        fprintf(stderr, "Try 'audiomix --help' for more information.\n");
        exit(EXIT_FAILURE);
    }
  }
  argc -= optind;
  argv += optind;

  if (argc == 1)
    first_arg = argv[0];
  else if (argc == 2)
  {
    first_arg = argv[0];
    second_arg = argv[1];
  }
  else if (argc > 2)
  {
    fprintf(stderr, "audiomix: unexpected argument '%s'\n", argv[2]);
    fprintf(stderr, "Try 'audiomix --help' for more information.\n");
    exit(EXIT_FAILURE);
  }

  if (local_file == 1)
  {
    if (first_arg)
    {
      filename = second_arg ? second_arg : first_arg;
    }
    else
    {
      fprintf(stderr, "audiomix: option requires an argument -- '-l, --local-file'\n");
      fprintf(stderr, "Try 'audiomix --help' for more information.\n");
      exit(EXIT_FAILURE);
    }
  }
  else
  {
    if (first_arg)
    {
      filename = tmp_filename;
    }
  }

  keyboard_display_offset = 11.5 * get_dimension(DIM_KEYBOARD_KEY_WHITE_WIDTH);

  /* init font */
  init_freetype();

  strcpy(settings.font_file, "./data/font/trim.ttf");
  settings.gui_scale = 1.0;
  settings.font_size = 12.0;

  if (get_font_file(settings.font_file))
  {
    render_font_texture(0, settings.font_file, (int)(settings.gui_scale * settings.font_size));
    render_font_texture(1, settings.font_file, (int)(settings.gui_scale * 1.5 * settings.font_size));
  }
  else
  {
    fprintf(stderr, "Can't find or resolve font file/name: \"%s\"\n", settings.font_file);
  }

  if (filename == NULL)
  {
    strcpy(window_title, "audiomix");
  }

  if (!glfwInit())
  {
    fprintf(stderr, "Failed to init GLFW\n");
    exit(EXIT_FAILURE);
  }

  glfwSetErrorCallback(&glfw_error_callback);

  window = glfwCreateWindow(fitting_window_width(), fitting_window_height(), window_title, NULL, NULL);

  if (!window)
  {
    fprintf(stderr, "Failed to create a Window\n");
    glfwTerminate();
    exit(EXIT_FAILURE);
  }

  glfwMakeContextCurrent(window);

  glfwSetWindowSizeCallback(window, &window_size_func);
  glfwSetWindowRefreshCallback(window, &window_refresh_func);

  glfwSetMouseButtonCallback(window, &mouse_button_func);
  glfwSetCursorPosCallback(window, &mouse_pos_func);
  glfwSetScrollCallback(window, &mouse_scroll_func);

  glfwSetKeyCallback(window, &key_func);
  glfwSetCharCallback(window, &char_func);

  upload_font_textures();

  int w = 0;
  int h = 0;
  glfwGetWindowSize(window, &w, &h);
  window_size_func(window, w, h);
  redisplay_needed = true;

  init_rack();

  start_audio();

  while (!glfwWindowShouldClose(window))
  {
    if (redisplay_needed)
    {
      render();

      redisplay_needed = false;
    }

    glfwWaitEventsTimeout(0.01);
  }

  glfwDestroyWindow(window);

  glfwTerminate();

  deinit_audio();

  return 0;
}

