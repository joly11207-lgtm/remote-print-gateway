'use strict';

/**
 * Web Serial gateway client (Website V1.1).
 *
 * This is a *transport*, not a compatibility engine: it opens the USB serial
 * port the user picks, sends exactly one command and returns the identity the
 * board reports. Nothing here decides what a printer is or what it can do.
 *
 * Wire protocol: newline-delimited JSON over the same UART the firmware logs
 * to. The browser sends one line:
 *
 *   {"type":"gateway_info"}
 *
 * and the firmware answers one line:
 *
 *   {"type":"gateway_info","gateway_id":"gw-esp32-1a2b3c",
 *    "hardware_family":"ESP32-WROOM-32E","firmware_version":"0.1.0",
 *    "protocol_version":1}
 *
 * Human debug lines ("Remote Thermal Printer Bridge V0.1 ...", "[HEAP] ...")
 * are also on that wire. Any line that does not start with "{" is a log line
 * and is skipped, never parsed and never shown as a device fact.
 *
 * There is deliberately NO generic send()/sendRaw()/sendHex()/console entry
 * point: V1.1 must not become a hardware command console. The only bytes this
 * module can write are INFO_REQUEST below.
 *
 * No DOM access: the client is usable from tests with an injected `serial`.
 */
(function (global) {
  const PROTOCOL_VERSION = 1;
  const BAUD_RATE = 115200;
  const DEFAULT_TIMEOUT_MS = 2500;
  const DEFAULT_ATTEMPTS = 3;
  // Bounded buffers: a chatty or broken device must not grow our memory. A
  // scan_result for ten devices is ~3 KB, so the line ceiling is larger than
  // the identity reply needs.
  const MAX_LINE_CHARS = 16384;
  const MAX_QUEUED_LINES = 32;
  const REQUIRED_FIELDS = ['gateway_id', 'hardware_family', 'firmware_version'];
  // The firmware grants 1000..8000 ms; anything else is a request it clamps.
  const SCAN_DEFAULT_MS = 4000;
  const SCAN_MIN_MS = 1000;
  const SCAN_MAX_MS = 8000;
  const SCAN_TIMEOUT_MS = 20000;
  // The only two commands this module can ever send.
  const INFO_REQUEST = '{"type":"gateway_info"}';
  const SCAN_COMMAND = 'scan';

  const STATE = {
    UNSUPPORTED: 'unsupported',
    DISCONNECTED: 'disconnected',
    SELECTING: 'selecting',
    OPENING: 'opening',
    CONNECTED: 'connected',
    ERROR: 'error',
  };

  class GatewaySerialError extends Error {
    constructor(code, message) {
      super(message || code);
      this.name = 'GatewaySerialError';
      this.code = code;
    }
  }

  function sleep(ms) {
    return new Promise((resolve) => setTimeout(resolve, ms));
  }

  function clampScanDuration(value) {
    const ms =
      typeof value === 'number' && isFinite(value) && value > 0 ? Math.floor(value) : SCAN_DEFAULT_MS;
    if (ms < SCAN_MIN_MS) return SCAN_MIN_MS;
    if (ms > SCAN_MAX_MS) return SCAN_MAX_MS;
    return ms;
  }

  function detectSerial() {
    try {
      return global && global.navigator && global.navigator.serial ? global.navigator.serial : null;
    } catch (error) {
      return null;  // a blocked/absent API is "unsupported", not a crash
    }
  }

  function validateInfo(payload) {
    const missing = REQUIRED_FIELDS.filter(
      (field) => typeof payload[field] !== 'string' || !payload[field].trim()
    );
    if (missing.length) {
      throw new GatewaySerialError('invalid_response', `missing field: ${missing.join(', ')}`);
    }
    const version = payload.protocol_version;
    return {
      gateway_id: payload.gateway_id.trim(),
      hardware_family: payload.hardware_family.trim(),
      firmware_version: payload.firmware_version.trim(),
      protocol_version: typeof version === 'number' && isFinite(version) ? version : PROTOCOL_VERSION,
    };
  }

  function validateDevice(raw) {
    if (!raw || typeof raw !== 'object' || Array.isArray(raw)) return null;
    return {
      name: typeof raw.name === 'string' ? raw.name : '',
      address: typeof raw.address === 'string' ? raw.address : '',
      rssi: typeof raw.rssi === 'number' && isFinite(raw.rssi) ? raw.rssi : null,
      // The advertisement carries at most one service in this firmware, so the
      // array is normally 0 or 1 long. Never padded.
      services: Array.isArray(raw.services)
        ? raw.services.filter((item) => typeof item === 'string' && item)
        : [],
      service_uuid: typeof raw.service_uuid === 'string' ? raw.service_uuid : '',
      fingerprint: typeof raw.fingerprint === 'string' ? raw.fingerprint : 'unknown',
      confidence: typeof raw.confidence === 'number' && isFinite(raw.confidence) ? raw.confidence : 0,
      evidence: typeof raw.evidence === 'string' ? raw.evidence : 'none',
      model_id: typeof raw.model_id === 'string' ? raw.model_id : 'unmatched',
      match_reason: typeof raw.match_reason === 'string' ? raw.match_reason : 'none',
    };
  }

  function validateScan(payload) {
    if (!Array.isArray(payload.devices)) {
      throw new GatewaySerialError('invalid_response', 'scan reply has no devices');
    }
    const devices = payload.devices.map(validateDevice).filter(Boolean);
    return {
      result: typeof payload.result === 'string' ? payload.result : 'UNKNOWN',
      transport: typeof payload.transport === 'string' ? payload.transport : 'unknown',
      scanned: typeof payload.scanned === 'number' && isFinite(payload.scanned)
        ? payload.scanned
        : devices.length,
      elapsed_ms:
        typeof payload.elapsed_ms === 'number' && isFinite(payload.elapsed_ms)
          ? payload.elapsed_ms
          : 0,
      protocol_version:
        typeof payload.protocol_version === 'number' && isFinite(payload.protocol_version)
          ? payload.protocol_version
          : PROTOCOL_VERSION,
      devices,
    };
  }

  class WebSerialGatewayClient {
    constructor(options) {
      const config = options || {};
      this.serial = config.serial !== undefined ? config.serial : detectSerial();
      this.timeoutMs = config.timeoutMs || DEFAULT_TIMEOUT_MS;
      this.maxAttempts = config.maxAttempts || DEFAULT_ATTEMPTS;
      // One bounded recovery when the board resets as the port opens.
      this.recoverOnDeviceLost = config.recoverOnDeviceLost !== false;
      this.recoverDelayMs = config.recoverDelayMs || 1200;
      this.state = this.serial ? STATE.DISCONNECTED : STATE.UNSUPPORTED;
      this.info = null;
      this.error = null;      // { code, message } - a code, never a raw stack
      this.port = null;
      this.reader = null;
      this.writer = null;
      this.buffer = '';
      this.lines = [];
      this.waiter = null;
      this.reading = false;
      this.closing = false;
      this.dead = null;        // set when the read stream dies (device removed)
      this.connecting = null;  // in-flight connect(): double clicks reuse it
      this.scanning = null;    // in-flight scan(): double clicks reuse it
      this.scanTimeoutMs = config.scanTimeoutMs || SCAN_TIMEOUT_MS;
      this.listeners = [];
      this.decoder = typeof TextDecoder === 'function' ? new TextDecoder() : null;
    }

    supported() {
      return Boolean(this.serial);
    }

    snapshot() {
      return {
        supported: Boolean(this.serial),
        state: this.state,
        info: this.info ? Object.assign({}, this.info) : null,
        error: this.error ? Object.assign({}, this.error) : null,
      };
    }

    onState(listener) {
      if (typeof listener === 'function') this.listeners.push(listener);
      return () => {
        this.listeners = this.listeners.filter((item) => item !== listener);
      };
    }

    // Previously authorised ports, for a hint only. This never opens anything:
    // V1.1 requires a click on Connect after every page load.
    async hasAuthorizedPort() {
      try {
        if (!this.serial || typeof this.serial.getPorts !== 'function') return false;
        const ports = await this.serial.getPorts();
        return Array.isArray(ports) && ports.length > 0;
      } catch (error) {
        return false;
      }
    }

    connect() {
      // Only ever one open() in flight: a second click must not race the first.
      if (this.connecting) return this.connecting;
      if (this.state === STATE.CONNECTED) return Promise.resolve(this.info);
      this.connecting = this._connect().then(
        (info) => {
          this.connecting = null;
          return info;
        },
        (error) => {
          this.connecting = null;
          throw error;
        }
      );
      return this.connecting;
    }

    // Must be called from a click: requestPort() is the first await, so the
    // user gesture is still valid when the picker opens.
    async _connect() {
      if (!this.serial || typeof this.serial.requestPort !== 'function') {
        // Degrade, never crash: an unsupported browser keeps the unsupported
        // state (the UI shows the friendly hint) instead of an error state.
        this._fail(
          new GatewaySerialError('unsupported', 'this browser has no Web Serial'),
          STATE.UNSUPPORTED
        );
      }

      this.error = null;
      this.info = null;
      this.buffer = '';
      this.lines = [];
      this._setState(STATE.SELECTING);

      let port;
      try {
        port = await this.serial.requestPort();
      } catch (error) {
        // NotFoundError = picker dismissed, SecurityError = permission denied.
        const name = error && error.name;
        const code = name === 'NotFoundError' || name === 'SecurityError' ? 'permission_denied' : 'request_failed';
        this._fail(new GatewaySerialError(code, (error && error.message) || 'port not selected'));
      }

      this._setState(STATE.OPENING);
      try {
        await port.open({ baudRate: BAUD_RATE });
      } catch (error) {
        this.port = port;
        const busy = error && error.name === 'InvalidStateError';
        await this._teardown();
        this._fail(new GatewaySerialError(busy ? 'busy' : 'open_failed', (error && error.message) || 'open failed'));
      }

      await this._lowerControlLines(port);

      this.port = port;
      await this._attach(port);

      let info;
      try {
        info = await this._requestInfo(port);
      } catch (error) {
        // Some ESP32-S3 boards reset when the port opens (Chrome raises DTR,
        // which drives EN). The chip re-enumerates, so the first handshake dies
        // with device_lost even though everything is fine. The port is already
        // authorised: retry it ONCE, without another picker and without a loop.
        if (!this.recoverOnDeviceLost || error.code !== 'device_lost') throw error;
        info = await this._recoverOnce(port);
      }

      this.info = info;
      this.error = null;
      this._setState(STATE.CONNECTED);
      return info;
    }

    // Take the streams of an already-open port. Shared by the first attempt and
    // by the single recovery after a board reset.
    async _attach(port) {
      this.closing = false;
      this.dead = null;
      try {
        this.writer = port.writable.getWriter();
      } catch (error) {
        await this._teardown();
        this._fail(new GatewaySerialError('open_failed', 'no writable stream'));
      }
      this.reading = true;
      // Runs until the port closes; never awaited, and it must not reject the
      // handshake promise: the status is carried by `error`/`state`.
      this._pump(port).catch(() => {});
    }

    async _recoverOnce(port) {
      await sleep(this.recoverDelayMs);
      try {
        await port.open({ baudRate: BAUD_RATE });
      } catch (error) {
        this._fail(new GatewaySerialError('device_lost', 'the device did not come back'));
      }
      this.port = port;
      this.error = null;
      this._setState(STATE.OPENING);
      await this._lowerControlLines(port);
      await this._attach(port);
      return this._requestInfo(port);  // a second failure is final
    }

    async disconnect() {
      await this._teardown();
      this.info = null;
      this.error = null;
      this._setState(this.serial ? STATE.DISCONNECTED : STATE.UNSUPPORTED);
    }

    // V1.2: the second and last command. Same one-in-flight rule as connect():
    // two clicks must not interleave two scans on one port.
    scan(options) {
      if (this.scanning) return this.scanning;
      this.scanning = this._scan(options).then(
        (result) => {
          this.scanning = null;
          return result;
        },
        (error) => {
          this.scanning = null;
          throw error;
        }
      );
      return this.scanning;
    }

    async _scan(options) {
      if (this.state !== STATE.CONNECTED || !this.port) {
        // Never silently reopen: a scan is not a reason to grab a port.
        throw new GatewaySerialError('not_connected', 'connect the gateway first');
      }
      const durationMs = clampScanDuration((options || {}).durationMs);
      try {
        await this._writeLine(`{"type":"${SCAN_COMMAND}","duration_ms":${durationMs}}`);
      } catch (error) {
        await this._teardown();
        this._fail(new GatewaySerialError('device_lost', 'write failed'));
      }
      try {
        return await this._awaitScan();
      } catch (error) {
        // A failed scan does not mean a dead link: only a dead stream does.
        if (error.code === 'device_lost') {
          await this._teardown();
          this._setState(STATE.ERROR);
        }
        throw error;
      }
    }

    async _awaitScan() {
      const deadline = Date.now() + this.scanTimeoutMs;
      for (;;) {
        const remaining = deadline - Date.now();
        if (remaining <= 0) {
          throw new GatewaySerialError('scan_timeout', 'the scan did not finish');
        }
        const line = await this._waitLine(remaining);
        const text = String(line).trim();
        if (!text || text.charAt(0) !== '{') continue;  // human log line

        let payload;
        try {
          payload = JSON.parse(text);
        } catch (error) {
          throw new GatewaySerialError('invalid_response', 'scan reply is not JSON');
        }
        if (payload && payload.type === 'error') {
          throw new GatewaySerialError('gateway_refused', String(payload.error || 'refused'));
        }
        if (!payload || payload.type !== 'scan_result') {
          throw new GatewaySerialError('wrong_type', 'expected scan_result');
        }
        return validateScan(payload);
      }
    }

    // A dev board resets when DTR/RTS toggle on open: on a DevKitC those lines
    // drive EN/IO0. Chrome raises both by default, so drop them right away and
    // let the handshake run against the already-booted firmware.
    async _lowerControlLines(port) {
      try {
        if (port && typeof port.setSignals === 'function') {
          await port.setSignals({ dataTerminalReady: false, requestToSend: false });
        }
      } catch (error) {
        // Not every target exposes modem signals; the handshake still works.
      }
    }

    async _requestInfo(port) {
      let lastError = null;
      for (let attempt = 1; attempt <= this.maxAttempts; attempt += 1) {
        if (this.closing || this.port !== port) {
          await this._teardown();
          this._fail(new GatewaySerialError('device_lost', 'port closed during handshake'));
        }
        try {
          await this._writeLine(INFO_REQUEST);
        } catch (error) {
          await this._teardown();
          this._fail(new GatewaySerialError('device_lost', 'write failed'));
        }
        try {
          return await this._awaitInfo();
        } catch (error) {
          lastError = error;
          // A protocol answer (bad JSON, wrong type, refusal) is final; only a
          // silent board is worth retrying (a reboot mid-handshake).
          if (error.code !== 'timeout') break;
        }
      }
      await this._teardown();
      this._fail(lastError || new GatewaySerialError('timeout', 'no gateway_info line'));
    }

    async _awaitInfo() {
      const deadline = Date.now() + this.timeoutMs;
      for (;;) {
        const remaining = deadline - Date.now();
        if (remaining <= 0) throw new GatewaySerialError('timeout', 'no gateway_info line');
        const line = await this._waitLine(remaining);
        const text = String(line).trim();
        if (!text) continue;
        if (text.charAt(0) !== '{') continue;  // human log line on the same UART

        let payload;
        try {
          payload = JSON.parse(text);
        } catch (error) {
          throw new GatewaySerialError('invalid_response', 'reply is not JSON');
        }
        if (!payload || typeof payload !== 'object' || Array.isArray(payload)) {
          throw new GatewaySerialError('invalid_response', 'reply is not an object');
        }
        if (payload.type === 'error') {
          throw new GatewaySerialError('gateway_refused', String(payload.error || 'refused'));
        }
        if (payload.type !== 'gateway_info') {
          throw new GatewaySerialError('wrong_type', 'unexpected message type');
        }
        return validateInfo(payload);
      }
    }

    async _writeLine(text) {
      if (!this.writer) throw new GatewaySerialError('device_lost', 'no writer');
      const bytes = typeof TextEncoder === 'function' ? new TextEncoder().encode(`${text}\n`) : null;
      if (!bytes) throw new GatewaySerialError('unsupported', 'no TextEncoder');
      await this.writer.write(bytes);
    }

    async _pump(port) {
      let reader;
      try {
        reader = port.readable.getReader();
      } catch (error) {
        this._rejectWaiter(new GatewaySerialError('device_lost', 'no readable stream'));
        return;
      }
      this.reader = reader;
      try {
        for (;;) {
          const chunk = await reader.read();
          if (chunk.done) break;
          if (chunk.value) this._onChunk(this.decoder.decode(chunk.value, { stream: true }));
        }
      } catch (error) {
        if (this.reading) {
          this.dead = error;
          this._rejectWaiter(new GatewaySerialError('device_lost', 'read failed'));
          this.error = { code: 'device_lost', message: String((error && error.message) || error) };
          if (this.state === STATE.CONNECTED) this._setState(STATE.ERROR);
        }
      }
    }

    _onChunk(text) {
      this.buffer += text;
      let index = this.buffer.indexOf('\n');
      while (index !== -1) {
        this._pushLine(this.buffer.slice(0, index));
        this.buffer = this.buffer.slice(index + 1);
        index = this.buffer.indexOf('\n');
      }
      if (this.buffer.length > MAX_LINE_CHARS) this.buffer = '';  // unterminated garbage
    }

    _pushLine(line) {
      if (this.waiter) {
        this.waiter.deliver(line);
        return;
      }
      this.lines.push(line);
      if (this.lines.length > MAX_QUEUED_LINES) this.lines.shift();
    }

    _waitLine(timeoutMs) {
      if (this.dead) {
        // The port died before (or while) we started listening: report it now
        // instead of burning the whole timeout, so the caller can recover.
        return Promise.reject(new GatewaySerialError('device_lost', 'the port died'));
      }
      if (this.lines.length) return Promise.resolve(this.lines.shift());
      return new Promise((resolve, reject) => {
        const timer = setTimeout(() => {
          this.waiter = null;
          reject(new GatewaySerialError('timeout', 'no line from the gateway'));
        }, Math.max(1, timeoutMs));
        this.waiter = {
          deliver: (line) => {
            clearTimeout(timer);
            this.waiter = null;
            resolve(line);
          },
          reject: (error) => {
            clearTimeout(timer);
            this.waiter = null;
            reject(error);
          },
        };
      });
    }

    _rejectWaiter(error) {
      if (this.waiter && this.waiter.reject) this.waiter.reject(error);
    }

    // Release everything the port is holding. A closed browser tab, a failed
    // handshake and a user click all end up here: no reader, no writer, no
    // open port may survive, otherwise the COM port stays locked.
    async _teardown() {
      this.reading = false;
      this.closing = true;

      const reader = this.reader;
      this.reader = null;
      if (reader) {
        try {
          await reader.cancel();
        } catch (error) { /* already gone */ }
        try {
          reader.releaseLock();
        } catch (error) { /* already released */ }
      }

      const writer = this.writer;
      this.writer = null;
      if (writer) {
        try {
          writer.releaseLock();
        } catch (error) { /* already released */ }
      }

      const port = this.port;
      this.port = null;
      if (port) {
        try {
          await port.close();
        } catch (error) { /* closed by the OS / device removal */ }
      }

      this.buffer = '';
      this.lines = [];
      this._rejectWaiter(new GatewaySerialError('device_lost', 'port closed'));
      this.dead = null;
      this.closing = false;
    }

    _fail(error, state) {
      this.error = { code: error.code, message: error.message };
      this._setState(state || STATE.ERROR);
      throw error;
    }

    _setState(next) {
      this.state = next;
      const snapshot = this.snapshot();
      for (const listener of this.listeners) {
        try {
          listener(snapshot);
        } catch (error) {
          /* a broken listener must not break the state machine */
        }
      }
    }
  }

  global.WebSerialGatewayClient = WebSerialGatewayClient;
  global.GatewaySerialError = GatewaySerialError;
  global.WebSerialState = STATE;
  global.WEB_SERIAL_PROTOCOL_VERSION = PROTOCOL_VERSION;

  if (typeof module !== 'undefined' && module.exports) {
    module.exports = {
      WebSerialGatewayClient,
      GatewaySerialError,
      STATE,
      PROTOCOL_VERSION,
      INFO_REQUEST,
      SCAN_COMMAND,
      SCAN_DEFAULT_MS,
      SCAN_MIN_MS,
      SCAN_MAX_MS,
      SCAN_TIMEOUT_MS,
    };
  }
})(typeof globalThis !== 'undefined' ? globalThis : this);
