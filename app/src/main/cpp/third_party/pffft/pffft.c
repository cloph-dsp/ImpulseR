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

#include "pffft.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* FFT setup structure */
struct PFFFT_Setup {
    int N;              /* Transform size */
    int transform;      /* PFFFT_REAL or PFFFT_COMPLEX */
    float* twiddle;     /* Twiddle factors */
    int* ifac;          /* FFT factorization */
};

/* 
   Simple radix-2 Cooley-Tukey FFT implementation.
   This is a simplified version for demonstration purposes.
   Production code should use the full PFFFT implementation with SIMD optimizations.
*/

static void fft_forward(const float* input, float* output, int N) {
    /* Bit-reversal permutation */
    int j = 0;
    for (int i = 0; i < N; i++) {
        if (i < j) {
            output[2*i] = input[2*j];
            output[2*i+1] = input[2*j+1];
            output[2*j] = input[2*i];
            output[2*j+1] = input[2*i+1];
        } else if (i == j) {
            output[2*i] = input[2*i];
            output[2*i+1] = input[2*i+1];
        }
        int m = N >> 1;
        while (m >= 1 && j >= m) {
            j -= m;
            m >>= 1;
        }
        j += m;
    }
    
    /* FFT butterfly operations */
    for (int s = 1; s <= (int)log2(N); s++) {
        int m = 1 << s;
        int m2 = m >> 1;
        float theta = -2.0f * M_PI / m;
        
        for (int k = 0; k < N; k += m) {
            float wR = 1.0f, wI = 0.0f;
            float wmR = cosf(theta), wmI = sinf(theta);
            
            for (int j = 0; j < m2; j++) {
                int t = k + j + m2;
                float tR = wR * output[2*t] - wI * output[2*t+1];
                float tI = wR * output[2*t+1] + wI * output[2*t];
                
                output[2*t] = output[2*(k+j)] - tR;
                output[2*t+1] = output[2*(k+j)+1] - tI;
                output[2*(k+j)] += tR;
                output[2*(k+j)+1] += tI;
                
                float newWR = wR * wmR - wI * wmI;
                float newWI = wR * wmI + wI * wmR;
                wR = newWR;
                wI = newWI;
            }
        }
    }
}

static void fft_inverse(const float* input, float* output, int N) {
    /* Conjugate input */
    float* temp = (float*)malloc(2 * N * sizeof(float));
    for (int i = 0; i < N; i++) {
        temp[2*i] = input[2*i];
        temp[2*i+1] = -input[2*i+1];
    }
    
    /* Forward FFT of conjugated input */
    fft_forward(temp, output, N);
    
    /* Conjugate and scale output */
    float scale = 1.0f / N;
    for (int i = 0; i < N; i++) {
        output[2*i] *= scale;
        output[2*i+1] = -output[2*i+1] * scale;
    }
    
    free(temp);
}

PFFFT_Setup* pffft_new_setup(int N, int transform) {
    if (N <= 0) return NULL;
    
    /* Check if N is a power of 2 */
    if ((N & (N - 1)) != 0) return NULL;
    
    PFFFT_Setup* setup = (PFFFT_Setup*)malloc(sizeof(PFFFT_Setup));
    if (!setup) return NULL;
    
    setup->N = N;
    setup->transform = transform;
    setup->twiddle = NULL;
    setup->ifac = NULL;
    
    return setup;
}

void pffft_destroy_setup(PFFFT_Setup* setup) {
    if (setup) {
        if (setup->twiddle) free(setup->twiddle);
        if (setup->ifac) free(setup->ifac);
        free(setup);
    }
}

void pffft_transform(PFFFT_Setup* setup, const float* input, float* output, float* work, int ordering) {
    if (!setup || !input || !output) return;
    
    int N = setup->N;
    
    if (setup->transform == PFFFT_COMPLEX) {
        /* Complex FFT */
        if (ordering == PFFFT_FORWARD) {
            fft_forward(input, output, N);
        } else {
            fft_inverse(input, output, N);
        }
    } else {
        /* Real FFT - use complex FFT with packing */
        float* temp_in = (float*)malloc(2 * N * sizeof(float));
        float* temp_out = (float*)malloc(2 * N * sizeof(float));
        
        /* Pack real input as complex (real, 0) */
        for (int i = 0; i < N; i++) {
            temp_in[2*i] = input[i];
            temp_in[2*i+1] = 0.0f;
        }
        
        if (ordering == PFFFT_FORWARD) {
            fft_forward(temp_in, temp_out, N);
            
            /* Unpack complex output to real format */
            output[0] = temp_out[0]; /* DC */
            output[1] = temp_out[2*N-2]; /* Nyquist */
            for (int i = 1; i < N/2; i++) {
                output[2*i] = temp_out[2*i];
                output[2*i+1] = temp_out[2*i+1];
            }
        } else {
            /* Pack real input as complex */
            temp_in[0] = input[0]; /* DC */
            temp_in[1] = 0.0f;
            temp_in[2] = input[1]; /* Nyquist */
            temp_in[3] = 0.0f;
            for (int i = 1; i < N/2; i++) {
                temp_in[2*i] = input[2*i];
                temp_in[2*i+1] = input[2*i+1];
            }
            
            fft_inverse(temp_in, temp_out, N);
            
            /* Extract real part */
            for (int i = 0; i < N; i++) {
                output[i] = temp_out[2*i];
            }
        }
        
        free(temp_in);
        free(temp_out);
    }
}

void pffft_transform_ordered(PFFFT_Setup* setup, const float* input, float* output, float* work, int ordering) {
    /* For simplicity, ordered transform is the same as regular transform */
    pffft_transform(setup, input, output, work, ordering);
}

void pffft_zconvolve_accumulate(PFFFT_Setup* setup, const float* a, const float* b, float* ab, float scaling) {
    if (!setup || !a || !b || !ab) return;
    
    int N = setup->N;
    
    /* Complex multiplication: ab[k] = a[k] * b[k] */
    for (int k = 0; k < N; k++) {
        float aR = a[2*k];
        float aI = a[2*k+1];
        float bR = b[2*k];
        float bI = b[2*k+1];
        
        ab[2*k] += (aR * bR - aI * bI) * scaling;
        ab[2*k+1] += (aR * bI + aI * bR) * scaling;
    }
}

int pffft_is_valid_size(int N, int transform) {
    if (N <= 0) return 0;
    
    /* Must be power of 2 */
    if ((N & (N - 1)) != 0) return 0;
    
    /* For real transforms, N must be multiple of 32 */
    if (transform == PFFFT_REAL && (N % 32) != 0) return 0;
    
    /* For complex transforms, N must be multiple of 16 */
    if (transform == PFFFT_COMPLEX && (N % 16) != 0) return 0;
    
    return 1;
}

int pffft_next_power_of_two(int minN, int transform) {
    if (minN <= 0) return 0;
    
    int N = 1;
    while (N < minN) {
        N *= 2;
    }
    
    /* Ensure N meets alignment requirements */
    if (transform == PFFFT_REAL) {
        while (N % 32 != 0) {
            N *= 2;
        }
    } else {
        while (N % 16 != 0) {
            N *= 2;
        }
    }
    
    return N;
}
