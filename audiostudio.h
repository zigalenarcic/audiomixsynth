#ifndef _COMMON_H_
#define _COMMON_H_

#define PI_TIMES_2 (2.0 * M_PI)

#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))
#define MAX(x, y) ((x) > (y) ? (x) : (y))
#define MIN(x, y) ((x) < (y) ? (x) : (y))

#define container_of(ptr, type, member) ({  \
    const typeof( ((type *)0)->member ) *__member = (ptr); \
    (type *)( (char *)__member - offsetof(type, member));})

#define FREE_IF_NOT_NULL(x) if ((x)) free((x))

typedef struct {
  double x;
  double y;
} Point;

typedef struct {
  double x;
  double y;
  double w;
  double h;
} rect;

enum SLIDER_CURVE {
  SC_NO_CURVE = 0,
  SC_EXPONENTIAL,
  SC_LOG
};

struct Instrument_;

typedef struct Slider_
{
  char name[64];
  double min;
  double max;
  double value;
  int curve;
  int steps;
  rect pos;
  Point thumb_size;
  bool horizontal;

  double value_start_drag;
  struct Instrument_ *inst;
} Slider;

typedef void (* DrawFunction)(struct Instrument_ *, bool, Point);
typedef void (* AudioProcessFunction)(struct Instrument_ *, int, const void **inputs, void **outputs);

typedef struct Connection_ {
  bool is_input;
  rect pos;
  int index;
  struct Instrument_ *inst;

  struct Instrument_ *target_inst;
  int target_connection;

  double *buffer;
} Connection;

typedef struct Instrument_
{
  struct Instrument_ *prev;
  struct Instrument_ *next;

  char name[64];
  char user_name[64];

  rect rack_pos;
  double height;

  Slider sliders[256];
  int slider_count;

  Connection inputs[64];
  int num_inputs;
  Connection outputs[64];
  int num_outputs;

  DrawFunction draw;
  AudioProcessFunction process_audio;

} Instrument;

typedef struct Scrollbar_ {
  int thumb_position;
  int thumb_size;
  int thumb_hover;

  bool dragging;
  int thumb_mouse_down_thumb_position;

  struct timeval tv_last_wake;
  float alpha;
} Scrollbar;

typedef struct Rack_ {
  Instrument *first;
  bool show_back;

  double scroll_position;
  double target_scroll_position;
  double total_height;

  Scrollbar scrollbar;
} Rack;

#endif // _COMMON_H_

