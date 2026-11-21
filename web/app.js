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
  device: null,
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
    const error = new Error('The gateway service is not reachable from this page.');
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
        ? 'No gateway service on this origin.'
        : (data && data.message) || `Request failed (${response.status})`
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
        error.code === 'BACKEND_UNAVAILABLE' ? 'Gateway service offline' : 'Gateway unavailable';
      showError($('gateway-error'), error);
    }
  }
}

// Static preview: there is no gateway to report on, and pretending otherwise
// would make a demo look like a real installation.
function renderDemoGateway() {
  state.gateway = null;
  setDemoBarStatus();
  const name = $('gateway-name');
  if (!name) return;  // the bar already carries the state

  name.textContent = 'Gateway simulator ready';
  $('gateway-place').textContent = 'This deployment is not connected to a real Gateway.';

  const badge = $('gateway-status');
  badge.textContent = 'Demo';
  badge.className = 'badge is-demo';

  clearError($('gateway-error'));

  $('gateway-facts').replaceChildren(
    fact('Firmware', 'Demo build'),
    fact('Hardware', 'No real Gateway'),
    fact('Last seen', '—'),
    fact('Gateway ID', 'demo')
  );
  $('gateway-caps').replaceChildren(
    h('span', { class: 'chip', text: 'Simulator' }),
    h('span', { class: 'chip', text: 'No engine' })
  );
}

function setDemoBarStatus() {
  const node = $('bar-gateway');
  if (!node) return;
  node.textContent = 'Gateway: simulator ready';
  node.className = 'bar-status is-demo';
}

function setBarStatus(gateway) {
  const node = $('bar-gateway');
  if (!node) return;
  const online = Boolean(gateway) && gateway.status === 'online';
  // Never let a simulator look like a real board: the bar says so explicitly.
  const simulated = Boolean(gateway) && gateway.mode !== 'real';
  node.textContent = `Gateway: ${gateway ? gateway.status : 'offline'}${
    simulated ? ' · simulator' : ''
  }`;
  node.className = `bar-status is-${online ? 'online' : 'offline'}`;
}

function renderGateway(gateway) {
  const name = $('gateway-name');
  if (!name) return;  // this page shows the gateway in the bar only

  name.textContent = gateway.name;
  $('gateway-place').textContent = gateway.hardware_family;

  const badge = $('gateway-status');
  badge.textContent = gateway.status;
  badge.className = `badge is-${gateway.status === 'online' ? 'online' : 'offline'}`;

  if (gateway.reachable === false) {
    showError($('gateway-error'), {
      message:
        'Gateway is offline. Check that it is powered on and on the same network, then reload this page.',
      code: gateway.error,
    });
  } else {
    clearError($('gateway-error'));
  }

  const facts = $('gateway-facts');
  facts.replaceChildren(
    fact('Firmware', gateway.firmware_version),
    fact('Hardware', gateway.hardware_family),
    fact('Last seen', formatTime(gateway.last_seen)),
    fact('Gateway ID', gateway.gateway_id)
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
    chips.append(h('span', { class: 'chip', text: 'No capability reported' }));
  }
}

function fact(term, value) {
  return h('div', null, h('dt', { text: term }), h('dd', { text: value || '—' }));
}

function formatTime(iso) {
  if (!iso) return '—';
  const date = new Date(iso);
  if (Number.isNaN(date.getTime())) return iso;
  return date.toLocaleString(undefined, {
    year: 'numeric',
    month: 'short',
    day: '2-digit',
    hour: '2-digit',
    minute: '2-digit',
  });
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
    h('p', { class: 'headline', text: 'Demo build' }),
    h('h2', { text: 'The compatibility engine is not part of this deployment.' }),
    h('p', {
      text:
        'This static preview shows the flow, but a scan needs the Gateway service. Nothing was checked and nothing was added.',
    })
  );
}

function gatewayOfflineCard(error) {
  return h(
    'section',
    { class: 'card result result-blocked' },
    h('p', { class: 'headline', text: 'Gateway offline' }),
    h('h2', { text: 'Gateway is offline.' }),
    h('p', {
      text:
        'Your printer was not checked. The Gateway did not answer the scan request, so we cannot say anything about this printer yet.',
    }),
    h('p', { class: 'note', text: error && error.code ? error.code : 'GATEWAY_OFFLINE' }),
    h('div', { class: 'actions' }, retryButton('Try again'))
  );
}

