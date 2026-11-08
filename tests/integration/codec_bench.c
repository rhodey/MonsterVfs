/* Single-thread libfec cost per default slot, which bounds how far parity can grow. */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

void *init_rs_int(int symbol_bits, int polynomial, int first_root, int root_step, int roots, int padding);
void encode_rs_int(void *codec, unsigned *data, unsigned *parity);
int decode_rs_int(void *codec, unsigned *data, int *positions, int erasures);
void free_rs_int(void *codec);

static double now(void) {
  struct timespec time;
  clock_gettime(CLOCK_MONOTONIC, &time);
  return (double)time.tv_sec + (double)time.tv_nsec / 1e9;
}
int main(void) {
  const int payload = 4096 + 24 + 32;
  const int parity_sizes[] = {32, 64, 128, 256, 512, 1024};
  for (size_t i = 0; i < sizeof(parity_sizes) / sizeof(parity_sizes[0]); i++) {
    int parity = parity_sizes[i], symbols = payload / 2, roots = parity / 2, total = symbols + roots;
    void *codec = init_rs_int(16, 0x1100b, 0, 1, roots, 65535 - total);
    unsigned *codeword = calloc((size_t)total, sizeof(unsigned));
    unsigned *damaged = malloc((size_t)total * sizeof(unsigned));
    if (!codec || !codeword || !damaged) return 1;
    srand(1);
    for (int j = 0; j < symbols; j++) codeword[j] = (unsigned)rand() & 0xffff;
    int encodes = parity > 256 ? 40 : 400, decodes = parity > 256 ? 5 : 40, capacity = roots / 2, good = 0;
    double start = now();
    for (int j = 0; j < encodes; j++) {
      codeword[0] ^= (unsigned)j & 0xffff;
      encode_rs_int(codec, codeword, codeword + symbols);
    }
    double encode = (now() - start) / encodes, decode = 0;
    for (int j = 0; j < decodes; j++) {
      memcpy(damaged, codeword, (size_t)total * sizeof(unsigned));
      for (int k = 0; k < capacity; k++) damaged[(k * 97 + j) % symbols] ^= 0x5a5a;
      start = now();
      int corrected = decode_rs_int(codec, damaged, NULL, 0);
      decode += now() - start;
      good += corrected == capacity && memcmp(damaged, codeword, (size_t)total * sizeof(unsigned)) == 0;
    }
    if (good != decodes) { fprintf(stderr, "correction failed at R=%d\n", parity); return 1; }
    printf("{\"parity_bytes\":%d,\"encode_us\":%.1f,\"encode_mib_s\":%.1f,\"correctable_symbols\":%d,\"correct_us\":%.1f}\n",
           parity, encode * 1e6, payload / encode / 1048576, capacity, decode / decodes * 1e6);
    free_rs_int(codec);
    free(codeword);
    free(damaged);
  }
  return 0;
}
