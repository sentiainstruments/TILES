#pragma once
#define isfinite(x) __builtin_isfinite(x)
#define fabsf(x) __builtin_fabsf(x)
#define fminf(a,b) __builtin_fminf(a,b)
#define fmaxf(a,b) __builtin_fmaxf(a,b)
#define sqrtf(x) __builtin_sqrtf(x)
float powf(float,float); float expf(float); float exp2f(float); float sinf(float); float cosf(float);
