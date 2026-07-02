/*
   PFFFT : a Pretty Fast FFT.
   
   Copyright (c) 2013  Julien Pommier (pommier@modartt.com)
   
   BSD 3-Clause License
   
   Redistribution and use in source and binary forms, with or without
   modification, are permitted provided that the following conditions are met:
   
   1. Redistributions of source code must retain the above copyright notice, this
      list of conditions and the following disclaimer.
   
   2. Redistributions in binary form must reproduce the above copyright notice,
      this list of conditions and the following disclaimer in the documentation
      and/or other materials provided with the distribution.
   
   3. Neither the name of the copyright holder nor the names of its
      contributors may be used to endorse or promote products derived from
      this software without specific prior written permission.
   
   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
   AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
   IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
   DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
   FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
   DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
   SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
   CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
   OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
   OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#ifndef PFFFT_H
#define PFFFT_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 
   PFFFT can handle transforms of complex numbers and real numbers.
   For complex transforms, the input and output arrays are interleaved
   as [real0, imag0, real1, imag1, ...].
   For real transforms, the input is [real0, real1, ...] and the output
   is [real0, real1, ..., realN/2, imagN/2-1, ..., imag1].
*/

/* Opaque type for FFT setup */
typedef struct PFFFT_Setup PFFFT_Setup;

/* 
   Create a new FFT setup for the given transform size.
   The transform size must be a multiple of 16 for complex transforms
   or a multiple of 32 for real transforms.
   
   @param N Transform size
   @param transform PFFFT_REAL or PFFFT_COMPLEX
   @return Pointer to setup, or NULL on failure
*/
PFFFT_Setup* pffft_new_setup(int N, int transform);

/* 
   Free an FFT setup.
   
   @param setup Pointer to setup to free
*/
void pffft_destroy_setup(PFFFT_Setup* setup);

/* Transform types */
#define PFFFT_REAL    0
#define PFFFT_COMPLEX 1

/* 
   Perform a forward FFT transform.
   
   @param setup FFT setup
   @param input Input array (2*N floats for complex, N floats for real)
   @param output Output array (same size as input)
   @param work Temporary work array (2*N floats for complex, N floats for real)
*/
void pffft_transform(PFFFT_Setup* setup, const float* input, float* output, float* work, int ordering);

/* 
   Perform an inverse FFT transform.
   
   @param setup FFT setup
   @param input Input array (2*N floats for complex, N floats for real)
   @param output Output array (same size as input)
   @param work Temporary work array (2*N floats for complex, N floats for real)
*/
void pffft_transform_ordered(PFFFT_Setup* setup, const float* input, float* output, float* work, int ordering);

/* Ordering modes */
#define PFFFT_FORWARD  0
#define PFFFT_BACKWARD 1

/* 
   Perform a convolution in the frequency domain.
   
   @param setup FFT setup
   @param a First spectrum (2*N floats)
   @param b Second spectrum (2*N floats)
   @param ab Output spectrum (2*N floats) = a * b
*/
void pffft_zconvolve_accumulate(PFFFT_Setup* setup, const float* a, const float* b, float* ab, float scaling);

/* 
   Check if the given size is valid for PFFFT.
   
   @param N Transform size
   @param transform PFFFT_REAL or PFFFT_COMPLEX
   @return 1 if valid, 0 otherwise
*/
int pffft_is_valid_size(int N, int transform);

/* 
   Get the next valid size >= minN.
   
   @param minN Minimum size
   @param transform PFFFT_REAL or PFFFT_COMPLEX
   @return Next valid size
*/
int pffft_next_power_of_two(int minN, int transform);

#ifdef __cplusplus
}
#endif

#endif /* PFFFT_H */
