// Exercise actual builders for both platforms; never contact a device.
const fs = require('node:fs'), vm = require('node:vm'), assert = require('node:assert/strict');
const app = fs.readFileSync('web/app.js','utf8'), ring = fs.readFileSync('web/ring.js','utf8');
const node = (tag, props = {}, ...children) => ({tag,props,children:children.flat(Infinity).filter(Boolean),
  append(...items){this.children.push(...items.flat(Infinity).filter(Boolean));}, textContent:'', innerHTML:''});
function run(codename, platform) {
  const elements = new Map(['sections','nav','foot'].map(id=>[id,node('div')]));
  const settings = [{name:'mic_level',group:'Voice'},{name:'volume',group:'Sound'}];
  if (platform !== 'fire_tv') settings.push({name:'led_brightness',group:'Lights'});
  const existingRing = node('div', {class:'original-echo-ring'});
  const context = {
    h:node, $:id=>elements.get(id), controls:new Map(), VOLUMES:['volume'], state:{settings,device:{version:'test'}},
    echo:{hello:{codename,platform}}, icon:()=>null, document:{title:''},
    cardHead:(title,help)=>node('header',{},title,help), ringBox:existingRing,
    settingRow:s=>node('setting',{name:s.name}), buildVolumes:()=>{},
    buildSystem:()=>{},buildEchos:()=>{},watchNav:()=>{},
  };
  vm.createContext(context);
  for (const pattern of [/const isCube = .*;/, /const SECTIONS = \[[\s\S]+?\n\];/,
      /function sectionForDevice\(sec\) \{[\s\S]+?\n\}/, /function sectionShell\(sec\) \{[\s\S]+?\n\}/])
    vm.runInContext(app.match(pattern)[0],context);
  vm.runInContext(ring.match(/function buildRing\(el\) \{[\s\S]+?\n\}/)[0],context);
  vm.runInContext(app.match(/function build\(\) \{[\s\S]+?\n\}/)[0]+'\nbuild();',context);
  const lights=elements.get('sections').children.find(n=>n.props.id==='sec-Lights');
  assert.ok(lights,'Lights navigation must be present');
  assert.ok(elements.get('nav').children.some(n=>n.props.href==='#sec-Lights'));
  const text=JSON.stringify(elements.get('sections'));
  if (platform === 'fire_tv') {
    for (const word of ['Cube and TV light bars','Listening','Thinking','Speaking','Microphones off','Setup / approval']) assert.ok(text.includes(word),word);
    assert.equal((text.match(/cube-bar-preview/g)||[]).length,8);
    assert.ok(text.includes('Devices and arbitration'));
    assert.ok(!text.includes('original-echo-ring'));
    assert.equal(context.document.title,'Cube Assist settings');
  } else {
    assert.ok(text.includes('original-echo-ring'));
    assert.ok(text.includes('The equalizer is Amazon'));
    assert.ok(!text.includes('cube-bar-preview'));
    assert.ok(!text.includes('Devices and arbitration'));
    assert.ok(!text.includes('Cube Assist'));
    assert.equal(context.document.title,'Echo settings');
  }
}
for (const name of ['donut','biscuit','radar','checkers','crown','cronos','athena']) run(name,undefined);
run('gazelle','fire_tv');
// Selection must come from the advertised platform, not just a hardcoded codename.
run('gazelle',undefined);
console.log('PASS: Echo defaults preserved, Cube capability selection, eight bar states and navigation');


function systemPage(cube, action) {
  const root = node('section');
  const context = {
    h:node, isCube:()=>cube, state:{wifi:{}}, echo:{hello:{action}}, icon:()=>null,
    nameRow:()=>node('name'), wifiRow:()=>node('wifi-control'), settingRow:()=>null,
    infoRow:(...args)=>node('row',{},...args), logRow:()=>node('logs'),
    exportFile:()=>{}, restartEcho:()=>{}, factoryReset:()=>{},
    HELP:{restart:{help:'Echo restart help'}}, root,
  };
  vm.createContext(context);
  vm.runInContext(app.match(/function act\([^\n]+\{[\s\S]+?\n\}/)[0],context);
  vm.runInContext(app.match(/function buildSystem\(el, list\) \{[\s\S]+?\n\}/)[0]+'\nbuildSystem(root, []);',context);
  return JSON.stringify(root);
}
const echoSystem=systemPage(false), cubeSystem=systemPage(true);
assert.ok(echoSystem.includes('wifi-control'));
assert.ok(echoSystem.includes('Opens adb over Wi-Fi for 30 minutes'));
assert.ok(echoSystem.includes('Factory reset'));
assert.ok(echoSystem.includes('Home Assistant app finds it over Bluetooth'));
assert.ok(!echoSystem.includes('Reset Cube Assist'));
assert.ok(!cubeSystem.includes('wifi-control'));
assert.ok(!cubeSystem.includes('Opens adb over Wi-Fi for 30 minutes'));
assert.ok(cubeSystem.includes('Reset Cube Assist'));
assert.ok(cubeSystem.includes('Fire TV settings, apps and Wi-Fi stay as they are'));
console.log('PASS: Echo networking/ADB/factory reset retained; Cube system controls isolated');

const showSystem=systemPage(false,'volume_keys');
assert.ok(!showSystem.includes('holding the action button for 10 seconds'));
assert.ok(showSystem.includes('volume keys'));
console.log('PASS: Echo Show approval uses volume keys and reset omits the unsupported hold action');
