/*
 * audio.c
 *
 * Implementation of audio creation software
 *
 * Initial date: 2024-01-13 13:20 UTC+1:00
 *
 * Author: Ziga Lenarcic
 *
 * Public domain.
 */

#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <stdbool.h>
#include <string.h>

#include <jack/jack.h>
#include <jack/midiport.h>

#include "audiostudio.h"

#define JACK_CLIENT_NAME "Audio Studio"

///////////////////////////////////////////////////////////////////////////////
// DECLARATIONS
///////////////////////////////////////////////////////////////////////////////

typedef double sample_t;

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

jack_port_t *input_port[2];
int input_port_count;
jack_port_t *input_port_midi;
jack_port_t *output_port_1;
jack_port_t *output_port_2;
jack_client_t *client;

extern char keyboard_state[256];
extern Rack the_rack;
void redisplay(void);

///////////////////////////////////////////////////////////////////////////////
// Audio engine
///////////////////////////////////////////////////////////////////////////////

pthread_t audio_thread_obj;

enum {
  ATS_STARTUP = 0,
  ATS_PROCESSING,
  ATS_STOPPED
};

int audio_thread_state = ATS_STARTUP;
int wakeup_audio_thread;

double *main_output_buffer[2];
double *main_input_buffer[2];
double *temp_buffers[10];
double *empty_buffer;
int main_frames;

int num_buffers = 1024;
double *big_buffer;
int *free_buffers;
int free_buffers_count;

sample_t *allocate_buffer(void)
{
  if (free_buffers_count > 0)
  {
    free_buffers_count--;
    int idx = free_buffers[free_buffers_count];

    return &big_buffer[idx * main_frames];
  }
  else
  {
    printf("No buffers\n");

    return NULL;
  }
}

double sample_rate = 48000.0f;

delay_line_t *delay1[10];

double *allocate_double(int n)
{
  return (double *)calloc(n, sizeof(double));
}

double saw[256];

void init_waveforms(void)
{
    for (int i = 0; i < ARRAY_SIZE(saw); i++)
    {
        saw[i] = -1.0 + 2.0 * ((double)i / (ARRAY_SIZE(saw) - 1));
    }
}

double get_waveform(double *data, int data_len, double phase)
{
    double pos = phase * data_len;

    int pos_0 = (int)pos;
    double off = pos - pos_0;

    return (1 - off) * data[pos_0] + off * data[(pos_0 + 1 >= data_len) ? 0 : pos_0 + 1];
}

void allocate_main_buffers(int nframes)
{
  main_frames = nframes;
  for (int i = 0; i < ARRAY_SIZE(main_output_buffer); i++)
  {
    FREE_IF_NOT_NULL(main_output_buffer[i]);
    main_output_buffer[i] = allocate_double(main_frames);
  }
  for (int i = 0; i < ARRAY_SIZE(main_input_buffer); i++)
  {
    FREE_IF_NOT_NULL(main_input_buffer[i]);
    main_input_buffer[i] = allocate_double(main_frames);
  }

  for (int i = 0; i < ARRAY_SIZE(temp_buffers); i++)
  {
    FREE_IF_NOT_NULL(temp_buffers[i]);
    temp_buffers[i] = allocate_double(main_frames);
  }

  FREE_IF_NOT_NULL(empty_buffer);
  empty_buffer = allocate_double(main_frames);

  FREE_IF_NOT_NULL(big_buffer);
  big_buffer = allocate_double(main_frames * num_buffers);
  FREE_IF_NOT_NULL(free_buffers);
  free_buffers = malloc(sizeof(int) * num_buffers);
  free_buffers_count = num_buffers;
  for (int i = 0; i < num_buffers; i++)
    free_buffers[num_buffers - i - 1] = i;
}

typedef struct adsr_ {
    double attack;
    double decay;
    double sustain;
    double release;
} adsr;

typedef struct adsr_state_ {
    bool note_off;
    double note_off_value;
    double note_off_time;
} adsr_state;

double get_adsr(adsr *o, adsr_state *env_state, double time, int *finished)
{
    if (env_state->note_off == false)
    {
        if (time <= o->attack)
        {
            return time / o->attack; /* 0 -> 1 */
        }
        else if (time <= (o->attack + o->decay))
        {
            double rel_time = (time - o->attack) / o->decay; /* 0 -> 1 */

            return o->sustain + (1 - o->sustain) * (1 - rel_time);
        }
        else
        {
            return o->sustain;
        }
    }
    else
    {
        double rel_time = (time - env_state->note_off_time) / o->release;
        if (rel_time > 1.0)
        {
            if (finished)
                *finished = 1;
            return 0.0;
        }

        return env_state->note_off_value * (1.0 - rel_time);
    }
}

