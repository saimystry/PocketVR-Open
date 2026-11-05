// SPDX-License-Identifier: Apache-2.0
// The page only talks to the native host: "state" messages arrive about once a
// second and every user action goes back as one small command.
'use strict';
const $ = id => document.getElementById(id);
const host = window.chrome && window.chrome.webview;
const send = (action, extra = {}) => host && host.postMessage({ action, ...extra });

const friendly = {
  'video.codec': ['Video format', 'Automatic picks the safest option'],
  'video.fps': ['Frame rate', 'Frames per second sent to the phone'],
  'video.resolution': ['Stream resolution', 'Auto starts at full size. Applies the next time the phone connects'],
  'video.render_scale': ['Antialiasing (supersampling)', 'Renders extra pixels for smoother edges, but is slower. Restart SteamVR if it does not change'],
  'video.bitrate_mbps': ['Starting bitrate (Mbps)', 'Higher reduces blocky areas'],
  'video.min_bitrate_mbps': ['Lowest bitrate (Mbps)', ''],
  'video.max_bitrate_mbps': ['Highest bitrate (Mbps)', 'USB can use higher values than Wi-Fi'],
  'video.slices': ['Encoder slices', 'More can lower delay slightly'],
  'video.send_spread_pct': ['Wi-Fi smoothing', 'Spreads each frame out to avoid Wi-Fi loss, adding a little delay. 0 turns it off'],
  'debug.test_pattern': ['Show test picture', 'Replaces the game with a sky gradient and fine lines, to compare picture quality'],
  'debug.latency_log': ['Record delay to a file', 'Saves latency.csv in the logs folder'],
  'fec.adaptive': ['Automatic error protection', 'Helps on unreliable Wi-Fi'],
  'fec.ratio_pct': ['Error protection (%)', 'Extra data sent to survive packet loss'],
  'display.ipd_mm': ['Eye distance (mm)', 'The distance between your pupils'],
  'display.fov_scale': ['Field of view', ''],
  'display.eye_size': ['Size of each eye', 'Bigger hides the black border around the picture'],
  'lens.screen_to_lens_m': ['Lens to screen', ''],
  'lens.inter_lens_distance_m': ['Space between lenses', ''],
  'lens.screen_width_m': ['Screen width', ''],
  'lens.screen_height_m': ['Screen height', ''],
  'lens.vertical_offset_m': ['Lens height on screen', ''],
  'lens.k1': ['Lens curve 1', ''],
  'lens.k2': ['Lens curve 2', ''],
  'lens.fov_left_deg': ['Outer view limit', ''],
  'lens.fov_right_deg': ['Inner view limit', ''],
  'lens.fov_bottom_deg': ['Bottom view limit', ''],
  'lens.fov_top_deg': ['Top view limit', ''],
  'display.vignette_strength': ['Vignette strength', 'Darkens the edges of the view'],
  'display.vignette_size': ['Vignette size', ''],
  'display.vignette_softness': ['Vignette softness', ''],
  'display.vignette_dynamic': ['Vignette follows head speed', 'Can reduce motion sickness'],
  'display.chromatic_correction': ['Colour fringe correction', ''],
  'display.edge_mode': ['Picture edges', 'What shows past the edge of the image'],
  'tracking.fusion': ['Head tracking method', ''],
  'tracking.rate_hz': ['Tracking rate (Hz)', ''],
  'tracking.yaw_correction': ['Drift correction', 'Keeps forward pointing forward'],
  'tracking.max_prediction_ms': ['Maximum prediction (ms)', ''],
  'tracking.prediction_ms': ['Prediction', 'Number in ms, or auto'],
  'tracking.eye_height_m': ['Eye height (m)', 'Your height in VR, seated or standing'],
  'reprojection.enabled': ['Smooth head movement', 'Keeps turning smooth when frames are late'],
  'pacing.target_lead_ms': ['Frame timing lead (ms)', ''],
  'ui.stats_overlay': ['Performance overlay', 'Shows numbers inside the headset'],
  'power.keep_awake': ['Keep phone awake', ''],
  'power.thermal_guard': ['Protect phone from overheating', 'Lowers quality when the phone gets hot'],
  'security.encrypt_media': ['Encrypt video', 'Leave on unless you are testing speed'],
  'debug.log_level': ['Log detail', ''],
  'debug.dump_frames': ['Record video for support', ''],
  'connection.control_port': ['Control port', ''],
  'connection.media_port': ['Video port', ''],
};
const groupNames = {
  video: 'Video', fec: 'Network', display: 'Display', lens: 'Lenses', tracking: 'Head tracking',
  reprojection: 'Head tracking', pacing: 'Video', ui: 'Display', power: 'Phone',
  security: 'Network', debug: 'Support', connection: 'Network', input: 'Controller buttons',
};
const words = name => name.replace(/_/g, ' ').replace(/^./, c => c.toUpperCase());

