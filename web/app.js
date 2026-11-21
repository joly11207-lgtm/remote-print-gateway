'use strict';

/**
 * Site controller (V1).
 *
 * One static script for every page: `document.body.dataset.page` decides which
 * initialiser runs. Still no framework and no build step, so GitHub -> Vercel is
 * a file upload.
 *
 * This file only talks to the gateway API and renders what the engine decided.
 * It never decides compatibility itself: no matching, no capability resolution,
 * no lifecycle switching. When the UI needs to know "can I test this?", it reads
 * report.recommended_safe_test.executable from the engine.
 */

// Fallback only: the real gateway (when the deployment has one) is discovered
// through GET /api/gateways. The browser never holds the gateway's LAN address -
// it only ever talks to this origin, which proxies to the ESP32.
const DEFAULT_GATEWAY_ID = 'gw-sim-001';
const DEV_MODE = new URLSearchParams(window.location.search).get('dev') === '1';
// Set by <body data-page="...">. Every page loads this same file.
const PAGE = document.body.dataset.page || 'home';
// <body data-demo="1"> marks a build with no Python engine behind it. Only that
// build may turn a missing API into a friendly demo state; a real deployment
// still reports every failure as an error.
const DEMO_BUILD = document.body.dataset.demo === '1';

// Presentation labels only. The values themselves stay canonical from the API.
const TRANSPORT_LABELS = {
  ble: 'Bluetooth LE',
  bluetooth_classic: 'Bluetooth Classic',
  tcp: 'Wi-Fi / network',
  ipp: 'Wi-Fi (IPP)',
  usb: 'USB',
};

const PROTOCOL_LABELS = {
  tiny_iprint: 'Tiny / iPrint',
  escpos: 'ESC/POS',
  memobird: 'Memobird',
  ipp: 'IPP',
};

const DEVICE_PRESETS = {
  x18: { brand: '爱记星', model: 'X18', app: '爱记星' },
  g2d: { brand: 'MEMOBIRD', model: 'G2D', app: 'Memobird' },
  unknown: { brand: 'XP', model: 'XP-460B', app: '' },
};

const state = {
  gatewayId: DEFAULT_GATEWAY_ID,
  gateway: null,
  caseId: null,
  report: null,
  notice: null,
  device: null,
  demo: false,  // true after the "simulator ready" fallback rendered
  usb: null,    // Web Serial panel: { client, regions, authorized }
};

const $ = (id) => document.getElementById(id);

// --------------------------------------------------------------------------- //
// tiny helpers
// --------------------------------------------------------------------------- //
function h(tag, props, ...children) {
  const node = document.createElement(tag);
  for (const [key, value] of Object.entries(props || {})) {
    if (value === null || value === undefined || value === false) continue;
    if (key === 'class') node.className = value;
    else if (key === 'text') node.textContent = String(value);
    else if (key === 'onclick') node.addEventListener('click', value);
    else node.setAttribute(key, value === true ? '' : String(value));
  }
  for (const child of children.flat()) {
    if (child === null || child === undefined || child === false) continue;
    node.append(child.nodeType ? child : document.createTextNode(String(child)));
  }
  return node;
}

async function api(method, path, body) {
  const options = { method, headers: {} };
  if (body !== undefined) {
    options.headers['Content-Type'] = 'application/json';
    options.body = JSON.stringify(body);
  }
  let response;
  try {
    response = await fetch(path, options);
  } catch (cause) {
    // Static hosting without the gateway service (e.g. a bare Vercel preview):
    // the page must still render, it just cannot ask the engine anything.
    const error = new Error(I18N.t('dyn.error.backendUnavailable'));
    error.code = 'BACKEND_UNAVAILABLE';
    error.status = 0;
    throw error;
  }
  const data = await response.json().catch(() => null);
  if (!response.ok) {
    // A static host answers /api/* with HTML (404 on Vercel, 501 from a plain
    // file server): there is no engine on this origin at all. A real engine
    // always answers JSON, so a non-JSON body is a deployment fact, not a
    // failed request.
    const noBackend = data === null;
    const error = new Error(
      noBackend
        ? I18N.t('dyn.error.noBackend')
        : (data && data.message) || I18N.t('dyn.error.requestFailed', { status: response.status })
    );
    error.code = noBackend ? 'NO_BACKEND' : (data && data.error) || `HTTP_${response.status}`;
    error.status = response.status;
    throw error;
  }
  return data || {};
}

// True only when the absence of an engine explains the failure.
function noBackend(error) {
  return error.code === 'NO_BACKEND' || error.code === 'BACKEND_UNAVAILABLE';
}

function showError(node, error) {
  node.textContent = `${error.message}${error.code ? ` (${error.code})` : ''}`;
  node.hidden = false;
}

function clearError(node) {
  node.textContent = '';
  node.hidden = true;
}

function labelFor(map, value) {
  return map[value] || value || '—';
}

