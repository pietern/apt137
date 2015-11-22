#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "channel.h"

void channel_init(channel *c) {
  c->raw = NULL;
  c->size = 0;

  c->wedge_mean = NULL;
  c->wedge_stddev = NULL;
}

uint16_t *channel_alloc_line(channel *c) {
  uint32_t size = c->size;
  c->size += CHANNEL_WORDS;
  c->raw = realloc(c->raw, c->size * sizeof(uint16_t));
  memset(c->raw + size, 0, CHANNEL_WORDS * sizeof(uint16_t));
  return c->raw + size;
}

int channel_to_pgm(channel *c, FILE *f) {
  int width = CHANNEL_WORDS;
  int height = c->size / width;
  int i;
  int j;

  fprintf(f, "P2 %d %d %d\n", width, height, 65535);
  for (i = 0; i < height; i++) {
    for (j = 0; j < width; j++) {
      fprintf(f, "%d ", c->raw[i * width + j]);
    }

    fprintf(f, "\n");
  }

  return 0;
}

void channel_compute_wedge_stats(channel *c) {
  const int N = TELEMETRY_WEDGE_WORDS;
  uint16_t width = CHANNEL_WORDS;
  uint16_t height = c->size / width;
  uint16_t i;
  uint16_t j;
  uint32_t v;
  uint64_t sums[8];
  uint64_t sq_sums[8];
  uint64_t sum = 0;
  uint64_t sq_sum = 0;
  uint64_t stddev;

  // Initialize arrays
  for (i = 0; i < 8; i++) {
    sums[i] = 0;
    sq_sums[i] = 0;
  }

  // Allocate space for wedge mean and stddev
  c->wedge_mean = calloc(height, sizeof(c->wedge_mean[0]));
  c->wedge_stddev = calloc(height, sizeof(c->wedge_stddev[0]));

  for (i = 0; i < height; i++) {
    // Subtract line from accumulators
    sum -= sums[i & 0x7];
    sq_sum -= sq_sums[i & 0x7];

    // Compute sum/squared sum for this line
    sums[i & 0x7] = 0;
    sq_sums[i & 0x7] = 0;
    for (j = CHANNEL_WORDS - TELEMETRY_WORDS; j < CHANNEL_WORDS; j++) {
      v = c->raw[i * width + j];
      sums[i & 0x7] += v;
      sq_sums[i & 0x7] += v*v;
    }

    // Add line to accumulators
    sum += sums[i & 0x7];
    sq_sum += sq_sums[i & 0x7];

    // Compute stddev
    stddev = sqrt((sq_sum - (sum * sum) / N) / N);

    // Store measurements
    c->wedge_mean[i] = sum / N;
    c->wedge_stddev[i] = stddev;

    // Accumulate data for at least 8 lines before
    // asserting minimum deviation.
    if (i < 7) {
      continue;
    }
  }
}

int channel_find_frame_offset(channel *c) {
  uint16_t width = CHANNEL_WORDS;
  uint16_t height = c->size / width;
  uint16_t *mean = c->wedge_mean;
  uint16_t *stddev = c->wedge_stddev;
  uint16_t i;
  uint16_t j ;

  for (i = 0; i < height;) {
    // Check if minimum wrt previous line (only if not first wedge)
    if (i > 0 && j > 0 && stddev[i-1] < stddev[i]) {
      // Not a minimum; reset
      j = 0;
      i++;
      continue;
    }

    // Check if minimum wrt next line (only if not last wedge)
    if (i < (height - 1) && j < 7 && stddev[i+1] < stddev[i]) {
      // Not a minimum; reset
      j = 0;
      i++;
      continue;
    }

    // Check for brightness increase (only if not first wedge)
    if (j > 0 && mean[i] < mean[i-8]) {
      // No increasing brightness; reset
      j = 0;
      i++;
      continue;
    }

    j++;
    i += 8;
    if (j == 8) {
      // Found 8 consecutive wedges of increasing brightness!
      return i - 64;
    }
  }

  return -1;
}
