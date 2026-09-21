// Copyright (c) 2008-2009 Bjoern Hoehrmann <bjoern@hoehrmann.de>
// 
// Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated documentation files (the "Software"), to deal in the Software without restriction, including without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is furnished to do so, subject to the following conditions:
// 
// The above copyright notice and this permission notice shall be included in all copies or substantial portions of the Software.
// 
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

#include <stdint.h>
#include <stdio.h>

#define UTF8_ACCEPT 0
#define UTF8_REJECT 12

enum utf8_decode_error {
    UTF8_OK = 0,
    UTF8_ERROR,
    UTF8_NOT_ENOUGH_BYTES,
    UTF8_TOO_MANY_BYTES,
    UTF8_INVALID_START_HEADER,
    UTF8_INVALID_CONTINUATION_HEADER,
    UTF8_CODEPOINT_TOO_BIG,
};

static const uint8_t utf8_automaton[] = {
  // The first part of the table maps bytes to character classes that
  // to reduce the size of the transition table and create bitmasks.
   0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,  0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
   0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,  0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
   0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,  0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
   0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,  0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
   1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,  9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,
   7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,  7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,
   8,8,2,2,2,2,2,2,2,2,2,2,2,2,2,2,  2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,
  10,3,3,3,3,3,3,3,3,3,3,3,3,4,3,3, 11,6,6,6,5,8,8,8,8,8,8,8,8,8,8,8,

  // The second part is a transition table that maps a combination
  // of a state of the automaton and a character class to a state.
   0,12,24,36,60,96,84,12,12,12,48,72, 12,12,12,12,12,12,12,12,12,12,12,12,
  12, 0,12,12,12,12,12, 0,12, 0,12,12, 12,24,12,12,12,12,12,24,12,24,12,12,
  12,12,12,12,12,12,12,24,12,12,12,12, 12,24,12,12,12,12,12,12,12,24,12,12,
  12,12,12,12,12,12,12,36,12,36,12,12, 12,36,12,12,12,12,12,36,12,36,12,12,
  12,36,12,12,12,12,12,12,12,12,12,12, 
};

uint32_t next_state(uint32_t state, uint8_t byte, uint32_t* codepoint_pointer) {
  uint32_t type = utf8_automaton[byte];
  uint32_t codepoint = *codepoint_pointer;

  *codepoint_pointer = (state != UTF8_ACCEPT) ?
    (byte & 0x3fu) | (codepoint << 6) :
    (0xff >> type) & (byte);

  uint32_t new_state = utf8_automaton[256 + state + type];
  return new_state;
}

struct string {
  uint8_t* text;
  uint64_t length;
}; 

enum utf8_decode_error next_codepoint(uint8_t* text, uint64_t length, uint64_t* index, uint32_t* codepoint_out) {
  uint32_t state = 0;
  *codepoint_out = 0;
  if (*index >= length) {
    return UTF8_NOT_ENOUGH_BYTES;
  }
  state = next_state(state, text[*index], codepoint_out);
  if (state == UTF8_REJECT) return UTF8_ERROR;
  (*index)++;
  if (state == UTF8_ACCEPT) {
    return UTF8_OK;
  }
  if (*index >= length) {
    return UTF8_NOT_ENOUGH_BYTES;
  }
  state = next_state(state, text[*index], codepoint_out);
  if (state == UTF8_REJECT) return UTF8_ERROR;
  (*index)++;
  if (state == UTF8_ACCEPT) {
    return UTF8_OK;
  }
  if (*index >= length) {
    return UTF8_NOT_ENOUGH_BYTES;
  }
  state = next_state(state, text[*index], codepoint_out);
  if (state == UTF8_REJECT) return UTF8_ERROR;
  (*index)++;
  if (state == UTF8_ACCEPT) {
    return UTF8_OK;
  }
  if (*index >= length) {
    return UTF8_NOT_ENOUGH_BYTES;
  }
  state = next_state(state, text[*index], codepoint_out);
  if (state == UTF8_REJECT) return UTF8_ERROR;
  (*index)++;
  if (state == UTF8_ACCEPT) {
    return UTF8_OK;
  }
  return UTF8_ERROR;
}

uint64_t count_codepoints(uint8_t* text, uint64_t length) {
  enum utf8_decode_error return_code = 0;
  uint64_t count = 0;
  uint64_t index = 0;
  uint32_t current_codepoint;
  while (index < length) {
    return_code = next_codepoint(text, length, &index, &current_codepoint);
    if (return_code) {
      break;
    }
    count++;
  }
  return count;
}

/* int main() { */
/*     uint8_t test_string[] = "\x41\x00\x7F\xC2\x80\xC2\xBF\xDF\x80\xDF\xBF\xE0\xA0\x80\xE0\xBF\xBF\xE1\x80\x80\xEC\xBF\xBF\xED\x80\x80\xED\x9F\xBF\xEE\x80\x80\xEF\xBF\xBF\xF0\x90\x80\x80\xF0\xBF\xBF\xBF\xF1\x80\x80\x80\xF3\xBF\xBF\xBF\xF4\x80\x80\x80\xF4\x8F\xBF\xBF"; */
/*     return (int) count_codepoints(test_string, sizeof(test_string) - 1); */
/* } */
