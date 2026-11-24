import { banks, f, makePreset, name, rename, setF, setG, validate } from './preset'
import type { Engine, Common } from './preset'

// Factory slot names, bank, position, unison, cutoff and ADSR extracted from
// firmware/board-bringup: firmware/src/audio/factory_presets.c, 8120bdf.
// This is a partial parameter snapshot, NOT a full factory preset record.
// Slot 24 is empty. FM and grain auditioning uses the existing local preview.
const rows = `1 acid Classic .4285714 1 260 1 500 .65 30
2 reese Classic .4285714 2 900 5 100 1 150
3 wobble Voice .6 2 420 3 100 1 80
4 sub Classic 0 1 2000 2 200 1 120
5 solid FM
6 rumble Classic 0 1 320 1 900 .2 500
7 pluck Classic .45 3 450 1 900 .3 250
8 lead Classic .5 3 2500 5 500 .8 300
9 stab Classic .4285714 3 1200 1 350 0 120
10 hoover Classic .72 3 4000 5 300 .9 400
11 tine FM
12 super Classic .4285714 7 6000 5 500 .85 400
13 haze Classic .4285714 3 1800 250 1500 .85 1200
14 air Glass .15 3 3000 600 2000 .8 2500
15 choir Voice .15 3 4000 400 1000 .9 1500
16 cloud Classic .4285714 0 2200 400 1500 .85 2000
17 dust Voice .15 0 5000 50 800 .8 1500
18 scatter Classic .25 0 12000 80 1000 .9 1500
19 glass Glass .55 1 9000 1 2500 0 1500
20 pure Glass 0 1 16000 4 100 1 300
21 chime FM
22 shimmer Glass .5 0 9000 150 3000 .7 3000
23 throb Glass .4 2 2500 20 1200 .7 900
25 yoi Talk .05 2 3000 3 200 1 120
26 growl Growl .5 2 600 3 100 1 80
27 riddim Fold .55 1 500 2 100 1 60
28 pound Fold .15 1 900 1 250 0 60
29 deep Sweep .15 1 1200 2 700 .6 250
30 punch FM
31 future Classic .4285714 7 4000 5 600 .8 300
32 organ Organ .5714286 1 7000 2 600 .5 120
33 dub Sweep .35 2 1100 1 320 0 200
34 rave Fold .45 3 3500 1 400 0 150
35 screech Growl .75 2 4500 4 300 .85 200
36 brass FM
37 drift Sweep .25 5 9000 700 2000 .9 2500
38 warm Classic .2857143 7 900 600 2000 .9 2500
39 tunnel Growl .2 3 700 800 2000 .9 3000
40 whisper Talk .5 0 6000 200 1000 .85 2000
41 riser Sweep .1 5 12000 2500 1000 1 1500
42 halo Organ .15 3 5000 400 1000 .9 2000
43 mallet FM
44 metal Growl .85 1 12000 1 2500 0 1500
45 bleep Bits .25 1 8000 1 180 0 120
46 drops Sweep .1 1 14000 1 900 0 600
47 ember Fold .3 2 5000 2 2200 0 1200
48 keys Organ .45 1 6000 2 1800 .55 250
49 neuro Neuro .45 2 900 2 100 1 60
50 tear Neuro .7 2 3000 2 100 1 60
51 wub Classic .4285714 2 250 3 100 1 80
52 rip Hard .3 2 2500 1 250 .7 80
53 snarl Growl .1 2 1200 3 100 1 80
54 drop Classic 0 1 3000 1 1800 .3 400
55 laser Classic .5714286 2 6000 1 250 .6 150
56 horn Talk .4285714 3 2500 8 400 .8 200
57 chop Talk .85 1 6000 1 220 0 100
58 siren Classic .5714286 2 5000 5 100 1 300
59 zap Neuro .9 1 9000 1 180 0 100
60 smash Fold .6 3 4000 1 350 0 120
61 doom Growl .15 5 500 900 2000 .9 3000
62 fall Sweep .9 5 12000 5 4000 0 1000
63 impact Classic 0 1 8000 1 1500 0 800
64 void Neuro .2 3 1500 1200 2000 .9 3000
65 ghost Voice .1 5 5000 800 2000 .85 3000
66 swell Classic .4285714 7 400 2000 1000 1 1500
67 glint Sweep .1 3 12000 1 900 0 500
68 spark Glass .85 1 14000 1 800 0 500
69 musicbox Glass .6 1 12000 1 1200 0 700
70 steel Hard .85 1 10000 1 2200 0 1200
71 arp Classic .4285714 2 800 1 250 0 150
72 glow Fold .2 3 4000 5 1800 .2 1200
73 thump Classic 0 1 2500 1 220 0 50
74 gravel Hard .5 1 400 1 200 .2 60
75 burn Classic .4285714 1 300 1 400 .6 40
76 grind Hard .75 2 800 2 100 1 60
77 roll Sweep .2 1 1500 1 150 0 40
78 under Fold .1428571 1 300 2 300 .8 150
79 slam Fold .55 3 3500 1 300 0 120
80 alarm Hard .4 2 5000 2 100 1 150
81 scream Neuro .55 3 5000 5 300 .85 250
82 clang Hard .95 1 9000 1 500 0 250
83 gate Classic .4285714 5 3500 5 100 1 200
84 ping Classic 0 1 12000 1 350 0 200
85 dread Growl .25 3 300 1500 2000 .9 2800
86 hiss Sweep .4 3 3000 1000 2000 .9 3000
87 ritual Talk .2 3 2500 600 2000 .9 2500
88 fog Bits .3 3 2500 1200 2000 .85 3000
89 cathedral Organ 1 3 6000 300 2000 .95 3500
90 tide Sweep .15 5 10000 800 2000 .9 3000
91 tick Hard .9 1 14000 .5 70 0 40
92 anvil Hard 1 2 12000 1 2500 0 1500
93 glitch Bits .8 1 10000 1 120 0 60
94 gong Growl .6 2 8000 2 4000 0 3000
95 zip Classic .5714286 1 8000 1 80 0 60
96 blast Hard .5 7 6000 1 600 0 800`
const grains: Record<number, number[]> = {16:[240,24,.12,8,.9],17:[35,26,.35,25,1],18:[120,28,.15,10,.8],22:[260,20,.1,6,1],40:[70,28,.45,14,1]}
// Local draft approximation, including FM envelopes for a usable audition.
const fm:Record<number, number[]>={5:[80,1,10,300,.8,160],11:[72,1,2,1200,.1,500],21:[76,3,2,1800,0,1500],30:[80,1,2,450,.4,160],36:[80,1,100,500,.85,300],43:[70,4,1,500,0,350]}
const factory = new Map<number,Uint8Array>()
for (const line of rows.split('\n')) {
  const [id,label,bank,...v]=line.split(' ');const slot=+id
  const engine:Engine=bank==='FM'?1:grains[slot]?2:0
  let p=rename(makePreset(engine),label)
  if(engine===1){const [out,ratio,a,d,s,r]=fm[slot];p[260+4*21+16]=out;p[260+4*21+18]=ratio;for(const [k,n] of Object.entries({attack:a,decay:d,sustain:s,release:r}))p=setF(p,k as Common,n)}
  else {const [wave,unison,cutoff,a,d,s,r]=v.map(Number);p[14]=banks.indexOf(bank);p[15]=Math.max(1,unison);for(const [k,n] of Object.entries({wave,cutoff,attack:a,decay:d,sustain:s,release:r,detune:unison>1?12:0}))p=setF(p,k as Common,n)}
  if(engine===2){const [size,density,spray,scatter,spread]=grains[slot];for(const [k,n] of Object.entries({size,density,spray,scatter,spread}))p=setG(p,k as Parameters<typeof setG>[1],n);if(slot===18){p[188]=1;p[189]=7}}
  factory.set(slot,validate(p))
}
export const pageColors = [['#ff0073','#ff4da6','#991aff','#d98cff'],['#0026ff','#008cff','#00e6d9','#8cd9ff'],['#26ff00','#8cff00','#00ff73','#ccff8c'],['#ff0000','#ff4d00','#ff9900','#ffd98c']]
export const fxNames=['Reverb','Delay','Chorus','Drive','Crush','Pump']
export const fxKeys:Common[]=['reverb','delay','chorus','fxDrive','crush','pump']
export const fxColors=['#3366ff','#00cccc','#994dff','#ff5900','#b3ff00','#ff0099']
export const previewNotes=[36,60,57,72]
export const trackColors=['#ff1f00','#ff5900','#ff9e00','#ff004d']
export type SoundSelection={slot:number;track:number;record:Uint8Array;label:string}
const saved=new Map<number,Uint8Array>() // explicit local saves for this app session
const trackSlots=[1,7,13,19]
const effects=Array.from({length:4},()=>[0,0,0,0,0,0])
export function modeTrack(mode:string){return ['prism','duet'].includes(mode)?3:['bounce','ripples','sand','orbits','cradle'].includes(mode)?4:mode==='drums'?1:2}
export function slotAt(page:number,pad:number){return page*24+pad}
export function slotPage(slot:number){return Math.floor((slot-1)/24)}
export function wrap(n:number,length:number){return ((n%length)+length)%length}
export function slotRecord(slot:number){return (saved.get(slot)??factory.get(slot))?.slice()}
export function slotName(slot:number){const p=slotRecord(slot);return p?name(p):''}
export function isSaved(slot:number){return saved.has(slot)}
export function selectedSlot(track:number){return trackSlots[track-1]}
export function selectSlot(track:number,slot:number){const p=slotRecord(slot);if(!p)return false;trackSlots[track-1]=slot;effects[track-1]=fxKeys.map(k=>f(p,k));return true}
export function getEffects(track:number){return effects[track-1].slice()}
export function setEffect(track:number,col:number,value:number){effects[track-1][col]=Math.max(0,Math.min(1,value))}
export function effectValue(pad:number){return (3-Math.floor((pad-1)/6))/3}
export function saveVirtualPreset(slot:number,p:Uint8Array){if(!Number.isInteger(slot)||slot<1||slot>96)throw Error('Invalid simulator slot');saved.set(slot,validate(p.slice()))}
export function makeSelection(track:number):SoundSelection|null{const slot=selectedSlot(track);let record=slotRecord(slot);if(!record)return null;getEffects(track).forEach((v,i)=>{record=setF(record!,fxKeys[i],v)});return {slot,track,record:record!,label:isSaved(slot)?'Local saved draft':'Factory-inspired preview draft'}}
for(let t=1;t<=4;t++)selectSlot(t,trackSlots[t-1])
