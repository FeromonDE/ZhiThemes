const fs=require('node:fs'),assert=require('node:assert/strict'),path=require('node:path');
const source=fs.readFileSync(path.join(__dirname,'theme.js'),'utf8');
const start=source.indexOf('  function trophyScreenRecord('),end=source.indexOf('  function trophyNameKey(',start);
assert(start>=0&&end>start);
function harness(app='trophy',on=true){
  const lines=[],T={};
  const capture=new Function('T','APP','THEME','globalThis','console',source.slice(start,end)+'\nreturn trophyScreenRecord;')(
    T,app,{screenDump:on},{},{log:s=>lines.push(s)});
  return {capture,lines,T};
}
// Real production row shape shown by build 382's system-screen map, with
// unknown counts initially and the user's screenshot counts after loading.
const row={id:'20:NPWR77777_00',name:'Прагмата',platforms:['prospero'],progress:null,
  numUnlockedPlatinum:null,numUnlockedGold:null,numUnlockedSilver:null,numUnlockedBronze:null};
const h=harness();h.capture({data:row});h.capture({data:{...row}});
assert.equal(h.lines.length,1);assert(h.lines[0].startsWith('THEME-DUMP trophy-records 0\n'));
assert.deepEqual(JSON.parse(h.lines[0].split('\n')[1]).earned,[null,null,null,null]);
const loaded={...row,progress:8,numUnlockedPlatinum:0,numUnlockedGold:0,numUnlockedSilver:1,numUnlockedBronze:4};
h.capture({data:loaded});assert.equal(h.lines.length,2);assert(h.lines[1].startsWith('THEME-DUMP trophy-records 1\n'));
assert.deepEqual(JSON.parse(h.lines[1].split('\n')[1]).earned,[0,0,1,4]);
// Native rowData has its definition and record under separate keys.
h.capture({rowData:{titleId:row.id,titleConf:{name:row.name,platforms:row.platforms},titleRecord:loaded}});
assert.equal(h.lines.length,2,'same row is deduplicated across wrapper components');
for(const [app,on]of [['home',true],['hub',true],['trophy',false]]){
  const off=harness(app,on);off.capture({data:loaded});assert.equal(off.lines.length,0);
}
h.capture({data:{...loaded,id:'PPSA02530'}});assert.equal(h.lines.length,2);
// Capturing useful data is independent of React DevTools and the map's
// 220-entry limit; cap distinct diagnostic updates to prevent file growth.
for(let i=0;i<600;i++)h.capture({data:{...loaded,name:'Synthetic '+i}});
assert.equal(h.lines.length,512);
console.log('Trophy screen capture tests passed: production props, unknown/earned counts, deduplication, opt-in and bounds.');