int audio_buffer_size_callback(jack_nframes_t nframes, void *arg)
{
  printf("Buffer size set to %d\n", (int)nframes);
  if (nframes > 0)
  {
    allocate_main_buffers((int)nframes);
  }
}

void process_audio_synth(Instrument *inst, int nframes, const void **inputs, void **outputs);

void process_audio_io_device(Instrument *inst, int nframes, const void **inputs, void **outputs)
{
  double *input_l = (double *)inputs[0];
  double *input_r = (double *)inputs[1];

  double volume = inst->sliders[0].value;

  double *output_l = main_output_buffer[0];
  double *output_r = main_output_buffer[1];

  while (nframes--)
  {
    output_l[0] = volume * input_l[0];
    output_r[0] = volume * input_r[0];

    input_l += 1;
    input_r += 1;

    output_l += 1;
    output_r += 1;
  }
}

void process_reverb(double *input_l, double *input_r, double *output_l, double *output_r, int nframes, double mix);

void process_audio_chorus(Instrument *inst, int nframes, const void **inputs, void **outputs)
{
  double *input_l = (double *)inputs[0];
  double *input_r = (double *)inputs[1];

  double *output_l = (double *)outputs[0];
  double *output_r = (double *)outputs[1];

  process_reverb(input_l, input_r, output_l, output_r, nframes, inst->sliders[2].value);
#if 0
  while (nframes--)
  {
    output_l[0] = input_l[0];
    output_r[0] = input_r[0];



    input_l += 1;
    input_r += 1;
    output_l += 1;
    output_r += 1;
  }
#endif
}

Instrument *process_sequence_a[256];
Instrument *process_sequence_b[256];
Instrument **process_sequence = &process_sequence_a[0];

void recalculate_audio_graph(void)
{
  Instrument *root = the_rack.first;

  Instrument **process_sequence_new = (process_sequence == &process_sequence_a[0]) ? &process_sequence_b[0] : &process_sequence_a[0];

  memset(process_sequence_new, 0, sizeof(Instrument *) * 256);
  int count = 0;

  Instrument *stack[1024];
  int stack_pos = 0;

  stack[0] = root;
  stack_pos = 1;
  process_sequence_new[count++] = root;

  while (stack_pos > 0)
  {
    Instrument *current = stack[stack_pos - 1];
    stack_pos--;

    for (int i = 0; i < current->num_inputs; i++)
    {
      Instrument *inst = current->inputs[i].target_inst;
      if (inst)
      {
        bool found = false;
        for (int j = 0; j < count; j++)
        {
          if (process_sequence_new[j] == inst)
          {
            /* this instrument is already processed - move it to later (earlier) position */
            memmove(&process_sequence_new[j], &process_sequence_new[j + 1], (count - (j + 1)) * sizeof(Instrument *));
            count--;
            break;
          }
        }
        process_sequence_new[count++] = inst;

        stack[stack_pos++] = inst;
      }
    }
  }

  /* switch atomically */
  process_sequence = process_sequence_new;
}

void process_audio(void)
{
  Instrument **current_seq = process_sequence;

  int count = 0;
  while (current_seq[count])
    count++;
    
  double *tmp[2];
  tmp[0] = temp_buffers[0];
  tmp[1] = temp_buffers[1];

  double *inputs[64];
  double *outputs[64];

#if 1
  //printf("processing..\n");
  for (int i_inst = count - 1; i_inst >= 0; i_inst--)
  {
    Instrument *inst = current_seq[i_inst];

    //printf("process %s\n", inst->name);

    for (int i = 0; i < inst->num_inputs; i++)
    {
      sample_t *buf = inst->inputs[i].target_inst->outputs[inst->inputs[i].target_connection].buffer;
      inputs[i] = buf ? buf : empty_buffer;
    }

    for (int i = 0; i < inst->num_outputs; i++)
    {
      if (!inst->outputs[i].buffer)
        inst->outputs[i].buffer = allocate_buffer();

      sample_t *buf = inst->outputs[i].buffer;
      outputs[i] = buf ? buf : temp_buffers[i];
    }

    inst->process_audio(inst, main_frames, (const void **)inputs, (void **)outputs);
  }
#endif

#if 0
  double *tmp[2];
  tmp[0] = temp_buffers[0];
  tmp[1] = temp_buffers[1];

  the_rack.first->process_audio(the_rack.first, main_frames, (void **)tmp);

  process_reverb(temp_buffers[0], temp_buffers[1], main_output_buffer[0], main_output_buffer[1], main_frames);
  process_audio_synth(temp_buffers[0], temp_buffers[1], main_frames);

  process_reverb(temp_buffers[0], temp_buffers[1], main_output_buffer[0], main_output_buffer[1], main_frames);

  //process_reverb(main_input_buffer[1], main_input_buffer[1], main_output_buffer[0], main_output_buffer[1], main_frames);
#endif
}

