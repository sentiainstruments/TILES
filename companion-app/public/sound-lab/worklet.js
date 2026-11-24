const imports={env:{fmaxf:Math.max,fminf:Math.min,powf:Math.pow,exp2f:x=>2**x,expf:Math.exp,cosf:Math.cos,sinf:Math.sin}};
class ResonanceProcessor extends AudioWorkletProcessor {
 constructor(options){super();this.dsp=new WebAssembly.Instance(options.processorOptions.module,imports).exports;this.dsp.studio_init(sampleRate);this.port.onmessage=({data:m})=>{const d=this.dsp;switch(m.type){case 'params':d.studio_params(...m.values);break;case 'expression':d.studio_expression(...m.values);break;case 'gain':d.studio_gain(m.value);break;case 'on':d.studio_note_on(m.note,m.velocity);break;case 'off':d.studio_note_off(m.note);break;case 'panic':d.studio_panic();break;}};this.port.postMessage({type:'ready'});}
 process(inputs,outputs){const out=outputs[0];if(!out?.length)return true;const d=this.dsp;for(let offset=0;offset<out[0].length;offset+=128){const n=Math.min(128,out[0].length-offset);d.studio_render(n);out[0].set(new Float32Array(d.memory.buffer,d.studio_left(),n),offset);if(out[1])out[1].set(new Float32Array(d.memory.buffer,d.studio_right(),n),offset);}return true;}
}
registerProcessor('resonance',ResonanceProcessor);
