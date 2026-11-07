// SPDX-License-Identifier: GPL-3.0-only
// The host's web page (src/web/web_server.cpp serves it and the API under /api). Plain JavaScript, no build step.
// Every value from the host goes into the page as text (textContent), never as HTML.
'use strict';

const $ = (id) => document.getElementById(id);
let currentTab = 'overview';
let pollTimer = null;

class HttpError extends Error {
  constructor(status, body) {
    super((body && body.error) || `HTTP ${status}`);
    this.status = status;
    this.body = body;
  }
}

// The header tells the host the request comes from this page: another site can't send it without asking first.
async function api(path, body) {
  const options = { credentials: 'same-origin', headers: { 'X-Prospero': '1' } };
  if (body !== undefined) {
    options.method = 'POST';
    options.headers['Content-Type'] = 'application/json';
    options.body = JSON.stringify(body);
  }
  const response = await fetch(path, options);
  const type = response.headers.get('Content-Type') || '';
  const data = type.includes('json') ? await response.json() : await response.text();
  if (response.status === 401 && path !== '/api/login') {
    showLogin();
    throw new HttpError(401, data);
  }
  if (!response.ok) throw new HttpError(response.status, data);
  return data;
}

function el(tag, props = {}, ...children) {
  const node = document.createElement(tag);
  for (const [key, value] of Object.entries(props)) {
    if (key === 'class') node.className = value;
    else if (key === 'text') node.textContent = value;
    else if (key.startsWith('on')) node.addEventListener(key.slice(2), value);
    else node.setAttribute(key, value);
  }
  for (const child of children) if (child != null) node.append(child);
  return node;
}

let noticeTimer = null;
// Shown at the top of the window, wherever the page is scrolled to; a tap closes it.
function notice(text, bad = false, sticky = false) {
  const box = $('notice');
  box.textContent = text;
  box.classList.toggle('bad', bad);
  box.hidden = false;
  clearTimeout(noticeTimer);
  if (!sticky) noticeTimer = setTimeout(() => { box.hidden = true; }, bad ? 8000 : 5000);
}
$('notice').addEventListener('click', () => { $('notice').hidden = true; });

function failed(error) {
  if (error instanceof HttpError && error.status === 401) return;
  notice(error.message || String(error), true);
}

// Login.
function showLogin() {
  stopPolling();
  $('app').hidden = true;
  $('login').hidden = false;
  $('login-user').focus();
}

function showApp() {
  $('login').hidden = true;
  $('app').hidden = false;
  selectTab(currentTab);
}

$('login-form').addEventListener('submit', async (event) => {
  event.preventDefault();
  const error = $('login-error');
  error.textContent = '';
  const button = event.submitter || $('login-form').querySelector('button');
  button.disabled = true;
  try {
    await api('/api/login', { user: $('login-user').value, password: $('login-password').value,
      remember: $('login-remember').checked });
    $('login-password').value = '';
    showApp();
    api('/api/session').then((s) => showPending(s.settings_pending), () => {});
  } catch (e) {
    error.textContent = e.body && e.body.wait ? `Too many failed logins. Try again in ${e.body.wait} s.` : e.message;
  } finally {
    button.disabled = false;
  }
});

$('logout').addEventListener('click', async () => {
  try { await api('/api/logout', {}); } catch (e) { /* signed out anyway */ }
  showLogin();
});

// Tabs.
function selectTab(name) {
  currentTab = name;
  for (const button of document.querySelectorAll('.tabs button')) {
    button.setAttribute('aria-selected', String(button.dataset.tab === name));
  }
  for (const section of document.querySelectorAll('.tab')) section.hidden = section.id !== `tab-${name}`;
  if (name !== 'overview') $('risk').hidden = true;  // the risky setup notice is Overview's; Devices says it per device
  stopPolling();
  if (name === 'overview' || name === 'devices') startPolling();
  else if (name === 'settings') loadSettings();
  else if (name === 'logs') loadLog();
}

