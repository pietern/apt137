#include <math.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>

#include "decoder.h"
#include "common.h"

static uint32_t npow2(uint32_t v) {
  v--;
  v |= v >> 1;
  v |= v >> 2;
  v |= v >> 4;
  v |= v >> 8;
  v |= v >> 16;
  v++;
  return v;
}

static const char *pos2time(decoder *s, uint32_t pos) {
  static char str[10];
  uint32_t div = pos / s->sr;
  uint32_t rem = pos % s->sr;
  snprintf(str, sizeof(str), "%02d:%02d.%03d", div / 60, div % 60, (1000 * rem) / s->sr);
  return str;
}

void decoder_init(decoder *s, uint16_t sample_rate) {
  s->sr = sample_rate;

  // Initialize static, sample rate dependant variables
  double phi = M_TAU * ((float)CARRIER_FREQ / (float)s->sr);
  s->cosphi2 = cos(phi) * 2.0f;
  s->sinphi = sin(phi);

  // Initialize buffers
  s->pos = 0;
  s->npos = 0;
  s->len = npow2(s->sr);
  s->mask = s->len - 1;
  s->raw = calloc(s->len, sizeof(s->raw[0]));
  s->ampl = calloc(s->len, sizeof(s->ampl[0]));
  s->msum = calloc(s->len, sizeof(s->msum[0]));

  // Single synchronization pulse at 1040Hz
  s->sync_pulse = (s->sr) / SYNC_PULSE_FREQ;

  // Synchronization signal (channel A) is 7 cycles at 1040Hz
  s->sync_window = (7 * s->sr) / SYNC_PULSE_FREQ;

  // Initialize channels
  channel_init(&s->a);
  channel_init(&s->b);
}

// The signal is amplitude-modulated on a 2.4KHz carrier.
// Amplitude of this carrier is derived from two consecutive samples,
// taking into account their phase difference given the sample rate.
//
// If the sample rate were 9.6KHz, there would be 4 samples per cycle,
// with a phase difference of 90 degrees. Then, amplitude can easily
// be computed by sqrt(a^2 + b^2).
//
// For any other phase difference, this computation is parameterized
// by the phase difference.
//
//static unsigned short _apt_carrier_amplitude(decoder *s, int pos) {
//  double a = (double)s->raw[I(s, pos-1)];
//  double b = (double)s->raw[I(s, pos)];
//  return (unsigned short) sqrt(a*a + b*b - a*b*s->cosphi2) / s->sinphi;
//}
static void _decoder_fill_amplitude_buffer(decoder *s, int size) {
  double prev;
  double cur;
  double prev2;
  double cur2;
  int npos = s->npos & s->mask;
  int i;

  prev = (double)s->raw[(npos - 1) & s->mask];
  prev2 = prev * prev;
  for (i = 0; i < size; i++) {
    cur = (double)s->raw[npos];
    cur2 = cur * cur;
    s->ampl[npos] = sqrt(prev2 + cur2 - prev*cur*s->cosphi2) / s->sinphi;

    // Advance to next sample
    prev = cur;
    prev2 = cur2;
    npos = (npos + 1) & s->mask;
  }
}

static void _decoder_fill_moving_sum_buffer(decoder *s, uint32_t size) {
  uint32_t npos = s->npos;
  uint32_t epos = s->npos + size;

  for (; npos < epos; npos++) {
    s->msum[npos & s->mask] =
      s->msum[(npos - 1) & s->mask]
        - s->ampl[(npos - s->sync_window) & s->mask]
        + s->ampl[npos & s->mask];
  }
}

int8_t apt_fill_buffer(decoder *s, FILE *f) {
  uint32_t pos = s->pos & s->mask;
  uint32_t npos = s->npos & s->mask;
  uint32_t size;
  size_t rv;

  if (npos < pos) {
    size = pos - npos;
  } else {
    size = (s->len - npos) + pos;
  }

  // Keep 'sync_window' samples in the history so that
  // the sync detector can look back far enough.
  size -= s->sync_window;

  // Either the whole size is read in one chunk, or in two if
  // the index wraps around.
  if (npos + size <= s->len) {
    rv = fread(s->raw + npos, sizeof(s->raw[0]), size, f);
    if (rv < size) {
      return -1;
    }
  } else {
    uint32_t suffix_size = s->len - npos;
    uint32_t prefix_size = size - suffix_size;

    rv = fread(s->raw + npos, sizeof(s->raw[0]), suffix_size, f);
    if (rv < suffix_size) {
      return -1;
    }

    rv = fread(s->raw, sizeof(s->raw[0]), prefix_size, f);
    if (rv < prefix_size) {
      return -1;
    }
  }

  _decoder_fill_amplitude_buffer(s, size);

  _decoder_fill_moving_sum_buffer(s, size);

  s->npos += size;

  return 0;
}

