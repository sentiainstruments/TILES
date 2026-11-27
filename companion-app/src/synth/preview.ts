import {f,g,x,banks} from './preset'
const clamp=(v:number,a:number,b:number)=>Math.max(a,Math.min(b,v))
// Local illustrative waveforms, not Sentia's actual generated wavetable banks.
export function harmonics(bank:number,position:number){const n=32,p=clamp(position,0,1);const a=new Float32Array(n+1),b=new Float32Array(n+1);for(let i=1;i<=n;i++){let h=0;switch(banks[bank]){case 'Classic':h=(i===1?1-p:0)+p*((i%2?1:.7)/i);break;case 'Glass':h=(i===1?1:0)+p*(i%3===1?.8/i**.5:.08/i);break;case 'Voice':case 'Talk':h=(.13+Math.exp(-(((i-(2+p*10))/2)**2))+.5*Math.exp(-(((i-14)/3)**2)))/i**.45;break;case 'Fold':h=i%2?Math.sin(i*p*2+.4)/i**.7:0;break;case 'Bits':h=(i%2?1:.5)/i**(1.6-p);break;case 'Organ':h=[0,1,.1,.35,.25,.12,.1,.1,.07][i]||0;h*=i===1?1:p;break;case 'Sweep':h=1/i*Math.exp(-i*(1-p)*.4);break;default:h=(i===1?1:0)+p*Math.sin(i*(.8+p))*Math.exp(-i*.06)/i**.45;}b[i]=h}return {a,b}}
export function wavePoints(bank:number,pos:number){const {b}=harmonics(bank,pos);return Array.from({length:128},(_,j)=>{let s=0;for(let i=1;i<b.length;i++)s+=b[i]*Math.sin(i*j/127*Math.PI*2);return s})}
type Voice={wavetable: {osc:OscillatorNode;pan:StereoPannerNode;spread:number}[];engine:number;gain:GainNode;nodes:AudioScheduledSourceNode[];filter:BiquadFilterNode;expression:GainNode;pressure:number;midi:boolean;waveKey?:string;release:number;timer?:number;stop:()=>void}
export class Preview {
 context:AudioContext|null=null; output:GainNode|null=null; analyser:AnalyserNode|null=null; voices=new Map<number,Voice>(); tails=new Set<Voice>();
 async enable(){if(!this.context){this.context=new AudioContext();this.output=this.context.createGain();this.output.gain.value=.22;this.analyser=this.context.createAnalyser();this.analyser.fftSize=1024;this.output.connect(this.analyser).connect(this.context.destination)}await this.context.resume()}
 noteOn(note:number,p:Uint8Array,velocity:number,voiceId=note,midi=false){const c=this.context,o=this.output;if(!c||!o)return;this.noteOff(voiceId);if(this.voices.size+this.tails.size>=12){const tail=this.tails.values().next().value;if(tail)tail.stop();else {const key=this.voices.keys().next().value;if(key!==undefined){this.voices.get(key)?.stop();this.voices.delete(key)}}}const t=c.currentTime,hz=440*2**((note-69)/12),gain=c.createGain(),expression=c.createGain(),filter=c.createBiquadFilter();filter.type=['lowpass','bandpass','highpass','lowpass'][p[16]] as BiquadFilterType;filter.frequency.value=clamp(f(p,'cutoff'),40,18000);filter.Q.value=clamp(f(p,'resonance')*12,.1,12);filter.connect(gain).connect(expression).connect(o);const velocityDepth=midi?clamp(f(p,'velocityLevel'),0,1):.75;const peak=clamp(f(p,'level'),0,1)*(1-velocityDepth+velocityDepth*clamp(velocity,0,1));const a=Math.max(.005,f(p,'attack')/1000),d=Math.max(.005,f(p,'decay')/1000);gain.gain.setValueAtTime(0,t);gain.gain.linearRampToValueAtTime(peak,t+a);gain.gain.linearRampToValueAtTime(peak*clamp(f(p,'sustain'),0,1),t+a+d);const nodes:AudioScheduledSourceNode[]=[];const wavetable:Voice['wavetable']=[];const oscillator=(frequency:number,level:number,pan=0)=>{const osc=c.createOscillator(),amp=c.createGain(),st=c.createStereoPanner();osc.frequency.value=frequency;amp.gain.value=level;st.pan.value=pan;osc.connect(amp).connect(st).connect(filter);nodes.push(osc);osc.start(t);return {osc,st}};
 let interval:number|undefined;let stopped=false;let voice:Voice;
 if(p[3]===1){ // simplified local two-operator FM; device uses full DX7 algorithm
 const carrier=oscillator(hz*Math.max(.5,p[260+5*21+18]||.5),.8*p[260+5*21+16]/99).osc;const mod=c.createOscillator(),depth=c.createGain();mod.frequency.value=hz*Math.max(.5,p[260+4*21+18]||.5);depth.gain.value=hz*3*p[260+4*21+16]/99;mod.connect(depth).connect(carrier.frequency);nodes.push(mod);mod.start(t);
 } else if(p[3]===2){ // local grain preview from approximated bank, not tape sample
 const {b}=harmonics(p[14],f(p,'wave'));const buf=c.createBuffer(1,Math.floor(c.sampleRate*.5),c.sampleRate);const data=buf.getChannelData(0);for(let j=0;j<data.length;j++){let y=0;for(let i=1;i<=16;i++)y+=b[i]*Math.sin(j/c.sampleRate*hz*i*2*Math.PI);data[j]=y*.3}
 const grain=()=>{if(stopped)return;const src=c.createBufferSource(),env=c.createGain(),pan=c.createStereoPanner();src.buffer=buf;src.loop=true;src.playbackRate.value=2**((Math.random()*2-1)*g(p,'scatter')/1200);pan.pan.value=(Math.random()*2-1)*g(p,'spread');const now=c.currentTime,len=clamp(g(p,'size')/1000,.015,.5);env.gain.setValueAtTime(0,now);env.gain.linearRampToValueAtTime(.25,now+len*.3);env.gain.linearRampToValueAtTime(0,now+len);src.connect(env).connect(pan).connect(filter);src.start(now,Math.random()*.45*g(p,'spray'));src.stop(now+len);nodes.push(src);src.onended=()=>{const i=nodes.indexOf(src);if(i>=0)nodes.splice(i,1);src.disconnect();env.disconnect();pan.disconnect()}};grain();interval=window.setInterval(grain,1000/clamp(g(p,'density'),2,45));
 }else {const {a:real,b:imag}=harmonics(p[14],f(p,'wave'));const w=c.createPeriodicWave(real,imag);const count=Math.max(1,Math.min(7,p[15]));for(let i=0;i<count;i++){const spread=count===1?0:2*i/(count-1)-1;const {osc,st}=oscillator(hz,1/count,spread*clamp(f(p,'width'),0,1));osc.setPeriodicWave(w);osc.detune.value=spread*f(p,'detune');wavetable.push({osc,pan:st,spread})}if(x(p,'sub')>.001){const sub=oscillator(hz/2,x(p,'sub')*.4).osc;sub.type=p[424]?'square':'sine'}}
 voice={engine:p[3],wavetable,gain,nodes,filter,expression,pressure:0,midi,release:clamp(f(p,'release')/1000,.025,5),stop:()=>{if(stopped)return;stopped=true;clearInterval(interval);clearTimeout(voice.timer);for(const n of nodes){try{n.stop()}catch{}n.disconnect()}gain.disconnect();expression.disconnect();filter.disconnect();this.tails.delete(voice)}};this.voices.set(voiceId,voice);this.applyExpression(voice,p)
 }
 // Pressure is per voice, not global. Keep the ADSR and expression gain independent.
 private applyExpression(v:Voice,p:Uint8Array){
  const c=this.context;if(!c||v.engine!==p[3])return;
  const t=c.currentTime,pressure=v.midi?v.pressure:0;
  const cutoff=clamp(f(p,'cutoff')*2**(pressure*f(p,'pressureOct')),40,18000);
  v.filter.frequency.setTargetAtTime(cutoff,t,.015);
  v.expression.gain.setTargetAtTime(1+pressure*clamp(f(p,'pressureLevel'),0,2),t,.015);
  if(v.wavetable.length){
   // Quantize only the pressure contribution to avoid rebuilding waves for tiny changes.
   const pos=clamp(f(p,'wave')+Math.round(pressure*127)/127*f(p,'wavePressure'),0,1);
   const key=`${p[14]}:${pos}`;
   if(v.waveKey!==key){
    const {a,b}=harmonics(p[14],pos),wave=c.createPeriodicWave(a,b);
    for(const w of v.wavetable)w.osc.setPeriodicWave(wave);
    v.waveKey=key;
   }
  }
 }
 setPressure(voiceId:number,pressure:number,p:Uint8Array){
  const v=this.voices.get(voiceId);if(!v||!v.midi)return;
  v.pressure=clamp(pressure,0,1);this.applyExpression(v,p);
 }
 update(p:Uint8Array){
  const c=this.context;if(!c||!this.voices.size)return;const t=c.currentTime;
  for(const v of this.voices.values()){
   if(v.engine!==p[3])continue;
   v.filter.type=['lowpass','bandpass','highpass','lowpass'][p[16]] as BiquadFilterType;
   v.filter.Q.setTargetAtTime(clamp(f(p,'resonance')*12,.1,12),t,.015);
   this.applyExpression(v,p);
   for(const w of v.wavetable){
    w.osc.detune.setTargetAtTime(w.spread*f(p,'detune'),t,.015);
    w.pan.pan.setTargetAtTime(w.spread*clamp(f(p,'width'),0,1),t,.015);
   }
  }
 }
 noteOff(note:number){const c=this.context,v=this.voices.get(note);if(!c||!v)return;this.voices.delete(note);this.tails.add(v);const t=c.currentTime;v.gain.gain.cancelAndHoldAtTime(t);v.gain.gain.linearRampToValueAtTime(0,t+v.release);v.timer=window.setTimeout(v.stop,(v.release+.03)*1000)}
 stopMidi(){for(const [id,v] of this.voices){if(v.midi){v.stop();this.voices.delete(id)}}for(const v of this.tails){if(v.midi)v.stop()}}
 panic(){for(const v of this.voices.values())v.stop();for(const v of this.tails)v.stop();this.voices.clear();this.tails.clear()}
 close(){this.panic();void this.context?.close();this.context=null;this.output=null;this.analyser=null}
}
