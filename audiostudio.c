/*
 * audiostudio.c
 *
 * Implementation of audio creation software
 *
 * Initial date: 2023-12-26 21:33 UTC+1:00
 *
 * Author: Ziga Lenarcic
 *
 * Public domain.
 */

#include <sys/time.h>
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

#include "ft2build.h"
#include FT_FREETYPE_H

#include "audiostudio.h"
#include "audio.c"

#define VERSION_MAJOR 0
#define VERSION_MINOR 0
#define VERSION_PATCH 1

#define FONT_CHAR_WIDTH 7
#define FONT_CHAR_HEIGHT 14

Rack the_rack;
Transport transport;
Instrument *midi_input_instrument;

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

enum {
  FONT_DEFAULT = 0,
  FONT_BIG = 1,
  FONT_TINY = 2,
};
FontData *fonts[12];

double scale = 1.0;
double font_size = 12.0;

GLFWwindow *window;

int window_width;
int window_height;

Slider *slider_drag;
Instrument *selected_instrument;

char tooltip[128];

Point mpos;
Point mpos_left_down;

bool redisplay_needed = false;

bool transport_visible = true;

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

Point point_add(Point p1, Point p2)
{
  return (Point){p1.x + p2.x, p1.y + p2.y};
}

Point mul_point(Point in, double scale)
{
  return (Point){in.x * scale, in.y * scale};
}

Point floor_point(Point in)
{
  return (Point){floorf(in.x), floorf(in.y)};
}

rect make_rect(double x, double y, double w, double h)
{
  return (rect){x, y, w, h};
}

rect make_rect_from_midpoint(Point mid, double w, double h)
{
  return (rect){mid.x - w * 0.5, mid.y - h * 0.5, w, h};
}

rect move_rect(rect in, Point off)
{
  return make_rect(in.x + off.x, in.y + off.y, in.w, in.h);
}

Point rect_midpoint(rect in)
{
  return (Point){in.x + 0.5 * in.w, in.y + 0.5 * in.h};
}

rect rect_grow(rect in, double amount_x, double amount_y)
{
  return make_rect(in.x - amount_x, in.y - amount_y, in.w + 2 * amount_x, in.h + 2 * amount_y);
}

bool inside_rect(rect in, Point pos)
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

#define DIM_RACK_WIDTH 640.0
#define DIM_ROTARY_RANGE 150.0
#define DIM_SCROLLBAR_THUMB_MIN_HEIGHT 5.0
#define DIM_SCROLLBAR_THUMB_MARGIN 2.0 // 2.0
#define DIM_SCROLLBAR_WIDTH 17.0 //14.0
#define DIM_SCROLLBAR_MARGIN 6.0
#define DIM_SCROLL_AMOUNT 22.0
#define DIM_RACK_MARGIN 25.0
#define DIM_RACK_VERTICAL_MARGIN 32.0 // 35.0
#define DIM_RACK_FADE_MARGIN 15.0
#define DIM_TEXT_HORIZONTAL_MARGIN 5.0
#define DIM_SCROLL_OVERLAP 5.0
#define DIM_KEYBOARD_KEY_WHITE_WIDTH 20.0
#define DIM_KEYBOARD_KEY_WHITE_HEIGHT 100.0
#define DIM_TRANSPORT_HEIGHT 50.0
#define DIM_BUTTON_SPACING 5.0

double get_dim(double dim)
{
  return scale * dim;
}

double rack_height_unit(double units)
{
  /* rack width = 19", 1U = 1.75" tall */
  return units * (int)(get_dim(DIM_RACK_WIDTH) / 19.0 * 1.75);
}

rect get_rack_window(void)
{
  if (transport_visible)
  return make_rect(get_dim(DIM_RACK_MARGIN), get_dim(DIM_RACK_VERTICAL_MARGIN),
      get_dim(DIM_RACK_WIDTH) + get_dim(DIM_SCROLLBAR_WIDTH), window_height - 2 * get_dim(DIM_RACK_VERTICAL_MARGIN) - get_dim(DIM_KEYBOARD_KEY_WHITE_HEIGHT) - get_dim(DIM_TRANSPORT_HEIGHT));
  else
  return make_rect(get_dim(DIM_RACK_MARGIN), get_dim(DIM_RACK_VERTICAL_MARGIN),
      get_dim(DIM_RACK_WIDTH) + get_dim(DIM_SCROLLBAR_WIDTH), window_height - 2 * get_dim(DIM_RACK_VERTICAL_MARGIN) - get_dim(DIM_KEYBOARD_KEY_WHITE_HEIGHT));
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

  the_rack.scrollbar.thumb_size = clamp(thumb_size_tmp, get_dim(DIM_SCROLLBAR_THUMB_MIN_HEIGHT), rack_window.h);
  //the_rack.scrollbar.thumb_position = round((double)the_rack.scroll_position / (doc_height - 1) * rack_window.h);
  the_rack.scrollbar.thumb_position = round((double)the_rack.scroll_position / (doc_height - rack_window.h) * (rack_window.h - the_rack.scrollbar.thumb_size));
}

int scrollbar_thumb_position_to_scroll_position(int thumb_position)
{
  rect rack_window = get_rack_window();
  int doc_height = document_height(rack_window);
  int thumb_size_tmp = (double)rack_window.h / (doc_height - 1) * rack_window.h;

  the_rack.scrollbar.thumb_size = clamp(thumb_size_tmp, get_dim(DIM_SCROLLBAR_THUMB_MIN_HEIGHT), rack_window.h);

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
  return get_dim(DIM_RACK_WIDTH) + 2 * get_dim(DIM_RACK_MARGIN);
}

int fitting_window_height(void)
{
  return 1024;
}

#define RGBA(r, g, b, a) (((a) << 24) | ((b) << 16) | ((g) << 8) | (r))
#define RGBAF(r, g, b, a) (((CLAMP_BYTE(a * 255.0)) << 24) | ((CLAMP_BYTE(b * 255.0)) << 16) | ((CLAMP_BYTE(g * 255.0)) << 8) | (CLAMP_BYTE(r * 255.0)))
#define GRAY(x) RGBAF((x), (x), (x), 1.0)
#define RED(c) ((int)(((c) & 0xff)))
#define GREEN(c) ((int)(((c) & 0xff00) >> 8))
#define BLUE(c) ((int)(((c) & 0xff0000) >> 16))
#define ALPHA(c) ((int)(((c) & 0xff000000) >> 24))
#define CLAMP_BYTE(x) (((x) < 0) ? (uint8_t)0 : (((x) > 255) ? (uint8_t)255 : (uint8_t)(x)))

Color set_color(Color color)
{
  glColor4ubv((uint8_t *)&color);
  return color;
}

void set_color_alpha(Color color, float alpha)
{
  glColor4ub(RED(color), GREEN(color), BLUE(color), CLAMP_BYTE((int)(alpha * 255.0f)));
}

Color color_brightness(Color color, float amount)
{
  return RGBA(CLAMP_BYTE(RED(color) + amount * 255.0f),
      CLAMP_BYTE(GREEN(color) + amount * 255.0f),
      CLAMP_BYTE(BLUE(color) + amount * 255.0f),
      ALPHA(color));
}

Color color_multiply(Color color, float amount)
{
  return RGBA(CLAMP_BYTE(RED(color) * amount),
      CLAMP_BYTE(GREEN(color) * amount),
      CLAMP_BYTE(BLUE(color) * amount),
      ALPHA(color));
}

Color color_with_alpha(Color color, float alpha)
{
  return RGBA(RED(color), GREEN(color), BLUE(color), CLAMP_BYTE((int)(255.0 * alpha)));
}

struct timeval tv_render;
Color color_main = RGBAF(0, 0, 0.5, 1.0);
Color color_select = RGBAF(1.0, 0.0, 0.5, 1.0);

double timeval_difference_sec(struct timeval *tv_start, struct timeval *tv_end)
{
  return (double)(tv_end->tv_sec - tv_start->tv_sec) + 1.0e-6 * ((double)((int)tv_end->tv_usec - (int)tv_start->tv_usec));
}

