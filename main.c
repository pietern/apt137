#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "decoder.h"
#include "channel.h"

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
  exit(1);
}

int main(int argc, char **argv) {
  char c;
  FILE *input = NULL;
  char *ca_file = NULL;
  char *cb_file = NULL;
  uint32_t sample_rate = 0;

  while ((c = getopt(argc, argv, "a:b:r:")) != -1) {
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

  if (ca_file != NULL) {
    FILE *f = fopen(ca_file, "w");
    if (f == NULL) {
      perror("fopen(channel A)");
      exit(1);
    }

    channel_to_pgm(&d->a, f);
    fclose(f);
  }

  if (cb_file != NULL) {
    FILE *f = fopen(cb_file, "w");
    if (f == NULL) {
      perror("fopen(channel B)");
      exit(1);
    }

    channel_to_pgm(&d->b, f);
    fclose(f);
  }

  return 0;
}
