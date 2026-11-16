const fs = require('node:fs');
const path = require('node:path');
const src = fs.readFileSync(path.join(__dirname, 'theme.js'), 'utf8');
new Function(src); // syntax check without running the Home hooks
const start = src.indexOf('  function trophyValue('), end = src.indexOf("  var NP_SUFFIXES =", start);
if (start < 0 || end < start) throw Error('trophy helpers missing');
const T = {}, callbacks = [], requests = [];
const api = new Function('T','APP','THEME','log','setTimeout','globalThis','hubTitleId','fetchTrophies',
  src.slice(start,end) + '\nreturn {trophyValue,cachedTrophies,trophyFetchExport,observeTrophyFetch,trophyFetcher,waitForTrophyFetcher};'
)(T,'hub',{fakeTrophies:true},()=>{},f=>callbacks.push(f),
  {__themeTrophyAsk:ids=>requests.push(ids)},()=>'PPSA00001',()=>{});

function assert(ok,message){if(!ok)throw new Error(message);}
function fetchTrophyProgressData(){return Promise.resolve({earnedTrophies:{gold:1}});}
const cases=[
 [{fetchTrophyProgressData: function anonymous(){}},false],
 [{default:fetchTrophyProgressData},false],
 [{default:{fetchTrophyProgressData}},false],
 [{default:{default:fetchTrophyProgressData}},true],
];
for(const [value,legacy] of cases)assert(api.trophyFetchExport(value,legacy),'export shape');
assert(!api.trophyFetchExport({default:function Unrelated(){}},false),'unrelated module rejected');
const cyclic={};cyclic.default=cyclic;
assert(!api.trophyFetchExport(cyclic,false),'cyclic exports bounded');
const unsafe={};Object.defineProperty(unsafe,'broken',{enumerable:true,get(){throw Error('getter');}});
assert(!api.trophyFetchExport(unsafe,false),'throwing getters ignored');
T.require=function(){throw Error('wrong module 2559');};
T.require.c={8123:{exports:{fetchTrophyProgressData}}};
assert(api.trophyFetcher().fn===fetchTrophyProgressData,'renumbered provider from cache');
delete T.trophyFetcher;delete T.trophyFetchScan;
T.require.c={};T.trophyLegacyChecked=true;
assert(!api.trophyFetcher(),'provider absent');
api.waitForTrophyFetcher('PPSA00001');
api.observeTrophyFetch({fetchTrophyProgressData},9001);
assert(callbacks.length===1,'late provider schedules retry');
callbacks.shift()();
assert(requests[0]==='PPSA00001','late provider replays the requested game');
T.trophyCache={PPSA00001:{id:'PPSA00001',none:true}};
assert(!api.cachedTrophies('PPSA00001'),'empty placeholder is never valid cached data');
T.trophyCache.PPSA00001={id:'PPSA00001',e:[0,1,2,3]};
assert(api.cachedTrophies('PPSA00001'),'successful data remains cached');

const incomingStart = src.indexOf("      if (key !== 'trophies' || !THEME.fakeTrophies) return;");
const incomingEnd = src.indexOf('\n    };\n', incomingStart);
assert(incomingStart>=0 && incomingEnd>incomingStart,'incoming handler found');
const receive = new Function('T','THEME','log','setTimeout','trophyListeners','cachedTrophies','trophyValue','done',
  'return function(key,text){' + src.slice(incomingStart,incomingEnd) + '\n};'
)(T,{fakeTrophies:true},()=>{},f=>f(),[],api.cachedTrophies,api.trophyValue,()=>{});
T.done={};T.focusedId='PPSA00001';T.trophyCache={};
receive('trophies',JSON.stringify({id:'PPSA00001',none:true}));
assert(!T.trophyCache.PPSA00001,'initial empty reply must not poison the Home cache');
receive('trophies',JSON.stringify({id:'PPSA00001',e:[0,1,2,3],p:10}));
const cached=T.trophyCache.PPSA00001;
receive('trophies',JSON.stringify({id:'PPSA00001',none:true}));
assert(T.trophies===cached,'reload placeholder must not overwrite already available trophies');
console.log('Trophy regression tests passed: renamed modules, export shapes, safe discovery, late retry and empty replies.');
