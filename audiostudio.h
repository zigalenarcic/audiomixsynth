#ifndef AUDIOSTUDIO_H
#define AUDIOSTUDIO_H

#include <stdint.h>

#define PI_TIMES_2 (2.0 * M_PI)

#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))
#define MAX(x, y) ((x) > (y) ? (x) : (y))
#define MIN(x, y) ((x) < (y) ? (x) : (y))

#define container_of(ptr, type, member) ({  \
    const typeof( ((type *)0)->member ) *__member = (ptr); \
    (type *)( (char *)__member - offsetof(type, member));})

#define FREE_IF_NOT_NULL(x) if ((x)) free((x))

enum {
  SYNTH_OSC1_SHAPE = 0,
  SYNTH_OSC1_OCTAVE,
  SYNTH_OSC1_SEMITONE,
  SYNTH_OSC1_DETUNE,
  SYNTH_OSC1_VOICES,
  SYNTH_OSC1_VOICES_DETUNE,
  
  SYNTH_OSC2_SHAPE,
  SYNTH_OSC2_OCTAVE,
  SYNTH_OSC2_SEMITONE,
  SYNTH_OSC2_DETUNE,
  SYNTH_OSC2_VOICES,
  SYNTH_OSC2_VOICES_DETUNE,

  SYNTH_OSC3_SHAPE,
  SYNTH_OSC3_OCTAVE,
  SYNTH_OSC3_SEMITONE,
  SYNTH_OSC3_DETUNE,
  SYNTH_OSC3_VOICES,
  SYNTH_OSC3_VOICES_DETUNE,

  SYNTH_OSC1_OSC2_VOLUME_RATIO,
  SYNTH_OSC3_VOLUME_RATIO,

  SYNTH_FILTER_CUTOFF,

  SYNTH_VOLUME,
  SYNTH_SLIDER_COUNT
};

extern double sample_rate;
extern double bpm;
extern double position;
extern bool recording;
extern bool playing;

enum {
  ET_NOTE = 1,
  ET_CC = 2,
};

typedef struct Event {
  //double time_s;
  double time_seq; // in beats
  double duration;

  uint8_t type;
  uint8_t val1;
  uint8_t val2;
  uint8_t val3;
} Event;

typedef struct EventBatch {
  Event events[10000];
  int event_count;
} EventBatch;

typedef struct Sequencer_ {
  EventBatch track[10];

  EventBatch recorder;

} Sequencer;

extern Sequencer sequencer;

typedef uint32_t Color;

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

enum {
  SLIDER_STYLE_HORIZONTAL = 0,
  SLIDER_STYLE_VERTICAL,
  SLIDER_STYLE_ROTARY,
  SLIDER_STYLE_TOGGLE_SWITCH,
  SLIDER_STYLE_RADIO_BUTTON,
  SLIDER_STYLE_TRANSPORT_BUTTON,
};

typedef struct Slider_
{
  char name[64];
  double min;
  double max;
  double value;
  int curve;
  int discrete;
  const char **string_values;
  rect pos;
  Point thumb_size;
  int style;
  double rotary_start;
  double rotary_range;

  void (*callback)(struct Slider_ *, int);

  double value_start_drag;
  struct Instrument_ *inst;
} Slider;

typedef void (* DrawFunction)(struct Instrument_ *, bool, Point);
typedef void (* MidiProcessFunction)(struct Instrument_ *, int, int, int);
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

#define MAX_SYNTH_POLYPHONY 64
#define MAX_DETUNE_VOICES 7
#define NUM_SYNTH_OSC 3
struct synth_data {
  int note[MAX_SYNTH_POLYPHONY];
  uint32_t phase_delta[MAX_SYNTH_POLYPHONY][NUM_SYNTH_OSC][MAX_DETUNE_VOICES];
  uint32_t phase[MAX_SYNTH_POLYPHONY][NUM_SYNTH_OSC][MAX_DETUNE_VOICES];
};

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

  Color background_color;

  Connection inputs[64];
  int num_inputs;
  Connection outputs[64];
  int num_outputs;

  DrawFunction draw;
  MidiProcessFunction process_midi;
  AudioProcessFunction process_audio;

  void *specific_data;

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

typedef struct Transport_ {
  Slider sliders[100];
  int slider_count;
} Transport;

extern Rack the_rack;
extern Transport transport;
extern Instrument *midi_input_instrument;
void midi_user_input(int key, int note_on, int velocity);

#endif // AUDIOSTUDIO_H

