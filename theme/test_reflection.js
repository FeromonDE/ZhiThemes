// Regression: still reflections must remain one image even in old 24/128-step themes.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const source = fs.readFileSync(path.join(__dirname, 'theme.js'), 'utf8');
const start = source.indexOf('  var reflLogs = 0;');
const end = source.indexOf('  /* theme layers on the Home', start);
assert.ok(start >= 0 && end > start);
let layout = { w: 238, h: 238 }, stateIndex, effectKey, cleanup, updates = 0;
const requests = [], interpolations = [];
const theme = { tileReflection: 0.5, tileReflectionHeight: 0.5, tileReflectionGap: 4,
  tileReflectionSteps: 0, tileReflectionWaves: 0, tileReflectionFlip: 'scaleY', tileCorners: 16, tileSize: 106 };
const context = vm.createContext({ THEME: theme, console: { log: (s) => requests.push(s) }, log: () => {},
  T: { React: {
    useState: () => stateIndex++ === 0 ? [layout, (v) => { layout = v; }] : [0, () => updates++],
    useEffect: (fn, deps) => {
      if (effectKey !== deps[0]) { if (cleanup) cleanup(); effectKey = deps[0]; cleanup = fn(); }
    },
    createElement: (type, props, ...children) => ({ type, props, children })
  }, rn: { View: 'View', Image: 'Image', Animated: { View: 'Animated.View' } },
  reflWave: { interpolate: (config) => { interpolations.push(config); return config; } } }
});
vm.runInContext(source.slice(start, end), context);
const render = () => { stateIndex = 0; return context.TileReflection({ id: 'TEST', uri: 'file:///user/app/TEST00001/sce_sys/icon0.png?ts=1' }); };
const deliver = () => context.reflectionReceived('zt-reflection-' + effectKey, JSON.stringify('/test/reflection.png'));

assert.equal(render().children.length, 0, 'pending reflection does not build strips on the UI thread');
assert.equal(requests.length, 1);
assert.ok(requests[0].startsWith('THEME-REFLECTION '));
assert.ok(!requests[0].includes('?ts='), 'native file path excludes query string');
assert.equal(deliver(), true);
assert.equal(updates, 1, 'completion redraws the mounted component');
for (const steps of [0, 2, 8, 24, 64, 128, 500]) {
  theme.tileReflectionSteps = steps;
  const view = render();
  assert.equal(view.children.length, 1);
  const image = view.children[0];
  assert.equal(image.type, 'Image', 'still reflection uses one image regardless of legacy strip count');
  assert.equal(image.props.source.uri, 'file:///test/reflection.png');
  assert.equal(image.props.style.height, 119);
  assert.equal(image.props.style.top, 242);
  assert.equal(image.props.style.opacity, 0.5);
  assert.equal(image.props.style.transform, undefined, 'cached pixels are already mirrored');
}
assert.equal(requests.length, 1, 'renders reuse the prepared texture');

theme.tileReflectionWaves = 6;
for (const steps of [0, 8, 24, 128]) {
  theme.tileReflectionSteps = steps;
  const strips = render().children[0];
  assert.equal(strips.length, 8, 'ripple node count is bounded');
  let y = 242;
  for (const strip of strips) {
    assert.equal(strip.type, 'Animated.View');
    assert.equal(strip.props.style.top, y);
    assert.equal(strip.props.style.opacity, 0.5, 'baked fade is continuous across ripple slices');
    const image = strip.children[0];
    assert.equal(image.props.style.top, -(y - 242));
    assert.equal(image.props.style.height, 119);
    y += strip.props.style.height;
  }
  assert.equal(y, 361);
}

theme.tileReflectionWaves = 0;
render().children[0].props.onError();
const fallback = render().children[0];
assert.equal(fallback.length, 8, 'unsupported image fallback cannot restore the expensive 128-strip path');
assert.equal(fallback.at(-1).props.style.opacity, 0);
for (const flip of ['scaleY', 'rotateX', 'rotate']) {
  theme.tileReflectionFlip = flip;
  assert.ok(render().children[0][0].children[0].props.style.transform.length > 0);
}

const oldKey = effectKey;
layout = { w: 1000, h: 2000 };
assert.equal(render().children.length, 0, 'new geometry waits for its own texture');
assert.notEqual(effectKey, oldKey);
assert.equal(context.reflectionWaiters[oldKey].length, 0, 'old subscription cleaned up');
const dims = requests.at(-1).split('|').slice(-4).map(Number);
assert.ok(dims[0] <= 1024 && dims[1] <= 1536 && dims[2] <= dims[1]);
context.reflectionReceived('zt-reflection-' + effectKey, '""');
assert.ok(render().children[0].length <= 8, 'native decode failure uses bounded fallback');
assert.equal(context.reflectionReceived('unrelated', '""'), false);

layout = null;
const pending = render();
assert.equal(pending.children.length, 0);
pending.props.onLayout({ nativeEvent: { layout: { width: 0, height: 100 } } });
assert.equal(layout, null);
pending.props.onLayout({ nativeEvent: { layout: { width: 200, height: 300 } } });
assert.equal(layout.w, 200);
assert.equal(layout.h, 300);
console.log('Reflection tests passed: one still image, async preparation, bounded ripples/fallback, layout and reuse.');