let shownPhone = null;
let state = {}, built = false, toastTimer, qrText = '';
const editing = new Set();
function toast(text) {
  const box = $('toast');
  box.textContent = text; box.hidden = false;
  clearTimeout(toastTimer);
  toastTimer = setTimeout(() => { box.hidden = true; }, 4500);
}
function show(id) {
  document.querySelectorAll('.tab').forEach(t => t.classList.toggle('active', t.dataset.page === id));
  document.querySelectorAll('.page').forEach(p => p.classList.toggle('active', p.id === id));
}
document.querySelectorAll('.tab').forEach(tab => tab.addEventListener('click', () => show(tab.dataset.page)));

function setStatus(kind, title, detail, label, action) {
  $('dot').className = 'dot ' + kind;
  $('headline').textContent = title;
  $('subline').textContent = detail;
  const main = $('mainAction');
  main.hidden = !label;
  main.textContent = label || '';
  main.onclick = action || null;
}
function renderHome() {
  const running = !state.error && state.status;
  const session = running && state.network ? state.network.session : null;
  const streaming = running && state.status.video_streaming;
  if (!running) {
    if (state.graphics_problem)
      setStatus('bad', "This PC can't run VR", state.graphics_problem + ' does not support DirectX 11, which SteamVR needs. Install PocketVR on your gaming PC.');
    else if (!state.steamvr_installed)
      setStatus('bad', 'SteamVR is needed', 'Install SteamVR from Steam, then come back here.', 'Open SteamVR in Steam', () => send('steamvr.store'));
    else if (state.launching)
      setStatus('busy', 'Starting SteamVR', 'This can take up to a minute.');
    else if (state.vr_running)
      setStatus('bad', 'PocketVR did not start', 'SteamVR is running without PocketVR. Close SteamVR and start it again. If this keeps happening, save a support file from Help.');
    else
      setStatus('', 'SteamVR is not running', 'Start it to connect your phone.', 'Start SteamVR', () => send('steamvr.start'));
  } else if (state.status.gpu_lost) {
    setStatus('bad', 'The graphics card reset', 'Restart SteamVR to continue.', 'Restart SteamVR', () => send('steamvr.restart'));
  } else if (streaming) {
    setStatus('good', 'Streaming to your phone', 'Put your phone in the headset and start a game.');
  } else if (session) {
    setStatus('warn', 'Phone connected', 'Waiting for a picture from SteamVR.');
  } else {
    setStatus('busy', 'Waiting for your phone', 'Open PocketVR on your phone and tap Connect.');
  }
  const pending = running && state.network ? state.network.pending_phone : null;
  shownPhone = pending ? pending.key : null;
  $('approval').hidden = !pending;
  if (pending) $('approvalText').textContent = 'Phone ' + pending.id + ' at ' + pending.ip + '. Only allow it if it is yours.';
  $('live').hidden = !session;
  if (session) {
    const latency = session.latency || {};
    $('sDelay').textContent = latency.samples ? Math.round(latency.total.p50_ms) + ' ms' : '–';
    $('sFrame').textContent = latency.samples ? Math.round(latency.average_frame_kilobits) + ' kbit' : '–';
    const quality = session.adaptation || {};
    const format = quality.format || {};
    const current = format.current || {};
    const phone = (session.phone_stats && session.phone_stats.values) || {};
    $('sRoute').textContent = session.transport === 'tcp' ? 'USB cable' : 'Wi-Fi or tethering';
    $('sFormat').textContent = current.width ? current.width + ' × ' + current.height : '–';
    $('sBitrate').textContent = quality.encoder_accepted_bitrate_bps ? (quality.encoder_accepted_bitrate_bps / 1e6).toFixed(0) + ' Mbps' : '–';
    $('sFps').textContent = phone.video_fps ? Math.round(phone.video_fps) + ' fps' : '–';
  }
  $('actions').hidden = !session;
}
function renderPhone() {
  const adb = state.adb || {};
  $('adbState').textContent = adb.message || '';
  $('installApk').disabled = !adb.ready || state.installing;
  $('installApk').textContent = state.installing ? 'Installing…' : 'Install on phone';
  const qr = state.qr;
  if (qr && qr.text !== qrText) {
    qrText = qr.text;
    const size = qr.size;
    let path = '';
    qr.rows.forEach((row, y) => { for (let x = 0; x < size; ++x) if (row[x] === '1') path += `M${x} ${y}h1v1h-1z`; });
    $('qrBox').innerHTML = `<svg viewBox="0 0 ${size} ${size}" shape-rendering="crispEdges"><path d="${path}"/></svg>`;
    $('qrBox').classList.add('on');
    $('qrText').textContent = qr.label;
  } else if (!qr) {
    qrText = '';
    $('qrBox').classList.remove('on');
    $('qrText').textContent = 'The code appears here when SteamVR is running.';
  }
}
// How a setting's stored value appears on its slider.
const sliderViews = {
  'video.render_scale': { scale: 100, unit: '%', step: 5 },
  'video.send_spread_pct': { unit: '%', step: 5 },
  'video.bitrate_mbps': { step: 1 }, 'video.min_bitrate_mbps': { step: 1 }, 'video.max_bitrate_mbps': { step: 1 },
  'display.ipd_mm': { unit: ' mm', step: 0.1 },
  'display.fov_scale': { scale: 100, unit: '%', step: 1 },
  'display.eye_size': { scale: 100, unit: '%', step: 1 },
  'lens.screen_to_lens_m': { scale: 1000, unit: ' mm', step: 0.1 },
  'lens.inter_lens_distance_m': { scale: 1000, unit: ' mm', step: 0.1 },
  'lens.screen_width_m': { scale: 1000, unit: ' mm', step: 0.1 },
  'lens.screen_height_m': { scale: 1000, unit: ' mm', step: 0.1 },
  'lens.vertical_offset_m': { scale: 1000, unit: ' mm', step: 0.1 },
  'lens.k1': { step: 0.01 }, 'lens.k2': { step: 0.01 },
  'lens.fov_left_deg': { unit: '\u00b0', step: 1 }, 'lens.fov_right_deg': { unit: '\u00b0', step: 1 },
  'lens.fov_bottom_deg': { unit: '\u00b0', step: 1 }, 'lens.fov_top_deg': { unit: '\u00b0', step: 1 },
  'tracking.eye_height_m': { unit: ' m', step: 0.01 },
};
function slider(key, spec, value, apply) {
  const view = sliderViews[key] || {};
  const scale = view.scale || 1, unit = view.unit || '';
  const step = view.step || (spec.type === 'integer' ? 1 : (spec.maximum - spec.minimum) / 100);
  const digits = step >= 1 ? 0 : step >= 0.1 ? 1 : step >= 0.01 ? 2 : 3;
  const box = document.createElement('div'); box.className = 'slider';
  const range = document.createElement('input'); range.type = 'range';
  range.min = spec.minimum * scale; range.max = spec.maximum * scale; range.step = step;
  const readout = document.createElement('output');
  const label = shown => { readout.textContent = Number(shown).toFixed(digits) + unit; };
  range.show = stored => { range.value = stored * scale; label(stored * scale); };
  range.show(value);
  let sending, holding;
  range.oninput = () => {
    label(range.value);
    editing.add(key);
    clearTimeout(sending); clearTimeout(holding);
    // Applied while dragging, so the change shows in the headset straight away.
    sending = setTimeout(() => {
      const stored = Number(range.value) / scale;
      apply(spec.type === 'integer' ? Math.round(stored) : Number(stored.toFixed(6)));
    }, 200);
    holding = setTimeout(() => editing.delete(key), 1500);
  };
  box.append(range, readout);
  return [box, range];
}
function field(section, name, spec, value) {
  const key = section + '.' + name;
  const [title, detail] = friendly[key] || [words(name.replace(/^bind_/, '')), ''];
  const line = document.createElement('div');
  line.className = 'line';
  const text = document.createElement('div');
  const b = document.createElement('b'); b.textContent = title; text.append(b);
  if (detail) { const s = document.createElement('span'); s.textContent = detail; text.append(s); }
  line.append(text);
  const apply = v => send('rpc', { request: { method: 'settings.set', key, value: v } });
  let control;
  if (spec.type === 'boolean') {
    const wrap = document.createElement('label'); wrap.className = 'switch';
    control = document.createElement('input'); control.type = 'checkbox'; control.checked = !!value;
    control.onchange = () => apply(control.checked);
    wrap.append(control, document.createElement('i')); line.append(wrap);
  } else if (spec.enum) {
    control = document.createElement('select');
    spec.enum.forEach(option => {
      const item = document.createElement('option');
      item.value = JSON.stringify(option);
      item.textContent = typeof option === 'string' ? words(option) : option;
      control.append(item);
    });
    control.value = JSON.stringify(value);
    control.onchange = () => apply(JSON.parse(control.value));
    line.append(control);
  } else if ((spec.type === 'number' || spec.type === 'integer') && spec.minimum !== undefined &&
             spec.maximum !== undefined && section !== 'connection') {
    const [box, range] = slider(key, spec, value, apply);
    control = range;
    line.append(box);
  } else {
    control = document.createElement('input');
    const numeric = spec.type === 'number' || spec.type === 'integer';
    control.type = numeric ? 'number' : 'text';
    if (numeric) {
      control.step = spec.type === 'integer' ? 1 : 'any';
      if (spec.minimum !== undefined) control.min = spec.minimum;
      if (spec.maximum !== undefined) control.max = spec.maximum;
    }
    control.value = value;
    control.onfocus = () => editing.add(key);
    control.onblur = () => editing.delete(key);
    control.onchange = () => {
      const number = Number(control.value);
      apply(control.value !== '' && !Number.isNaN(number) ? number : control.value);
    };
    line.append(control);
  }
  control.dataset.key = key;
  return line;
}
function buildSettings(values, schema, supported) {
  const groups = new Map();
  for (const [section, sectionSpec] of Object.entries(schema.properties))
    for (const [name, spec] of Object.entries(sectionSpec.properties)) {
      const key = section + '.' + name;
      if (!supported.includes(key) || key === 'video.preset' || key === 'video.adaptive') continue;
      const group = groupNames[section] || words(section);
      if (!groups.has(group)) groups.set(group, []);
      groups.get(group).push(field(section, name, spec.anyOf ? { type: 'text' } : spec, values[section][name]));
    }
  const box = $('groups');
  box.replaceChildren();
  for (const [title, lines] of groups) {
    const heading = document.createElement('h3');
    heading.textContent = title;
    box.append(heading, ...lines);
  }
}
let profilesShown = '';
function renderProfiles() {
  const list = state.profiles;
  if (!list || JSON.stringify(list) === profilesShown) return;
  profilesShown = JSON.stringify(list);
  const box = $('profiles');
  box.replaceChildren();
  const add = (name, saved) => {
    const button = document.createElement('button');
    const title = document.createElement('b'); title.textContent = name;
    const detail = document.createElement('span'); detail.textContent = saved ? 'Saved by you' : 'Starting point';
    button.append(title, detail);
    button.onclick = () => send('rpc', { request: { method: 'profiles.apply', name } });
    if (saved) button.oncontextmenu = event => {
      event.preventDefault();
      send('rpc', { request: { method: 'profiles.delete', name } });
    };
    box.append(button);
  };
  list.built_in.forEach(name => add(name, false));
  list.saved.forEach(name => add(name, true));
}
function renderSettings() {
  renderProfiles();
  const settings = state.settings;
  const enabled = !!settings;
  document.querySelectorAll('#settings button, #settings input, #settings select').forEach(c => { c.disabled = !enabled; });
  if (!settings) return;
  if (!built) { buildSettings(settings.values, settings.schema, state.supported || []); built = true; }
  document.querySelectorAll('#quality button').forEach(b => b.classList.toggle('on', b.dataset.preset === settings.values.video.preset));
  $('adaptive').checked = settings.values.video.adaptive;
  document.querySelectorAll('#groups [data-key]').forEach(control => {
    const key = control.dataset.key;
    if (editing.has(key) || (control.type !== 'range' && document.activeElement === control)) return;
    const [section, name] = key.split('.');
    const value = settings.values[section][name];
    if (control.type === 'checkbox') control.checked = !!value;
    else if (control.tagName === 'SELECT') control.value = JSON.stringify(value);
    else if (control.show) control.show(value);
    else control.value = value;
  });
}
if (host) host.addEventListener('message', event => {
  const message = event.data;
  if (message.type === 'toast') { toast(message.text); return; }
  if (message.type !== 'state') return;
  state = message;
  renderHome(); renderPhone(); renderSettings();
});
document.querySelectorAll('#quality button').forEach(button => button.addEventListener('click', () =>
  send('rpc', { request: { method: 'settings.set', key: 'video.preset', value: button.dataset.preset } })));