uint32_t decoder_find_sync(decoder *s, int32_t search_length, int32_t *max_response_dst) {
  uint32_t pos = s->pos;
  uint32_t epos = s->pos + search_length;
  uint16_t avg;
  uint32_t max_pos = 0;
  int32_t max_response = INT32_MIN;

  // Search for best response from sync pulse detector
  for (; pos < epos; pos++) {
    uint32_t sync_base = pos - s->sync_window - 1;
    uint32_t sync_pos;
    int32_t sync_response = 0;
    uint8_t j;
    uint8_t k;

    avg = s->msum[pos & s->mask] / s->sync_window;

    // Compute sync detector response
    for (j = 0; j < 7; j++) {
      sync_pos = sync_base + (j * s->sr) / SYNC_PULSE_FREQ;

      // High side of pulse
      for (k = 0; k < (s->sync_pulse / 2); k++) {
        sync_response += s->ampl[(sync_pos + k) & s->mask] - avg;
      }

      // Skip sample if we have an odd number of samples per pulse
      if (s->sync_pulse & 1) {
        k++;
      }

      // Low side of pulse
      for (; k < s->sync_pulse; k++) {
        sync_response -= s->ampl[(sync_pos + k) & s->mask] - avg;
      }
    }

    // Normalize sync detector response
    sync_response /= (14 * (s->sync_pulse & ~0x1));
    if (sync_response > max_response) {
      max_response = sync_response;
      max_pos = pos;
    }
  }

  if (max_response_dst) {
    *max_response_dst = max_response;
  }

  // Move over tail end of sync train
  max_pos = max_pos + (7 * s->sr) / WORD_FREQ;
  return max_pos;
}

void decoder_read_line(decoder *s, channel *c, int start_pos) {
  uint16_t *buf;
  int i;
  int j;
  int spos;
  int epos;
  uint32_t sum;

  // Allocate space for this line
  buf = channel_alloc_line(c);

  // Iterate over words in line
  for (i = 0; i < CHANNEL_WORDS; i++) {
    spos = start_pos + ((i + 0) * s->sr) / WORD_FREQ;
    epos = start_pos + ((i + 1) * s->sr) / WORD_FREQ;

    // Compute average of available samples for word
    sum = 0;
    for (j = spos; j < epos ; j++) {
      sum += s->ampl[j & s->mask];
    }

    buf[i] = sum / (epos - spos);
  }
}

int8_t decoder_read_loop(decoder *s, FILE *f) {
  int8_t rv;
  int8_t i;
  uint32_t search_limit = s->sr;
  uint32_t detect_pos;
  int32_t resp;
  int64_t resp_arr[16];
  int64_t resp_sum = 0;
  int64_t resp_sq_sum = 0;
  int16_t resp_dev;
  unsigned has_lock = 0;

  // Initialize array with detector responses
  memset(resp_arr, 0, sizeof(resp_arr));

  // Main read loop
  for (i = 0;; i++) {
    rv = apt_fill_buffer(s, f);
    if (rv < 0) {
      return rv;
    }

    // Run synchronization pulse detector
    detect_pos = decoder_find_sync(s, search_limit, &resp);

    // Replace the (i - 16)th value with new detector response
    resp_sum += (resp) - (resp_arr[i & 0xf]);
    resp_sq_sum += (resp * resp) - (resp_arr[i & 0xf] * resp_arr[i & 0xf]);
    resp_arr[i & 0xf] = resp;
    resp_dev = (unsigned int) sqrt((resp_sq_sum - (resp_sum * resp_sum) / 16) / 16);

    // Use detector response stddev to conclude signal lock
    if (!has_lock) {
      if (resp_dev < 50) {
        if (verbosity) {
          fprintf(stderr, "[%s]: Acquired lock\n", pos2time(s, s->pos));
        }
        has_lock = 1;
      }
    } else {
      if (resp_dev > 200) {
        if (verbosity) {
          fprintf(stderr, "[%s]: Lost lock\n", pos2time(s, s->pos));
        }
        has_lock = 0;
      }
    }

    if (has_lock) {
      search_limit = (SYNC_WORDS * s->sr) / WORD_FREQ;
    } else {
      search_limit = (2 * (SYNC_WORDS + CHANNEL_WORDS) * s->sr) / WORD_FREQ;
    }

    s->pos = detect_pos;

    // Process channel A
    decoder_read_line(s, &s->a, s->pos);

    // Skip over channel A and sync train for channel B
    s->pos += ((CHANNEL_WORDS + SYNC_WORDS) * s->sr) / WORD_FREQ;

    // Process channel B
    decoder_read_line(s, &s->b, s->pos);

    // Skip over channel B
    s->pos += (CHANNEL_WORDS * s->sr) / WORD_FREQ;
  }
}
