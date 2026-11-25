// TILES TP v1, 508 bytes. Verified against firmware/src/audio/presets.c.
export type Engine = 0 | 1 | 2
export const banks = ['Classic','Glass','Voice','Talk','Growl','Fold','Bits','Sweep','Organ','Neuro','Hard']
export const engines = ['Wavetable','FM','Granular']
export const common = ['wave','wavePressure','waveSlide','waveEnv','waveLfo','detune','width','drive','fold','cutoff','resonance','keyTrack','filterEnv','velocityOct','pressureOct','slideOct','lfoOct','attack','decay','sustain','release','modAttack','modDecay','modSustain','modRelease','lfoRate','lfoDelay','vibrato','pitchEnv','pitchEnvTime','glide','level','velocityLevel','pressureLevel','drift','reverb','delay','chorus','fxDrive','dark','crush','pump'] as const
export type Common = typeof common[number]
export const grainFields = ['size','density','jitter','spray','scan','scatter','octaveUp','octaveDown','fifth','reverse','spread','skew','sizePressure','densityPressure','sprayPressure','spraySlide','scatterSlide'] as const
export type Grain = typeof grainFields[number]
const commonRanges = [[-1,2],[-2,2],[-2,2],[-2,2],[-2,2],[0,100],[0,1],[0,8],[0,1],[0,20000],[0,1],[-1,2],[-8,8],[-8,8],[-8,8],[-8,8],[-8,8],[0,60000],[0,60000],[0,1],[0,60000],[0,60000],[0,60000],[0,1],[0,60000],[0,64],[0,10000],[0,1200],[-48,48],[0,10000],[0,10000],[0,4],[0,1],[0,2],[0,100],[0,1],[0,1],[0,1],[0,1],[0,1],[0,1],[0,1]]
const grainRanges = [[0,2000],[0,1000],[0,1],[0,1],[-8,8],[0,2400],[0,1],[0,1],[0,1],[0,1],[0,1],[0,1],[-4,4],[-4,4],[-1,1],[-1,1],[-2400,2400]]
export const extraFields = ['noise','sub','lfo2Rate','lfo2Oct','lfo2Wave','lfo2Cents','lfo2Level'] as const
export type Extra = typeof extraFields[number]
const extraRanges = [[0,1],[0,1],[0,64],[-8,8],[-2,2],[-1200,1200],[0,1]]
const clamp=(v:number,a:number,b:number)=>Math.min(b,Math.max(a,v))
export function validate(p:Uint8Array) {
 if(p.length!==508||p[0]!==84||p[1]!==80||p[2]!==1||p[3]>2) throw Error('Expected a TILES TP v1 508-byte preset')
 const n=Array.from(p.slice(4,14));const end=n.indexOf(0);const chars=end<0?n:n.slice(0,end);if(!chars.length||chars.some(c=>c<32||c>126))throw Error('Invalid preset name')
 if(p[14]>10||p[15]>7||p[16]>3||p[17]>2||p[188]>1||p[189]>7||p[190]>127||p[424]>1||p[19]>4||p[425]>4)throw Error('Invalid preset enum')
 const d=new DataView(p.buffer,p.byteOffset,p.byteLength);for(const [start,count] of [[20,42],[192,17],[416,2],[428,7]]) for(let i=0;i<count;i++)if(!Number.isFinite(d.getFloat32(start+i*4,true)))throw Error('Non-finite preset value')
 return p
}
export function name(p:Uint8Array){return String.fromCharCode(...p.slice(4,14)).replace(/\0.*$/,'')}
export function rename(p:Uint8Array,n:string){const q=p.slice();q.fill(0,4,14);q.set(Array.from(n.replace(/[^\x20-\x7e]/g,'').slice(0,10),c=>c.charCodeAt(0)),4);return q}
export function byte(p:Uint8Array,offset:number,value:number){const q=p.slice();q[offset]=value;return q}
export function f(p:Uint8Array,k:Common){return new DataView(p.buffer,p.byteOffset,p.byteLength).getFloat32(20+common.indexOf(k)*4,true)}
function write(p:Uint8Array,offset:number,v:number,range:number[]){const q=p.slice();new DataView(q.buffer).setFloat32(offset,clamp(Number.isFinite(v)?v:0,range[0],range[1]),true);return q}
export function setF(p:Uint8Array,k:Common,v:number){const i=common.indexOf(k);return write(p,20+i*4,v,commonRanges[i])}
export function g(p:Uint8Array,k:Grain){return new DataView(p.buffer,p.byteOffset,p.byteLength).getFloat32(192+grainFields.indexOf(k)*4,true)}
export function setG(p:Uint8Array,k:Grain,v:number){const i=grainFields.indexOf(k);return write(p,192+i*4,v,grainRanges[i])}
export function x(p:Uint8Array,k:Extra){return new DataView(p.buffer,p.byteOffset,p.byteLength).getFloat32(428+extraFields.indexOf(k)*4,true)}
export function setX(p:Uint8Array,k:Extra,v:number){const i=extraFields.indexOf(k);return write(p,428+i*4,v,extraRanges[i])}
export function fmDb(p:Uint8Array,slide:boolean){return new DataView(p.buffer,p.byteOffset,p.byteLength).getFloat32(slide?420:416,true)}
export function setFmDb(p:Uint8Array,slide:boolean,v:number){return write(p,slide?420:416,v,[0,24])}
export function crc32(p:Uint8Array){let c=0xffffffff;for(const b of p){c^=b;for(let j=0;j<8;j++)c=(c>>>1)^((c&1)?0xedb88320:0)}return ((c^0xffffffff)>>>0).toString(16).padStart(8,'0')}
export function parseRecord(lines:string[]){if(lines[0]!=='bytes=508'||lines.slice(1).some(l=>!/^([0-9a-f]{2})+$/i.test(l)))throw Error(lines[0]||'Invalid PRESET GET response');const h=lines.slice(1).join('');return validate(Uint8Array.from(h.match(/../g)||[],v=>parseInt(v,16)))}
export function makePreset(engine:Engine){let p=new Uint8Array(508);p.set([84,80,1,engine]);p=rename(p,['Init Wave','Init FM','Init Grain'][engine]);p[15]=1;p[190]=60;
 const defaults:Partial<Record<Common,number>>={wave:.4,width:.5,cutoff:6500,resonance:.15,keyTrack:.2,attack:20,decay:400,sustain:.65,release:600,modAttack:20,modDecay:400,modSustain:.1,modRelease:300,lfoRate:2,level:.35,velocityLevel:.7,pressureOct:1.5,waveSlide:.3,reverb:.18};for(const [k,v] of Object.entries(defaults))p=setF(p,k as Common,v)
 p=setX(p,'lfo2Rate',.5);for(const [k,v] of Object.entries({size:100,density:18,spray:.25,spread:.7,scatter:12,skew:.5}))p=setG(p,k as Grain,v)
 // DX7 unpacked voice: initialize six operators, algorithm 32 (all carriers).
 for(let op=0;op<6;op++){const o=260+op*21;p.set([90,55,35,55,99,75,70,0,39,0,0,0,0,0,0,0,op===5?90:0,0,1,0,7],o)}
 p.set([99,99,99,99,50,50,50,50,31,0,1,35,0,0,0,1,0,0,24],386);p.set(Array.from('Init FM   ',c=>c.charCodeAt(0)),405);return p
}
export function download(p:Uint8Array){const a=document.createElement('a');const copy=new Uint8Array(p);a.href=URL.createObjectURL(new Blob([copy.buffer],{type:'application/octet-stream'}));a.download=`${name(p).trim().replace(/[^a-z0-9_-]/gi,'_')||'preset'}.tiles-preset`;a.click();setTimeout(()=>URL.revokeObjectURL(a.href),1000)}
