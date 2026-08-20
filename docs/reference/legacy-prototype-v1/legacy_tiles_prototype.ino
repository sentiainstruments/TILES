#include <Wire.h>
#include <Adafruit_MPR121.h>
#include <Adafruit_PWMServoDriver.h>
#include <math.h>
#include <MIDIUSB.h>

/* ================================
   CONSTANTS
================================ */

#define NUM_PADS 16
#define NUM_VOICES 8

#define BASE_NOTE 60
#define PRESS_RANGE 1700

#define DEADZONE 20
#define SCALE_SELECT_PRESS 60

#define PREVIEW_PWM 3500
#define PREVIEW_TIME 30
#define HAPTIC_MAX 3200
#define SELECT_MODE_HOLD_PWM 900

#define DEBUG_HEATMAP_INTERVAL 400

#define STANDBY_TIMEOUT 60000UL      // 1 minutes
#define STANDBY_SWITCH_INTERVAL 9000UL

#define LED_DIM 60
#define LED_MED 120

/* ================================
   PINS
================================ */

const int S0=4,S1=5,S2=6,S3=7;
const int MUX_SIG=A0;

const int DATA=10,CLK=11,LATCH=12;

const int OCT_DOWN_PIN=22;
const int OCT_UP_PIN=21;
const int SCALE_PIN=20;

const int LED_OCT_DOWN=25;
const int LED_OCT_UP=24;
const int LED_SCALE=23;

/* ================================
   DEVICES
================================ */

Adafruit_MPR121 capA;
Adafruit_MPR121 capB;
Adafruit_PWMServoDriver pwm(0x40);

/* ================================
   SCALE MODES
================================ */

enum ScaleMode
{
MODE_CHROMATIC,
MODE_MAJOR,
MODE_MINOR,
MODE_HARMONIC_MINOR,
MODE_PENTATONIC,
MODE_DORIAN,
MODE_MIXOLYDIAN
};

ScaleMode scaleMode=MODE_CHROMATIC;

/* ================================
   STATE
================================ */

bool scaleSelectorActive=false;
bool standbyActive=false;

int octaveOffset=0;
uint16_t ledState=0;

uint32_t lastActivityTime=0;
int standbyMode=0;
uint32_t standbyLastSwitch=0;

/* ================================
   SCALE TABLES
================================ */

int majorScale[7]={0,2,4,5,7,9,11};
int minorScale[7]={0,2,3,5,7,8,10};
int harmonicMinorScale[7]={0,2,3,5,7,8,11};
int pentScale[5]={0,2,4,7,9};
int dorianScale[7]={0,2,3,5,7,9,10};
int mixoScale[7]={0,2,4,5,7,9,10};

/* ================================
   STRUCTURES
================================ */

struct Pad
{
bool touched=false;
bool lastTouched=false;

int baseline=0;
float hallFilt=0;
float delta=0;

bool noteOn=false;
int voice=-1;

uint32_t previewEnd=0;
uint32_t touchLatch=0;
uint32_t selectHoldStart=0;
uint32_t confirmClickTime=0;

uint8_t confirmClickStage=0;

bool blockRetriggerUntilRelease=false;
bool selectLatched=false;

};

struct Voice
{
bool active=false;
int pad=0;
int channel=2;
uint32_t startTime=0;
float aftertouchFilt=0;
};

Pad pads[NUM_PADS];
Voice voices[NUM_VOICES];

int hallRaw[NUM_PADS];
uint32_t lastHeatmap=0;

/* ================================
   MUX CONTROL
================================ */

void setMux(byte ch)
{
digitalWrite(S0,ch&1);
digitalWrite(S1,(ch>>1)&1);
digitalWrite(S2,(ch>>2)&1);
digitalWrite(S3,(ch>>3)&1);
}

