#include "modal.h"
#include <math.h>
#include <string.h>

#define PI 3.14159265358979323846f
#define LN1000 6.907755278982137f

static float clamp(float x, float lo, float hi) {
    return !isfinite(x) ? lo : fminf(hi, fmaxf(lo, x));
}
static float mix(float a, float b, float t) { return a + (b-a)*t; }

/* Original illustrative spectra, not measurements of real instruments.
 * Keep the fundamental tuned; upper modes morph toward inharmonic ratios. */
static const float wood[MODAL_COUNT] = {1, 2, 3.01f, 4.07f, 5.18f, 6.3f, 7.6f, 9.1f, 10.8f, 12.5f, 14.8f, 17.2f};
static const float glass[MODAL_COUNT] = {1, 2.32f, 3.78f, 5.12f, 6.85f, 8.42f, 10.3f, 12.1f, 14.4f, 17.2f, 19.8f, 23.1f};
static const float metal[MODAL_COUNT] = {1, 1.48f, 2.09f, 2.76f, 4.05f, 5.43f, 7.02f, 9.13f, 11.6f, 14.9f, 18.4f, 22.7f};

float modal_ratio(unsigned mode, float material) {
    if (mode >= MODAL_COUNT) return 0;
    material = clamp(material,0,1);
    return material <= .5f ? mix(wood[mode],glass[mode],material*2)
                           : mix(glass[mode],metal[mode],material*2-1);
}
float modal_t60(float decay) { return .12f * powf(50.f, clamp(decay,0,1)); }

static float noise(ModalVoice *v) {
    uint32_t x=v->rng;
    x ^= x<<13; x ^= x>>17; x ^= x<<5;
    v->rng=x;
    return ((float)(x>>8) / 8388608.f)-1.f;
}

/* Recompute coefficients at control rate, not every audio sample.
 * A complex pole of magnitude r decays by 60 dB in T60 seconds:
 * r = exp(-ln(1000)/(T60*sample_rate)). */
static void update_modes(ModalVoice *v, ModalExpression e, int initial) {
    const float smooth = .16f;
    v->pressure = initial ? clamp(e.pressure,0,1) : mix(v->pressure,clamp(e.pressure,0,1),smooth);
    v->slide = initial ? clamp(e.slide,0,1) : mix(v->slide,clamp(e.slide,0,1),smooth);
    v->bend = initial ? clamp(e.bend,-48,48) : mix(v->bend,clamp(e.bend,-48,48),smooth);
    float material=clamp(v->params.material + (v->slide-.5f)*.36f,0,1);
    float bright=clamp(v->params.brightness + .32f*v->pressure,0,1);
    float f0=440.f*exp2f((v->note+v->bend-69.f)/12.f);
    float t60=modal_t60(v->params.decay)*(1.f+.8f*v->pressure);
    if (v->released) t60=fminf(t60,.25f);
    for (unsigned i=0;i<MODAL_COUNT;i++) {
        ModalMode *m=&v->modes[i];
        float ratio=modal_ratio(i,material), f=f0*ratio;
        /* Higher modes decay faster. Suppress modes near/above Nyquist. */
        float duration=t60/(1.f+(float)i*(.18f+.35f*(1.f-material)));
        float r=expf(-LN1000/(duration*v->sample_rate));
        float audible=clamp((.47f*v->sample_rate-f)/(.07f*v->sample_rate),0,1);
        float theta=2.f*PI*fminf(f,.47f*v->sample_rate)/v->sample_rate;
        m->c=r*cosf(theta); m->s=r*sinf(theta);
        m->gain=.52f*expf(-(float)i*mix(.8f,.14f,bright))*audible;
        float pan = i==0 ? 0.f : ((i%2u)?-1.f:1.f)*v->params.spread*(.25f+.75f*(float)i/(MODAL_COUNT-1));
        m->pan_l=sqrtf(.5f*(1.f-pan)); m->pan_r=sqrtf(.5f*(1.f+pan));
    }
}

void modal_start(ModalVoice *v, float sample_rate, float midi_note,
                 float velocity, ModalParams params, uint32_t seed) {
    memset(v,0,sizeof *v);
    v->sample_rate=clamp(sample_rate,8000,192000);
    v->note=clamp(midi_note,0,127); v->velocity=clamp(velocity,0,1);
    v->params=(ModalParams){clamp(params.material,0,1),clamp(params.decay,0,1),clamp(params.brightness,0,1),clamp(params.spread,0,1)};
    v->rng=seed?seed:1u;
    v->excitation=.07f*v->velocity;
    update_modes(v,(ModalExpression){0,.5f,0},1);
    /* A strike starts all resonators. Alternate phase to reduce initial peaks. */
    for (unsigned i=0;i<MODAL_COUNT;i++) v->modes[i].re=v->velocity*((i%2u)?-.65f:.65f);
}

void modal_release(ModalVoice *v) { v->released=1; v->control_count=0; }

void modal_render(ModalVoice *v, ModalExpression expression, float *left, float *right) {
    if (v->control_count==0) {
        update_modes(v,expression,0);
        v->control_count=MODAL_CONTROL_INTERVAL;
    }
    --v->control_count;
    /* Very short noisy strike gives the resonators an organic attack. */
    float strike = v->excitation > 1e-6f ? v->excitation*noise(v) : 0.f;
    v->excitation *= .975f;
    float l=0,r=0;
    for (unsigned i=0;i<MODAL_COUNT;i++) {
        ModalMode *m=&v->modes[i];
        float re=m->c*m->re-m->s*m->im+strike*((i%2u)?-.3f:.3f);
        float im=m->s*m->re+m->c*m->im;
        /* Prevent subnormal tails from consuming unnecessary CPU. */
        if (fabsf(re)+fabsf(im)<1e-15f) re=im=0;
        m->re=re; m->im=im;
        float sample=im*m->gain;
        l+=sample*m->pan_l; r+=sample*m->pan_r;
    }
    /* Conservative gain; no hidden limiter in the DSP voice. */
    *left+=.38f*l; *right+=.38f*r;
    ++v->age;
}