// --------------------------------------------------------------------------- //
// gateway card
// --------------------------------------------------------------------------- //
async function discoverGateway() {
  // Real gateway first: a deployment that points RTB_REAL_GATEWAY_URL at an
  // ESP32 gets real scans without touching this file; a bare dev machine keeps
  // using the simulator adapter.
  const list = await api('GET', '/api/gateways');
  const gateways = (list && list.gateways) || [];
  const chosen =
    gateways.find((item) => item.mode === 'real') ||
    gateways.find((item) => item.gateway_id === state.gatewayId) ||
    gateways[0];
  if (chosen) state.gatewayId = chosen.gateway_id;
  return api('GET', `/api/gateways/${encodeURIComponent(state.gatewayId)}`);
}

async function loadGateway() {
  try {
    const gateway = await discoverGateway();
    state.gateway = gateway;
    state.demo = false;
    setBarStatus(gateway);
    renderGateway(gateway);
  } catch (error) {
    // The demo build has no engine on purpose: say so instead of painting a red
    // request error. Any other deployment keeps the real error path below.
    if (DEMO_BUILD && noBackend(error)) {
      renderDemoGateway();
      return;
    }
    state.gateway = null;
    setBarStatus(null);
    const name = $('gateway-name');
    if (name) {
      // Offline is a Gateway fact, never a printer verdict.
      name.textContent =
        error.code === 'BACKEND_UNAVAILABLE'
          ? I18N.t('dyn.gateway.offlineTitle')
          : I18N.t('dyn.gateway.unavailableTitle');
      showError($('gateway-error'), error);
    }
  }
}

// Static preview: there is no gateway to report on, and pretending otherwise
// would make a demo look like a real installation.
function renderDemoGateway() {
  state.gateway = null;
  state.demo = true;
  setDemoBarStatus();
  const name = $('gateway-name');
  if (!name) return;  // the bar already carries the state

  name.textContent = I18N.t('dyn.gateway.demoTitle');
  $('gateway-place').textContent = I18N.t('dyn.gateway.demoPlace');

  const badge = $('gateway-status');
  badge.textContent = I18N.t('dyn.gateway.demoBadge');
  badge.className = 'badge is-demo';

  clearError($('gateway-error'));

  $('gateway-facts').replaceChildren(
    fact(I18N.t('dyn.fact.firmware'), I18N.t('dyn.gateway.demoFirmware')),
    fact(I18N.t('dyn.fact.hardware'), I18N.t('dyn.gateway.demoHardware')),
    fact(I18N.t('dyn.fact.lastSeen'), '—'),
    fact(I18N.t('dyn.fact.gatewayId'), 'demo')
  );
  $('gateway-caps').replaceChildren(
    h('span', { class: 'chip', text: I18N.t('dyn.gateway.demoChip') }),
    h('span', { class: 'chip', text: I18N.t('dyn.gateway.demoChip2') })
  );
}

function setDemoBarStatus() {
  const node = $('bar-gateway');
  if (!node) return;
  node.textContent = I18N.t('bar.demo');
  node.className = 'bar-status is-demo';
}

function setBarStatus(gateway) {
  const node = $('bar-gateway');
  if (!node) return;

  // A live USB link is a real board, so it wins over everything the API says
  // about the (simulated) deployment gateway. An authorised-but-closed port
  // never reaches this branch: only an open port sets state 'connected'.
  const usb = state.usb ? usbSnapshot() : null;
  if (usb && usb.state === 'connected') {
    node.textContent = I18N.t('bar.usb');
    node.className = 'bar-status is-usb';
    return;
  }

  const online = Boolean(gateway) && gateway.status === 'online';
  // Never let a simulator look like a real board: the bar says so explicitly.
  const simulated = Boolean(gateway) && gateway.mode !== 'real';
  const status = I18N.t(online ? 'dyn.gateway.online' : 'dyn.gateway.offline');
  node.textContent = simulated
    ? I18N.t('bar.simulator', { status })
    : I18N.t('bar.gateway', { status });
  node.className = `bar-status is-${online ? 'online' : 'offline'}`;
}

function renderGateway(gateway) {
  const name = $('gateway-name');
  if (!name) return;  // this page shows the gateway in the bar only

  name.textContent = gateway.name;
  $('gateway-place').textContent = gateway.hardware_family;

  const badge = $('gateway-status');
  badge.textContent = I18N.t(
    gateway.status === 'online' ? 'dyn.gateway.online' : 'dyn.gateway.offline'
  );
  badge.className = `badge is-${gateway.status === 'online' ? 'online' : 'offline'}`;

  if (gateway.reachable === false) {
    showError($('gateway-error'), {
      message: I18N.t('dyn.gateway.offlineMessage'),
      code: gateway.error,
    });
  } else {
    clearError($('gateway-error'));
  }

  const facts = $('gateway-facts');
  facts.replaceChildren(
    fact(I18N.t('dyn.fact.firmware'), gateway.firmware_version),
    fact(I18N.t('dyn.fact.hardware'), gateway.hardware_family),
    fact(I18N.t('dyn.fact.lastSeen'), formatTime(gateway.last_seen)),
    fact(I18N.t('dyn.fact.gatewayId'), gateway.gateway_id)
  );

  const chips = $('gateway-caps');
  chips.replaceChildren(
    ...gateway.supported_transports.map((value) =>
      h('span', { class: 'chip', text: labelFor(TRANSPORT_LABELS, value) })
    ),
    ...gateway.supported_protocols.map((value) =>
      h('span', { class: 'chip', text: labelFor(PROTOCOL_LABELS, value) })
    )
  );
  if (!chips.children.length) {
    chips.append(h('span', { class: 'chip', text: I18N.t('dyn.gateway.noCaps') }));
  }
}

