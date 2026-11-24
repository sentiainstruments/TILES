#include "modal.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>

static void render(ModalVoice *v,ModalExpression e,float *l,float *r) {
    *l=*r=0;modal_render(v,e,l,r);assert(isfinite(*l)&&isfinite(*r));
}
int main(void) {
    ModalParams p={.5f,.65f,.6f,.8f}; ModalExpression e={0,.5f,0};
    ModalVoice a,b,c;float l,r,bl,br,cl,cr;
    modal_start(&a,48000,69,.8f,p,42);modal_start(&b,48000,69,.8f,p,42);
    modal_start(&c,48000,69,0,p,42);
    double first=0,last=0,stereo=0,expression=0;float peak=0;
    for(unsigned i=0;i<48000*5;i++) {
        render(&a,e,&l,&r);render(&b,e,&bl,&br);render(&c,e,&cl,&cr);
        assert(l==bl&&r==br);assert(cl==0&&cr==0);
        peak=fmaxf(peak,fmaxf(fabsf(l),fabsf(r)));
        if(i<4800)first+=l*l+r*r;
        if(i>=48000*5-4800)last+=l*l+r*r;
        stereo+=fabsf(l-r);
        if(i==48000){modal_release(&a);modal_release(&b);}
    }
    assert(first>0 && last<first*1e-6 && stereo>1 && peak<1);
    modal_start(&a,48000,60,.9f,p,1);modal_start(&b,48000,60,.9f,p,1);
    for(unsigned i=0;i<24000;i++) {render(&a,e,&l,&r);render(&b,(ModalExpression){.9f,.95f,2},&bl,&br);expression+=fabsf(l-bl)+fabsf(r-br);}
    assert(expression>1);
    /* Fundamental pitch: isolate mode 0 and count positive crossings. */
    modal_start(&a,48000,69,.9f,p,1);
    float prev=0;unsigned crossings=0;
    for(unsigned i=0;i<4800;i++){
        render(&a,e,&l,&r);
        if(prev<0&&a.modes[0].im>=0)crossings++;
        prev=a.modes[0].im;
    }
    assert(crossings>=43&&crossings<=45);
    assert(modal_ratio(0,0)==1&&modal_ratio(0,1)==1);
    assert(modal_ratio(1,0)!=modal_ratio(1,1));
    assert(modal_t60(0)<modal_t60(1));
    /* High notes and extreme controls must remain finite. */
    modal_start(&a,48000,127,1,(ModalParams){1,1,1,1},0);
    for(unsigned i=0;i<48000;i++){render(&a,(ModalExpression){1,1,48},&l,&r);assert(fabsf(l)<1&&fabsf(r)<1);}
    printf("PASS: deterministic, silent at zero velocity, release decay, stereo, expression, pitch, finite extremes; voice peak %.3f\n",peak);
}
