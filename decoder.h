#ifndef _DECODER_H
#define _DECODER_H

#include <stdint.h>

#include "channel.h"

#ifndef M_TAU
#define M_TAU 6.28318530717958647693
#endif

#define CARRIER_FREQ 2400
#define WORD_FREQ 4160
#define SYNC_PULSE_FREQ (WORD_FREQ / 4)

typedef struct {
  // Sample rate.
  uint16_t sr;

  // Cosine/sine of carrier phase difference between two
  // samples given the sample rate above.
  // Both are used to compute the instantaneous signal amplitude.
  float cosphi2;
  float sinphi;

  uint32_t pos; // Position in buffer
  uint32_t npos; // Position for new data
  uint32_t len; // Length of buffer
  uint32_t mask; // Length mask
  int16_t *raw; // Raw samples
  uint16_t *ampl; // Instantaneous amplitude
  uint32_t *msum; // Moving sum

  // Number of samples in single synchronization pulse
  uint16_t sync_pulse;

  // Number of samples in synchronization signal
  uint16_t sync_window;

  channel a;
  channel b;
} decoder;

void decoder_init(decoder *s, uint16_t sample_rate);

int8_t decoder_read_loop(decoder *s, FILE *f);

#endif