function fact(term, value) {
  return h('div', null, h('dt', { text: term }), h('dd', { text: value || '—' }));
}

function formatTime(iso) {
  if (!iso) return '—';
  const date = new Date(iso);
  if (Number.isNaN(date.getTime())) return iso;
  return date.toLocaleString(I18N.lang, {
    year: 'numeric',
    month: 'short',
    day: '2-digit',
    hour: '2-digit',
    minute: '2-digit',
  });
}

// --------------------------------------------------------------------------- //
// USB gateway (Web Serial identity handshake, read-only)
// --------------------------------------------------------------------------- //
// serial.js owns the transport and its state machine. This section only maps
// that state to localised copy and buttons: no compatibility decision is made
// here, and V1.1 never scans a printer over USB.
const USB_STATE_KEYS = {
  unsupported: 'usb.state.unsupported',
  disconnected: 'usb.state.disconnected',
  selecting: 'usb.state.selecting',
  opening: 'usb.state.opening',
  connected: 'usb.state.connected',
  error: 'usb.state.error',
};

// Transport error code -> user copy. Codes stay stable; the words do not.
const USB_ERROR_KEYS = {
  unsupported: 'usb.error.unsupported',
  permission_denied: 'usb.error.permission',
  request_failed: 'usb.error.open',
  open_failed: 'usb.error.open',
  busy: 'usb.error.busy',
  timeout: 'usb.error.timeout',
  invalid_response: 'usb.error.badResponse',
  wrong_type: 'usb.error.badResponse',
  gateway_refused: 'usb.error.badResponse',
  device_lost: 'usb.error.deviceLost',
};

function usbClient() {
  if (!state.usb) {
    const Serial = typeof WebSerialGatewayClient === 'function' ? WebSerialGatewayClient : null;
    state.usb = {
      // No serial.js on this page (or no Web Serial in this browser): the
      // panel still renders, as "unsupported", instead of throwing.
      client: Serial ? new Serial() : null,
      regions: Array.from(document.querySelectorAll('[data-usb-region]')),
      authorized: false,
    };
  }
  return state.usb;
}

function usbSnapshot() {
  const usb = state && state.usb;
  if (usb && usb.client) return usb.client.snapshot();
  return { supported: false, state: 'unsupported', info: null, error: null };
}

function usbNode(region, name) {
  return region.querySelector(`[data-usb="${name}"]`);
}

function renderUsb() {
  const usb = state.usb;
  if (!usb || !usb.regions.length) return;

  const snap = usbSnapshot();
  const busy = snap.state === 'selecting' || snap.state === 'opening';
  const connected = snap.state === 'connected';

  for (const region of usb.regions) {
    // A page may word the idle/connected states its own way (/lab says "No USB
    // Gateway connected." where /setup says "Gateway not connected").
    const idleKey = region.getAttribute('data-usb-idle-label');
    const connectedKey = region.getAttribute('data-usb-connected-label');

    const stateNode = usbNode(region, 'state');
    if (stateNode) {
      const key =
        snap.state === 'disconnected' && idleKey
          ? idleKey
          : USB_STATE_KEYS[snap.state] || 'usb.state.disconnected';
      stateNode.textContent = I18N.t(key);
    }

    const badge = usbNode(region, 'badge');
    if (badge) {
      const key = connected
        ? connectedKey || 'usb.badge.connected'
        : 'usb.badge.disconnected';
      badge.textContent = I18N.t(key);
      badge.className = `badge is-${connected ? 'online' : 'demo'}`;
    }

    const facts = usbNode(region, 'facts');
    if (facts) {
      const info = snap.info;
      if (info) {
        facts.replaceChildren(
          fact(I18N.t('usb.fact.gatewayId'), info.gateway_id),
          fact(I18N.t('usb.fact.hardware'), info.hardware_family),
          fact(I18N.t('usb.fact.firmware'), info.firmware_version),
          fact(I18N.t('usb.fact.connection'), I18N.t('usb.connection.usb'))
        );
        facts.hidden = false;
      } else {
        facts.replaceChildren();
        facts.hidden = true;
      }
    }

    const error = usbNode(region, 'error');
    if (error) {
      if (snap.state === 'unsupported') {
        error.textContent = I18N.t('usb.error.unsupported');
        error.hidden = false;
      } else if (snap.state === 'error' && snap.error) {
        error.textContent = I18N.t(USB_ERROR_KEYS[snap.error.code] || 'usb.error.open');
        error.hidden = false;
      } else {
        clearError(error);
      }
    }

    const hint = usbNode(region, 'hint');
    if (hint) hint.hidden = !(usb.authorized && !connected);

    const connect = region.querySelector('[data-usb-action="connect"]');
    if (connect) {
      connect.hidden = connected;
      connect.disabled = busy || snap.state === 'unsupported';
      connect.setAttribute('aria-busy', busy ? 'true' : 'false');
    }
    const disconnect = region.querySelector('[data-usb-action="disconnect"]');
    if (disconnect) {
      disconnect.hidden = !connected;
      disconnect.disabled = busy;
    }
  }
}

