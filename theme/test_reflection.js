// Exercise the actual reflection component without needing the PS5 renderer.
// Run: node theme/test_reflection.js
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');

const source = fs.readFileSync(path.join(__dirname, 'theme.js'), 'utf8');
const start = source.indexOf('  var reflLogs = 0;');
const end = source.indexOf('  /* theme layers on the Home', start);
assert.ok(start >= 0 && end > start, 'reflection component found');
let layout = { w: 238, h: 238 };
let updatedLayout;
let interpolations = [];
const theme = {
  tileReflection: 0.5, tileReflectionHeight: 0.5, tileReflectionGap: 4,
  tileReflectionSteps: 0, tileReflectionWaves: 0, tileReflectionFlip: 'scaleY',
  tileCorners: 16, tileSize: 106
};
const context = vm.createContext({
  THEME: theme,
  T: {
    React: {
      useState: () => [layout, (value) => { updatedLayout = value; }],
      createElement: (type, props, ...children) => ({ type, props, children })
    },
    rn: { View: 'View', Image: 'Image', Animated: { View: 'Animated.View' } },
    reflWave: { interpolate: (config) => { interpolations.push(config); return config; } }
  },
  log: () => {}
});
vm.runInContext(source.slice(start, end), context);
const render = () => context.TileReflection({ id: 'TEST', uri: 'file:///test.png' });
const strips = () => render().children[0];

function checkGeometry(list, height, gap = 4) {
  let y = layout.h + gap;
  for (const strip of list) {
    const style = strip.props.style;
    assert.equal(style.top, y, 'adjacent strips have no gap or overlap');
    assert.ok(style.height > 0);
    assert.equal(style.overflow, 'hidden');
    const image = strip.children[0];
    assert.equal(image.type, 'Image');
    assert.equal(image.props.source.uri, 'file:///test.png');
    assert.equal(image.props.style.top, -(y - layout.h - gap), 'same image crop across strips');
    assert.equal(image.props.style.height, layout.h);
    assert.equal(image.props.style.width, layout.w);
    y += style.height;
  }
  assert.equal(y, layout.h + gap + height, 'exact reflection height');
  if (list.length > 1) assert.equal(list[0].props.style.opacity, theme.tileReflection);
  assert.equal(list.at(-1).props.style.opacity, 0, 'no opaque bottom edge');
  for (let i = 1; i < list.length; i++) {
    assert.ok(list[i].props.style.opacity <= list[i - 1].props.style.opacity);
    assert.ok(list[i].props.style.opacity >= 0);
  }
}

const automatic = strips();
assert.equal(automatic.length, 119, 'automatic density follows height');
checkGeometry(automatic, 119);
for (let i = 1; i < automatic.length; i++) {
  assert.ok(automatic[i - 1].props.style.opacity - automatic[i].props.style.opacity < 2 / 255,
    'bright tile fade changes by less than two alpha levels per strip');
}

for (const steps of [2, 8, 24, 64, 128, 500]) {
  theme.tileReflectionSteps = steps;
  const list = strips();
  assert.equal(list.length, Math.min(119, steps, 128));
  checkGeometry(list, 119);
}

for (const steps of [0, undefined, -1, NaN]) {
  theme.tileReflectionSteps = steps;
  assert.equal(strips().length, 119, 'missing/automatic count uses height');
}

layout = { w: 600.25, h: 900.5 };
theme.tileReflectionSteps = 0;
theme.tileReflectionGap = -3;
const tall = strips();
assert.equal(tall.length, 128, 'large reflections have a bounded view count');
checkGeometry(tall, 450, -3);

layout = { w: 100, h: 5 };
theme.tileReflectionHeight = 0.1;
const tiny = strips();
assert.equal(tiny.length, 1);
checkGeometry(tiny, 1, -3);

layout = null;
const pending = render();
assert.equal(pending.children.length, 0, 'waits for a measured layout');
pending.props.onLayout({ nativeEvent: { layout: { width: 0, height: 100 } } });
assert.equal(updatedLayout, undefined);
pending.props.onLayout({ nativeEvent: { layout: { width: 200, height: 300 } } });
assert.equal(updatedLayout.w, 200);
assert.equal(updatedLayout.h, 300);

layout = { w: 200, h: 256 };
theme.tileReflectionHeight = 0.5;
theme.tileReflectionGap = 4;
theme.tileReflectionWaves = 6;
for (const steps of [8, 128]) {
  theme.tileReflectionSteps = steps;
  interpolations = [];
  const list = strips();
  checkGeometry(list, 128);
  assert.ok(list.every((strip) => strip.type === 'Animated.View'));
  const half = list.findIndex((strip) => strip.props.style.top === layout.h + 4 + 64);
  assert.ok(half >= 0);
  assert.ok(Math.abs(interpolations[half].outputRange[0] /
    (6 * (0.3 + 0.7 * (64 + list[half].props.style.height) / 128)) -
    Math.sin(2 * Math.PI * 0.48)) < 1e-10, 'ripple phase follows depth, not strip count');
}

theme.tileReflectionWaves = 0;
for (const flip of ['scaleY', 'rotateX', 'rotate']) {
  theme.tileReflectionFlip = flip;
  const transform = strips()[0].children[0].props.style.transform;
  assert.equal(JSON.stringify(transform), JSON.stringify({
    scaleY: [{ scaleY: -1 }], rotateX: [{ rotateX: '180deg' }],
    rotate: [{ rotate: '180deg' }, { scaleX: -1 }]
  }[flip]), 'existing image flip modes are preserved');
}
console.log('Reflection tests passed (density, fade, geometry, layout, ripples, flip).');
