const fs=require('node:fs'),path=require('node:path'),assert=require('node:assert/strict');
const source=fs.readFileSync(path.join(__dirname,'theme.js'),'utf8');
const start=source.indexOf('  function trophyValue('),end=source.indexOf('  var NP_SUFFIXES =',start);
assert(start>=0 && end>start);
function harness(){
  let now=0,id=0;const timers=new Map(),logs=[],T={done:{}};
  const methods=new Function('T','APP','THEME','log','setTimeout','clearTimeout','globalThis','hubTitleId','fetchTrophies','Date',
    source.slice(start,end)+'\nreturn {trophyOfflineFetcher,trophyErrorText,trophyFetcher};')(
      T,'hub',{fakeTrophies:true},s=>logs.push(s),
      (fn,ms)=>{timers.set(++id,{fn,time:now+ms});return id;},n=>timers.delete(n),{},()=>'',()=>{}, {now:()=>now});
  return {...methods,T,timers,logs,advance(ms){now+=ms;for(const [n,t]of [...timers])if(t.time<=now){timers.delete(n);t.fn();}}};
}
async function flush(){for(let i=0;i<80;i++)await Promise.resolve();}
function deferred(){let resolve,reject;const promise=new Promise((a,b)=>{resolve=a;reject=b;});return {promise,resolve,reject};}
function native(overrides={}){
  const calls=[];
  const api={
    createAccessor:()=>0, deleteAccessor:()=>{},
    isOnlineModeAvailable(){throw Error('online capability must not be queried');},
    getPlayedTitleIdListByNpTitleId:()=>['local-title'],getTitleIdListByNpTitleId:()=>['local-title'],
    getTitleConf:()=>({numPlatinum:1,numGold:3,numSilver:14,numBronze:20}),
    getTitleRecord:()=>({numUnlockedPlatinum:0,numUnlockedGold:2,numUnlockedSilver:4,numUnlockedBronze:8,progress:37}),
    ...overrides
  };
  for(const name of Object.keys(api)){const fn=api[name];api[name]=function(args){assert.equal(this,api);calls.push([name,args]);return fn(args);};}
  return {api,calls};
}
async function main(){
  // Reproduce the affected played title with nonzero local records. The
  // stock service chooses its own online mode and is deliberately unusable.
  {
    const h=harness(),n=native();
    h.T.require=id=>{
      if(id===2578)return {fetchTitleData(){throw Error('stock online chooser called');}};
      if(id===210)return {TrophyAccessor:n.api};
      if(id===8)return {User:{launchedUser:452565539}};
      throw Error('unexpected module '+id);
    };h.T.require.c={};
    const provider=h.trophyFetcher();assert.equal(provider.label,'TrophyAccessor (offline)');
    const data=await provider.fn({npTitleId:'PPSA02530_00'});
    assert.deepEqual(data.earnedTrophies,{platinum:0,gold:2,silver:4,bronze:8});assert.equal(data.progress,37);
    assert.deepEqual(n.calls[0],['createAccessor',{userId:452565539,mode:'offline'}]);
    assert.deepEqual(n.calls.find(x=>x[0]==='getTitleRecord')[1],{accessorId:0,titleId:'local-title',accountId:'me'});
    assert.equal(n.calls.filter(x=>x[0]==='deleteAccessor').length,1);
    assert.equal(h.timers.size,0);
  }
  // Real failures retain their nested native code and always close the handle.
  {
    const h=harness(),n=native({getTitleRecord(){throw Object.assign(Error('TrophyHelperError'),{error:{code:'TEST_NATIVE_FAILURE',errorMessage:'record failure'}});}});
    await assert.rejects(h.trophyOfflineFetcher(n.api,{launchedUser:1})({npTitleId:'PPSA02530_00'}),/getTitleRecord.*TEST_NATIVE_FAILURE/);
    assert.equal(n.calls.filter(x=>x[0]==='deleteAccessor').length,1);assert.equal(h.timers.size,0);
  }
  // A stale first mapping must not hide an available record in a later mapping.
  {
    const h=harness(),n=native({getPlayedTitleIdListByNpTitleId:()=>['old','local-title'],getTitleConf:p=>{if(p.titleId==='old')throw Error('stale title');return {numPlatinum:1,numGold:3};}});
    const data=await h.trophyOfflineFetcher(n.api,{launchedUser:1})({npTitleId:'PPSA02530_00'});
    assert.equal(data.earnedTrophies.gold,2);assert.equal(n.calls.filter(x=>x[0]==='deleteAccessor').length,1);
  }
  // An unplayed title can expose definitions and actual zero counts.
  {
    const h=harness(),n=native({getPlayedTitleIdListByNpTitleId:()=>[],getTitleRecord:()=>({numUnlockedPlatinum:0,numUnlockedGold:0,numUnlockedSilver:0,numUnlockedBronze:0,progress:0})});
    const data=await h.trophyOfflineFetcher(n.api,{launchedUser:1})({npTitleId:'PPSA11386_00'});
    assert.equal(data.earnedTrophies.gold,0);assert.equal(data.definedTrophies.gold,3);
    assert(n.calls.some(x=>x[0]==='getTitleIdListByNpTitleId'));
  }
  // A timed-out read releases the handle; its late reply cannot reopen work.
  {
    const h=harness(),r=deferred(),n=native({getTitleRecord:()=>r.promise});
    const result=h.trophyOfflineFetcher(n.api,{launchedUser:1})({npTitleId:'PPSA02530_00'});
    const rejected=assert.rejects(result,/getTitleRecord: timeout/);
    await flush();h.advance(1200);await flush();await rejected;
    assert.equal(n.calls.filter(x=>x[0]==='deleteAccessor').length,1);
    r.resolve({numUnlockedGold:999});await flush();assert.equal(n.calls.filter(x=>x[0]==='deleteAccessor').length,1);
    assert.equal(h.timers.size,0);
  }
  // Even a handle allocated after create timed out must be cleaned up (0 is valid).
  {
    const h=harness(),r=deferred(),n=native({createAccessor:()=>r.promise});
    const result=h.trophyOfflineFetcher(n.api,{launchedUser:1})({npTitleId:'PPSA02530_00'});
    const rejected=assert.rejects(result,/createAccessor: timeout/);
    await flush();h.advance(1200);await flush();await rejected;
    r.resolve(0);await flush();assert.deepEqual(n.calls.at(-1),['deleteAccessor',{accessorId:0}]);assert.equal(h.timers.size,0);
  }
  // Cleanup failure must not discard successfully read earned trophies.
  {
    const h=harness(),r=deferred(),n=native({deleteAccessor:()=>r.promise});
    const result=h.trophyOfflineFetcher(n.api,{launchedUser:1})({npTitleId:'PPSA02530_00'});
    await flush();h.advance(500);await flush();assert.equal((await result).earnedTrophies.gold,2);
    assert(h.logs.some(x=>x.includes('deleteAccessor: timeout')));assert.equal(h.timers.size,0);
  }
  // Missing records must remain errors, rather than fabricated zero achievements.
  {
    const h=harness(),n=native({getTitleRecord:()=>({})});
    await assert.rejects(h.trophyOfflineFetcher(n.api,{launchedUser:1})({npTitleId:'PPSA02530_00'}),/no earned counts/);
    assert.equal(n.calls.filter(x=>x[0]==='deleteAccessor').length,1);
    const cycle={message:'TrophyHelperError',error:{code:'TEST_CODE'}};cycle.error.cause=cycle;
    assert(h.trophyErrorText(cycle).includes('TEST_CODE'));
  }
  console.log('Offline trophy tests passed: nonzero records, no online chooser, cleanup, late handles, timeouts and native errors.');
}
main().catch(e=>{console.error(e);process.exitCode=1;});