function initUsb() {
  const usb = usbClient();
  if (!usb.regions.length) return;  // this page has no USB card

  for (const region of usb.regions) {
    const connect = region.querySelector('[data-usb-action="connect"]');
    if (connect) connect.addEventListener('click', connectUsb);
    const disconnect = region.querySelector('[data-usb-action="disconnect"]');
    if (disconnect) disconnect.addEventListener('click', disconnectUsb);
  }

  if (!usb.client) {
    renderUsb();  // the snapshot already reads "unsupported"
    return;
  }

  usb.client.onState(() => {
    renderUsb();
    setBarStatus(state.gateway);
  });

  // A port the user authorised on an earlier visit is a hint only: V1.1 always
  // requires a click, it never opens a port on page load.
  usb.client.hasAuthorizedPort().then((authorized) => {
    usb.authorized = authorized;
    renderUsb();
  });

  // Leaving the page must release the COM port, not strand a reader on it.
  window.addEventListener('pagehide', () => {
    if (state.usb && state.usb.client) state.usb.client.disconnect();
  });

  renderUsb();
}

// Only a real click reaches requestPort(): the transport calls it as its first
// await, so the user gesture is still valid when the picker opens.
async function connectUsb(event) {
  const button = event.currentTarget;
  const usb = usbClient();
  if (!usb.client) return;
  button.disabled = true;
  try {
    await usb.client.connect();
  } catch (error) {
    // The transport already recorded a stable code; renderUsb() localises it.
  } finally {
    button.disabled = false;
    renderUsb();
    setBarStatus(state.gateway);
  }
}

async function disconnectUsb(event) {
  const button = event.currentTarget;
  const usb = usbClient();
  if (!usb.client) return;
  button.disabled = true;
  try {
    await usb.client.disconnect();
  } finally {
    button.disabled = false;
    renderUsb();
    setBarStatus(state.gateway);
  }
}

// --------------------------------------------------------------------------- //
// flow: add printer -> rescue form -> scan
// --------------------------------------------------------------------------- //
function bindDeviceButtons() {
  document.querySelectorAll('[data-device]').forEach((button) => {
    button.addEventListener('click', () => selectDevice(button.dataset.device));
  });
}

function initAdd() {
  $('btn-not-listed').addEventListener('click', () => {
    $('add-printer-card').hidden = true;
    $('rescue-card').hidden = false;
    $('field-brand').focus();
  });

  $('rescue-form').addEventListener('submit', startScan);
  $('btn-not-listed').disabled = false;
  bindDeviceButtons();

  // This page *is* the add-printer entry point: show the choices immediately.
  $('add-printer-card').hidden = false;
  if (DEV_MODE) {
    $('dev-devices').hidden = false;
    document.body.classList.add('dev-mode');
  }
}

// Lab workbench: same engine calls as the consumer flow, but the result is read
// as evidence (fingerprint + advanced details) instead of a single verdict.
function initLab() {
  bindDeviceButtons();
  $('btn-analyze').addEventListener('click', analyzeInLab);
  initUsb();
}

async function analyzeInLab(event) {
  const button = event.currentTarget;
  clearError($('form-error'));
  $('result').replaceChildren();
  button.disabled = true;
  try {
    const created = await api('POST', '/api/compatibility/cases', buildCaseBody());
    state.caseId = created.case_id;
    const report = await api(
      'POST',
      `/api/compatibility/cases/${encodeURIComponent(state.caseId)}/scan`
    );
    state.report = report;
    renderResult(report);
  } catch (error) {
    renderScanError(error);
  } finally {
    button.disabled = false;
  }
}

// Pages whose content is static markup. They share the gateway bar, which
// loadGateway() fills in for every page.
function initStaticPage() {}

// Setup: documentation steps plus the one real operation (USB identity).
function initSetup() {
  initUsb();
}

function selectDevice(key) {
  state.device = state.device === key ? null : key;
  document.querySelectorAll('[data-device]').forEach((button) => {
    button.classList.toggle('is-active', button.dataset.device === state.device);
  });
  const preset = DEVICE_PRESETS[key];
  if (state.device && preset) {
    $('field-brand').value = preset.brand;
    $('field-model').value = preset.model;
    $('field-app').value = preset.app;
  }
}

function buildCaseBody() {
  const connection = document.querySelector('input[name="connection"]:checked');
  const body = {
    gateway_id: state.gatewayId,
    brand: $('field-brand').value.trim(),
    model: $('field-model').value.trim(),
    original_app: $('field-app').value.trim(),
    // The user's own words. The gateway fingerprint, not this, decides transport.
    connection_hint: connection ? connection.value : 'unknown',
  };
  // device_key is a simulator selector. A real gateway must identify the device
  // itself, so the value is only sent while the simulator is in use.
  const simulator = !state.gateway || state.gateway.mode !== 'real';
  if (state.device && simulator) body.device_key = state.device;
  return body;
}