void add_midi_event(int count, const unsigned char *buffer)
{
  if (count == 3)
  {
    char key = buffer[1] & 0x7f;
    switch (buffer[0])
    {
      case 0x90: /* note on */
        keyboard_input(key, 1, buffer[2]);
        keyboard_state[key] = 1;
        redisplay();
        break;
      case 0x80: /* note off */
        keyboard_input(key, 0, buffer[2]);
        keyboard_state[key] = 0;
        redisplay();
        break;
      default:
        break;
    }
  }

}

int process_callback(jack_nframes_t nframes, void *arg)
{
  //printf("process %d frames\n", (int)nframes);

  jack_default_audio_sample_t *out[2];
  jack_default_audio_sample_t *in[2];

  void *midi = jack_port_get_buffer(input_port_midi, nframes);;

  out[0] = jack_port_get_buffer(output_port_1, nframes);
  out[1] = jack_port_get_buffer(output_port_2, nframes);

  in[0] = jack_port_get_buffer(input_port[0], nframes);
  in[1] = jack_port_get_buffer(input_port[1], nframes);

  //printf("callback %p %p %p %p %p\n", out[0], out[1], in[0], in[1], midi);

  {
    jack_midi_event_t event;
    jack_nframes_t i = 0;
    jack_nframes_t event_count = jack_midi_get_event_count(midi);

    for (i = 0; i < event_count; i++)
    {
      jack_midi_event_get(&event, midi, i);
      printf("Midi event time %d, %zu bytes %02x %02x %02x\n", event.time, event.size, event.buffer[0], event.size > 1 ? event.buffer[1] : 0, event.size > 2 ? event.buffer[2] : 0);

      add_midi_event(event.size, event.buffer);
    }
  }

  /* copy from internal buffer */
  for (int i = 0; i < nframes; i++)
  {
    out[0][i] = (float)main_output_buffer[0][i]; 
    out[1][i] = (float)main_output_buffer[1][i]; 

    main_input_buffer[0][i] = (double)in[0][i]; 
    main_input_buffer[1][i] = (double)in[1][i]; 

    /* loopback */
    //out[0][i] = in[0][i]; 
    //out[1][i] = in[1][i]; 
  }
  //printf("input buffer %f %f\n", main_input_buffer[0][0], main_input_buffer[1][0]);
  *(volatile int *)&wakeup_audio_thread = 1;

  return 0;
}

void init_machines(void)
{
  delay1[0] = make_delay_line(4799, 0.742);
  delay1[1] = make_delay_line(4999, 0.733);
  delay1[2] = make_delay_line(5399, 0.715);
  delay1[3] = make_delay_line(5801, 0.697);

  delay1[4] = make_delay_line(1051, 0.7);
  delay1[5] = make_delay_line(337, 0.7);
  delay1[6] = make_delay_line(113, 0.7);

  init_waveforms();
}

