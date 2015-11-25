#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <getopt.h>

#include "decoder.h"
#include "channel.h"
#include "common.h"

unsigned verbosity = 0;

void usage(int argc, char **argv) {
  fprintf(stderr, "Usage: %s [OPTION]... [FILE]\n", argv[0]);
  fprintf(stderr, "\n");
  fprintf(stderr, "Decode APT signal from audio.\n");
  fprintf(stderr, "\n");
  fprintf(stderr, "Read audio from STDIN if FILE is not specified.\n");
  fprintf(stderr, "\n");
  fprintf(stderr, "Options:\n");
  fprintf(stderr, "  -r RATE    sample rate\n");
  fprintf(stderr, "  -a FILE    write channel A to FILE\n");
  fprintf(stderr, "  -b FILE    write channel B to FILE\n");
  fprintf(stderr, "  -n         normalize image intensity\n");
  fprintf(stderr, "  -v         verbose output\n");
  exit(1);
}

void write_channel(channel *c, unsigned normalize, const char *path) {
  FILE *f;

  if (normalize) {
    channel_normalize(c);
  }

  f = fopen(path, "w");
  if (f == NULL) {
    perror("fopen()");
    exit(1);
  }

  channel_to_pgm(c, f);
  fclose(f);
}

int main(int argc, char **argv) {
  char c;
  FILE *input = NULL;
  char *ca_file = NULL;
  char *cb_file = NULL;
  uint32_t sample_rate = 0;
  unsigned normalize = 0;
  int rv;

  while ((c = getopt(argc, argv, "a:b:r:nv")) != -1) {
    switch (c) {
    case -1:
      break;
    case 'a':
      ca_file = strdup(optarg);
      break;
    case 'b':
      cb_file = strdup(optarg);
      break;
    case 'r':
      sample_rate = atoi(optarg);
      break;
    case 'n':
      normalize = 1;
      break;
    case 'v':
      verbosity = 1;
      break;
    default:
      usage(argc, argv);
    }
  }

  if (sample_rate == 0) {
    usage(argc, argv);
  }

  if (optind < argc) {
    input = fopen(argv[optind], "r");
    if (input == NULL) {
      perror("fopen(input)");
      exit(1);
    }
  } else {
    input = stdin;
  }

  decoder *d = malloc(sizeof(*d));
  decoder_init(d, sample_rate);
  decoder_read_loop(d, input);

  fclose(input);

  rv = channel_detect_telemetry(&d->a);
  if (rv < 0) {
    fprintf(stderr, "Could not detect telemetry on channel A\n");
    exit(1);
  }

  rv = channel_detect_telemetry(&d->b);
  if (rv < 0) {
    fprintf(stderr, "Could not detect telemetry on channel B\n");
    exit(1);
  }

  if (ca_file != NULL) {
    write_channel(&d->a, normalize, ca_file);
  }

  if (cb_file != NULL) {
    write_channel(&d->b, normalize, cb_file);
  }

  return 0;
}