async function startScan(event) {
  event.preventDefault();
  clearError($('form-error'));
  $('result').replaceChildren();
  $('btn-scan').disabled = true;
  startProgress();

  try {
    const created = await api('POST', '/api/compatibility/cases', buildCaseBody());
    state.caseId = created.case_id;
    await scanExistingCase();
  } catch (error) {
    stopProgress();
    renderScanError(error);
  } finally {
    $('btn-scan').disabled = false;
  }
}

// Retry keeps the same CREATED case: an offline gateway or a printer that was
// switched off must not burn a case (and must never create a PrinterInstance).
async function scanExistingCase() {
  if (!state.caseId) return;
  const report = await api(
    'POST',
    `/api/compatibility/cases/${encodeURIComponent(state.caseId)}/scan`
  );
  state.report = report;
  finishProgress();
  renderResult(report);
}

function retryScan(button) {
  button.disabled = true;
  clearError($('form-error'));
  $('result').replaceChildren();
  startProgress();
  scanExistingCase()
    .catch((error) => {
      stopProgress();
      renderScanError(error);
    })
    .finally(() => {
      button.disabled = false;
    });
}

function renderScanError(error) {
  // Same rule as the gateway card: only the demo build may soften a missing
  // engine. Every other failure keeps its own message.
  if (DEMO_BUILD && noBackend(error)) {
    $('result').replaceChildren(demoBackendCard());
    return;
  }
  const gatewayDown = ['GATEWAY_OFFLINE', 'GATEWAY_TIMEOUT', 'GATEWAY_INVALID_RESPONSE'].includes(
    error.code
  );
  if (gatewayDown) {
    $('result').replaceChildren(gatewayOfflineCard(error));
    return;
  }
  if (error.code === 'PRINTER_NOT_FOUND') {
    $('result').replaceChildren(printerNotFoundCard());
    return;
  }
  showError($('form-error'), error);
}

function demoBackendCard() {
  return h(
    'section',
    { class: 'card result result-wait' },
    h('p', { class: 'headline', text: I18N.t('dyn.demoCard.headline') }),
    h('h2', { text: I18N.t('dyn.demoCard.title') }),
    h('p', { text: I18N.t('dyn.demoCard.body') })
  );
}

function gatewayOfflineCard(error) {
  return h(
    'section',
    { class: 'card result result-blocked' },
    h('p', { class: 'headline', text: I18N.t('dyn.offlineCard.headline') }),
    h('h2', { text: I18N.t('dyn.offlineCard.title') }),
    h('p', { text: I18N.t('dyn.offlineCard.body') }),
    h('p', { class: 'note', text: error && error.code ? error.code : 'GATEWAY_OFFLINE' }),
    h('div', { class: 'actions' }, retryButton(I18N.t('dyn.btn.tryAgain')))
  );
}

function printerNotFoundCard() {
  return h(
    'section',
    { class: 'card result result-wait' },
    h('p', { class: 'headline', text: I18N.t('dyn.notFound.headline') }),
    h('h2', { text: I18N.t('dyn.notFound.title') }),
    h('p', { text: I18N.t('dyn.notFound.body') }),
    h('p', { text: I18N.t('dyn.notFound.body2') }),
    h('div', { class: 'actions' }, retryButton(I18N.t('dyn.btn.scanAgain')))
  );
}

function retryButton(label) {
  return h(
    'button',
    { class: 'btn primary', type: 'button', onclick: (event) => retryScan(event.currentTarget) },
    label
  );
}

// --------------------------------------------------------------------------- //
// progress (UI state only, no fake percentages)
// --------------------------------------------------------------------------- //
let progressTimer = null;

function startProgress() {
  const card = $('progress-card');
  card.hidden = false;
  const items = Array.from($('progress-steps').children);
  items.forEach((item) => {
    item.className = '';
  });
  let index = 0;
  items[0].classList.add('is-active');
  clearInterval(progressTimer);
  progressTimer = setInterval(() => {
    if (index >= items.length - 1) return;
    items[index].classList.remove('is-active');
    items[index].classList.add('is-done');
    index += 1;
    items[index].classList.add('is-active');
  }, 850);
}

function stopProgress() {
  clearInterval(progressTimer);
  $('progress-card').hidden = true;
}

function finishProgress() {
  clearInterval(progressTimer);
  const items = Array.from($('progress-steps').children);
  items.forEach((item) => {
    item.classList.remove('is-active');
    item.classList.add('is-done');
  });
  setTimeout(() => {
    $('progress-card').hidden = true;
  }, 320);
}

// --------------------------------------------------------------------------- //
// result rendering: one branch per engine status
// --------------------------------------------------------------------------- //
function renderResult(report, notice) {
  state.report = report;
  state.notice = notice || null;
  const host = $('result');
  const card = h('section', { class: 'card result' });

  switch (report.status) {
    case 'COMPLETED':
      card.classList.add('result-ok');
      card.append(completedView(report));
      break;
    case 'TEST_PASSED':
      card.classList.add('result-ok');
      card.append(passedView(report));
      break;
    case 'TEST_FAILED':
      card.classList.add('result-wait');
      card.append(testFailedView(report));
      break;
    case 'READY_FOR_TEST':
      card.classList.add('result-ok');
      card.append(readyView(report));
      break;
    case 'WAITING_FIRMWARE':
      card.classList.add('result-wait');
      card.append(waitingView(report));
      break;
    case 'MATCHED':
    case 'ANALYZED':
      card.classList.add('result-wait');
      card.append(firmwareView(report));
      break;
    case 'NEEDS_REVIEW':
      card.classList.add('result-wait');
      card.append(reviewView(report));
      break;
    default:
      card.classList.add('result-blocked');
      card.append(blockedView(report));
  }

  if (notice) card.append(h('p', { class: 'note', text: notice }));
  card.append(advancedDetails(report));
  host.replaceChildren(card);
  card.scrollIntoView({ behavior: 'smooth', block: 'nearest' });
}