for (const button of document.querySelectorAll('.tabs button')) {
  button.addEventListener('click', () => selectTab(button.dataset.tab));
}

function startPolling() {
  refresh();
  pollTimer = setInterval(refresh, 2000);
}

function stopPolling() {
  clearInterval(pollTimer);
  pollTimer = null;
}

// Overview and devices: one request gives both.
let refreshing = false;
async function refresh() {
  if (refreshing || document.hidden) return;
  refreshing = true;
  try {
    const data = await api('/api/overview');
    renderHost(data.host, data.status);
    showPending(data.host.settings_pending);
    renderStatus(data.host, data.status);
    renderPairings(data.pairings);
    renderStreams(data.streams);
    lastDevices = data.devices;
    const trusted = data.trusted || [];
    renderRisk(data.devices, trusted, data.risk_ack || []);
    // The names of the profiles a device plays as: asked for once, they show from the next refresh on.
    if (!consoleUsers && data.devices.some((d) => d.profiles && !['free', 'none'].includes(d.profiles))) loadUsers().catch(() => {});
    renderDevices(data.devices, data.streams, trusted);
  } catch (e) {
    failed(e);
  } finally {
    refreshing = false;
  }
}

function renderHost(host, status) {
  $('host-name').textContent = host.name || 'ProsperoSunshine';
  $('host-facts').textContent = `ProsperoSunshine ${host.version} · firmware ${host.firmware}`;
  document.title = `${host.name || 'ProsperoSunshine'} · ProsperoSunshine`;
}

function renderStatus(host, status) {
  const facts = [
    ['Stream', status.running === '1' ? `running${status.client ? ` to ${status.client}` : ''}` : 'none'],
    ['App', status.app || '–'],
    ['Pending pairings', status.pending || '0'],
    ['Version', `${host.version} (${host.commit})`],
    ['Firmware', host.firmware],
    ['Base port', String(host.port)],
  ];
  $('status').replaceChildren(...facts.flatMap(([key, value]) => [el('dt', { text: key }), el('dd', { text: value })]));
}

function renderPairings(pairings) {
  const list = $('pairings');
  if (!pairings.length) {
    list.replaceChildren(el('li', { class: 'empty', text: 'No device is waiting for a PIN.' }));
    return;
  }
  list.replaceChildren(...pairings.map((p) => el('li', {},
    el('div', { class: 'grow' },
      el('div', { class: 'title', text: p.name || 'Moonlight' }),
      el('div', { class: 'sub', text: p.address || '' })),
    el('span', { class: 'badge', text: 'waiting for PIN' }),
    el('button', { class: 'danger', text: 'Deny', onclick: () => denyPairing(p) }))));
  // With several waiting, the PIN goes to the one picked here.
  if (pairings.length > 1) {
    $('pin-form').dataset.ids = JSON.stringify(pairings.map((p) => p.id));
  } else {
    delete $('pin-form').dataset.ids;
  }
}

function renderStreams(streams) {
  const list = $('streams');
  if (!streams.length) {
    list.replaceChildren(el('li', { class: 'empty', text: 'Nothing is streaming.' }));
    return;
  }
  list.replaceChildren(...streams.map((s) => {
    const parts = [s.address, s.codec, s.hdr === '1' ? 'HDR' : null, s.mode, s.kbps !== '-' ? `${s.kbps} kbps` : null,
      sending(s), `${s.pads} pad${s.pads === '1' ? '' : 's'}`, duration(s.secs)].filter(Boolean);
    // A paired device's name leads to it in Devices, where its profiles are, without having to recall what it was named.
    const title = s.uuid
      ? el('button', { type: 'button', class: 'title link', text: s.name || s.address, title: 'Show this device in Devices',
        onclick: () => showDevice(s.uuid) })
      : el('div', { class: 'title', text: s.name || s.address });
    return el('li', {},
      el('div', { class: 'grow' },
        title,
        el('div', { class: 'sub' }, ...parts.flatMap((part, i) => (i ? [' · ', part] : [part])), latency(s.rtt_ms))),
      el('span', { class: 'badge live', text: s.role === 'joined' ? 'joined' : 'live' }),
      s.uuid ? el('button', { class: 'danger', text: 'End stream', onclick: () => endStream(s) }) : null);
  }));
}

