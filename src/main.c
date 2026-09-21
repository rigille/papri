#include "utf8.h"

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>

/* A placeholder driver, and for now also the smoke test that the vendored
 * decoder links and runs. It reads standard input into a fixed buffer and
 * reports what it found. The fixed buffer goes away in M1, when the rope
 * replaces it. */

#define INPUT_CAPACITY (1024 * 1024)

/* File scope rather than a local: the subset prefers nonaddressable locals,
 * and a megabyte on the stack is a poor idea regardless. */
static uint8_t input[INPUT_CAPACITY];

/* requires: standard input is readable and standard output is writable.
 * ensures:  at most INPUT_CAPACITY bytes are read from standard input, and
 *           one line naming their count and the number of code points in
 *           their longest valid UTF-8 prefix is written to standard output;
 *           the result is 0.
 */
int main(void)
{
    FILE *input_stream;
    size_t read_count;
    uint64_t byte_count;
    uint64_t codepoint_count;

    /* `stdin` is an extern pointer, so reading it is a load and may not sit
     * inside a call argument. */
    input_stream = stdin;
    read_count = fread(input, 1, INPUT_CAPACITY, input_stream);

    byte_count = read_count;
    codepoint_count = count_codepoints(input, byte_count);

    printf("%" PRIu64 " bytes, %" PRIu64 " code points\n",
           byte_count, codepoint_count);
    return 0;
}