function printerLabel(report) {
  const brand = (report.brand || '').trim();
  const model = (report.model || '').trim();
  if (brand && model) return `${brand} ${model}`;
  const profile = (report.candidates || []).find(
    (candidate) => candidate.profile_id === report.candidate_profile
  );
  return (
    (profile && profile.display_name) ||
    report.candidate_profile ||
    I18N.t('dyn.printerFallback')
  );
}

function testButton(report) {
  return h(
    'button',
    {
      class: 'btn primary',
      type: 'button',
      onclick: (event) => runSafeTest(event.currentTarget),
    },
    I18N.t('dyn.btn.test')
  );
}

function readyView(report) {
  const safeTest = report.recommended_safe_test;
  const executable = Boolean(safeTest && safeTest.executable);
  return h(
    'div',
    null,
    h('p', { class: 'headline', text: I18N.t('dyn.ready.headline') }),
    h('h2', { text: I18N.t('dyn.ready.title') }),
    h('p', { text: I18N.t('dyn.ready.body') }),
    h('span', { class: 'printer-name', text: printerLabel(report) }),
    h(
      'dl',
      { class: 'summary' },
      h('dt', { text: I18N.t('dyn.ready.printer') }),
      h('dd', { text: printerLabel(report) }),
      h('dt', { text: I18N.t('dyn.ready.gateway') }),
      h('dd', { text: I18N.t('dyn.ready.gatewayReady') })
    ),
    h('p', { text: I18N.t('dyn.ready.noUpdate') }),
    h('div', { class: 'actions' }, executable ? testButton(report) : null),
    executable
      ? null
      : h('p', { class: 'note', text: I18N.t('dyn.ready.notTestable') })
  );
}

function passedView(report) {
  const run = report.safe_test_run || {};
  return h(
    'div',
    null,
    h('p', { class: 'headline', text: I18N.t('dyn.passed.headline') }),
    h('h2', { text: I18N.t('dyn.passed.title') }),
    h('p', { text: I18N.t('dyn.passed.body') }),
    run.simulated ? h('p', { class: 'note', text: I18N.t('dyn.passed.simulated') }) : null,
    h(
      'div',
      { class: 'actions' },
      h(
        'button',
        {
          class: 'btn primary',
          type: 'button',
          onclick: (event) => completePrinter(event.currentTarget),
        },
        I18N.t('dyn.btn.addPrinter')
      )
    )
  );
}

function testFailedView(report) {
  const run = report.safe_test_run || {};
  const response = run.gateway_response || {};
  const reason = response.reason;
  return h(
    'div',
    null,
    h('p', { class: 'headline', text: I18N.t('dyn.failed.headline') }),
    h('h2', { text: I18N.t('dyn.failed.title') }),
    h('p', { text: I18N.t('dyn.failed.body') }),
    reason ? h('p', { class: 'note', text: I18N.t('dyn.failed.reason', { reason }) }) : null,
    h('p', { text: I18N.t('dyn.failed.body2') }),
    h(
      'div',
      { class: 'actions' },
      h(
        'button',
        {
          class: 'btn primary',
          type: 'button',
          onclick: (event) => runSafeTest(event.currentTarget),
        },
        I18N.t('dyn.btn.testAgain')
      )
    )
  );
}

function completedView(report) {
  return h(
    'div',
    null,
    h('p', { class: 'headline', text: I18N.t('dyn.added.headline') }),
    h('h2', { text: I18N.t('dyn.added.title') }),
    h('p', { text: I18N.t('dyn.added.printerId', { id: report.printer_id }) }),
    h(
      'div',
      { class: 'actions' },
      h('button', { class: 'btn ghost', type: 'button', onclick: resetFlow }, I18N.t('dyn.btn.done'))
    )
  );
}

function waitingView(report) {
  return h(
    'div',
    null,
    h('p', { class: 'headline', text: I18N.t('dyn.waiting.headline') }),
    h('h2', { text: I18N.t('dyn.waiting.title') }),
    h('span', { class: 'printer-name', text: printerLabel(report) }),
    h('p', { text: I18N.t('dyn.waiting.body1') }),
    h('p', { text: I18N.t('dyn.waiting.body2') }),
    report.firmware_available && report.minimum_firmware_version
      ? h('p', {
          text: I18N.t('dyn.waiting.update', {
            version: report.minimum_firmware_version,
            channel: report.firmware_channel,
          }),
        })
      : null,
    h(
      'div',
      { class: 'actions' },
      h(
        'button',
        {
          class: 'btn primary',
          type: 'button',
          onclick: (event) => refreshCapability(event.currentTarget, false),
        },
        I18N.t('dyn.btn.checkUpdate')
      )
    ),
    devPanel(report)
  );
}