// The round trip, coloured as cloud gaming services rate it: below 40 ms good, up to 80 ms playable, above that poor.
function latency(rtt) {
  const ms = Number(rtt);
  if (!rtt || rtt === '-' || !Number.isFinite(ms)) return null;
  const rating = ms < 40 ? 'good' : ms <= 80 ? 'fair' : 'poor';
  return el('span', { class: `latency ${rating}`, title: 'Round trip to the device', text: ` · ${ms} ms` });
}

// What goes out against what the stream asked for: from 85 % good, from 60 % fair, below that poor. No colour in the first
// seconds, while the rate is still settling, or without both values.
function sending(s) {
  if (!s.sent_kbps || s.sent_kbps === '-') return null;
  const sent = Number(s.sent_kbps), asked = Number(s.kbps);
  const text = `sending ${s.sent_kbps} kbps`;
  if (!Number.isFinite(sent) || !(asked > 0) || s.role === 'starting' || !(Number(s.secs) >= 10)) return text;
  const share = sent / asked;
  const rating = share >= 0.85 ? 'good' : share >= 0.6 ? 'fair' : 'poor';
  return el('span', { class: `rate ${rating}`, text,
    title: `${Math.round(share * 100)} % of the ${asked} kbps asked for. A still picture needs less, so a low share there is no fault.` });
}

function duration(secs) {
  const total = Number(secs) || 0;
  const h = Math.floor(total / 3600), m = Math.floor((total % 3600) / 60), s = total % 60;
  return h ? `${h} h ${m} min` : m ? `${m} min ${s} s` : `${s} s`;
}

// Devices whose badge explanation is open, kept while the list refreshes.
const whyOpen = new Set();

// The device a stream card led to: Devices scrolls to it and marks it for a moment, also across a refresh.
let highlight = { uuid: null, until: 0 };

function showDevice(uuid) {
  highlight = { uuid, until: Date.now() + 2500 };
  selectTab('devices');
  const item = [...$('devices').children].find((li) => li.dataset.uuid === uuid);
  if (!item) return;
  item.classList.add('flash');
  item.scrollIntoView({ block: 'center', behavior: 'smooth' });
  setTimeout(() => item.classList.remove('flash'), 2500);
}

// Why a device is risky or trusted, under it, with the button that trusts it or stops trusting it.
function explanation(d, profiles) {
  // Only a device that can play as any profile can be trusted; a spectator's badge just explains.
  const box = el('div', { class: `why ${profiles.badgeClass}` },
    el('span', { text: profiles.why }),
    profiles.trusted === undefined ? null : el('button', { type: 'button', class: profiles.trusted ? 'ghost' : 'primary',
      text: profiles.trusted ? 'Stop trusting' : 'I trust this device', onclick: () => setTrust(d, !profiles.trusted) }));
  box.hidden = !whyOpen.has(d.uuid);
  return box;
}

async function setTrust(d, trusted) {
  const name = d.name || d.uuid;
  if (trusted && !confirm(`Trust ${name}? It keeps playing as any profile on the console, yours included. Devices shows it `
    + 'as Trusted, and the risky setup notice leaves it out.')) return;
  try {
    await api('/api/devices/trust', { uuids: [d.uuid], trusted });
    notice(trusted ? `${name} is trusted.` : `${name} is no longer trusted.`);
    refresh();
  } catch (e) { failed(e); }
}

