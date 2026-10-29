const assert = require('node:assert/strict')
const fs = require('node:fs')
const path = require('node:path')
const os = require('node:os')
const ts = require('/opt/nvm/versions/node/v22.16.0/lib/node_modules/typescript')
const source = path.join(__dirname, 'companion-app/src/project')
const tmp = fs.mkdtempSync(path.join(os.tmpdir(), 'tiles-workspace-'))
const target = path.join(tmp, 'companion-app/src/project')
fs.mkdirSync(target, {recursive:true})
fs.mkdirSync(path.join(tmp, 'companion-app/fixtures'), {recursive:true})
fs.writeFileSync(path.join(tmp, 'companion-app/fixtures/tiles-settings.json'), JSON.stringify({
  settings:[
    {key:'color.root',type:'color',default:'660066'},
    {key:'pedal.mode',type:'enum',default:'sustain',values:['sustain','expression']},
  ],
  alphaSettings:[{key:'layout.melodic',type:'enum',default:'rows',values:['rows','columns']}],
}))
for(const name of ['model.ts','catalog.ts','deviceApply.ts']) {
  const content=fs.readFileSync(path.join(source,name),'utf8')
  const output=ts.transpileModule(content,{fileName:name,compilerOptions:{module:ts.ModuleKind.CommonJS,target:ts.ScriptTarget.ES2022,esModuleInterop:true}}).outputText
  fs.writeFileSync(path.join(target,name.replace('.ts','.js')),output)
}
const model=require(path.join(target,'model.js'))
const {reviewPreset,applyReviewed}=require(path.join(target,'deviceApply.js'))
const project=model.createProject('Live Performance', {'color.root':'660066'})
assert.equal(project.presets.length,1)
assert.equal(model.validateProject(JSON.parse(model.serializeProject(project))).name,'Live Performance')
assert.throws(()=>model.validateProject({...project,version:99}),/unsupported/)
assert.throws(()=>model.validateProject({...project,presets:[{...project.presets[0],scales:[{slot:1,name:'Bad',intervals:[1,3]}]}]}),/invalid custom scale/)
assert.throws(()=>model.validateProject({...project,presets:[{...project.presets[0],settings:{'color.root':'ZZZ\nSET x'}}]}),/invalid settings/)
const memory={'color.root':'660066','pedal.mode':'sustain','layout.melodic':'rows'}
const scales={}
const seen=[]
global.window={dispatchEvent:()=>{},tiles:{usb:{command:async(line)=>{
  seen.push(line)
  if(line==='SCHEMA') return [
    'id=1792 key=color.root type=color default=660066',
    'id=256 key=pedal.mode type=enum values=sustain|expression default=sustain',
    'id=2304 key=layout.melodic type=enum values=rows|columns default=rows',
  ]
  if(line==='LIST') return Object.entries(memory).map(([k,v])=>`${k}=${v}`)
  if(line==='SCALES') return Object.values(scales)
  const [action,key,...rest]=line.split(' ')
  if(action==='GET') return [memory[key]]
  if(action==='SET') {memory[key]=rest.join(' ');return ['OK']}
  if(line.startsWith('SCALE PUT')) {scales[rest[0]]=line.replace('SCALE PUT ','');return ['OK']}
  if(line.startsWith('SCALE GET')) return [scales[key]]
  return ['ERR unsupported-command']
}}}}
;(async()=>{
  const preset={...project.presets[0],settings:{'color.root':'ABCDEF','pedal.mode':'expression','layout.melodic':'rows','fake.key':'x'},scales:[]}
  const plan=await reviewPreset(preset)
  assert.equal(plan.changes.length,2)
  assert.equal(plan.alreadyMatching,1)
  assert.deepEqual(plan.unsupported,['fake.key'])
  assert.equal(memory['color.root'],'660066', 'Review must not write hardware')
  const result=await applyReviewed(plan,false,()=>{})
  assert.equal(result.updated,2)
  assert.equal(result.skipped,1)
  assert.equal(memory['color.root'],'ABCDEF')
  assert.equal(memory['pedal.mode'],'expression')
  assert.equal(seen.filter(row=>row.startsWith('SET ')).length,2)
  console.log('PASS: versioned project file, scale and settings validation, safe review, apply and GET verification')
})().catch(error=>{console.error(error);process.exitCode=1}).finally(()=>fs.rmSync(tmp,{recursive:true,force:true}))