function firmwareView(report) {
  return h(
    'div',
    null,
    h('p', { class: 'headline', text: I18N.t('dyn.firmware.headline') }),
    h('h2', { text: I18N.t('dyn.firmware.title') }),
    h('span', { class: 'printer-name', text: printerLabel(report) }),
    h('p', { text: I18N.t('dyn.firmware.body') }),
    report.minimum_firmware_version
      ? h('p', {
          text: I18N.t('dyn.firmware.available', {
            version: report.minimum_firmware_version,
            channel: report.firmware_channel,
          }),
        })
      : null,
    h(
      'div',
      { class: 'actions' },
      h(
        'button',
        {
          class: 'btn primary',
          type: 'button',
          onclick: (event) => refreshCapability(event.currentTarget, false),
        },
        I18N.t('dyn.btn.checkUpdate')
      )
    ),
    devPanel(report)
  );
}

function reviewView(report) {
  return h(
    'div',
    null,
    h('p', { class: 'headline', text: I18N.t('dyn.review.headline') }),
    h('h2', { text: I18N.t('dyn.review.title') }),
    h('p', { text: I18N.t('dyn.review.body') }),
    h('div', { class: 'actions' }, tryAgainButton())
  );
}

function blockedView(report) {
  return h(
    'div',
    null,
    h('p', { class: 'headline', text: I18N.t('dyn.blocked.headline') }),
    h('h2', { text: I18N.t('dyn.blocked.title') }),
    h('p', { text: I18N.t('dyn.blocked.body') }),
    h('p', { text: I18N.t('dyn.blocked.body2') }),
    h('div', { class: 'actions' }, tryAgainButton())
  );
}

function tryAgainButton() {
  return h(
    'button',
    { class: 'btn ghost', type: 'button', onclick: resetFlow },
    I18N.t('dyn.btn.tryAgain')
  );
}

// --------------------------------------------------------------------------- //
// actions: test / complete / refresh / dev fixtures
// --------------------------------------------------------------------------- //
async function runSafeTest(button) {
  if (!state.caseId) return;
  button.disabled = true;
  try {
    // No result is sent: the gateway executes the registry-approved test and
    // reports what the printer actually answered. A real gateway rejects a
    // client-chosen result outright, so the UI must never send one.
    const report = await api(
      'POST',
      `/api/compatibility/cases/${encodeURIComponent(state.caseId)}/test`
    );
    renderResult(report);
  } catch (error) {
    button.disabled = false;
    renderResult(state.report, `${error.message} (${error.code})`);
  }
}

async function completePrinter(button) {
  if (!state.caseId) return;
  button.disabled = true;
  try {
    const report = await api(
      'POST',
      `/api/compatibility/cases/${encodeURIComponent(state.caseId)}/complete`
    );
    renderResult(report);
  } catch (error) {
    button.disabled = false;
    renderResult(state.report, `${error.message} (${error.code})`);
  }
}

async function refreshCapability(button, withSimulated) {
  if (!state.caseId) return;
  if (button) button.disabled = true;
  try {
    const body = withSimulated ? { include_simulated_firmware: true } : {};
    const report = await api(
      'POST',
      `/api/compatibility/cases/${encodeURIComponent(state.caseId)}/refresh-capability`,
      body
    );
    const stillWaiting = report.status === 'WAITING_FIRMWARE' && !report.firmware_available;
    renderResult(report, stillWaiting ? I18N.t('dyn.refresh.waiting') : undefined);
  } catch (error) {
    if (button) button.disabled = false;
    renderResult(state.report, `${error.message} (${error.code})`);
  }
}

function devPanel(report) {
  if (!DEV_MODE) return null;
  if (!['WAITING_FIRMWARE', 'MATCHED', 'ANALYZED'].includes(report.status)) return null;

  return h(
    'div',
    { class: 'dev-panel' },
    h('p', { class: 'dev-label', text: I18N.t('dyn.dev.label') }),
    h(
      'div',
      { class: 'actions' },
      h(
        'button',
        {
          class: 'btn ghost',
          type: 'button',
          onclick: (event) => devInstallFixture(event.currentTarget),
        },
        I18N.t('dyn.dev.install')
      ),
      h(
        'button',
        {
          class: 'btn ghost',
          type: 'button',
          onclick: (event) => refreshCapability(event.currentTarget, true),
        },
        I18N.t('dyn.dev.refresh')
      ),
      h(
        'button',
        {
          class: 'btn ghost',
          type: 'button',
          onclick: (event) => devGatewayUpgrade(event.currentTarget),
        },
        I18N.t('dyn.dev.upgrade')
      )
    ),
    h('p', { class: 'note', text: I18N.t('dyn.dev.note') })
  );
}

async function devInstallFixture(button) {
  button.disabled = true;
  try {
    await api('POST', '/api/dev/simulated-firmware', { installed: true });
    await refreshCapability(null, true);
  } catch (error) {
    button.disabled = false;
    renderResult(
      state.report,
      I18N.t('dyn.dev.unavailable', { error: `${error.message} (${error.code})` })
    );
  }
}