void readHall()
{
for(int i=0;i<NUM_PADS;i++)
{
setMux(i);

/* allow mux to settle */
delayMicroseconds(40);

/* single read (ADC averaging handles stability) */
hallRaw[i] = analogRead(MUX_SIG);
}
}

/* ================================
   LED SHIFT REGISTER
================================ */

void shift16(uint16_t v)
{
digitalWrite(LATCH,LOW);

for(int i=15;i>=0;i--)
{
digitalWrite(CLK,LOW);
digitalWrite(DATA,(v>>i)&1);
digitalWrite(CLK,HIGH);
}

digitalWrite(LATCH,HIGH);
}

/* ================================
   STANDBY ANIMATIONS
================================ */

void standbySmoothWave()
{
  static uint32_t lastTime = 0;
  static float t = 0;

  uint32_t now = millis();
  float dt = (now - lastTime) * 0.001; // seconds
  lastTime = now;

  t += dt * 2;  // speed control

  ledState = 0;

  for(int p=0;p<16;p++)
  {
    int x = p % 4;
    int y = p / 4;

    float dist = x + y;
    float wave = sin(t - dist * 0.8);

    if(wave > 0.6)
      ledState |= (1<<p);
  }
}
void standbyCenterRipple()
{
  static uint32_t lastTime = 0;
  static float t = 0;

  uint32_t now = millis();
  float dt = (now - lastTime) * 0.001;
  lastTime = now;

  t += dt * 1.9;  // speed control

  ledState = 0;

  for(int p=0;p<16;p++)
  {
    int x = p % 4;
    int y = p / 4;

    float dx = x - 1.5;
    float dy = y - 1.5;
    float d = sqrt(dx*dx + dy*dy);

    float wave = sin(t - d * 2.0);

    if(wave > 0.55)
      ledState |= (1<<p);
  }
}

void runStandbyAnimation()
{
  if(millis() - standbyLastSwitch > STANDBY_SWITCH_INTERVAL)
  {
    standbyMode++;
    if(standbyMode > 1) standbyMode = 0;
    standbyLastSwitch = millis();
  }

  if(standbyMode == 0) standbySmoothWave();
  else standbyCenterRipple();
}

/* ================================
   SCALE INTERVAL
================================ */

int scaleInterval(int degree)
{

switch(scaleMode)
{

case MODE_MAJOR:
return majorScale[degree%7]+12*(degree/7);

case MODE_MINOR:
return minorScale[degree%7]+12*(degree/7);

case MODE_HARMONIC_MINOR:
return harmonicMinorScale[degree%7]+12*(degree/7);

case MODE_PENTATONIC:
return pentScale[degree%5]+12*(degree/5);

case MODE_DORIAN:
return dorianScale[degree%7]+12*(degree/7);

case MODE_MIXOLYDIAN:
return mixoScale[degree%7]+12*(degree/7);

case MODE_CHROMATIC:
default:
return degree;

}

}

/* ================================
   NOTE MAPPING
================================ */

int noteForPad(int p)
{

int row=p/4;
int col=p%4;

int musicalRow=3-row;

int degree=musicalRow*4+col;

int note=BASE_NOTE + scaleInterval(degree);

note+=octaveOffset*12;

if(note<0)note=0;
if(note>127)note=127;

return note;

}

/* ================================
   VOICE ALLOCATION
================================ */

int allocateVoice(int pad)
{

for(int i=0;i<NUM_VOICES;i++)
{
if(!voices[i].active)
{
voices[i].active=true;
voices[i].pad=pad;
voices[i].channel=2+i;
voices[i].startTime=millis();
voices[i].aftertouchFilt=0;
return i;
}
}

int idx=0;
uint32_t oldest=voices[0].startTime;

for(int i=1;i<NUM_VOICES;i++)
{
if(voices[i].startTime<oldest)
{
oldest=voices[i].startTime;
idx=i;
}
}

int oldPad = voices[idx].pad;

usbMIDI.sendNoteOff(noteForPad(oldPad),0,voices[idx].channel);

pads[oldPad].noteOn = false;
pads[oldPad].voice  = -1;
pads[oldPad].touchLatch = 0;
pads[oldPad].blockRetriggerUntilRelease = true;

voices[idx].pad = pad;

return idx;

}