void draw_rect(rect in)
{
  glBegin(GL_TRIANGLE_STRIP);
  glVertex2i(in.x, in.y);
  glVertex2i(in.x + in.w, in.y);
  glVertex2i(in.x, in.y + in.h);
  glVertex2i(in.x + in.w, in.y + in.h);
  glEnd();
}

void draw_line(Point in, Point in2)
{
  glBegin(GL_LINE_STRIP);
  glVertex2i(in.x, in.y);
  glVertex2i(in2.x, in2.y);
  glEnd();
}

void draw_rect_with_colors(rect in, Color top_left, Color top_right, Color bottom_left, Color bottom_right)
{
  glBegin(GL_TRIANGLE_STRIP);
  set_color(top_left);
  glVertex2i(in.x, in.y);
  set_color(top_right);
  glVertex2i(in.x + in.w, in.y);
  set_color(bottom_left);
  glVertex2i(in.x, in.y + in.h);
  set_color(bottom_right);
  glVertex2i(in.x + in.w, in.y + in.h);
  glEnd();
}

void draw_rect_outline(rect in)
{
  glTranslatef(0.5, 0.5, 0); /* fix missing pixel in the corner */
  in.w -= 1; /* to match normal quads */
  in.h -= 1;
  glBegin(GL_LINE_STRIP);
  glVertex2i(in.x, in.y);
  glVertex2i(in.x + in.w, in.y);
  glVertex2i(in.x + in.w, in.y + in.h);
  glVertex2i(in.x, in.y + in.h);
  glVertex2i(in.x, in.y);
  glEnd();
  glTranslatef(-0.5, -0.5, 0);
}

/*
   !"#$%&'()*+,-./
0123456789:;<=>?
@ABCDEFGHIJKLMNO
PQRSTUVWXYZ[\]^_
`abcdefghijklmno
pqrstuvwxyz{|}~
*/

int put_char_gl(FontData *font, int x, int y, char c)
{
  int ret = 0;
  int w = FONT_CHAR_WIDTH;
  int h = FONT_CHAR_HEIGHT;

  if (!font)
    return -1;

  if (c < 32)
  {
    // unknown character
    glDisable(GL_BLEND);
    draw_rect_outline(make_rect(x + 1, y + 1, w - 2, h - 2));
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
      draw_rect_outline(make_rect(x + 1, y + 1, w - 2, h - 2));
      glEnable(GL_BLEND);
      ret = font->character_width;
    }
  }

  return ret;
}

int get_char_advance(FontData *font, char c)
{
  int ret = 0;
  int w = FONT_CHAR_WIDTH;
  int h = FONT_CHAR_HEIGHT;

  if (!font)
    return -1;

  if (c < 32)
  {
    // unknown character
  }
  else
  {
    int idx = (int)c;
    if (font->chars[idx].available)
    {
      //int w = font->chars[idx].width;
      //int h = font->chars[idx].height;
      //int x_start = x + font->chars[idx].left;
      //int y_start = y - font->chars[idx].top + font->character_height + 2;

      ret = font->chars[idx].advance;
    }
    else
    {
      ret = font->character_width;
    }
  }

  return ret;
}

size_t get_string_size(int idx_font, const char *str, double *width, double *height)
{
  size_t count = 0;
  *width = 0;
  *height = 0;

  FontData *font = fonts[idx_font];
  if (!font)
    return -1;

  *height = font->character_height;

  while (*str)
  {
    count++;
    *width += get_char_advance(font, *str);
    str++;
  }

  return count;
}

size_t draw_string(int idx_font, int x, int y, const char *str)
{
  size_t count = 0;

  FontData *font = fonts[idx_font];
  if (!font)
    return -1;

  glBindTexture(GL_TEXTURE_2D, font->texture_id);
  glEnable(GL_BLEND);
  glEnable(GL_TEXTURE_2D);

  while (*str)
  {
    count++;
    x += put_char_gl(font, x, y, *str);
    str++;
  }

  glDisable(GL_TEXTURE_2D);
  glDisable(GL_BLEND);

  return count;
}

