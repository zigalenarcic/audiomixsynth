#ifndef __AUDIO_H
#define __AUDIO_H

#include "common.h"

typedef struct {
  double *buf;
  int index;
  int length;
  double feedback;
} delay_line_t;

delay_line_t *make_delay_line(int length, double feedback);
void start_audio(void);
void deinit_audio(void);
void keyboard_input(int key, int note_on, int velocity);
void process_audio_synth(Instrument *inst, int nframes, const void **inputs, void **outputs);
void process_audio_io_device(Instrument *inst, int nframes, const void **inputs, void **outputs);
void process_audio_chorus(Instrument *inst, int nframes, const void **inputs, void **outputs);
void recalculate_audio_graph(void);

#endif // __AUDIO_H

