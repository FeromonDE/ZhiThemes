const assert = require('node:assert/strict');
const { execFileSync } = require('node:child_process');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');

const source = fs.readFileSync(path.join(__dirname, 'theme.js'), 'utf8');
const temp = fs.mkdtempSync(path.join(os.tmpdir(), 'zt-library-'));
try {
  for (const fixture of [
    { tileSize: 106, focusSize: 168, rowOffsetY: 0, tileVertical: false, expectedTop: 7 },
    { tileSize: 150, focusSize: 238, rowOffsetY: 130, tileVertical: false, expectedTop: 163 },
    { tileSize: 106, focusSize: 168, rowOffsetY: 0, tileVertical: true, expectedTop: 91 },
    { tileSize: 106, focusSize: 168, rowOffsetY: -40, tileVertical: false, expectedTop: -33 }
  ]) {
    const { expectedTop, ...settings } = fixture;
    let script = source;
    for (const [key, value] of Object.entries(settings))
      script = script.replace(new RegExp(`^(    ${key}: )[^,]+`, 'm'), (_, prefix) => prefix + value);
    const input = path.join(temp, 'theme.js');
    fs.writeFileSync(input, script);
    const result = JSON.parse(execFileSync(process.execPath, [path.join(__dirname, 'theme_harness.js'), input], {
      encoding: 'utf8', env: { ...process.env, THEME_APP: 'home', HARNESS_LIBRARY_LAYOUT: '1', HARNESS_HOOKS: '1', HARNESS_ANIM: '1' }
    }));
    assert.equal(result.errors, 0);
    assert.equal(result.hubMarginTop, 128);
    const hidePreview = fixture.tileSize !== 106 || fixture.rowOffsetY !== 0;
    for (const { preview, open, back, left } of result.libraryLayout) {
      assert.deepEqual(open, { top: expectedTop, height: 1080 });
      assert.equal(back.top, expectedTop);
      assert.equal(back.opacity, hidePreview ? 0 : undefined);
      assert.equal(preview.top, hidePreview ? 2000 : 7);
      assert.equal(left.top, preview.top);
    }
  }
} finally {
  fs.rmSync(temp, { force: true, recursive: true });
}
console.log('Game and App Library layout passed: stock, moved, resized, vertical, and exit focus path.');