function printerNotFoundCard() {
  return h(
    'section',
    { class: 'card result result-wait' },
    h('p', { class: 'headline', text: 'Not found' }),
    h('h2', { text: "We couldn't find your printer." }),
    h('p', {
      text:
        'Make sure the printer is switched on, close to the Gateway, and not connected to another phone or app.',
    }),
    h('p', { text: 'Nothing was added. You can scan again.' }),
    h('div', { class: 'actions' }, retryButton('Scan again'))
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
  return (profile && profile.display_name) || report.candidate_profile || 'Your printer';
}

function testButton(report) {
  return h(
    'button',
    {
      class: 'btn primary',
      type: 'button',
      onclick: (event) => runSafeTest(event.currentTarget),
    },
    'Test compatibility'
  );
}

function readyView(report) {
  const safeTest = report.recommended_safe_test;
  const executable = Boolean(safeTest && safeTest.executable);
  return h(
    'div',
    null,
    h('p', { class: 'headline', text: 'Compatible' }),
    h('h2', { text: 'Good news — your printer looks compatible.' }),
    h('p', { text: 'We found a compatible printer profile.' }),
    h('span', { class: 'printer-name', text: printerLabel(report) }),
    h(
      'dl',
      { class: 'summary' },
      h('dt', { text: 'Printer' }),
      h('dd', { text: printerLabel(report) }),
      h('dt', { text: 'Gateway' }),
      h('dd', { text: 'Ready' })
    ),
    h('p', { text: 'No Gateway software update is required.' }),
    h('div', { class: 'actions' }, executable ? testButton(report) : null),
    executable
      ? null
      : h('p', {
          class: 'note',
          text: 'This printer cannot be tested on this Gateway yet.',
        })
  );
}

function passedView(report) {
  const run = report.safe_test_run || {};
  return h(
    'div',
    null,
    h('p', { class: 'headline', text: 'Test passed' }),
    h('h2', { text: 'Compatibility test passed.' }),
    h('p', { text: 'Your printer is ready to add.' }),
    run.simulated
      ? h('p', {
          class: 'note',
          text: 'Development mode: this test was simulated. No printer was touched.',
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
          onclick: (event) => completePrinter(event.currentTarget),
        },
        'Add Printer'
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
    h('p', { class: 'headline', text: 'Test not passed' }),
    h('h2', { text: 'Compatibility test did not pass.' }),
    h('p', {
      text:
        'The Gateway reached your printer, but the compatibility test did not come back cleanly.',
    }),
    reason ? h('p', { class: 'note', text: `Reason: ${reason}` }) : null,
    h('p', { text: 'You can run the test again. Nothing was added yet.' }),
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
        'Test again'
      )
    )
  );
}

function completedView(report) {
  return h(
    'div',
    null,
    h('p', { class: 'headline', text: 'Added' }),
    h('h2', { text: 'Printer added successfully.' }),
    h('p', { text: `Printer ID: ${report.printer_id}` }),
    h(
      'div',
      { class: 'actions' },
      h(
        'button',
        { class: 'btn ghost', type: 'button', onclick: resetFlow },
        'Done'
      )
    )
  );
}

function waitingView(report) {
  return h(
    'div',
    null,
    h('p', { class: 'headline', text: 'Recognized' }),
    h('h2', { text: 'We recognize this printer.' }),
    h('span', { class: 'printer-name', text: printerLabel(report) }),
    h('p', {
      text:
        'Your Gateway does not yet have the software needed to communicate with this printer.',
    }),
    h('p', {
      text:
        "We'll be able to add support through a Gateway update when a compatible firmware release is available.",
    }),
    report.firmware_available && report.minimum_firmware_version
      ? h('p', {
          text: `A Gateway update is available: ${report.minimum_firmware_version} (${report.firmware_channel}).`,
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
        'Check for compatibility update'
      )
    ),
    devPanel(report)
  );
}

function firmwareView(report) {
  return h(
    'div',
    null,
    h('p', { class: 'headline', text: 'Gateway update needed' }),
    h('h2', { text: 'This printer needs newer Gateway software.' }),
    h('span', { class: 'printer-name', text: printerLabel(report) }),
    h('p', {
      text:
        'The printer was recognized, but this Gateway version cannot talk to it yet.',
    }),
    report.minimum_firmware_version
      ? h('p', {
          text: `Available update: ${report.minimum_firmware_version} (${report.firmware_channel}).`,
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
        'Check for compatibility update'
      )
    ),
    devPanel(report)
  );
}