/* ================================
   DEBUG MATRIX
================================ */

void printDebugMatrix()
{

Serial.println("T touch  N note  S scale");

for(int r=0;r<4;r++)
{

for(int c=0;c<4;c++)
{

int p=r*4+c;

Serial.print("[");

if(scaleSelectorActive && p<=6)
Serial.print("S");
else if(pads[p].touched)
Serial.print("T");
else
Serial.print(" ");

if(pads[p].noteOn)
Serial.print("N");
else
Serial.print(" ");

Serial.print(" ");

int v=map(pads[p].delta,0,PRESS_RANGE,0,127);

if(v<0)v=0;
if(v>127)v=127;

if(v<10)Serial.print(" ");

Serial.print(v);

Serial.print("] ");

}

Serial.println();

}

Serial.println();

}

/* ================================
   MODE LEDS
================================ */

void updateModeLEDs()
{

uint32_t t=millis();

analogWrite(LED_OCT_UP,0);
analogWrite(LED_OCT_DOWN,0);
analogWrite(LED_SCALE,0);

if(scaleSelectorActive)
{
int pulse=(sin(t*0.004)+1)*LED_MED;
analogWrite(LED_SCALE,pulse);
}

if(octaveOffset==1)
analogWrite(LED_OCT_UP,LED_DIM);

else if(octaveOffset>=2)
{
int pulse=(sin(t*0.004)+1)*LED_MED;
analogWrite(LED_OCT_UP,pulse);
}

if(octaveOffset==-1)
analogWrite(LED_OCT_DOWN,LED_DIM);

else if(octaveOffset<=-2)
{
int pulse=(sin(t*0.004)+1)*LED_MED;
analogWrite(LED_OCT_DOWN,pulse);
}

}

/* ================================
   SETUP
================================ */

void setup()
{

Serial.begin(115200);

Wire.begin();
Wire.setClock(400000);

pinMode(S0,OUTPUT);
pinMode(S1,OUTPUT);
pinMode(S2,OUTPUT);
pinMode(S3,OUTPUT);

pinMode(DATA,OUTPUT);
pinMode(CLK,OUTPUT);
pinMode(LATCH,OUTPUT);

pinMode(OCT_UP_PIN,INPUT_PULLUP);
pinMode(OCT_DOWN_PIN,INPUT_PULLUP);
pinMode(SCALE_PIN,INPUT_PULLUP);

pinMode(LED_OCT_UP,OUTPUT);
pinMode(LED_OCT_DOWN,OUTPUT);
pinMode(LED_SCALE,OUTPUT);

analogReadResolution(12);
analogReadAveraging(8);

capA.begin(0x5A);
capB.begin(0x5B);

capA.writeRegister(0x5B, 0x01);
capB.writeRegister(0x5B, 0x01);

/* thresholds (cap touch) */

capA.setThresholds(6,3);
capB.setThresholds(6,3);

pwm.begin();
pwm.setPWMFreq(220);

readHall();

for(int i=0;i<NUM_PADS;i++)
{
pads[i].baseline=hallRaw[i];
pads[i].hallFilt=hallRaw[i];
}

lastActivityTime = millis();
standbyLastSwitch = millis();

Serial.println("SENTIA - TILES ready");

}

/* ================================
   LOOP
================================ */