async function devGatewayUpgrade(button) {
  button.disabled = true;
  try {
    await api('POST', '/api/dev/gateway-upgrade', {});
    await refreshCapability(null, false);
  } catch (error) {
    button.disabled = false;
    renderResult(
      state.report,
      I18N.t('dyn.dev.upgradeUnavailable', { error: `${error.message} (${error.code})` })
    );
  }
}

// --------------------------------------------------------------------------- //
// advanced details (technical values, collapsed by default)
// --------------------------------------------------------------------------- //
function advancedDetails(report) {
  const safeTest = report.recommended_safe_test;
  const rows = [
    ['dyn.row.caseStatus', report.status],
    ['dyn.row.compatibility', report.compatibility_status],
    ['dyn.row.nextAction', report.next_action],
    ['dyn.row.match', report.match_source],
    ['dyn.row.confidence', `${Math.round((report.confidence || 0) * 100)}%`],
    ['dyn.row.profile', report.candidate_profile || '—'],
    ['dyn.row.transport', report.candidate_transport],
    ['dyn.row.protocol', report.candidate_protocol],
    ['dyn.row.connectionHint', report.connection_hint || '—'],
    ['dyn.row.scanSource', (report.scan_source && report.scan_source.kind) || '—'],
    ['dyn.row.fingerprintId', report.fingerprint_id || '—'],
    [
      'dyn.row.transportSource',
      (report.normalized_fingerprint && report.normalized_fingerprint.transport_source) || '—',
    ],
    ['dyn.row.evidence', (report.reason_codes || []).join(', ') || '—'],
    [
      'dyn.row.scanLatency',
      report.scan && report.scan.elapsed_ms != null ? `${report.scan.elapsed_ms} ms` : '—',
    ],
    ['dyn.row.safeTest', safeTest ? safeTest.safe_test_id : '—'],
    [
      'dyn.row.safeTestSimulated',
      String(Boolean(report.safe_test_run && report.safe_test_run.simulated)),
    ],
    ['dyn.row.executable', safeTest ? String(safeTest.executable) : '—'],
    ['dyn.row.firmwareRequired', String(Boolean(report.requires_firmware_update))],
    ['dyn.row.firmwareAvailable', String(Boolean(report.firmware_available))],
    ['dyn.row.minimumFirmware', report.minimum_firmware_version || '—'],
    ['dyn.row.firmwareChannel', report.firmware_channel || '—'],
    ['dyn.row.caseId', report.case_id || '—'],
  ];

  return h(
    'details',
    { class: 'advanced' },
    h('summary', { text: I18N.t('dyn.advanced.summary') }),
    h(
      'table',
      null,
      h(
        'tbody',
        null,
        ...rows.map(([key, value]) =>
          h('tr', null, h('td', { text: I18N.t(key) }), h('td', { text: value }))
        )
      )
    )
  );
}

// --------------------------------------------------------------------------- //
// reset / boot
// --------------------------------------------------------------------------- //
// Back to the choices, not to a blank page: /printers/add shows the options
// again and the Lab keeps its picker ready for the next fixture.
function resetFlow() {
  state.caseId = null;
  state.report = null;
  state.notice = null;
  state.device = null;
  $('result').replaceChildren();
  if ($('rescue-card')) $('rescue-card').hidden = true;
  if ($('add-printer-card')) $('add-printer-card').hidden = false;
  if ($('form-error')) clearError($('form-error'));
  if ($('rescue-form')) $('rescue-form').reset();
  document
    .querySelectorAll('[data-device]')
    .forEach((button) => button.classList.remove('is-active'));
  window.scrollTo({ top: 0, behavior: 'smooth' });
}

// One script for every page: <body data-page="..."> picks the initialiser.
const PAGE_INIT = {
  home: initStaticPage,
  setup: initSetup,
  lab: initLab,
  printers: initStaticPage,
  add: initAdd,
  print: initStaticPage,
  compatibility: initStaticPage,
};

// Rebuild everything the script rendered when the language changes: static
// markup is handled by I18N.apply(), these are the nodes it cannot know about.
function rerenderForLanguage() {
  if (state.gateway) {
    setBarStatus(state.gateway);
    renderGateway(state.gateway);
  } else if (state.demo) {
    renderDemoGateway();
  } else {
    setBarStatus(null);
  }
  renderUsb();
  if (state.report) renderResult(state.report, state.notice || undefined);
}

function boot() {
  I18N.init();
  // The bar and the gateway card are rendered by this script, so their first
  // paint is localised here rather than through data-i18n.
  setBarStatus(null);
  if ($('gateway-name')) $('gateway-name').textContent = I18N.t('home.gatewayLoading');
  if (DEV_MODE && $('dev-devices')) {
    $('dev-devices').hidden = false;
    document.body.classList.add('dev-mode');
  }
  const init = PAGE_INIT[PAGE];
  if (init) init();
  document.addEventListener('rtg:langchange', rerenderForLanguage);
  loadGateway();
}

if (document.readyState === 'loading') {
  document.addEventListener('DOMContentLoaded', boot);
} else {
  boot();
}