function reviewView(report) {
  return h(
    'div',
    null,
    h('p', { class: 'headline', text: 'Needs review' }),
    h('h2', { text: 'We found a possible match, but it needs a closer look.' }),
    h('p', {
      text:
        'We recorded your printer, but the evidence was not strong enough to test it automatically.',
    }),
    h('div', { class: 'actions' }, tryAgainButton())
  );
}

function blockedView(report) {
  return h(
    'div',
    null,
    h('p', { class: 'headline', text: 'Not identified' }),
    h('h2', { text: "We couldn't safely identify this printer yet." }),
    h('p', {
      text:
        'Your printer information has been recorded, but we don\'t have enough evidence to test it safely.',
    }),
    h('p', { text: 'Please check the brand/model information or try again later.' }),
    h('div', { class: 'actions' }, tryAgainButton())
  );
}

function tryAgainButton() {
  return h(
    'button',
    { class: 'btn ghost', type: 'button', onclick: resetFlow },
    'Try again'
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
    renderResult(
      report,
      stillWaiting
        ? 'Still waiting for a compatible Gateway update. Nothing has changed yet.'
        : undefined
    );
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
    h('p', { class: 'dev-label', text: 'Development fixtures' }),
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
        'Install simulated firmware fixture'
      ),
      h(
        'button',
        {
          class: 'btn ghost',
          type: 'button',
          onclick: (event) => refreshCapability(event.currentTarget, true),
        },
        'Refresh capability'
      ),
      h(
        'button',
        {
          class: 'btn ghost',
          type: 'button',
          onclick: (event) => devGatewayUpgrade(event.currentTarget),
        },
        'Simulate Gateway upgrade'
      )
    ),
    h('p', {
      class: 'note',
      text:
        'Simulated firmware and the simulated Gateway upgrade exist for demos only. ' +
        'They never reach a real printer.',
    })
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
      `Development fixtures are unavailable: ${error.message} (${error.code})`
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
      `Gateway upgrade simulation unavailable: ${error.message} (${error.code})`
    );
  }
}

// --------------------------------------------------------------------------- //
// advanced details (technical values, collapsed by default)
// --------------------------------------------------------------------------- //
function advancedDetails(report) {
  const safeTest = report.recommended_safe_test;
  const rows = [
    ['Case status', report.status],
    ['Compatibility', report.compatibility_status],
    ['Next action', report.next_action],
    ['Match', report.match_source],
    ['Confidence', `${Math.round((report.confidence || 0) * 100)}%`],
    ['Profile', report.candidate_profile || '—'],
    ['Transport', report.candidate_transport],
    ['Protocol', report.candidate_protocol],
    ['Connection hint', report.connection_hint || '—'],
    ['Scan source', (report.scan_source && report.scan_source.kind) || '—'],
    ['Fingerprint ID', report.fingerprint_id || '—'],
    [
      'Transport source',
      (report.normalized_fingerprint && report.normalized_fingerprint.transport_source) || '—',
    ],
    ['Evidence', (report.reason_codes || []).join(', ') || '—'],
    [
      'Scan latency',
      report.scan && report.scan.elapsed_ms != null ? `${report.scan.elapsed_ms} ms` : '—',
    ],
    ['Safe test', safeTest ? safeTest.safe_test_id : '—'],
    ['Safe test simulated', String(Boolean(report.safe_test_run && report.safe_test_run.simulated))],
    ['Executable', safeTest ? String(safeTest.executable) : '—'],
    ['Firmware required', String(Boolean(report.requires_firmware_update))],
    ['Firmware available', String(Boolean(report.firmware_available))],
    ['Minimum firmware', report.minimum_firmware_version || '—'],
    ['Firmware channel', report.firmware_channel || '—'],
    ['Case ID', report.case_id || '—'],
  ];

  return h(
    'details',
    { class: 'advanced' },
    h('summary', { text: 'Advanced details' }),
    h(
      'table',
      null,
      h(
        'tbody',
        null,
        ...rows.map(([key, value]) =>
          h('tr', null, h('td', { text: key }), h('td', { text: value }))
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
  setup: initStaticPage,
  lab: initLab,
  printers: initStaticPage,
  add: initAdd,
  print: initStaticPage,
  compatibility: initStaticPage,
};

function boot() {
  if (DEV_MODE && $('dev-devices')) {
    $('dev-devices').hidden = false;
    document.body.classList.add('dev-mode');
  }
  const init = PAGE_INIT[PAGE];
  if (init) init();
  loadGateway();
}

if (document.readyState === 'loading') {
  document.addEventListener('DOMContentLoaded', boot);
} else {
  boot();
}