void init_audio(void)
{
  jack_options_t options = JackNullOption;

  const char *client_name = JACK_CLIENT_NAME;
  const char *server_name = NULL;

  jack_status_t status;

  client = jack_client_open(client_name, options, &status, server_name);

  if (!client)
  {
    fprintf(stderr, "jack_client_open() failed 0x%2.0x\n", status);
    if (status & JackServerFailed)
      fprintf(stderr, "Unable to connect to JACK server\n");
    exit(EXIT_FAILURE);
  }

  if (status & JackServerStarted)
    printf("JACK server started\n");

  if (status & JackNameNotUnique)
  {
    client_name = jack_get_client_name(client);
    printf("New unique name assigned: \"%s\"\n", client_name);
  }

  sample_rate = jack_get_sample_rate(client);
  printf("Sample rate: %f Hz\n", sample_rate);

  jack_set_buffer_size_callback(client, &audio_buffer_size_callback, NULL);

  /* add callback */
  printf("setting process callback\n");
  jack_set_process_callback(client, &process_callback, NULL);

  init_machines();

  // jack_on_shutdown(client, &jack_shutdown, 0);

  input_port_count = 0;
  for (int i = 0; i < 2; i++)
  {
    char name[20];
    snprintf(name, sizeof(name), "input_%d", i + 1);
    jack_port_t *port = jack_port_register(client, name, JACK_DEFAULT_AUDIO_TYPE, JackPortIsInput, 0);

    if (!port)
    {
      fprintf(stderr, "Failed to register a port: %s\n", name);
      exit(EXIT_FAILURE);
    }

    input_port[input_port_count++] = port;
  }

  output_port_1 = jack_port_register(client, "output_L", JACK_DEFAULT_AUDIO_TYPE, JackPortIsOutput, 0);

  if (!output_port_1)
  {
    fprintf(stderr, "Failed to register an output port\n");
    exit(EXIT_FAILURE);
  }

  output_port_2 = jack_port_register(client, "output_R", JACK_DEFAULT_AUDIO_TYPE, JackPortIsOutput, 0);

  if (!output_port_2)
  {
    fprintf(stderr, "Failed to register an output port\n");
    exit(EXIT_FAILURE);
  }

  input_port_midi = jack_port_register(client, "midi_in", JACK_DEFAULT_MIDI_TYPE, JackPortIsInput, 0);

  if (!input_port_midi)
  {
    fprintf(stderr, "Failed to register the input midi port\n");
    exit(EXIT_FAILURE);
  }

  if (jack_activate(client))
  {
    fprintf(stderr, "Cannot activate client\n");
    exit(EXIT_FAILURE);
  }

  /* connect port */
  const char **ports;

  {
    ports = jack_get_ports(client, NULL, NULL, JackPortIsPhysical | JackPortIsInput);
    if (!ports)
    {
      fprintf(stderr, "Failed to get physical output ports\n");
      exit(EXIT_FAILURE);
    }

    if (jack_connect(client, jack_port_name(output_port_1), ports[0]))
    {
      fprintf(stderr, "Failed to connect to physical output ports\n");
    }

    if (jack_connect(client, jack_port_name(output_port_2), ports[1]))
    {
      fprintf(stderr, "Failed to connect to physical output ports\n");
    }

    jack_free(ports);
  }

  {
    ports = jack_get_ports(client, NULL, NULL, JackPortIsPhysical | JackPortIsOutput);
    if (!ports)
    {
      fprintf(stderr, "Failed to get physical input ports\n");
      exit(EXIT_FAILURE);
    }

    if (ports[0])
    {
      if (jack_connect(client, ports[0], jack_port_name(input_port[0])))
      {
        fprintf(stderr, "Failed to connect to physical output ports\n");
      }
    }

    if (ports[1])
    {
      if (jack_connect(client, ports[1], jack_port_name(input_port[1])))
      {
        fprintf(stderr, "Failed to connect to physical output ports\n");
      }
    }

    jack_free(ports);
  }

  ports = jack_get_ports(client, NULL, JACK_DEFAULT_MIDI_TYPE, JackPortIsPhysical|JackPortIsOutput);
  if (!ports)
  {
    fprintf(stderr, "Failed to get physical input ports\n");
    exit(EXIT_FAILURE);
  }

  //printf("port %s\n", ports[0]);

  /* connect to last midi port */
  const char *s = NULL;

  {
    int i = 0;
    while (ports[i])
    {
      s = ports[i];
      i++;
    }
  }

  if (s)
  {
    if (jack_connect(client, s, jack_port_name(input_port_midi)))
    {
      fprintf(stderr, "Failed to connect to midi port\n");
    }
  }

  jack_free(ports);

  printf("init_audio finished.\n");
}

void deinit_audio(void)
{
  if (client)
  {
    jack_client_close(client);
    client = NULL;
  }
}


void *audio_thread_func(void *arg)
{
  printf("Starting audio thread\n");
  wakeup_audio_thread = 1;

  while (1)
  {
    if (*(volatile int *)&wakeup_audio_thread == 1)
    {
      *(volatile int *)&wakeup_audio_thread = 0;
      switch (audio_thread_state)
      {
        case ATS_STARTUP:
          printf("Init audio\n");
          init_audio();
          audio_thread_state = ATS_PROCESSING;
          break;
        case ATS_PROCESSING:
          process_audio();
          break;
        case ATS_STOPPED:
          break;
      }
    }
    else
    {
    }
  }

  return NULL;
}

void start_audio(void)
{
  pthread_create(&audio_thread_obj, NULL, &audio_thread_func, NULL);
}

