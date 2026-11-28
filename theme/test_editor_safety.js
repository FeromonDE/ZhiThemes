const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');
const src = fs.readFileSync(require('node:path').join(__dirname, 'editor.html'), 'utf8');
async function flush() { for (let i = 0; i < 30; i++) await Promise.resolve(); }
async function applyDuringEdit(stale = false, switched = null) {
  const code = src.slice(src.indexOf('  function apply() {'), src.indexOf('  /* Home reloads in this ShellUI'));
  let release, sent, noticed = false;
  const c = vm.createContext({ source: 'A', loaded: 'old', staleTheme: stale, status() {},
    themeSwitchedStatus() { noticed = true; }, themeSwitchedOnPs5() { noticed = true; },
    activeOnPs5: () => Promise.resolve(switched), syncFromPs5: () => Promise.resolve([]),
    request: (m, p, b) => p === '/theme.js' ? (sent = b, new Promise(r => release = r)) : Promise.resolve(),
    setInterval: () => 0, refreshLog() {}, refreshLayout() {}, refreshReloads() {}, fail(e) { throw e; } });
  vm.runInContext(code, c);
  const job = c.apply(); await flush();
  if (stale || switched) {
    await job; assert(noticed); assert.equal(sent, undefined); assert.equal(c.loaded, 'old'); return;
  }
  assert(release); c.source = 'B'; release(); await job;
  assert.equal(sent, 'A'); assert.equal(c.loaded, 'A'); assert.notEqual(c.source, c.loaded);
}
async function install(failure, allow = true) {
  const a = src.indexOf("  document.getElementById('file').onchange ="), b = src.indexOf('  /* your font:', a);
  const input = {}, calls = [], failures = [], scheduled = [];
  let release;
  const file = {name: 'A.ztheme'}, response = {ok: failure !== 'http', text: () => Promise.resolve(
    failure === 'http' ? 'CRC mismatch' : JSON.stringify({theme: 'A'}))};
  const c = vm.createContext({ document: {getElementById: () => input}, source: 'unsaved', loaded: 'old',
    unsavedOk: () => allow, themeSafe: s => s, themeFound: () => 'A', uploadName: s => s,
    confirm: () => true, enc: encodeURIComponent, status() {}, fail: e => failures.push(e.message),
    setTimeout: fn => scheduled.push(fn), location: {reload: () => calls.push('reload')},
    upPost: (url, body) => { calls.push({url, body}); return new Promise((resolve, reject) => {
      release = () => failure === 'upload' ? reject(Error('connection lost')) : resolve(response);
    }); } });
  vm.runInContext(src.slice(a, b), c);
  input.onchange({target: {files: [file], value: 'A.ztheme'}});
  if (!allow) { assert.equal(calls.length, 0); return; }
  assert.equal(c.loaded, 'old', 'upload must not mark unsaved changes saved');
  assert.equal(calls[0].body, file); assert.equal(calls[0].url, '/install?name=A.ztheme&replace=A');
  release(); await flush();
  if (failure) {
    assert.equal(failures.length, 1); assert.equal(c.loaded, 'old'); assert.equal(scheduled.length, 0);
  } else {
    assert.equal(failures.length, 0); assert.equal(c.loaded, 'unsaved'); assert.equal(scheduled.length, 1);
    scheduled[0](); assert.equal(calls.at(-1), 'reload');
  }
}
async function sgdbSearch() {
  const a = src.indexOf('  function sgGameId('), b = src.indexOf('  function sgGrids(', a);
  const requests = [], opened = [];
  const c = vm.createContext({ sg: {}, drawTiles() {}, fail(e) { throw e; },
    sgFetch: url => new Promise(resolve => requests.push({ url, resolve })),
    sgGrids: (game, page) => opened.push({ game: { ...game }, page }) });
  vm.runInContext(src.slice(a, b), c);
  const old = c.sgSearch('Pragmata');
  const direct = c.sgSearch('https://www.steamgriddb.com/game/12345/grids?alpha=1');
  assert.equal(requests[0].url, '/sgdb/search?q=Pragmata');
  assert.equal(requests[1].url, '/sgdb/search?id=12345');
  const game = { id: 12345, name: 'Pragmata' };
  requests[1].resolve(JSON.stringify({ data: game })); await direct;
  requests[0].resolve(JSON.stringify({ data: [{ id: 9, name: 'Old result' }] })); await old;
  assert.deepEqual(opened, [{ game, page: 0 }]);
  assert.equal(c.sgGameId('id:00123'), '123');
  assert.equal(c.sgGameId('1917'), '');
  assert.equal(c.sgGameId('https://steamgriddb.com.example/game/12345'), '');
}
(async () => {
  await applyDuringEdit(); await applyDuringEdit(true); await applyDuringEdit(false, 'Other');
  for (const failure of ['upload', 'http', null]) await install(failure);
  await install(null, false);
  await sgdbSearch();
  console.log('Editor tests passed: Apply snapshots, install failures, SteamGridDB name/link lookup and stale search responses.');
})().catch(e => { console.error(e); process.exitCode = 1; });