// A badge that explains itself: a tap shows or hides the reason under the device.
function explainedBadge(d, profiles, why) {
  const badge = el('button', { type: 'button', class: `badge ${profiles.badgeClass}`, text: profiles.badge,
    title: 'Why?', 'aria-expanded': String(!why.hidden) });
  badge.addEventListener('click', () => {
    why.hidden = !why.hidden;
    badge.setAttribute('aria-expanded', String(!why.hidden));
    if (why.hidden) whyOpen.delete(d.uuid); else whyOpen.add(d.uuid);
  });
  return badge;
}

function renderDevices(devices, streams = [], trusted = []) {
  const list = $('devices');
  if (!devices.length) {
    list.replaceChildren(el('li', { class: 'empty', text: 'No device is paired yet.' }));
    return;
  }
  // A rename in progress keeps its field while the list refreshes.
  if (list.querySelector('input:focus')) return;
  list.replaceChildren(...devices.map((d) => {
    const stream = streams.find((s) => s.uuid === d.uuid);
    const profiles = profilesOf(d.profiles, trusted.includes(d.uuid));
    const why = profiles.why ? explanation(d, profiles) : null;
    const flash = highlight.uuid === d.uuid && Date.now() < highlight.until;
    return el('li', { 'data-uuid': d.uuid, class: flash ? 'flash' : '' },
      el('div', { class: 'grow' },
        el('div', { class: 'title', text: d.name || d.uuid }),
        el('div', { class: 'sub', text: d.mode ? `last mode ${d.mode}` : 'not streamed yet' }),
        el('div', { class: 'sub', text: `Profiles: ${profiles.text}` })),
      why ? explainedBadge(d, profiles, why)
        : profiles.badge ? el('span', { class: `badge ${profiles.badgeClass}`, text: profiles.badge }) : null,
      d.streaming === '1' ? el('span', { class: 'badge live', text: 'streaming' }) : null,
      d.streaming === '1' ? el('button', { class: 'danger', text: 'End stream',
        onclick: () => endStream(stream || { uuid: d.uuid, name: d.name }) }) : null,
      el('button', { text: 'Profiles', onclick: () => editProfiles(d) }),
      el('button', { text: 'Rename', onclick: () => rename(d) }),
      el('button', { class: 'danger', text: 'Unpair', onclick: () => unpair(d) }),
      why);
  }));
}

// The risky setup notice, under the tabs on Overview: devices that can play as any profile on the console, those set to
// "Can choose freely" (every device paired before profiles existed) and those not set yet. Dismiss hides it until the page
// loads again; "Don't show again" makes it stop naming these devices, and nothing else: Devices still marks them Risky, and
// a device that turns risky later brings the notice back. Devices trusted under their badge in Devices are left out too.
let riskDismissed = false;
let noticedRisk = [];

function riskyDevices(devices) {
  return devices.filter((d) => d.profiles == null || d.profiles === 'free');
}

function renderRisk(devices, trusted, acknowledged) {
  const untrusted = riskyDevices(devices).filter((d) => !trusted.includes(d.uuid));
  noticedRisk = untrusted.filter((d) => !acknowledged.includes(d.uuid));
  const names = (list) => list.map((d) => d.name || d.uuid).join(', ');
  const n = noticedRisk.length;
  $('risk-text').textContent = n === 1
    ? `Risky setup: ${names(noticedRisk)} can play as any profile on this console, with its games and saves.`
    : `Risky setup: ${n} paired devices can play as any profile on this console, with its games and saves (${names(noticedRisk)}).`;
  $('risk').hidden = currentTab !== 'overview' || riskDismissed || n === 0;
  const note = $('devices-risk');
  note.hidden = untrusted.length === 0;
  note.textContent = untrusted.length === 1
    ? `${names(untrusted)} can play as any profile on this console. Choose its profiles to limit it, or tap its badge to trust it.`
    : `${untrusted.length} devices can play as any profile on this console (${names(untrusted)}). Choose their profiles to limit them, or tap a badge to trust that device.`;
}