///////////////////////////////////////////////////////////////////////////////
// Instrument
///////////////////////////////////////////////////////////////////////////////

int note[10] = {-1, -1, -1, -1, -1, -1, -1, -1, -1, -1};
double phase_delta[10];
double phase[7 * 10];

void process_audio_synth(Instrument *inst, int nframes, const void **inputs, void **outputs)
{
  static double last_y = 0.0;
  double *output_l = (double *)outputs[0];
  double *output_r = (double *)outputs[1];

  double freq = inst->sliders[0].value;
  double volume = inst->sliders[1].value;
  double filter_cutoff = inst->sliders[2].value;
  int voices = (int)inst->sliders[3].value;
  double detune = inst->sliders[4].value;

  volume *= 1.0 / voices * (1.0 + (voices - 1) * 0.15);

  double a = (2 * M_PI * filter_cutoff / sample_rate) /
    (2 * M_PI * filter_cutoff / sample_rate + 1);

  while (nframes--)
  {
    double o = 0.0f;
    for (int i = 0; i < 10; i++)
    {
      if (note[i] != -1)
      {
        for (int j = 0; j < voices; j++)
          o += get_waveform(saw, ARRAY_SIZE(saw), phase[i + j * 10]);

        for (int j = 0; j < voices; j++)
        {
          phase[i + j * 10] += freq * phase_delta[i] * (1.0 + (j - (voices / 2.0 + 0.5)) * detune / voices);
          if (phase[i + j * 10] >= 1.0)
            phase[i + j * 10] -= 1.0;
        }
      }
    }

    /* low pass filter */
    double val = a * o + (1.0 - a) * last_y;
    last_y = val;

    output_l[0] = volume * val;
    output_r[0] = volume * val;

    output_l += 1;
    output_r += 1;
  }
}

delay_line_t *make_delay_line(int length, double feedback)
{
  delay_line_t *l = (delay_line_t *)calloc(1, sizeof(delay_line_t));

  l->buf = allocate_double(length);
  l->length = length;
  l->feedback = feedback;

  return l;
}

double process_delay_line_comb(delay_line_t *l, double sample)
{
  /* store the sample */
  double y = l->buf[l->index]; /* load sample */
  l->buf[l->index] = sample + l->feedback * y;

  int i_plus_1 = l->index + 1;
  if (i_plus_1 >= l->length)
    i_plus_1 = 0;
  l->index = i_plus_1;

  return y;
}

double process_delay_line_allpass(delay_line_t *l, double sample)
{
  /* store the sample */
  double y = l->buf[l->index] - l->feedback * sample;
  l->buf[l->index] = sample + l->feedback * y;

  int i_plus_1 = l->index + 1;
  if (i_plus_1 == l->length)
    i_plus_1 = 0;
  l->index = i_plus_1;

  return y;
}

void process_reverb(double *input_l, double *input_r, double *output_l, double *output_r, int nframes, double mix)
{
  for (int i = 0; i < nframes; i++)
  {
    double sample = 0.5 * (input_l[i] + input_r[i]);
    double y = sample;

    //printf("out [%03d] %2.6f %2.6f \n", i, y, y2);
    y = process_delay_line_allpass(delay1[4], y);
    y = process_delay_line_allpass(delay1[5], y);
    y = process_delay_line_allpass(delay1[6], y);


    double x1 = process_delay_line_comb(delay1[0], y);
    double x2 = process_delay_line_comb(delay1[1], y);
    double x3 = process_delay_line_comb(delay1[2], y);
    double x4 = process_delay_line_comb(delay1[3], y);

    double l = x1 + x2 + x3 + x4;
    double r = x1 + x3 - x2 - x4;
    output_l[i] = (1.0 - mix) * sample + mix * l;
    output_r[i] = (1.0 - mix) * sample + mix * r;
    /*
    output_l[i] = x1;
    output_r[i] = x3;
    */
  }
}

double key_to_frequency(int key)
{
  /* note 0 = C0 */
  /* note 9 + 5 * 12 = A4 - 440 Hz */
  return 440.0f * powf(2.0f, (key - (9 + 5 * 12)) / 12.0);
}

void keyboard_input(int key, int note_on, int velocity)
{
  for (int i = 0; i < 10; i++)
  {
    if (note_on)
    {
      if (note[i] == -1)
      {
        note[i] = key;
        phase_delta[i] = key_to_frequency(key) / sample_rate;
        break;
      }
    }
    else
    {
      if (note[i] == key)
      {
        note[i] = -1;
        break;
      }
    }
  }
}

