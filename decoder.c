#include <math.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdio.h>
#include <errno.h>

#include "decoder.h"
#include "common.h"

static unsigned int npow2(unsigned int v) {
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
  int div = pos / s->sr;
  int rem = pos % s->sr;
  snprintf(str, sizeof(str), "%02d:%02d.%03d", div / 60, div % 60, (1000 * rem) / s->sr);
  return str;
}

void decoder_init(decoder *s, uint32_t sample_rate) {
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
  s->mavg = calloc(s->len, sizeof(s->mavg[0]));

  // Initialize channels
  channel_init(&s->a);
  channel_init(&s->b);
}

static int _read(short *buf, FILE *f, int count) {
  int bytes_to_read;
  int bytes_read;
  char *byte_offset;

  bytes_to_read = sizeof(buf[0]) * count;
  byte_offset = (char *) buf;
  while (bytes_to_read > 0) {
    bytes_read = fread(byte_offset, 1, bytes_to_read, f);
    if (bytes_read < 0) {
      return -1;
    }

    if (bytes_read == 0) {
      errno = 0;
      return -1;
    }

    bytes_to_read -= bytes_read;
    byte_offset += bytes_read;
  }

  return 0;
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

int apt_fill_buffer(decoder *s, FILE *f) {
  int pos = s->pos & s->mask;
  int npos = s->npos & s->mask;
  int size;
  int rv;

  if (npos < pos) {
    size = pos - npos;
  } else {
    size = (s->len - npos) + pos;
  }

  // Always keep 1 sample in history for accurate
  // instantaneous amplitude computation.
  size--;

  // Either the whole size is read in one chunk, or in two if
  // the index wraps around.
  if (npos + size <= s->len) {
    rv = _read(s->raw + npos, f, size);
    if (rv < 0) {
      return rv;
    }
  } else {
    int suffix_size = s->len - npos;
    int prefix_size = size - suffix_size;

    rv = _read(s->raw + npos, f, suffix_size);
    if (rv < 0) {
      return rv;
    }

    rv = _read(s->raw, f, prefix_size);
    if (rv < 0) {
      return rv;
    }
  }

  _decoder_fill_amplitude_buffer(s, size);

  s->npos += size;

  return size;
}

int decoder_find_sync(decoder *s, int search_length, int *max_response_dst) {
  int sync_window; // Window size for pulse train
  int sync_pulse; // Number of samples in single cycle
  int pos;
  unsigned short avg;
  int i;
  int j;

  // Synchronization signal (channel A) is 7 cycles at 1040Hz
  sync_window = (7 * s->sr) / SYNC_PULSE_FREQ;

  // Single synchronization pulse at 1040Hz
  sync_pulse = (s->sr) / SYNC_PULSE_FREQ;

  // Initialize msum at s->pos-1 so it can be
  // iteratively computed starting from s->pos.
  pos = s->pos-1;
  s->msum[pos & s->mask] = 0;
  for (i = sync_window-1; i >= 0; i--) {
    s->msum[pos & s->mask] += s->ampl[(pos - i) & s->mask];
  }

  // Search for best response from sync pulse detector
  int max_pos = 0;
  int max_response = INT32_MIN;
  for (i = 0; i < search_length; i++) {
    pos = s->pos + i;

    // Compute sum
    s->msum[pos & s->mask] =
      s->msum[(pos - 1) & s->mask]
      - s->ampl[(pos - sync_window) & s->mask] // Subtract sample
      + s->ampl[pos & s->mask];                // Add sample

    // Compute average
    avg = s->msum[pos & s->mask] / sync_window;

    // Compute sync detector response
    int sync_base = pos - sync_window - 1;
    int sync_pos;
    int sync_response = 0;
    for (j = 0; j < 7; j++) {
      int k = 0;
      int d;

      sync_pos = sync_base + (j * s->sr) / SYNC_PULSE_FREQ;

      // High side of pulse
      for (; k < (sync_pulse / 2); k++) {
        d = s->ampl[(sync_pos + k) & s->mask] - avg;
        sync_response += d;
      }

      // Skip sample if we have an odd number of samples per pulse
      if (sync_pulse & 1) {
        k++;
      }

      // Low side of pulse
      for (; k < sync_pulse; k++) {
        d = s->ampl[(sync_pos + k) & s->mask] - avg;
        sync_response -= d;
      }
    }

    // Normalize sync detector response
    sync_response /= (14 * (sync_pulse & ~0x1));
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

int decoder_read_loop(decoder *s, FILE *f) {
  int rv;
  int i;
  int search_limit = s->sr;
  int detect_pos;
  int resp;
  int resp_arr[16];
  int resp_sum = 0;
  int resp_sq_sum = 0;
  unsigned int resp_dev;
  unsigned has_lock = 0;

  // Initialize array with detector responses
  for (i = 0; i < 16; i++) {
    resp_arr[i] = 0;
  }

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