$('risk-review').addEventListener('click', () => selectTab('devices'));
$('risk-dismiss').addEventListener('click', () => {
  riskDismissed = true;
  $('risk').hidden = true;
});
$('risk-never').addEventListener('click', async () => {
  const list = noticedRisk;
  if (!list.length) return;
  if (!confirm(`Stop warning about ${list.length === 1 ? 'this device' : `these ${list.length} devices`} here? `
    + 'They can still play as any profile on the console, with its games and saves, and Devices still marks them Risky. '
    + 'A device that becomes risky later is warned about again.')) return;
  try {
    await api('/api/risk/ack', { uuids: list.map((d) => d.uuid) });
    $('risk').hidden = true;
    notice('This notice won\'t show again for these devices.');
    refresh();
  } catch (e) { failed(e); }
});

// Profiles: which console profiles a paired device may play as (src/control/device_profiles.hpp). The host keeps them as
// text: null (not set: the console asks, as before), "free", "none", or user ids and "guest" separated by commas.
let lastDevices = [];
let consoleUsers = null;  // [{id, name}], asked for once per page load

async function loadUsers() {
  if (!consoleUsers) consoleUsers = (await api('/api/users')).users;
  return consoleUsers;
}

function userName(id) {
  if (!consoleUsers) return `profile ${id}`;
  const user = consoleUsers.find((u) => u.id === id);
  return user ? (user.name || `profile ${id}`) : `profile ${id} (no longer on the console)`;
}

function parseProfiles(text) {
  if (text == null) return { set: false, free: false, users: [], guest: false };
  if (text === 'free') return { set: true, free: true, users: [], guest: false };
  if (text === 'none') return { set: true, free: false, users: [], guest: false };
  const items = text.split(',');
  return { set: true, free: false, users: items.filter((i) => i !== 'guest'), guest: items.includes('guest') };
}

function formatProfiles(p) {
  if (p.free) return 'free';
  const items = [...p.users, ...(p.guest ? ['guest'] : [])];
  return items.length ? items.join(',') : 'none';
}

// The line and badge the device list shows.
function profilesOf(text, trusted = false) {
  const p = parseProfiles(text);
  if (!p.set || p.free) {
    const line = p.set ? 'can choose any profile on the console'
      : 'not set yet, the console asks who uses each controller and any profile can be picked';
    const how = p.set
      ? 'Whoever holds this device\'s controllers can pick any profile on the console, yours included, and load, overwrite or delete its saves.'
      : 'Until its profiles are chosen, this device works as "Can choose freely": the console asks who uses each controller, and any profile can be picked, yours included.';
    if (trusted) return { text: line, badge: 'Trusted', badgeClass: 'trusted', trusted: true, why: `You trust this device. ${how}` };
    return { text: line, badge: p.set ? 'Risky' : 'profiles not set', badgeClass: p.set ? 'risky' : 'unset', trusted: false,
      why: `${how} Choose its profiles to limit it, or trust it if it's yours.` };
  }
  const names = p.users.map(userName);  // a stored guest does nothing yet (Quick Play is coming later)
  if (!names.length) return { text: 'none, it can watch but its controllers don\'t join', badge: 'Spectator', badgeClass: 'spectator',
    why: 'This device can only watch: its controllers don\'t join and its keyboard doesn\'t reach the console. Tick a profile in Profiles so it can play.' };
  return { text: names.join(', '), badge: null };
}

function profilesSummary(form) {
  const free = form.querySelector('input[name="profiles-mode"][value="free"]').checked;
  const ticked = [...form.querySelectorAll('#profiles-list input:checked')];
  const box = $('profiles-summary');
  box.classList.toggle('risky', free);
  $('profiles-list').classList.toggle('off', free);
  // Under a free choice the profiles only look off: ticking one picks "Only these profiles" (editProfiles).
  for (const input of form.querySelectorAll('#profiles-list input')) input.disabled = input.value === 'guest';
  const names = ticked.filter((i) => i.value !== 'guest').map((i) => i.dataset.label);
  if (free) {
    box.textContent = 'Anyone using this device can play as any profile on this console. Choose this only for a device you trust as much as the console itself.';
  } else if (!names.length) {
    box.textContent = 'No profile: this device can watch, but its controllers don\'t join the game.';
  } else {
    box.textContent = `Its controllers join as ${names.join(', then ')}, in the order they are first used (or where each last switched to), `
      + 'without the console asking. A further controller doesn\'t join.';
  }
}

