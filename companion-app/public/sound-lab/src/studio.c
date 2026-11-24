#include "modal.h"
#include <stddef.h>
#define VOICES 8
#define BLOCK 128
static ModalVoice voices[VOICES];
static int active[VOICES], held[VOICES], notes[VOICES];
static unsigned order[VOICES], sequence;
static float level[VOICES], fade[VOICES];
static float sr=48000, gain=.65f;
static ModalParams params={.65f,1,.45f,1};
static ModalExpression expression={0,.5f,0};
static float left[BLOCK], right[BLOCK];
void *memset(void *dst,int val,size_t n){unsigned char *p=dst;while(n--)*p++=(unsigned char)val;return dst;}
void studio_init(float rate){sr=rate;sequence=0;for(int i=0;i<VOICES;i++)active[i]=held[i]=0;}
void studio_params(float a,float b,float c,float d){params=(ModalParams){a,b,c,d};for(int i=0;i<VOICES;i++)if(active[i])voices[i].params=params;}
void studio_expression(float p,float s,float b){expression=(ModalExpression){p,s,b};}
void studio_gain(float g){gain=g;}
void studio_note_on(int note,float velocity){int slot=-1;for(int i=0;i<VOICES;i++)if(!active[i]){slot=i;break;}
 if(slot<0){for(int i=0;i<VOICES;i++)if(!held[i]&&(slot<0||level[i]<level[slot]))slot=i;}
 if(slot<0){slot=0;for(int i=1;i<VOICES;i++)if(order[i]<order[slot])slot=i;}
 modal_start(&voices[slot],sr,(float)note,velocity,params,++sequence+123);active[slot]=held[slot]=1;notes[slot]=note;order[slot]=sequence;level[slot]=0;fade[slot]=0;
}
void studio_note_off(int note){for(int i=0;i<VOICES;i++)if(active[i]&&held[i]&&notes[i]==note){held[i]=0;modal_release(&voices[i]);}}
void studio_panic(void){for(int i=0;i<VOICES;i++)if(active[i]){held[i]=0;modal_release(&voices[i]);}}
static float absf(float x){return x<0?-x:x;}
static float soft(float x){float a=absf(x);return a<=.8f?x:(x<0?-1:1)*(.8f+.2f*(a-.8f)/(a-.6f));}
void studio_render(int frames){if(frames>BLOCK)frames=BLOCK;for(int j=0;j<frames;j++){float l=0,r=0;for(int i=0;i<VOICES;i++)if(active[i]){float a=0,b=0;modal_render(&voices[i],expression,&a,&b);if(fade[i]<1){fade[i]+=1.f/(sr*.002f);if(fade[i]>1)fade[i]=1;}l+=a*fade[i];r+=b*fade[i];level[i]=.999f*level[i]+.001f*(absf(a)+absf(b));if(!held[i]&&voices[i].age>sr*.1f&&level[i]<.000001f)active[i]=0;}left[j]=soft(l*gain);right[j]=soft(r*gain);}}
float *studio_left(void){return left;}float *studio_right(void){return right;}