void loop()
{

uint16_t tA=capA.touched();
uint16_t tB=capB.touched();

for(int i=0;i<8;i++)
{pads[i].touched = (tA & (1<<i)) != 0;
pads[i+8].touched = (tB & (1<<i)) != 0;}

readHall();

/* FAST WAKE CHECK (Standby Mode) */

if(standbyActive)
{
    for(int p=0;p<NUM_PADS;p++)
    {
        if(pads[p].touched)
        {
            float d = pads[p].baseline - hallRaw[p];

            if(d < 0) d = 0;
            if(d < DEADZONE) d = 0;
            else d -= DEADZONE;

            if(d > 15)
            {
                standbyActive = false;
                lastActivityTime = millis();
                break;
            }
        }
    }
}

/* MODE & SCALE BUTTONS */

static bool lastUp=false;
static bool lastDown=false;
static bool lastScale=false;

bool up=!digitalRead(OCT_UP_PIN);
bool down=!digitalRead(OCT_DOWN_PIN);
bool scale=!digitalRead(SCALE_PIN);

if(up && !lastUp)
{
octaveOffset++;
Serial.print("OCTAVE UP -> ");
Serial.println(octaveOffset);
}

if(down && !lastDown)
{
octaveOffset--;
Serial.print("OCTAVE DOWN -> ");
Serial.println(octaveOffset);
}

if(scale && !lastScale)
{
scaleSelectorActive=true;
Serial.println("SCALE SELECT");
}

lastUp=up;
lastDown=down;
lastScale=scale;

if(!standbyActive && (millis() - lastActivityTime > STANDBY_TIMEOUT))
{
    standbyActive = true;
    scaleSelectorActive = false;
    standbyLastSwitch = millis();
}

/* PAD LOOP */

ledState=0;

// if standby skip pad logic entirely
if(standbyActive)
{
    for(int p=0; p<NUM_PADS; p++)
    {
        pwm.setPWM(p,0,0);
    }

    runStandbyAnimation();
    shift16(ledState);
    updateModeLEDs();
    return;
}

for(int p=0;p<NUM_PADS;p++)
{

pads[p].hallFilt=pads[p].hallFilt*0.8+hallRaw[p]*0.2;

float d=pads[p].baseline-pads[p].hallFilt;

if(d<0)d=0;

if(d<DEADZONE)d=0;
else d-=DEADZONE;

pads[p].delta=d;

/* activity detection: touch + pressure wakes standby */

if(pads[p].touched && d > 15)
{
    lastActivityTime = millis();

    if(standbyActive)
    {
        standbyActive = false;
    }
}

/* touch start */

if(pads[p].touched && !pads[p].lastTouched)
{
pads[p].previewEnd = millis()+PREVIEW_TIME;
pads[p].selectLatched = false;
pads[p].touchLatch = millis()+30;
}

/* SCALE SELECT */

if(scaleSelectorActive)
{
    if(pads[p].touched)
    {
        if(pads[p].selectHoldStart == 0)
            pads[p].selectHoldStart = millis();

        uint32_t holdTime = millis() - pads[p].selectHoldStart;

        if(d > 80 && holdTime > 80 && !pads[p].selectLatched)
        {
            if(p <= 6)
            {
                scaleMode = (ScaleMode)p;
                scaleSelectorActive = false;

                Serial.print("MODE -> ");
                Serial.println(p);

                // prevent immediate note trigger
                pads[p].blockRetriggerUntilRelease = true;
                pads[p].touchLatch = 0;

                // double click confirm
                pads[p].confirmClickTime = millis();
                pads[p].confirmClickStage = 1;
                pads[p].selectHoldStart = 0;
            }

            pads[p].selectLatched = true;
        }
    }
    else
    {
        pads[p].selectHoldStart = 0;
    }
}

/* LED */

if(scaleSelectorActive)
{

if(p==scaleMode)
{
if((millis()/250)%2)
ledState|=(1<<p);
}
else if(p<=6)
ledState|=(1<<p);

}
else
{

if(pads[p].touched)
ledState|=(1<<p);

}

/* DISABLE MIDI NOTES WHILE SELECTING SCALE */

if(scaleSelectorActive)
{
uint32_t now=millis();

if(p<=6)
{
if(p==scaleMode && pads[p].touched)
{
pwm.setPWM(p,0,SELECT_MODE_HOLD_PWM);
}
else if(now<pads[p].previewEnd)
{
pwm.setPWM(p,0,PREVIEW_PWM);
}
else
{
pwm.setPWM(p,0,0);
}
}
else
{
pwm.setPWM(p,0,0);
}

pads[p].lastTouched=pads[p].touched;
continue;
}

/* NOTE */

if(!pads[p].noteOn && !pads[p].blockRetriggerUntilRelease && (pads[p].touched || millis() < pads[p].touchLatch))
{

float norm=d/(float)PRESS_RANGE;

int vel=pow(norm,0.6)*127;

if(vel<1)vel=1;
if(vel>127)vel=127;

if(d>15)
{

int v=allocateVoice(p);

pads[p].voice=v;
pads[p].noteOn=true;

usbMIDI.sendNoteOn(noteForPad(p),vel,voices[v].channel);

}

}

/* AFTERTOUCH */

if(pads[p].noteOn)
{

int v=pads[p].voice;

float norm=d/(float)PRESS_RANGE;

int at=norm*127;

if(at<0)at=0;
if(at>127)at=127;

voices[v].aftertouchFilt=
voices[v].aftertouchFilt*0.75+
at*0.25;

usbMIDI.sendAfterTouch(
voices[v].aftertouchFilt,
voices[v].channel
);

}

/* NOTE OFF */

if(!pads[p].touched && pads[p].noteOn)
{
    int v=pads[p].voice;

    usbMIDI.sendNoteOff(noteForPad(p),0,voices[v].channel);

    voices[v].active=false;
    pads[p].noteOn=false;
    pads[p].voice=-1;
}

if(!pads[p].touched)
{
    pads[p].blockRetriggerUntilRelease = false;
}

/* HAPTICS */

uint32_t now=millis();

// SCALE CONFIRM DOUBLE CLICK
if(pads[p].confirmClickStage > 0)
{
    uint32_t t = millis() - pads[p].confirmClickTime;

    if(pads[p].confirmClickStage == 1)
    {
        pwm.setPWM(p,0,2500);  // first click
        if(t > 40)
        {
            pads[p].confirmClickStage = 2;
            pads[p].confirmClickTime = millis();
        }
        continue;
    }

    if(pads[p].confirmClickStage == 2)
    {
        pwm.setPWM(p,0,0); // gap
        if(t > 40)
        {
            pads[p].confirmClickStage = 3;
            pads[p].confirmClickTime = millis();
        }
        continue;
    }

    if(pads[p].confirmClickStage == 3)
    {
        pwm.setPWM(p,0,2500); // second click
        if(t > 40)
        {
            pads[p].confirmClickStage = 0;
        }
        continue;
    }
}

if(now<pads[p].previewEnd)
pwm.setPWM(p,0,PREVIEW_PWM);

else
{

if(pads[p].touched)
{

float pressure=d/(float)PRESS_RANGE;

if(pressure<0)pressure=0;
if(pressure>1)pressure=1;

float curve=pow(pressure,2.2);

int strength=curve*HAPTIC_MAX;

if(pressure>0.05 && strength<120)
strength=120;

pwm.setPWM(p,0,strength);

}
else
pwm.setPWM(p,0,0);

}

pads[p].lastTouched=pads[p].touched;

}

if(standbyActive)
{
    runStandbyAnimation();
}

shift16(ledState);
updateModeLEDs();

/* MATRIX DEBUG */

bool anyActive=false;

for(int i=0;i<NUM_PADS;i++)
if(pads[i].touched || pads[i].noteOn)
{
anyActive=true;
break;
}

if(anyActive && millis()-lastHeatmap>DEBUG_HEATMAP_INTERVAL)
{
printDebugMatrix();
lastHeatmap=millis();
}

while(usbMIDI.read());

}