// Opens the profiles dialog for a device; after a pairing, "Decide later" leaves them unset.
async function editProfiles(device, afterPairing = false) {
  let users;
  try { users = await loadUsers(); } catch (e) { failed(e); return; }
  const dialog = $('profiles-dialog');
  const form = $('profiles-form');
  const current = parseProfiles(device.profiles);
  $('profiles-title').textContent = afterPairing ? `${device.name || 'The new device'} is paired` : `Profiles of ${device.name || device.uuid}`;
  $('profiles-text').textContent = afterPairing
    ? 'Which console profiles does this device play as? You can change this later in Devices.'
    : 'Which console profiles does this device play as?';
  // Guest first, but not yet: the host can't add a Quick Play guest without the console's question, and that question
  // lets whoever answers it pick any profile, also one someone at the console is playing on. Kept in view so it's known
  // to be coming; a device's stored "guest" is kept and does nothing until then.
  const guest = el('input', { type: 'checkbox', value: 'guest', disabled: '' });
  guest.dataset.label = 'Guest (Quick Play)';
  guest.checked = false;
  const blank = () => el('span', { class: 'avatar blank', 'aria-hidden': 'true' });
  const rows = [el('label', { class: 'later' }, guest, blank(), el('span', {}, 'Guest (Quick Play) ',
    el('span', { class: 'muted', text: 'a one-time guest, no savegames are kept. Coming later: for now, create up to ' +
      'four guest profiles on the console, one per controller, and tick them below' })))];
  rows.push(...users.map((u) => {
    const input = el('input', { type: 'checkbox', value: u.id });
    input.dataset.label = u.name || `profile ${u.id}`;
    input.checked = current.users.includes(u.id);
    // One step, not two: a profile picked while "Can choose freely" is on switches to "Only these profiles" with it ticked.
    input.addEventListener('click', () => {
      if (!form.querySelector('input[name="profiles-mode"][value="free"]').checked) return;
      input.checked = true;
      form.querySelector('input[name="profiles-mode"][value="list"]').checked = true;
      profilesSummary(form);
    });
    const picture = el('img', { class: 'avatar', src: `/api/users/${u.id}/picture`, alt: '', loading: 'lazy',
      onerror: () => picture.replaceWith(blank()) });
    return el('label', {}, input, picture, el('span', { text: u.name || `profile ${u.id}` }));
  }));
  $('profiles-list').replaceChildren(...rows);
  form.querySelector(`input[name="profiles-mode"][value="${current.free ? 'free' : 'list'}"]`).checked = true;
  $('profiles-later').textContent = afterPairing ? 'Decide later' : 'Cancel';
  profilesSummary(form);
  form.onchange = () => profilesSummary(form);
  dialog.returnValue = '';
  const answer = await new Promise((resolve) => {
    dialog.addEventListener('close', () => resolve(dialog.returnValue), { once: true });
    dialog.showModal();
  });
  if (answer !== 'save') {
    if (afterPairing) notice('The profiles are not set yet: until then the console asks who uses each controller.');
    return;
  }
  const free = form.querySelector('input[name="profiles-mode"][value="free"]').checked;
  const ticked = [...form.querySelectorAll('#profiles-list input:checked')].map((i) => i.value);
  const profiles = formatProfiles({ free, users: ticked.filter((v) => v !== 'guest'), guest: ticked.includes('guest') });
  try {
    await api('/api/devices/profiles', { uuid: device.uuid, profiles });
    notice('The profiles were saved.');
    refresh();
  } catch (e) { failed(e); }
}

