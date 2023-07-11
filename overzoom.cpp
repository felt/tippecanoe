#include <stdio.h>
#include <stdlib.h>
#include <getopt.h>

extern char *optarg;
extern int optind;

int detail = 12;
int buffer = 16;

void usage(char **argv) {
    fprintf(stderr, "Usage: %s oz ox oy nz nx ny < tile.pbf.gz > newtile.pbf.gz\n", argv[0]);
    fprintf(stderr, "to create tile nz/nx/ny from tile oz/ox/oy\n");
    exit(EXIT_FAILURE);
}

int main(int argc, char **argv) {
    int i;

    while ((i = getopt(argc, argv, "")) != -1) {
        switch (i) {
        default:
            usage(argv);
        }
    }

    if (argc - optind != 6) {
        usage(argv);
    }

    int oz = atoi(argv[optind]);
    int ox = atoi(argv[optind + 1]);
    int oy = atoi(argv[optind + 2]);

    int nz = atoi(argv[optind + 3]);
    int nx = atoi(argv[optind + 4]);
    int ny = atoi(argv[optind + 5]);
}
