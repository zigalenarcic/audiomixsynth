#ifndef _COMMON_H_
#define _COMMON_H_

typedef struct {
  double x;
  double y;
} vec2;

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
  vec2 thumb_size;
  bool horizontal;

  double value_start_drag;
  struct Instrument_ *inst;
} Slider;

typedef void (* DrawFunction)(struct Instrument_ *, bool, vec2);
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