// Ending a stream lets the device connect again right away; unpairing as well keeps it out until it pairs with a PIN.
// One dialog with three answers: 'unpair', 'end', or null for Cancel (also Esc).
function askEndStream(who) {
  const dialog = $('end-dialog');
  const question = `End the stream to ${who}? The game and other streams go on.`;
  if (typeof dialog.showModal !== 'function') {  // a browser without <dialog>
    if (!confirm(question)) return Promise.resolve(null);
    return Promise.resolve(confirm(`Also unpair ${who}, so it can't connect again without a new PIN?\n\nOK: end and unpair. Cancel: only end the stream.`) ? 'unpair' : 'end');
  }
  $('end-text').textContent = question;
  dialog.returnValue = '';
  return new Promise((resolve) => {
    dialog.addEventListener('close', () => resolve(dialog.returnValue || null), { once: true });
    dialog.showModal();
    $('end-only').focus();
  });
}

async function endStream(stream) {
  const who = stream.name || stream.address || 'this device';
  const choice = await askEndStream(who);
  if (!choice) return;
  const alsoUnpair = choice === 'unpair';
  try {
    if (alsoUnpair) {
      await api('/api/devices/unpair', { uuid: stream.uuid });
      notice('The stream was ended and the device unpaired.');
    } else {
      await api('/api/streams/end', { uuid: stream.uuid });
      notice('The stream was ended.');
    }
    refresh();
  } catch (e) { failed(e); }
}

async function denyPairing(pairing) {
  if (!confirm(`Deny the pairing request from ${pairing.name || 'Moonlight'}${pairing.address ? ` (${pairing.address})` : ''}?`)) return;
  try {
    await api('/api/pin/cancel', { id: pairing.id });
    notice('The pairing request was denied.');
    refresh();
  } catch (e) { failed(e); }
}

async function rename(device) {
  const name = prompt('New name for this device:', device.name);
  if (name === null || !name.trim() || name.trim() === device.name) return;
  try {
    await api('/api/devices/rename', { uuid: device.uuid, name: name.trim() });
    notice('The device was renamed.');
    refresh();
  } catch (e) { failed(e); }
}

async function unpair(device) {
  if (!confirm(`Unpair ${device.name || device.uuid}? A running stream of it ends, and it has to pair again with a PIN to stream.`)) return;
  try {
    await api('/api/devices/unpair', { uuid: device.uuid });
    notice('The device was unpaired.');
    refresh();
  } catch (e) { failed(e); }
}

$('pin-form').addEventListener('submit', async (event) => {
  event.preventDefault();
  const body = { pin: $('pin').value.trim(), name: $('pin-name').value.trim() };
  const ids = $('pin-form').dataset.ids ? JSON.parse($('pin-form').dataset.ids) : null;
  if (ids) {
    const index = Number(prompt(`${ids.length} devices are waiting. Which one (1 to ${ids.length}, in the list's order)?`, '1'));
    if (!(index >= 1 && index <= ids.length)) return;
    body.id = ids[index - 1];
  }
  // The new device is the one that wasn't paired before: its profiles are asked for right away.
  const before = new Set(lastDevices.map((d) => d.uuid));
  try {
    await api('/api/pin', body);
    $('pin').value = '';
    $('pin-name').value = '';
    notice('Paired.');
    const data = await api('/api/overview');
    lastDevices = data.devices;
    refresh();
    const added = data.devices.filter((d) => !before.has(d.uuid));
    if (added.length === 1) editProfiles(added[0], true);
  } catch (e) { failed(e); }
});

// Settings.
let settings = [];
async function loadSettings() {
  try {
    settings = (await api('/api/settings')).options;
    $('settings').replaceChildren(...settings.map(settingRow));
  } catch (e) { failed(e); }
}

