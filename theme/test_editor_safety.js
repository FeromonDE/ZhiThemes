const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');
const src = fs.readFileSync(require('node:path').join(__dirname, 'editor.html'), 'utf8');
async function applyDuringEdit() {
  const code = src.slice(src.indexOf('  function apply() {'), src.indexOf('  function resetFactory()'));
  let release, sent;
  const c = vm.createContext({ source: 'A', loaded: 'old', status() {},
    syncFromPs5: () => Promise.resolve([]),
    request: (m, p, b) => p === '/theme.js' ? (sent = b, new Promise(r => release = r)) : Promise.resolve(),
    setInterval: () => 0, refreshLog() {}, refreshLayout() {}, fail(e) { throw e; } });
  vm.runInContext(code, c);
  const job = c.apply();
  while (!release) await Promise.resolve();
  c.source = 'B'; release(); await job;
  assert.equal(sent, 'A');
  assert.equal(c.loaded, 'A', 'later unsaved edits must not be marked saved');
  assert.notEqual(c.source, c.loaded);
}
async function install(failure) {
  const a = src.indexOf('  function installTheme('), b = src.indexOf('  fetchThemes().catch', a);
  const events = [];
  let oldAssets = true, aborted = false, committed = false;
  const c = vm.createContext({ source: 'old', loaded: 'old', themesInfo: { active: 'A' },
    unsavedOk: () => true, fetchThemes: () => Promise.resolve({}), askThemeName: () => 'A',
    themeSafe: s => s, themeFound: () => 'A', confirm: () => true, uniqueTheme: () => 'B',
    enc: encodeURIComponent, status() {}, setTimeout() {}, location: { reload() {} },
    request: (m, p) => {
      events.push(p);
      assert.ok(!p.includes('/themes/clear'), 'old assets must not be cleared before an install');
      if (p.includes('/start')) return Promise.resolve('{"stage":".zt-stage-test"}');
      if (p.includes('/commit')) {
        if (failure === 'commit') return Promise.reject(new Error('commit failed'));
        committed = true; oldAssets = false;
      }
      if (p.includes('/abort')) aborted = true;
      if (p.startsWith('/theme.js') && failure === 'text') return Promise.reject(new Error('write failed'));
      return Promise.resolve('{}');
    },
    uploadAssets: (files, stage) => {
      assert.equal(stage, '.zt-stage-test'); assert.ok(oldAssets);
      events.push('upload');
      return failure === 'upload' ? Promise.reject(new Error('connection lost')) : Promise.resolve();
    } });
  vm.runInContext(src.slice(a, b), c);
  let failed = false;
  await c.installTheme('new', {}, 'A').catch(() => { failed = true; });
  if (failure) { assert.ok(failed && oldAssets && aborted && !committed); }
  else {
    assert.ok(committed && !aborted);
    assert.ok(events.indexOf('upload') < events.findIndex(p => p.includes('/commit')));
    assert.ok(events.some(p => p.startsWith('/theme.js?name=.zt-stage-test')));
  }
}
(async () => {
  await applyDuringEdit();
  for (const failure of ['upload', 'text', 'commit', null]) await install(failure);
  console.log('Editor safety tests passed: unsaved edits, staged upload/write/commit failures, successful install.');
})().catch(e => { console.error(e); process.exitCode = 1; });
