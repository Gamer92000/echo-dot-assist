const fs=require('node:fs'),vm=require('node:vm'),assert=require('node:assert/strict');
const app=fs.readFileSync('web/app.js','utf8'),ring=fs.readFileSync('web/ring.js','utf8');
const node=(tag,props={},...children)=>({tag,props,children:children.flat(Infinity).filter(x=>x!==null&&x!==undefined&&x!==false),append(...xs){this.children.push(...xs.flat(Infinity).filter(Boolean));},textContent:'',innerHTML:''});
function fn(source,name){const m=source.match(new RegExp('(?:async )?function '+name+'\\([^\\n]*\\) \\{[\\s\\S]*?\\n\\}'));assert.ok(m,name);return m[0];}
function run(codename,platform){
 const elements=new Map(['pages','nav','foot'].map(id=>[id,node('div')]));
 const settings=[{name:'mic_level',group:'Voice'},{name:'volume',group:'Sound'}];
 if(platform==='fire_tv')settings.push({name:'audio_output',group:'Audio routing'});else settings.push({name:'led_brightness',group:'Lights'});
 const c={h:node,$:id=>elements.get(id),controls:new Map(),quick:[],state:{settings,device:{version:'test'},wifi:{}},echo:{hello:{codename,platform}},document:{title:''},icon:()=>null,flow:x=>x,settingCards:x=>x,cardHead:(...x)=>node('header',{},...x),ringBox:node('original-echo-ring'),pageShell:p=>node('section',{id:p.id},p.intro),PAGE_BUILD:{},curPage:''};
 vm.createContext(c);vm.runInContext(app.match(/const isCube = .*;/)[0],c);vm.runInContext(app.match(/const PAGES = \[[\s\S]+?\n\];/)[0],c);vm.runInContext(fn(app,'pageForDevice'),c);
 vm.runInContext('let pages=[]; function pageOf(s){return (pages.find(p=>(p.groups||[]).includes(s.group))||{}).id;}'+fn(ring,'buildRing'),c);
 c.PAGE_BUILD.lights=(el,list)=>vm.runInContext('buildRing(target)',Object.assign(c,{target:el}));
 vm.runInContext(fn(app,'build')+'\nbuild();',c);
 const text=JSON.stringify(elements.get('pages')),nav=JSON.stringify(elements.get('nav'));
 assert.ok(nav.includes('#/lights'));assert.ok(nav.includes('#/compare'));
 if(platform==='fire_tv'){
  for(const word of ['Cube and TV light bars','Listening','Thinking','Speaking','Microphones off','Setup / approval','Devices and arbitration','Audio routing'])assert.ok(text.includes(word)||nav.includes(word),word);
  assert.equal((text.match(/cube-bar-preview/g)||[]).length,8);assert.ok(!text.includes('original-echo-ring'));
 }else {assert.ok(text.includes('original-echo-ring'));assert.ok(!text.includes('cube-bar-preview'));assert.ok(text.includes('The equalizer is Amazon'));}
 c.titled=(...x)=>node('card',{},...x);c.nameRow=()=>node('name');c.wifiRow=()=>node('wifi-control');c.settingRow=x=>x;c.logRow=()=>node('logs');c.infoRow=(...x)=>node('row',{},...x);c.exportFile=()=>{};c.restartEcho=()=>{};c.factoryReset=()=>{};c.HELP={restart:{help:'Echo restart'}};c.act=x=>x;c.el=node('section');
 vm.runInContext(fn(app,'buildSystem')+'\nbuildSystem(el,[]);',c);const sys=JSON.stringify(c.el);
 if(platform==='fire_tv'){assert.ok(!sys.includes('wifi-control'));assert.ok(!sys.includes('root shell over Wi-Fi'));assert.ok(sys.includes('Reset Cube Assist'));assert.ok(sys.includes('Fire TV apps, settings and Wi-Fi are unchanged'));}
 else {assert.ok(sys.includes('wifi-control'));assert.ok(sys.includes('root shell over Wi-Fi'));assert.ok(sys.includes('Factory reset'));}
}
for(const name of ['donut','biscuit','radar','checkers','crown','cronos','athena','cupcake','gazelle'])run(name,undefined);
run('gazelle','fire_tv');
console.log('PASS: current page navigation, Echo defaults, Cube routing/bar pages, safe system controls');
