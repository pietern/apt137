#ifndef _CHANNEL_H
#define _CHANNEL_H

#include <stdint.h>
#include <stdio.h>

#define SYNC_WORDS 39
#define SPACE_WORDS 47
#define CHANNEL_DATA_WORDS 909
#define TELEMETRY_WORDS 45
#define CHANNEL_WORDS (SPACE_WORDS + CHANNEL_DATA_WORDS + TELEMETRY_WORDS)

#define TELEMETRY_WEDGE_WORDS (8 * TELEMETRY_WORDS)

typedef struct {
  uint16_t *raw;
  uint32_t size;

  uint16_t *wedge_mean;
  uint16_t *wedge_stddev;
} channel;

void channel_init(channel *c);

uint16_t *channel_alloc_line(channel *c);

int channel_to_pgm(channel *c, FILE *f);

void channel_compute_wedge_stats(channel *c);

int channel_find_frame_offset(channel *c);

#endif