size_t draw_string_centered(int idx_font, int x, int y, const char *str)
{
  double width;
  double height;

  get_string_size(idx_font, str, &width, &height);

  return draw_string(idx_font, x - 0.5 * width, y - 0.5 * height, str);
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

#define KEYBOARD_NUM_OCTAVES 10

char gui_keyboard_state[256];
double keyboard_display_offset;

bool black_keys[12] = {false, true, false, true, false, false, true, false, true, false, true, false };
double key_pos[12] = {0, 0.5, 1, 1.5, 2, 3, 3.5, 4, 4.5, 5, 5.5, 6};

rect get_keyboard_key_rect(int key)
{
  double x = (((key / 12) * 7) + key_pos[key % 12]) * get_dim(DIM_KEYBOARD_KEY_WHITE_WIDTH);

  if (black_keys[key % 12])
  {
    double w = get_dim(DIM_KEYBOARD_KEY_WHITE_WIDTH) * 0.6f;
    return make_rect(x - keyboard_display_offset + 0.5 * get_dim(DIM_KEYBOARD_KEY_WHITE_WIDTH) - 0.5 * w, 0, w, get_dim(DIM_KEYBOARD_KEY_WHITE_HEIGHT) * 0.6);
  }
  else
    return make_rect(x - keyboard_display_offset, 0, get_dim(DIM_KEYBOARD_KEY_WHITE_WIDTH), get_dim(DIM_KEYBOARD_KEY_WHITE_HEIGHT));
}

Point get_keyboard_screen_pos(void)
{
  return (Point){0, window_height - get_dim(DIM_KEYBOARD_KEY_WHITE_HEIGHT)};
}

rect get_keyboard_screen_rect(void)
{
  return make_rect(0, window_height - get_dim(DIM_KEYBOARD_KEY_WHITE_HEIGHT), window_width, get_dim(DIM_KEYBOARD_KEY_WHITE_HEIGHT));
}

bool keyboard_keyboard_hit_test(Point pos, int *key)
{
  Point pos2 = floor_point(pos);
  for (int pass = 0; pass <= 1; pass++)
  {
    for (int i = 0; i < (KEYBOARD_NUM_OCTAVES * 12 + 1); i++)
    { 
      if ((int)black_keys[i % 12] ^ pass)
      {
        Point off = get_keyboard_screen_pos();

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

enum {
  MAP_LINEAR = 0,
  MAP_EXP = 1,
  MAP_LOG = 2,
  MAP_SQ = 3,
  MAP_SQRT = 4,
};

double lin_map(double x)
{
  return x;
}

double exp_map(double x)
{
  return (exp(x) - 1.0)/(M_E - 1.0);
}

double log_map(double x)
{
  return log((M_E - 1.0) * x + 1.0);
}

double sq_map(double x)
{
  return x * x;
}

double sqrt_map(double x)
{
  return sqrt(x);
}

typedef double (* map_func)(double);

map_func maps[][2] = {{&lin_map, &lin_map}, {&exp_map, &log_map}, {&log_map, &exp_map},
  {&sq_map, &sqrt_map}, {&sqrt_map, &sq_map}};

double relative_to_absolute(double rel_val, double min, double max, int curve)
{
  return min + (max - min) * maps[curve][0](rel_val);
}

double absolute_to_relative(double absolute, double min, double max, int curve)
{
  double rel_lin = (absolute - min) / (max - min);

  return clamp(maps[curve][1](rel_lin), 0.0, 1.0);
}

double slider_thumb_off(Slider *s, double rel_val)
{
  switch (s->style)
  {
    case SLIDER_STYLE_HORIZONTAL:
      return rel_val * (s->pos.w - s->thumb_size.x);
      break;
    case SLIDER_STYLE_VERTICAL:
      return rel_val * (s->pos.h - s->thumb_size.y);
      break;
    case SLIDER_STYLE_ROTARY:
      return rel_val * get_dim(DIM_ROTARY_RANGE);
      break;
    case SLIDER_STYLE_TOGGLE_SWITCH:
    default:
      return rel_val;
      break;
  }
}

double slider_rel_val(Slider *s, double thumb_off)
{
  switch (s->style)
  {
    case SLIDER_STYLE_HORIZONTAL:
      return clamp(thumb_off / (s->pos.w - s->thumb_size.x), 0.0, 1.0);
      break;
    case SLIDER_STYLE_VERTICAL:
      return clamp(thumb_off / (s->pos.h - s->thumb_size.y), 0.0, 1.0);
      break;
    case SLIDER_STYLE_ROTARY:
      return clamp(thumb_off / get_dim(DIM_ROTARY_RANGE), 0.0, 1.0);
      break;
    case SLIDER_STYLE_TOGGLE_SWITCH:
    default:
      break;
  }
}

double slider_value_to_screen_pos(Slider *s, double value)
{
  double rel_val = absolute_to_relative(value, s->min, s->max, s->curve);
  return slider_thumb_off(s, rel_val);
}

double slider_screen_pos_to_value(Slider *s, double pos)
{
  double rel_pos = slider_rel_val(s, pos);
  if (s->discrete)
    return clamp(round(relative_to_absolute(rel_pos, s->min, s->max, s->curve)), s->min, s->max);
  else
    return relative_to_absolute(rel_pos, s->min, s->max, s->curve);
}

double slider_get_string_value(Slider *s, char *dst, size_t dst_size)
{
  if (s->discrete && s->string_values)
  {
    int val = (int)s->value;
    bool valid = true;
    for (int i = 0; i <= val; i++)
    {
      if (!s->string_values[i])
      {
        valid = false;
        break;
      }
    }

    if (valid)
      snprintf(dst, dst_size, "%s", s->string_values[val]);
    else
      snprintf(dst, dst_size, "%d", (int)s->value);
  }
  else if (s->discrete)
  {
    snprintf(dst, dst_size, "%d", (int)s->value);
  }
  else
  {
    snprintf(dst, dst_size, "%0.6f", s->value);
  }

  return s->value;
}

rect slider_thumb_rect(Slider *s)
{
  switch (s->style)
  {
    case SLIDER_STYLE_HORIZONTAL:
    case SLIDER_STYLE_VERTICAL:
    default:
      {
        double rel_pos = absolute_to_relative(s->value, s->min, s->max, s->curve);
        double thumb_off = slider_thumb_off(s, rel_pos);

        if (s->style == SLIDER_STYLE_HORIZONTAL)
          return make_rect(s->pos.x + thumb_off, s->pos.y + 0.5 * (s->pos.h - s->thumb_size.y), s->thumb_size.x, s->thumb_size.y);
        else
          return make_rect(s->pos.x + 0.5 * (s->pos.w - s->thumb_size.x), s->pos.y + s->pos.h - thumb_off, s->thumb_size.x, s->thumb_size.y);
      }
      break;
    case SLIDER_STYLE_ROTARY:
    case SLIDER_STYLE_TOGGLE_SWITCH:
    case SLIDER_STYLE_RADIO_BUTTON:
      return make_rect(s->pos.x, s->pos.y, s->pos.w, s->pos.h);
      break;
  }
}

void draw_slider_generic(Slider *slider, Point off)
{
  rect r = move_rect(slider->pos, off);
  double rel_pos = absolute_to_relative(slider->value, slider->min, slider->max, slider->curve);

  switch (slider->style)
  {
    case SLIDER_STYLE_HORIZONTAL:
    case SLIDER_STYLE_VERTICAL:
    default:
      {
        set_grey(0.0);
        draw_rect(r);
        set_grey(0.5);
        double thumb_off = slider_thumb_off(slider, rel_pos);
        if (slider->style == SLIDER_STYLE_HORIZONTAL)
          draw_rect(make_rect(r.x, r.y, thumb_off, r.h));
        else
          draw_rect(make_rect(r.x, r.y + r.h - thumb_off, r.w, thumb_off));
        rect r2 = move_rect(slider_thumb_rect(slider), off);
        set_grey(1.0);
        draw_rect(r2);
      }
      break;
    case SLIDER_STYLE_ROTARY:
      {
        set_grey(0.5);
        draw_line((Point){r.x, r.y + 0.5 * r.h}, (Point){r.x + r.w, r.y + 0.5 * r.h});
        draw_line((Point){r.x + 0.5 * r.w, r.y}, (Point){r.x + 0.5 * r.w, r.y + r.h});

        Point p1 = point_add(rect_midpoint(slider->pos), off);
        double size = MIN(slider->pos.w, slider->pos.h);
        double angle = slider->rotary_start - rel_pos * slider->rotary_range;
        
        Point p2 = (Point){p1.x + cos(angle) * size / 2.0,
          p1.y - sin(angle) * size / 2.0};

        set_grey(1.0);
        draw_line(p1, p2);
      }
      break;
    case SLIDER_STYLE_TOGGLE_SWITCH:
      {
        set_grey(0.5);
        draw_rect_outline(r);
        if (slider->value > 0)
        {
          draw_line((Point){r.x, r.y}, (Point){r.x + r.w, r.y + r.h});
          draw_line((Point){r.x + r.w, r.y}, (Point){r.x, r.y + r.h});
        }
      }
      break;
    case SLIDER_STYLE_RADIO_BUTTON:
      {
        set_grey(0.5);
        //draw_rect_outline(r);

        int num_choices = (int)(slider->max - slider->min) + 1;
        for (int i = 0; i < num_choices; i++)
        {
          rect r_choice = make_rect(r.x, r.y + i * r.h / num_choices, r.w, 
r.h / num_choices);
          Point p = rect_midpoint(r_choice);

#define RADIO_OFF 10
          if ((int)(slider->value - slider->min) == i)
            set_color(RGBAF(1.0, 0.0, 0.0, 1.0));
          else
            set_grey(0.7);
          draw_rect(make_rect_from_midpoint((Point){r_choice.x + RADIO_OFF, p.y}, 3, 3));

          set_grey(0.7);
          if (slider->string_values)
          {
            draw_string(FONT_TINY, r_choice.x + RADIO_OFF + 5, r_choice.y + 2, slider->string_values[i]);
          }
          else
          {
            char tmp[10];
            snprintf(tmp, sizeof(tmp), "%2d", (int)slider->min + i);
            draw_string(FONT_TINY, r_choice.x + RADIO_OFF + 5, r_choice.y + 2, tmp);
          }
        }
      }
      break;
    case SLIDER_STYLE_TRANSPORT_BUTTON:
      {
        set_grey(0.5);
        draw_rect_outline(r);
        if (slider->value > 0)
        {
          draw_line((Point){r.x, r.y}, (Point){r.x + r.w, r.y + r.h});
          draw_line((Point){r.x + r.w, r.y}, (Point){r.x, r.y + r.h});
        }
        {
          set_grey(0.0);
          Point p = rect_midpoint(r);
          draw_string_centered(0, p.x, p.y, slider->name);
        }
      }
      break;
  }

}

void draw_instrument(Instrument *inst, bool back, Point off)
{
  Color color = inst->background_color;

  if (back)
  {
    rect r = move_rect(make_rect(0, 0, get_dim(DIM_RACK_WIDTH), inst->height), off);
    set_color(color_brightness(color, -0.2));
    draw_rect(r);
    set_color(color_brightness(color, 0.0));
    draw_rect_outline(r);

    glColor4f(1.0, 1.0, 1.0, 0.2);
    draw_string(1, off.x + 10, off.y + 10, inst->name);

    if (inst->num_inputs > 0)
    {
      for (int i = 0; i < inst->num_inputs; i++)
      {
        rect r = move_rect(inst->inputs[i].pos, off);
        set_grey(1.0);
        draw_rect_outline(r);
      }
    }

    for (int i = 0; i < inst->num_outputs; i++)
    {
      rect r = move_rect(inst->outputs[i].pos, off);
      set_grey(0.7);
      draw_rect_outline(r);
    }
  }
  else
  {
    rect r = move_rect(make_rect(0, 0, get_dim(DIM_RACK_WIDTH), inst->height), off);
    set_color(color);
    draw_rect(r);
    set_color(color_brightness(color, 0.2));
    draw_rect_outline(r);

    for (int i = 0; i < inst->slider_count; i++)
      draw_slider_generic(&inst->sliders[i], off);

    glColor4f(1.0, 1.0, 1.0, 0.5);
    draw_string(1, off.x + 10, off.y + 10, inst->name);
  }
}

void draw_io_device(Instrument *inst, bool back, Point off)
{
  draw_instrument(inst, back, off);


  Color color = inst->background_color;

  if (back)
  {
  }
  else
  {
    rect r = move_rect(make_rect(0, 0, get_dim(DIM_RACK_WIDTH), inst->height), off);
    //set_color(color);
    //draw_rect(r);
    set_color(color_brightness(color, 0.5));
    //draw_rect_outline(r);

    char tmp[64];
    snprintf(tmp, sizeof(tmp), "%d Hz", (int)sample_rate);
    draw_string(FONT_TINY, off.x + 150, off.y + 30, tmp);
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
  double min, double max, double value, int curve, int discrete, const char **string_values, rect pos, Point thumb_size, int style, struct Instrument_ *inst)
{
  strcpy(slider->name, name);
  slider->min = min;
  slider->max = max;
  slider->value = value;
  slider->curve = curve;
  slider->discrete = discrete;
  slider->string_values = string_values;
  slider->pos = pos;
  slider->thumb_size = thumb_size;
  slider->style = style;

  slider->rotary_start = (225.0 / 180.0) * M_PI;
  slider->rotary_range = (270.0 / 180.0) * M_PI;

  slider->value_start_drag = 0.0;
  slider->inst = inst;
  slider->callback = NULL;
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
  inst->height = rack_height_unit(5);
  inst->draw = &draw_instrument;
  inst->process_midi = &process_midi_synth;
  inst->process_audio = &process_audio_synth;

  inst->background_color = color_main;

  inst->num_inputs = 0;

  inst->specific_data = calloc(1, sizeof(struct synth_data));
  struct synth_data *data = (struct synth_data *)inst->specific_data;
  for (int i = 0; i < MAX_SYNTH_POLYPHONY; i++)
    data->note[i] = -1;

  inst->num_outputs = 2;
  init_connection(&inst->outputs[0], 0, false, (rect){510, 10, 10, 10}, inst);
  init_connection(&inst->outputs[1], 1, false, (rect){530, 10, 10, 10}, inst);

  int osc_gui_width = 60;
  int osc_x_pos = 20;

  static const char *osc_shape_names[] = {"Saw", "Square", "Triangle", "Sine", NULL};

  inst->slider_count = SYNTH_SLIDER_COUNT;

  //init_slider(&inst->sliders[SYNTH_OSC1_SHAPE], "Osc 1 Shape", 0.0, 3.0, 0.0, MAP_LINEAR, 1, osc_shape_names, (rect){osc_x_pos, 50, osc_gui_width, 10}, (Point){10, 10}, SLIDER_STYLE_HORIZONTAL, inst);
  init_slider(&inst->sliders[SYNTH_OSC1_SHAPE], "Osc 1 Shape", 0.0, 3.0, 0.0, MAP_LINEAR, 1, osc_shape_names, (rect){osc_x_pos, 50, osc_gui_width, 70}, (Point){10, 10}, SLIDER_STYLE_RADIO_BUTTON, inst);

  inst->sliders[SYNTH_OSC1_SHAPE].rotary_start = (150.0 / 180.0) * M_PI;
  inst->sliders[SYNTH_OSC1_SHAPE].rotary_range = (120.0 / 180.0) * M_PI;

  init_slider(&inst->sliders[SYNTH_OSC1_OCTAVE], "Osc 1 Octave", -2.0, 2.0, 0.0, MAP_LINEAR, 1, NULL, (rect){osc_x_pos, 130, osc_gui_width, 60}, (Point){10, 10}, SLIDER_STYLE_RADIO_BUTTON, inst);
  init_slider(&inst->sliders[SYNTH_OSC1_SEMITONE], "Osc 1 Semitone", -12.0, 12.0, 0.0, MAP_LINEAR, 1, NULL, (rect){osc_x_pos, 150, osc_gui_width, 10}, (Point){10, 10}, SLIDER_STYLE_HORIZONTAL, inst);
  init_slider(&inst->sliders[SYNTH_OSC1_DETUNE], "Osc 1 Detune", -50.0, 50.0, 0.0, MAP_LINEAR, 0, NULL, (rect){osc_x_pos, 190, osc_gui_width, 10}, (Point){10, 10}, SLIDER_STYLE_HORIZONTAL, inst);
  init_slider(&inst->sliders[SYNTH_OSC1_VOICES], "Osc 1 Voices", 1.0, MAX_DETUNE_VOICES, 1.0, MAP_LINEAR, 1, NULL, (rect){osc_x_pos, 230, osc_gui_width, 10}, (Point){10, 10}, SLIDER_STYLE_HORIZONTAL, inst);
  init_slider(&inst->sliders[SYNTH_OSC1_VOICES_DETUNE], "Osc 1 Voices Detune", 0.0, 100.0, 10.0, MAP_LINEAR, 0, NULL, (rect){osc_x_pos, 250, osc_gui_width, 10}, (Point){10, 10}, SLIDER_STYLE_HORIZONTAL, inst);

  osc_x_pos += osc_gui_width + 20;

  init_slider(&inst->sliders[SYNTH_OSC2_SHAPE], "Osc 2 Shape", 0.0, 3.0, 0.0, MAP_LINEAR, 1, osc_shape_names, (rect){osc_x_pos, 50, osc_gui_width, 10}, (Point){10, 10}, SLIDER_STYLE_HORIZONTAL, inst);

  inst->sliders[SYNTH_OSC2_SHAPE].rotary_start = (150.0 / 180.0) * M_PI;
  inst->sliders[SYNTH_OSC2_SHAPE].rotary_range = (120.0 / 180.0) * M_PI;

  init_slider(&inst->sliders[SYNTH_OSC2_OCTAVE], "Osc 2 Octave", -2.0, 2.0, 0.0, MAP_LINEAR, 1, NULL, (rect){osc_x_pos, 100, osc_gui_width, 10}, (Point){10, 10}, SLIDER_STYLE_HORIZONTAL, inst);
  init_slider(&inst->sliders[SYNTH_OSC2_SEMITONE], "Osc 2 Semitone", -12.0, 12.0, 0.0, MAP_LINEAR, 1, NULL, (rect){osc_x_pos, 150, osc_gui_width, 10}, (Point){10, 10}, SLIDER_STYLE_HORIZONTAL, inst);
  init_slider(&inst->sliders[SYNTH_OSC2_DETUNE], "Osc 2 Detune", -50.0, 50.0, 0.0, MAP_LINEAR, 0, NULL, (rect){osc_x_pos, 190, osc_gui_width, 10}, (Point){10, 10}, SLIDER_STYLE_HORIZONTAL, inst);
  init_slider(&inst->sliders[SYNTH_OSC2_VOICES], "Osc 2 Voices", 1.0, MAX_DETUNE_VOICES, 1.0, MAP_LINEAR, 1, NULL, (rect){osc_x_pos, 230, osc_gui_width, 10}, (Point){10, 10}, SLIDER_STYLE_HORIZONTAL, inst);
  init_slider(&inst->sliders[SYNTH_OSC2_VOICES_DETUNE], "Osc 2 Voices Detune", 0.0, 0.05, 0.01, MAP_LINEAR, 0, NULL, (rect){osc_x_pos, 250, osc_gui_width, 10}, (Point){10, 10}, SLIDER_STYLE_HORIZONTAL, inst);
  init_slider(&inst->sliders[SYNTH_OSC1_OSC2_VOLUME_RATIO], "Osc 1-2 Volume Ratio", 0.0, 1.0, 0.0, MAP_LINEAR, 0, NULL, (rect){osc_x_pos, 270, osc_gui_width, 10}, (Point){10, 10}, SLIDER_STYLE_HORIZONTAL, inst);

  osc_x_pos += osc_gui_width + 20;

  init_slider(&inst->sliders[SYNTH_OSC3_SHAPE], "Osc 3 Shape", 0.0, 3.0, 0.0, MAP_LINEAR, 1, osc_shape_names, (rect){osc_x_pos, 50, osc_gui_width, 10}, (Point){10, 10}, SLIDER_STYLE_HORIZONTAL, inst);

  inst->sliders[SYNTH_OSC3_SHAPE].rotary_start = (150.0 / 180.0) * M_PI;
  inst->sliders[SYNTH_OSC3_SHAPE].rotary_range = (120.0 / 180.0) * M_PI;

  init_slider(&inst->sliders[SYNTH_OSC3_OCTAVE], "Osc 3 Octave", -2.0, 2.0, 0.0, MAP_LINEAR, 1, NULL, (rect){osc_x_pos, 100, osc_gui_width, 10}, (Point){10, 10}, SLIDER_STYLE_HORIZONTAL, inst);
  init_slider(&inst->sliders[SYNTH_OSC3_SEMITONE], "Osc 3 Semitone", -12.0, 12.0, 0.0, MAP_LINEAR, 1, NULL, (rect){osc_x_pos, 150, osc_gui_width, 10}, (Point){10, 10}, SLIDER_STYLE_HORIZONTAL, inst);
  init_slider(&inst->sliders[SYNTH_OSC3_DETUNE], "Osc 3 Detune", -50.0, 50.0, 0.0, MAP_LINEAR, 0, NULL, (rect){osc_x_pos, 190, osc_gui_width, 10}, (Point){10, 10}, SLIDER_STYLE_HORIZONTAL, inst);
  init_slider(&inst->sliders[SYNTH_OSC3_VOICES], "Osc 3 Voices", 1.0, MAX_DETUNE_VOICES, 1.0, MAP_LINEAR, 1, NULL, (rect){osc_x_pos, 230, osc_gui_width, 10}, (Point){10, 10}, SLIDER_STYLE_HORIZONTAL, inst);
  init_slider(&inst->sliders[SYNTH_OSC3_VOICES_DETUNE], "Osc 3 Voices Detune", 0.0, 0.05, 0.01, MAP_LINEAR, 0, NULL, (rect){osc_x_pos, 250, osc_gui_width, 10}, (Point){10, 10}, SLIDER_STYLE_HORIZONTAL, inst);
  init_slider(&inst->sliders[SYNTH_OSC3_VOLUME_RATIO], "Osc 3 Volume Ratio", 0.0, 1.0, 0.0, MAP_LINEAR, 0, NULL, (rect){osc_x_pos, 270, osc_gui_width, 10}, (Point){10, 10}, SLIDER_STYLE_HORIZONTAL, inst);

  init_slider(&inst->sliders[SYNTH_FILTER_CUTOFF], "Filter", 10.0, 20000.0, 5000.0, MAP_EXP, 0, NULL, (rect){300, 30, 80, 80}, (Point){10, 10}, SLIDER_STYLE_ROTARY, inst);

  init_slider(&inst->sliders[SYNTH_VOLUME], "Volume", 0.0, 1.0, 0.2, MAP_LINEAR, 0, NULL, (rect){600, 20, 10, 150}, (Point){10, 10}, SLIDER_STYLE_VERTICAL, inst);
  return inst;
}

Instrument *make_io_device(void)
{
  Instrument *inst = AllocateInstrument();

  strcpy(inst->name, "IO Device");
  strcpy(inst->user_name, "IO");
  inst->height = rack_height_unit(1);
  inst->draw = &draw_io_device;
  inst->process_audio = &process_audio_io_device;

  inst->background_color = RGBAF(0.4, 0.4, 0.4, 1.0);

  inst->num_inputs = 2;
  init_connection(&inst->inputs[0], 0, true, (rect){10, 10, 10, 10}, inst);
  init_connection(&inst->inputs[1], 1, true, (rect){30, 10, 10, 10}, inst);

  inst->num_outputs = 0;

  inst->slider_count = 1;
  init_slider(&inst->sliders[0], "Volume", 0.0, 1.0, 0.8, 0, 0, NULL, (rect){200, 10, 100, 10}, (Point){10, 10}, SLIDER_STYLE_HORIZONTAL, inst);

  return inst;
}

Instrument *make_chorus(void)
{
  Instrument *inst = AllocateInstrument();

  strcpy(inst->name, "Chorus");
  strcpy(inst->user_name, "Chorus");
  inst->height = rack_height_unit(1);
  inst->draw = &draw_instrument;
  inst->process_audio = &process_audio_chorus;

  inst->background_color = color_main;

  inst->num_inputs = 2;
  init_connection(&inst->inputs[0], 0, true, (rect){10, 10, 10, 10}, inst);
  init_connection(&inst->inputs[1], 1, true, (rect){30, 10, 10, 10}, inst);


  inst->num_outputs = 2;
  init_connection(&inst->outputs[0], 0, false, (rect){10, 30, 10, 10}, inst);
  init_connection(&inst->outputs[1], 1, false, (rect){30, 30, 10, 10}, inst);

  inst->slider_count = 3;
  init_slider(&inst->sliders[0], "Rate", 0.2, 10.0, 1.0, 0, 0, NULL, (rect){10, 40, 100, 10}, (Point){10, 10}, SLIDER_STYLE_HORIZONTAL, inst);
  init_slider(&inst->sliders[1], "Depth", 0.2, 10.0, 1.0, 0, 0, NULL, (rect){150, 40, 100, 10}, (Point){10, 10}, SLIDER_STYLE_HORIZONTAL, inst);
  init_slider(&inst->sliders[2], "Mix", 0.0, 1.0, 0.0, 0, 0, NULL, (rect){280, 40, 100, 10}, (Point){10, 10}, SLIDER_STYLE_HORIZONTAL, inst);

  return inst;
}

void recalculate_rack_coordinates(void)
{
  Instrument *inst = the_rack.first;
  double height = 0;
  while (inst)
  {
    inst->rack_pos = make_rect(0, height, get_dim(DIM_RACK_WIDTH), inst->height);
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

enum {
  TRANSPORT_REC = 0,
  TRANSPORT_PLAY,
  TRANSPORT_STOP,
  TRANSPORT_LOAD,
  TRANSPORT_SAVE,
  TRANSPORT_SLIDER_COUNT
};

void save_song(const char *filename)
{
  FILE *f = fopen(filename, "wb");
  if (!f)
    return;

  Event *events = sequencer.track[0].events;
  for (int i = 0; i < sequencer.track[0].event_count; i++)
  {
    if (events[i].type = ET_NOTE)
      fprintf(f, "%d,%f,%d,%d,%f\n", 1, events[i].time_seq, events[i].val1, events[i].val2, events[i].duration);
  }

  fclose(f);
}

void load_song(const char *filename)
{
  FILE *f = fopen(filename, "rb");
  if (!f)
    return;

  fclose(f);
}

void start_playing()
{
  playing = true;
  transport.sliders[TRANSPORT_PLAY].value = 1.0;
}

void stop_playing()
{
  playing = false;
  transport.sliders[TRANSPORT_PLAY].value = 0.0;
}

void button_pressed_callback(Slider *s, int type)
{
  switch (type)
  {
    case 1:
      if (s == &transport.sliders[TRANSPORT_REC])
      {
        recording = !recording;
        transport.sliders[TRANSPORT_REC].value = recording ? 1.0 : 0.0;
        if (recording && !playing)
        {
          start_playing();
        }
      }
      else if (s == &transport.sliders[TRANSPORT_PLAY])
      {
        if (playing)
        {
          stop_playing();
        }
        else
          start_playing();
      }
      else if (s == &transport.sliders[TRANSPORT_STOP])
      {
        if (playing)
        {
          stop_playing();
        }
        if (recording)
        {
          recording = false;
          transport.sliders[TRANSPORT_REC].value = 0.0;
        }
        seq_time = 0;
      }
      else if (s == &transport.sliders[TRANSPORT_LOAD])
      {
        load_song("song.mix");
      }
      else if (s == &transport.sliders[TRANSPORT_SAVE])
      {
        save_song("song.mix");
      }
      break;
  }
}

void init_general(void)
{
  double button_size = get_dim(DIM_TRANSPORT_HEIGHT) - 2 * get_dim(DIM_BUTTON_SPACING);

  init_slider(&transport.sliders[TRANSPORT_REC], "Rec", 0, 1.0, 0.0, 0, 0, NULL,
      (rect){get_dim(DIM_BUTTON_SPACING), get_dim(DIM_BUTTON_SPACING), button_size, button_size}, (Point){10, 10}, SLIDER_STYLE_TRANSPORT_BUTTON, NULL);

  init_slider(&transport.sliders[TRANSPORT_PLAY], "Play", 0, 1.0, 0.0, 0, 0, NULL,
      (rect){get_dim(DIM_BUTTON_SPACING) + (button_size + get_dim(DIM_BUTTON_SPACING)), get_dim(DIM_BUTTON_SPACING), button_size, button_size}, (Point){10, 10}, SLIDER_STYLE_TRANSPORT_BUTTON, NULL);

  init_slider(&transport.sliders[TRANSPORT_STOP], "Stop", 0, 1.0, 0.0, 0, 0, NULL,
      (rect){get_dim(DIM_BUTTON_SPACING) + 2 * (button_size + get_dim(DIM_BUTTON_SPACING)), get_dim(DIM_BUTTON_SPACING), button_size, button_size}, (Point){10, 10}, SLIDER_STYLE_TRANSPORT_BUTTON, NULL);

  init_slider(&transport.sliders[TRANSPORT_LOAD], "Load", 0, 1.0, 0.0, 0, 0, NULL,
      (rect){get_dim(DIM_BUTTON_SPACING) + 10 * (button_size + get_dim(DIM_BUTTON_SPACING)), get_dim(DIM_BUTTON_SPACING), button_size, button_size}, (Point){10, 10}, SLIDER_STYLE_TRANSPORT_BUTTON, NULL);

  init_slider(&transport.sliders[TRANSPORT_SAVE], "Save", 0, 1.0, 0.0, 0, 0, NULL,
      (rect){get_dim(DIM_BUTTON_SPACING) + 11 * (button_size + get_dim(DIM_BUTTON_SPACING)), get_dim(DIM_BUTTON_SPACING), button_size, button_size}, (Point){10, 10}, SLIDER_STYLE_TRANSPORT_BUTTON, NULL);

  transport.sliders[TRANSPORT_REC].callback = &button_pressed_callback;
  transport.sliders[TRANSPORT_PLAY].callback = &button_pressed_callback;
  transport.sliders[TRANSPORT_STOP].callback = &button_pressed_callback;
  transport.sliders[TRANSPORT_LOAD].callback = &button_pressed_callback;
  transport.sliders[TRANSPORT_SAVE].callback = &button_pressed_callback;
  transport.slider_count = TRANSPORT_SLIDER_COUNT;
}

void init_rack(void)
{
  add_to_rack(make_io_device(), true);
  midi_input_instrument = add_to_rack(make_synth(), true);
  add_to_rack(make_chorus(), true);

  recalculate_audio_graph();
  recalculate_rack_coordinates();
}

rect get_scrollbar_rect(rect window, Scrollbar *scrollbar)
{
  return move_rect(
      make_rect(get_dim(DIM_SCROLLBAR_MARGIN), 0, get_dim(DIM_SCROLLBAR_WIDTH) - get_dim(DIM_SCROLLBAR_MARGIN), window.h),
      (Point){window.x + window.w - get_dim(DIM_SCROLLBAR_WIDTH), window.y});
}

rect get_scrollbar_thumb_rect(rect window, Scrollbar *scrollbar)
{
  return move_rect(
      make_rect(get_dim(DIM_SCROLLBAR_MARGIN) + get_dim(DIM_SCROLLBAR_THUMB_MARGIN),
        scrollbar->thumb_position + get_dim(DIM_SCROLLBAR_THUMB_MARGIN),
        get_dim(DIM_SCROLLBAR_WIDTH) - 2 * get_dim(DIM_SCROLLBAR_THUMB_MARGIN) - get_dim(DIM_SCROLLBAR_MARGIN),
        scrollbar->thumb_size - 2 * get_dim(DIM_SCROLLBAR_THUMB_MARGIN)),
      (Point){window.x + window.w - get_dim(DIM_SCROLLBAR_WIDTH) , window.y});
}

void slider_rewake(Scrollbar *scrollbar)
{
  gettimeofday(&scrollbar->tv_last_wake, NULL);
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

void draw_cable(Point start, Point end)
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

double slider_calculate_alpha(Scrollbar *scrollbar)
{
  static const double interval1 = 0.7;
  static const double interval2 = 0.7;
  double t_age = timeval_difference_sec(&scrollbar->tv_last_wake, &tv_render);

  if (t_age < interval1)
    return 1.0;
  else if (t_age < (interval1 + interval2))
    return 1.0 - (t_age - interval1) / (interval2);
  else
    return 0.0;
}

void draw_scrollbar(rect window, Scrollbar *scrollbar)
{
  scrollbar->alpha = slider_calculate_alpha(scrollbar);

  Color color = RGBAF(0.4, 0.4, 0.4, 1.0);

  if (scrollbar->alpha > 0.0)
  {
    glEnable(GL_BLEND);
    //draw_rect(get_scrollbar_rect(window, scrollbar));

    update_scrollbar();

    if (the_rack.scrollbar.thumb_hover)
      set_color_alpha(color_brightness(color, 0.3), scrollbar->alpha);
    else
      set_color_alpha(color_brightness(color, 0.2), scrollbar->alpha);

    if (DIM_SCROLLBAR_THUMB_MARGIN > 0)
      draw_rect_outline(get_scrollbar_rect(window, scrollbar));

    draw_rect(get_scrollbar_thumb_rect(window, scrollbar));
    glDisable(GL_BLEND);
  }
}

void draw_rack(rect rack_window)
{
  /* draw only within this area */
  glScissor(rack_window.x, window_height - (rack_window.y + rack_window.h) - get_dim(DIM_RACK_FADE_MARGIN) /* 0, 0 at lower left corner */, rack_window.w, rack_window.h + 2 * get_dim(DIM_RACK_FADE_MARGIN));
  glEnable(GL_SCISSOR_TEST);
  Point origin = (Point){rack_window.x, rack_window.y};
  {
    Instrument *inst = the_rack.first;
    while (inst)
    {
      Point screen_pos = (Point){origin.x + inst->rack_pos.x, origin.y + inst->rack_pos.y - the_rack.scroll_position};

      inst->draw(inst, the_rack.show_back, screen_pos);
      if (inst == selected_instrument)
      {
#if 0
        set_color(color_select);
        draw_rect_outline(move_rect(move_rect(inst->rack_pos, (Point){0, -the_rack.scroll_position}),
              origin));
#endif
      }
      inst = inst->next;
    }
  }

  /* draw connections */
  if (the_rack.show_back)
  {
    Instrument *inst = the_rack.first;
    while (inst)
    {
      Point screen_pos = (Point){origin.x + inst->rack_pos.x, origin.y + inst->rack_pos.y - the_rack.scroll_position};

      /* draw connections */
      for (int i = 0; i < inst->num_outputs; i++)
      {
        Instrument *dst_inst = inst->outputs[i].target_inst;
        int target_index = inst->outputs[i].target_connection;
        if (dst_inst && (target_index >= 0 && target_index < dst_inst->num_inputs))
        {
          rect r_start = move_rect(inst->outputs[i].pos, screen_pos);
          Point start = rect_midpoint(r_start);
          Point screen_pos2 = (Point){origin.x + dst_inst->rack_pos.x, origin.y + dst_inst->rack_pos.y - the_rack.scroll_position};
          rect r_end = move_rect(dst_inst->inputs[target_index].pos, screen_pos2);
          Point end = rect_midpoint(r_end);

          //printf("Draw connection %f %f %f %f\n", start.x, start.y, end.x, end.y);
          glColor3f(0.2, 1.0, 0.2);
          draw_rect_outline(r_start);
          glColor3f(0.2, 0.8, 0.2);
          draw_rect_outline(r_end);
          if (start.x < end.x)
          draw_cable(start, end);
          else
          draw_cable(end, start);
        }
      }
      inst = inst->next;
    }
  }

  draw_scrollbar(rack_window, &the_rack.scrollbar);

  glDisable(GL_SCISSOR_TEST);

  glEnable(GL_BLEND);

  draw_rect_with_colors(make_rect(rack_window.x, get_dim(DIM_RACK_VERTICAL_MARGIN) - get_dim(DIM_RACK_FADE_MARGIN), rack_window.w, get_dim(DIM_RACK_FADE_MARGIN)),
      RGBAF(0.0, 0.0, 0.0, 1.0),
      RGBAF(0.0, 0.0, 0.0, 1.0),
      RGBAF(0.0, 0.0, 0.0, 0.0),
      RGBAF(0.0, 0.0, 0.0, 0.0));
  
  draw_rect_with_colors(make_rect(rack_window.x, rack_window.y + rack_window.h, rack_window.w, get_dim(DIM_RACK_FADE_MARGIN)),
      RGBAF(0.0, 0.0, 0.0, 0.0),
      RGBAF(0.0, 0.0, 0.0, 0.0),
      RGBAF(0.0, 0.0, 0.0, 1.0),
      RGBAF(0.0, 0.0, 0.0, 1.0));

  glDisable(GL_BLEND);
}

rect get_transport_rect()
{
  rect r_rack = get_rack_window();

  return make_rect(0, r_rack.y + r_rack.h, window_width, get_dim(DIM_TRANSPORT_HEIGHT));
}

void draw_transport(rect r_transport)
{

  draw_rect_with_colors(r_transport,
      GRAY(0.7), GRAY(0.7),
      GRAY(0.8), GRAY(0.8));

  Point off = (Point){r_transport.x, r_transport.y};

  for (int i = 0; i < transport.slider_count; i++)
    draw_slider_generic(&transport.sliders[i], off);

  Point p = rect_midpoint(r_transport);
  char tmp[128];
  snprintf(tmp, sizeof(tmp), "%5d.%0.4f",
      ((int)seq_time) / 4, fmod(seq_time, 4.0));
  draw_string_centered(0, p.x, p.y, tmp);
}

void draw_keyboard(void)
{
  /* draw keyboard */

  for (int pass = 0; pass <= 1; pass++)
  {
    for (int i = 0; i < (KEYBOARD_NUM_OCTAVES * 12 + 1); i++)
    { 
      if ((int)black_keys[i % 12] ^ pass)
        continue;

      Point off = get_keyboard_screen_pos();

      rect key_rect = move_rect(get_keyboard_key_rect(i), off);

      if (gui_keyboard_state[i])
        black_keys[i % 12] ? set_grey(0.25f) : set_grey(0.75f);
      else
        black_keys[i % 12] ? set_grey(0.f) : set_grey(1.f);

      draw_rect(key_rect);

      black_keys[i % 12] ? set_grey(0.25f) : set_grey(0.f);

      draw_rect_outline(key_rect);

      if (i % 12 == 0) /* C key */
      {
        char tmp[3] = "1";
        snprintf(tmp, sizeof(tmp), "%d", abs(i / 12 - 1));

        set_color(RGBAF(0.0, 0.0, 0.0, 1.0));
        draw_string(0, key_rect.x + 5, key_rect.y + key_rect.h - 20, tmp);
      }
    }
  }
}

void render(void)
{
  glClearColor(0.0f, 0.0f, 0.0f, 1.f);
  glClear(GL_COLOR_BUFFER_BIT);
  glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
  glBlendEquation(GL_FUNC_ADD);
  glMatrixMode(GL_MODELVIEW);
  glLoadIdentity();
  glDisable(GL_BLEND);

  gettimeofday(&tv_render, NULL);

#if 0
  glBegin(GL_TRIANGLE_STRIP);
  glColor4f(0.0, 0.0, 0.0, 1.0);
  glVertex2i(0, 0);
  glVertex2i(0 + window_width, 0);
  glColor4f(0.0, 0.0, 0.4, 1.0);
  glVertex2i(0, 0 + window_height);
  glVertex2i(0 + window_width, 0 + window_height);
  glEnd();
#endif

#if 0
  glEnable(GL_BLEND);
  glColor4f(1.0, 1.0, 1.0, tv_render.tv_usec * 1.0e-6);
  draw_rect(make_rect(10, 500, 50, 50));
  glDisable(GL_BLEND);
#endif



  glColor3f(1.0, 1.0, 1.0);

  rect rack_window = get_rack_window();
  //glColor3f(0.0, 0.0, 1.0);
  //draw_rect_outline(rect_grow(rack_window, 2, 2));
  draw_rack(rack_window);

  if (transport_visible)
    draw_transport(get_transport_rect());

  draw_keyboard();

  if (tooltip[0] != '\0')
  {
    Point pos = (Point){mpos.x + 10, mpos.y + 10};

    set_grey(0.4);
    rect r = make_rect(pos.x, pos.y, strlen(tooltip) * 10, 25);
    draw_rect(r);
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
    gettimeofday(&the_rack.scrollbar.tv_last_wake, NULL);
    redisplay();
  }
}

int keyboard_key = -1;

void mouse_button_func(GLFWwindow *window, int button, int action, int mods)
{
  rect rack_window = get_rack_window();
  rect scrollbar_rect = get_scrollbar_rect(rack_window, &the_rack.scrollbar);
  rect thumb_rect = get_scrollbar_thumb_rect(rack_window, &the_rack.scrollbar);
  rect transport_rect = get_transport_rect();

  switch (button)
  {
    case GLFW_MOUSE_BUTTON_LEFT:
      if (action == GLFW_PRESS)
      {
        if (inside_rect(transport_rect, mpos))
        {
          Slider *sliders = transport.sliders;

          Point relative_pos = (Point){mpos.x - transport_rect.x, mpos.y - transport_rect.y};
          for (int i = 0; i < transport.slider_count; i++)
          {
            rect rslider = sliders[i].pos;
            if (inside_rect(rslider, relative_pos))
            {
              if (sliders[i].style == SLIDER_STYLE_TOGGLE_SWITCH)
              {
                sliders[i].value = !(int)sliders[i].value;
              }
              else if (sliders[i].style == SLIDER_STYLE_TRANSPORT_BUTTON)
              {
                //sliders[i].value = !(int)sliders[i].value;
                if (sliders[i].callback)
                {
                  sliders[i].callback(&sliders[i], 1);
                }
              }
              else if (sliders[i].style == SLIDER_STYLE_RADIO_BUTTON)
              {
                int num_choices = (int)(sliders[i].max - sliders[i].min) + 1;
                int new_value = sliders[i].min + (int)((relative_pos.y - rslider.y) / (rslider.h / num_choices));
                if (sliders[i].value != new_value)
                  sliders[i].value = new_value;
              }
              else
              {
                mpos_left_down = mpos;
                slider_drag = &sliders[i];
                sliders[i].value_start_drag = sliders[i].value;

                char tmp_value[32];
                tmp_value[0] = 0;
                slider_get_string_value(&sliders[i], tmp_value, sizeof(tmp_value));
                snprintf(tooltip, sizeof(tooltip), "%s: %s", sliders[i].name, tmp_value);
              }
              redisplay();
              break;
            }
          }
        }
        else if (inside_rect(thumb_rect, mpos))
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
            set_scroll_position(the_rack.scroll_position - (rack_window.h - get_dim(DIM_SCROLL_OVERLAP)));
          }
          else if ((mpos.y - scrollbar_rect.y) >= (the_rack.scrollbar.thumb_position + the_rack.scrollbar.thumb_size))
          {
            set_scroll_position(the_rack.scroll_position + rack_window.h - get_dim(DIM_SCROLL_OVERLAP));
          }
        }
        else if (inside_rect(rack_window, mpos))
        {
          /* check if interacting with the synth */

          Point rack_mpos = (Point){mpos.x - rack_window.x, mpos.y - rack_window.y + the_rack.scroll_position};

          Instrument *inst = the_rack.first;
          double height = 0;
          while (inst)
          {
            if ((rack_mpos.y >= inst->rack_pos.y)
                && (rack_mpos.y < (inst->rack_pos.y + inst->rack_pos.h))
                && inside_rect(inst->rack_pos, rack_mpos))
            {
              /* check if synth handles the click */

              bool handled = false;
              Point synth_mpos = (Point){rack_mpos.x - inst->rack_pos.x, rack_mpos.y - inst->rack_pos.y};
              Slider *sliders = inst->sliders;
              for (int i = 0; i < inst->slider_count; i++)
              {
                rect rslider = sliders[i].pos;
                if (inside_rect(rslider, synth_mpos))
                {
                  if (sliders[i].style == SLIDER_STYLE_TOGGLE_SWITCH)
                  {
                    sliders[i].value = !(int)sliders[i].value;
                  }
                  else if (sliders[i].style == SLIDER_STYLE_RADIO_BUTTON)
                  {
                    int num_choices = (int)(sliders[i].max - sliders[i].min) + 1;
                    int new_value = sliders[i].min + (int)((synth_mpos.y - rslider.y) / (rslider.h / num_choices));
                    if (sliders[i].value != new_value)
                      sliders[i].value = new_value;
                  }
                  else
                  {
                    mpos_left_down = mpos;
                    slider_drag = &sliders[i];
                    sliders[i].value_start_drag = sliders[i].value;

                    char tmp_value[32];
                    tmp_value[0] = 0;
                    slider_get_string_value(&sliders[i], tmp_value, sizeof(tmp_value));
                    snprintf(tooltip, sizeof(tooltip), "%s: %s", sliders[i].name, tmp_value);
                  }
                  handled = true;
                  redisplay();
                  break;
                }
              }

              if (!handled)
                selected_instrument = inst;
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
              gui_keyboard_state[keyboard_key] = 0;
              midi_user_input(keyboard_key, 0, 0);
              keyboard_key = -1;
            }
            gui_keyboard_state[key] = 1;
            midi_user_input(key, 1, 127);
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
          gui_keyboard_state[keyboard_key] = 0;
          midi_user_input(keyboard_key, 0, 0);
          keyboard_key = -1;
          redisplay();
        }

      }
      break;
    case GLFW_MOUSE_BUTTON_RIGHT: // right click
      break;
  }
}

void mouse_move_func(GLFWwindow *window, double x_d, double y_d)
{
  mpos.x = x_d;
  mpos.y = y_d;

  bool redisp = false;

  if (slider_drag)
  {
    double delta = slider_drag->style == SLIDER_STYLE_HORIZONTAL ? mpos.x - mpos_left_down.x : (- mpos.y + mpos_left_down.y);
    double start = slider_value_to_screen_pos(slider_drag, slider_drag->value_start_drag);
    slider_drag->value = slider_screen_pos_to_value(slider_drag, start + delta);
    update_instrument(slider_drag->inst);
    char tmp_value[32];
    tmp_value[0] = 0;
    slider_get_string_value(slider_drag, tmp_value, sizeof(tmp_value));
    snprintf(tooltip, sizeof(tooltip), "%s: %s", slider_drag->name, tmp_value);
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

    if (inside_rect(rack_window, mpos))
    {
      rect scrollbar_rect = get_scrollbar_rect(rack_window, &the_rack.scrollbar);
      rect thumb_rect = get_scrollbar_thumb_rect(rack_window, &the_rack.scrollbar);
      if (inside_rect(scrollbar_rect, mpos))
      {
        slider_rewake(&the_rack.scrollbar);
      }

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
      set_scroll_position(the_rack.scroll_position - get_dim(DIM_SCROLL_AMOUNT));
    }
    else if (yoffset < 0.0)
    {
      set_scroll_position(the_rack.scroll_position + get_dim(DIM_SCROLL_AMOUNT));
    }
  }
  else if (inside_rect(get_keyboard_screen_rect(), mpos))
  {
    if (yoffset < 0.0)
      keyboard_display_offset += 0.4 * get_dim(DIM_KEYBOARD_KEY_WHITE_WIDTH);
    if (yoffset > 0.0)
      keyboard_display_offset -= 0.4 * get_dim(DIM_KEYBOARD_KEY_WHITE_WIDTH);

    keyboard_display_offset = clamp(keyboard_display_offset, 0.0, (KEYBOARD_NUM_OCTAVES * 7 + 1) * get_dim(DIM_KEYBOARD_KEY_WHITE_WIDTH) - window_width);

    redisplay();
  }
}

int keyboard_octave = 4;

void keyboard_clear_input(void)
{
  /* clear all input */
  for (int i = 0; i < ARRAY_SIZE(gui_keyboard_state); i++)
  {
    if (gui_keyboard_state[i])
    {
      midi_user_input(i, 0, 64);
      gui_keyboard_state[i] = 0;
      redisplay();
    }
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
    {GLFW_KEY_BACKSLASH, 33},
  };

  for (int i = 0; i < ARRAY_SIZE(keys); i++)
  {
    if (keys[i].key == key && (action == GLFW_PRESS  || action == GLFW_RELEASE))
    {
      int newstate = action == GLFW_RELEASE ? 0 : 1;
      int velocity = 64;
      int note = (keyboard_octave + 1) * 12 + keys[i].note;
      if (gui_keyboard_state[note] != newstate)
      {
        midi_user_input(note, newstate, velocity);
        gui_keyboard_state[note] = newstate;
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
    case GLFW_KEY_ESCAPE: /* escape */
      keyboard_clear_input();
      break;
    case GLFW_KEY_ENTER:
    case GLFW_KEY_KP_ENTER:
      redisplay();
      break;
    case GLFW_KEY_LEFT:
      keyboard_clear_input();
      keyboard_octave = clamp(keyboard_octave - 1, -1, 8);
      break;
    case GLFW_KEY_RIGHT:
      keyboard_clear_input();
      keyboard_octave = clamp(keyboard_octave + 1, -1, 8);
      break;
    case GLFW_KEY_UP:
      set_scroll_position(the_rack.scroll_position - get_dim(DIM_SCROLL_AMOUNT));
      break;
    case GLFW_KEY_DOWN:
      set_scroll_position(the_rack.scroll_position + get_dim(DIM_SCROLL_AMOUNT));
      break;
    case GLFW_KEY_PAGE_UP:
      set_scroll_position(the_rack.scroll_position - 100 * get_dim(DIM_SCROLL_AMOUNT));
      break;
    case GLFW_KEY_PAGE_DOWN:
      set_scroll_position(the_rack.scroll_position + 100 * get_dim(DIM_SCROLL_AMOUNT));
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

void glfw_error_callback(int error, const char* desc)
{
  fprintf(stderr, "GLFW error: %s\n", desc);
}

int main(int argc, char *argv[])
{
  keyboard_display_offset = 7 * get_dim(DIM_KEYBOARD_KEY_WHITE_WIDTH);

  /* init font */
  init_freetype();

#define MAIN_FONT_FILENAME "./data/font/trim.ttf"

  if (get_font_file(MAIN_FONT_FILENAME))
  {
    render_font_texture(FONT_DEFAULT, MAIN_FONT_FILENAME, (int)(scale * font_size));
    render_font_texture(FONT_BIG, MAIN_FONT_FILENAME, (int)(scale * 1.5 * font_size));
    render_font_texture(FONT_TINY, MAIN_FONT_FILENAME, (int)(scale * 9));
  }
  else
  {
    fprintf(stderr, "Can't find or resolve font file/name: \"%s\"\n", MAIN_FONT_FILENAME);
  }

  if (!glfwInit())
  {
    fprintf(stderr, "Failed to init GLFW\n");
    exit(EXIT_FAILURE);
  }

  glfwSetErrorCallback(&glfw_error_callback);

  window = glfwCreateWindow(fitting_window_width(), fitting_window_height(), "Sound Playground", NULL, NULL);

  if (!window)
  {
    fprintf(stderr, "Failed to create a Window\n");
    glfwTerminate();
    exit(EXIT_FAILURE);
  }
  printf("%d\n", 1 << 16);

  glfwMakeContextCurrent(window);

  glfwSetWindowSizeCallback(window, &window_size_func);
  glfwSetWindowRefreshCallback(window, &window_refresh_func);

  glfwSetMouseButtonCallback(window, &mouse_button_func);
  glfwSetCursorPosCallback(window, &mouse_move_func);
  glfwSetScrollCallback(window, &mouse_scroll_func);

  glfwSetKeyCallback(window, &key_func);
  glfwSetCharCallback(window, &char_func);

  upload_font_textures();

  int w = 0;
  int h = 0;
  glfwGetWindowSize(window, &w, &h);
  window_size_func(window, w, h);
  redisplay_needed = true;

  init_general();

  init_rack();

  start_audio();

  while (!glfwWindowShouldClose(window))
  {
    //if (redisplay_needed)
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