function settingRow(option) {
  const id = `setting-${option.key}`;
  let control;
  if (option.kind === 'toggle') {
    control = el('input', { id, type: 'checkbox' });
    control.checked = option.value === '1';
  } else if (option.kind === 'choice') {
    control = el('select', { id },
      el('option', { value: '', text: `Default (${option.default})` }),
      ...option.choices.map((c) => el('option', { value: c.value, text: c.label })));
    // A value written by hand that the list doesn't offer stays as it is unless changed here.
    if (option.value != null && !option.choices.some((c) => c.value === option.value)) {
      control.append(el('option', { value: option.value, text: `${option.value} (set by hand)` }));
    }
    control.value = option.value ?? '';
  } else if (option.kind === 'number') {
    control = el('input', { id, type: 'number', min: option.min, max: option.max, step: option.step,
      placeholder: `Default: ${option.default}` });
    control.value = option.value ?? '';
  } else {
    control = el('input', { id, maxlength: '63', placeholder: `Default: ${option.default}` });
    control.value = option.value ?? '';
  }
  return el('div', { class: 'setting' },
    el('label', { for: id, class: 'title', text: option.label }),
    el('div', { class: 'help', text: option.help }),
    el('div', { class: 'control' }, control,
      option.kind === 'toggle' ? el('span', { class: 'default', text: `Default: ${option.default}` }) : null));
}

function settingValues() {
  const values = {};
  for (const option of settings) {
    const control = $(`setting-${option.key}`);
    if (option.kind === 'toggle') values[option.key] = control.checked ? '1' : null;
    else values[option.key] = control.value.trim() === '' ? null : control.value.trim();
  }
  return values;
}

async function saveSettings(restartAfter) {
  try {
    await api('/api/settings', { values: settingValues() });
    showPending(true);
    if (restartAfter || confirm('Saved. The host reads its settings when it starts.\n\nRestart the host now to apply them? Running streams end.')) {
      await restartHost(true);
    } else {
      notice('Saved. The settings apply when the host starts again.');
    }
  } catch (e) { failed(e); }
}

// The banner stays while sunshine.conf differs from what the running host read at its start.
function showPending(pending) {
  $('pending').hidden = !pending;
}
$('pending-restart').addEventListener('click', () => restartHost(false));

$('settings-form').addEventListener('submit', (event) => {
  event.preventDefault();
  saveSettings(false);
});
$('save-restart').addEventListener('click', () => saveSettings(true));

// Restart: the host hands a small helper to the ELF loader and stops; once it is gone, the helper starts it again.
async function restartHost(asked) {
  if (!asked && !confirm('Restart the host? Running streams end.')) return;
  try {
    await api('/api/restart', {});
  } catch (e) { failed(e); return; }
  stopPolling();
  notice('The host is restarting…', false, true);
  const started = Date.now();
  await new Promise((resolve) => setTimeout(resolve, 6000));
  while (Date.now() - started < 60000) {
    try {
      await api('/api/session');
      break;
    } catch (e) {
      if (e instanceof HttpError && e.status === 401) return;  // back, with the sessions of the old run gone
      await new Promise((resolve) => setTimeout(resolve, 2000));
    }
  }
  notice('The host is back.');
  selectTab(currentTab);
}
$('restart').addEventListener('click', () => restartHost(false));

// Logs.
async function loadLog() {
  const file = $('log-file').value;
  const path = file === 'report' ? '/api/diagnostics' : `/api/logs?file=${file}`;
  $('log-download').href = path;
  $('log-download').download = { report: 'diagnostics.txt', diag: 'host_diag.log' }[file] || 'sunshine.log';
  $('report-hint').hidden = file !== 'report';
  try {
    const text = await api(path);
    const box = $('log');
    box.textContent = text || '(empty)';
    box.scrollTop = box.scrollHeight;
  } catch (e) { failed(e); }
}
$('log-file').addEventListener('change', loadLog);
$('log-refresh').addEventListener('click', loadLog);

document.addEventListener('visibilitychange', () => { if (!document.hidden && pollTimer) refresh(); });

// Start: signed in already (a cookie from before), or the login.
api('/api/session').then((s) => { showApp(); showPending(s.settings_pending); },
  (e) => { if (!(e instanceof HttpError && e.status === 401)) showLogin(); });