$('adaptive').onchange = () => send('rpc', { request: { method: 'settings.set', key: 'video.adaptive', value: $('adaptive').checked } });
$('approve').onclick = () => send('rpc', { request: { method: 'pairing.answer', phone: shownPhone, allow: true } });
$('refuse').onclick = () => send('rpc', { request: { method: 'pairing.answer', phone: shownPhone, allow: false } });
$('saveProfile').onclick = () => {
  const name = $('profileName').value.trim();
  if (name) { send('rpc', { request: { method: 'profiles.save', name } }); $('profileName').value = ''; }
};
$('recenter').onclick = () => {
  send('rpc', { request: { method: 'recenter' } });
  toast('View recentered. This fixes left and right only.');
};
$('debugHelp').onclick = () => { $('debugSteps').hidden = !$('debugSteps').hidden; };
$('restore').onclick = event => {
  if (event.target.dataset.sure) {
    send('rpc', { request: { method: 'settings.reset' } });
    event.target.textContent = 'Restore defaults'; delete event.target.dataset.sure;
  } else { event.target.dataset.sure = '1'; event.target.textContent = 'Click again to restore all settings'; }
};
for (const [id, action] of Object.entries({
  controllers: 'open.input', installApk: 'phone.install', firewall: 'firewall.repair',
  restartVr: 'steamvr.restart',
  logs: 'open.logs', diag: 'diagnostics.save',
})) $(id).onclick = () => send(action);
send('ready');
