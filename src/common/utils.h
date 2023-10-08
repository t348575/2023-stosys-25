/*
 * MIT License
Copyright (c) 2021 - current
Authors:  Animesh Trivedi
This code is part of the Storage System Course at VU Amsterdam
Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:
The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.
THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
 */

#ifndef STOSYS_PROJECT_UTILS_H
#define STOSYS_PROJECT_UTILS_H

#include <cstdint>
#include <string>
#include <vector>

#define ONE_BILLION 1000000000.0

extern "C" {
void write_pattern(char *data, uint64_t len);
void print_pattern(const char *data, uint64_t len, bool in_hex);
void match_pattern(char *data, uint64_t len);
void write_pattern_with_start(char *data, uint64_t len, uint64_t s);
void match_pattern_with_start(char *data, uint64_t len, uint64_t s);

uint64_t u64_from_buffer(const uint8_t *buffer);
uint16_t u16_from_buffer(const uint8_t *buffer);

void u64_to_buffer(const uint64_t value, uint8_t *output);
void u16_to_buffer(const uint16_t value, uint8_t *output);

uint64_t closest_upper_multiple(uint64_t n, uint64_t x);
uint64_t closest_lower_multiple(uint64_t n, uint64_t x);
std::vector<std::string> split(const std::string &input, const std::string &delimiter);
}

uint64_t microseconds_since_epoch();
std::string get_vm_stats();
#endif // STOSYS_PROJECT_UTILS_H
