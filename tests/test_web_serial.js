/**
 * Web Serial gateway client tests (Website V1.1).
 *
 * Runs the REAL web/serial.js under Node with an injected fake SerialPort, so
 * the state machine, the wire format and the port release path are exercised
 * without a browser or a board.
 *
 * Usage: node tests/test_web_serial.js
 */

'use strict';

const assert = require('node:assert');
const path = require('node:path');
const { WebSerialGatewayClient } = require(path.join(__dirname, '..', 'web', 'serial.js'));

const INFO_LINE =
  '{"type":"gateway_info","gateway_id":"gw-esp32-a1b2c3",' +
  '"hardware_family":"ESP32-WROOM-32E","firmware_version":"0.2.0","protocol_version":1}\n';
const REQUEST_LINE = '{"type":"gateway_info"}\n';

const encoder = new TextEncoder();
const decoder = new TextDecoder();

let checks = 0;
let failures = 0;

function check(label, test) {
  checks += 1;
  try {
    test();
    console.log(`  ok   ${label}`);
  } catch (error) {
    failures += 1;
    console.log(`  FAIL ${label}: ${error.message}`);
  }
}

async function checkAsync(label, test) {
  checks += 1;
  try {
    await test();
    console.log(`  ok   ${label}`);
  } catch (error) {
    failures += 1;
    console.log(`  FAIL ${label}: ${error.message}`);
  }
}

function section(name) {
  console.log(`\n== ${name} ==`);
}

/** A SerialPort-shaped fake: real Web Streams, recorded writes. */
class FakePort {
  constructor(options) {
    this.options = options || {};
    this.opens = 0;
    this.opened = false;
    this.openedWith = null;
    this.closed = false;
    this.cancelled = false;
    this.signals = [];
    this.written = [];
    this._streams();
  }

  // A real SerialPort hands out NEW streams every time it is opened, which is
  // exactly what a re-enumerated USB device does after a reset.
  _streams() {
    this.readable = new ReadableStream({
      start: (controller) => {
        this.controller = controller;
      },
      cancel: () => {
        this.cancelled = true;
      },
    });
    this.writable = new WritableStream({
      write: (chunk) => {
        this.written.push(decoder.decode(chunk));
        if (this.onWrite) this.onWrite(this.written.length, this);
      },
    });
  }

  async open(options) {
    if (this.options.openError) throw this.options.openError;
    this.opens += 1;
    this.opened = true;
    this.closed = false;
    this._streams();
    this.openedWith = options;
  }

  async close() {
    this.closed = true;
    try {
      this.controller.close();
    } catch (error) {
      /* already cancelled by the reader */
    }
  }

  async setSignals(signals) {
    this.signals.push(signals);
  }

  reply(text) {
    this.controller.enqueue(encoder.encode(text));
  }

  /** Simulate the chip resetting mid-session: the stream errors and dies. */
  reset() {
    try {
      this.controller.error(new Error('device reset'));
    } catch (error) {
      /* already gone */
    }
  }
}

function fakeSerial(ports) {
  const queue = ports.slice();
  return {
    requested: 0,
    async requestPort() {
      this.requested += 1;
      if (!queue.length) throw notFound();
      return queue.shift();
    },
    async getPorts() {
      return queue.slice();
    },
  };
}

function notFound() {
  const error = new Error('No port selected by the user.');
  error.name = 'NotFoundError';
  return error;
}

function busy() {
  const error = new Error('The port is already open.');
  error.name = 'InvalidStateError';
  return error;
}

async function expectReject(promise, code) {
  let caught = null;
  try {
    await promise;
  } catch (error) {
    caught = error;
  }
  assert.ok(caught, `expected a rejection with code ${code}`);
  assert.strictEqual(caught.code, code, `expected ${code}, got ${caught.code}`);
  return caught;
}

function makeClient(port, extra) {
  const serial = fakeSerial([port]);
  const client = new WebSerialGatewayClient(
    Object.assign({ serial, timeoutMs: 200, maxAttempts: 3, recoverDelayMs: 5 }, extra || {})
  );
  return { client, serial };
}

async function main() {
  console.log('=== RTB Website V1.1: Web Serial gateway client ===');

  // ---- 1. browser without navigator.serial -------------------------------
  section('1. unsupported browser degrades, no exception');
  await checkAsync('no serial API -> unsupported state, rejected with a code', async () => {
    const client = new WebSerialGatewayClient({ serial: null });
    check('state is unsupported before any click', () => {
      assert.strictEqual(client.snapshot().state, 'unsupported');
      assert.strictEqual(client.supported(), false);
    });
    await expectReject(client.connect(), 'unsupported');
    check('still unsupported afterwards (no error state)', () => {
      assert.strictEqual(client.snapshot().state, 'unsupported');
      assert.strictEqual(client.snapshot().error.code, 'unsupported');
    });
    check('snapshot stays renderable', () => {
      const snap = client.snapshot();
      assert.strictEqual(snap.info, null);
      assert.ok(typeof snap.error.code === 'string');
    });
  });

  // ---- 2. permission denied ---------------------------------------------
  section('2. user dismisses the port picker');
  await checkAsync('NotFoundError -> permission_denied, nothing opened', async () => {
    const serial = {
      async requestPort() {
        throw notFound();
      },
      async getPorts() {
        return [];
      },
    };
    const client = new WebSerialGatewayClient({ serial, timeoutMs: 100 });
    await expectReject(client.connect(), 'permission_denied');
    check('state is error and no port is held', () => {
      assert.strictEqual(client.snapshot().state, 'error');
      assert.strictEqual(client.snapshot().error.code, 'permission_denied');
      assert.strictEqual(client.port, null);
      assert.strictEqual(client.reader, null);
      assert.strictEqual(client.writer, null);
    });
  });

  // ---- 3. port.open fails -----------------------------------------------
  section('3. the port is already held by another program');
  await checkAsync('InvalidStateError -> busy, port released', async () => {
    const port = new FakePort({ openError: busy() });
    const { client } = makeClient(port);
    await expectReject(client.connect(), 'busy');
    check('the half-open port was closed', () => {
      assert.strictEqual(client.snapshot().state, 'error');
      assert.strictEqual(port.closed, true);
      assert.strictEqual(client.port, null);
    });
    check('the button can be used again: a second click re-enters selecting', () => {
      assert.strictEqual(client.connecting, null);
    });
  });

  // ---- 4. happy path -----------------------------------------------------
  section('4. gateway_info handshake');
  let happyPort = null;
  let happyClient = null;
  await checkAsync('debug logs are skipped, one JSON line is accepted', async () => {
    const port = new FakePort();
    const { client, serial } = makeClient(port);
    port.onWrite = () => {
      // The firmware logs boot text on the same UART before it answers.
      port.reply('Remote Thermal Printer Bridge V0.1 - compile/mock verified only\n');
      port.reply('[ctrl] usb control ready\n');
      port.reply(INFO_LINE);
    };
    const info = await client.connect();
    check('identity fields are returned verbatim', () => {
      assert.deepStrictEqual(info, {
        gateway_id: 'gw-esp32-a1b2c3',
        hardware_family: 'ESP32-WROOM-32E',
        firmware_version: '0.2.0',
        protocol_version: 1,
      });
    });
    check('state is connected and the snapshot carries the identity', () => {
      assert.strictEqual(client.snapshot().state, 'connected');
      assert.strictEqual(client.snapshot().info.gateway_id, 'gw-esp32-a1b2c3');
    });
    check('exactly one request line was written, and only gateway_info', () => {
      assert.strictEqual(serial.requested, 1, 'requestPort must run once');
      assert.deepStrictEqual(port.written, [REQUEST_LINE]);
    });
    check('the port was opened at 115200 and DTR/RTS were dropped', () => {
      assert.deepStrictEqual(port.openedWith, { baudRate: 115200 });
      assert.deepStrictEqual(port.signals[0], {
        dataTerminalReady: false,
        requestToSend: false,
      });
    });
    happyPort = port;
    happyClient = client;
  });

  await checkAsync('no generic command surface exists on the client', async () => {
    for (const name of ['send', 'sendRaw', 'sendHex', 'write', 'command', 'console']) {
      assert.strictEqual(typeof happyClient[name], 'undefined', `${name} must not exist`);
    }
  });

  // ---- 5. malformed JSON -------------------------------------------------
  section('5. malformed JSON');
  await checkAsync('a brace line that is not JSON -> invalid_response, no retry', async () => {
    const port = new FakePort();
    const { client } = makeClient(port);
    port.onWrite = () => port.reply('{"type":"gateway_info",oops}\n');
    await expectReject(client.connect(), 'invalid_response');
    check('protocol errors are final, and the port is closed', () => {
      assert.strictEqual(port.written.length, 1);
      assert.strictEqual(port.closed, true);
      assert.strictEqual(client.snapshot().state, 'error');
    });
  });

  await checkAsync('a JSON line without the required fields -> invalid_response', async () => {
    const port = new FakePort();
    const { client } = makeClient(port);
    port.onWrite = () => port.reply('{"type":"gateway_info","gateway_id":"gw-x"}\n');
    await expectReject(client.connect(), 'invalid_response');
  });

  // ---- 6. wrong message type --------------------------------------------
  section('6. wrong message type');
  await checkAsync('a different device message is refused', async () => {
    const port = new FakePort();
    const { client } = makeClient(port);
    port.onWrite = () => port.reply('{"type":"printer_scan","status":"ok"}\n');
    await expectReject(client.connect(), 'wrong_type');
    check('the scan payload never reaches state.info', () => {
      assert.strictEqual(client.snapshot().info, null);
    });
  });

  await checkAsync('a firmware error line is refused with the firmware code', async () => {
    const port = new FakePort();
    const { client } = makeClient(port);
    port.onWrite = () =>
      port.reply('{"type":"error","error":"unsupported_command","protocol_version":1}\n');
    await expectReject(client.connect(), 'gateway_refused');
  });

  // ---- 7. timeout --------------------------------------------------------
  section('7. silence on the wire');
  await checkAsync('three bounded attempts, then timeout, then release', async () => {
    const port = new FakePort();
    const { client } = makeClient(port, { timeoutMs: 40 });
    await expectReject(client.connect(), 'timeout');
    check('retries are bounded and the port is closed', () => {
      assert.strictEqual(port.written.length, 3, 'maxAttempts = 3');
      assert.strictEqual(port.written.every((line) => line === REQUEST_LINE), true);
      assert.strictEqual(port.closed, true);
      assert.strictEqual(port.cancelled, true);
      assert.strictEqual(client.snapshot().state, 'error');
    });
  });

  // ---- 8. disconnect -----------------------------------------------------
  section('8. disconnect releases the port');
  await checkAsync('reader cancelled, writer released, port closed, state reset', async () => {
    const client = happyClient;
    const port = happyPort;
    await client.disconnect();
    check('no handles survive', () => {
      assert.strictEqual(port.closed, true);
      assert.strictEqual(port.cancelled, true);
      assert.strictEqual(client.port, null);
      assert.strictEqual(client.reader, null);
      assert.strictEqual(client.writer, null);
      assert.strictEqual(client.snapshot().state, 'disconnected');
      assert.strictEqual(client.snapshot().info, null);
    });
    check('the writer lock is gone, so the OS can reopen the port', () => {
      assert.strictEqual(port.writable.locked, false);
    });
  });

  // ---- 9. reconnect ------------------------------------------------------
  section('9. reconnect after disconnect');
  await checkAsync('a second connect uses a fresh port and succeeds', async () => {
    const first = new FakePort();
    const second = new FakePort();
    const serial = fakeSerial([first, second]);
    const client = new WebSerialGatewayClient({ serial, timeoutMs: 200 });
    first.onWrite = () => first.reply(INFO_LINE);
    second.onWrite = () =>
      second.reply(
        '{"type":"gateway_info","gateway_id":"gw-esp32-ffffff","hardware_family":"ESP32-S3-WROOM-1","firmware_version":"0.3.1","protocol_version":1}\n'
      );

    const one = await client.connect();
    await client.disconnect();
    const two = await client.connect();
    check('both handshakes completed and the second id is shown', () => {
      assert.strictEqual(one.gateway_id, 'gw-esp32-a1b2c3');
      assert.strictEqual(two.gateway_id, 'gw-esp32-ffffff');
      assert.strictEqual(serial.requested, 2);
      assert.strictEqual(first.closed, true);
      assert.strictEqual(client.snapshot().state, 'connected');
    });
    await client.disconnect();
  });

  await checkAsync('double click on Connect cannot open two ports', async () => {
    const port = new FakePort();
    const serial = fakeSerial([port]);
    const client = new WebSerialGatewayClient({ serial, timeoutMs: 200 });
    port.onWrite = () => port.reply(INFO_LINE);
    const [a, b] = await Promise.all([client.connect(), client.connect()]);
    check('one requestPort, one open, same identity', () => {
      assert.strictEqual(serial.requested, 1);
      assert.deepStrictEqual(a, b);
    });
    await client.disconnect();
  });

  // ---- 10. page switch / repeated render ---------------------------------
  section('10. re-render and page switch stay safe');
  await checkAsync('repeated snapshots, double disconnect, a throwing listener', async () => {
    const port = new FakePort();
    const { client } = makeClient(port);
    port.onWrite = () => port.reply(INFO_LINE);
    const unsubscribe = client.onState(() => {
      throw new Error('a broken listener');
    });
    await client.connect();
    check('a throwing listener does not break the state machine', () => {
      assert.strictEqual(client.snapshot().state, 'connected');
    });
    check('100 renders in a row do not throw', () => {
      for (let i = 0; i < 100; i += 1) {
        const snap = client.snapshot();
        assert.ok(['connected'].includes(snap.state));
      }
    });
    unsubscribe();
    await client.disconnect();
    await client.disconnect();  // pagehide after a manual disconnect
    check('disconnect is idempotent', () => {
      assert.strictEqual(client.snapshot().state, 'disconnected');
      assert.strictEqual(port.closed, true);
    });
  });

  await checkAsync('a disconnected device mid-session flips to error, not a crash',
    async () => {
      const port = new FakePort();
      const { client } = makeClient(port);
      port.onWrite = () => port.reply(INFO_LINE);
      await client.connect();
      // Simulate the cable being pulled: the stream errors.
      port.controller.error(new Error('device removed'));
      await new Promise((resolve) => setTimeout(resolve, 30));
      check('the state machine reports device_lost', () => {
        assert.strictEqual(client.snapshot().state, 'error');
        assert.strictEqual(client.snapshot().error.code, 'device_lost');
      });
      await client.disconnect();
    });

  await checkAsync('hasAuthorizedPort is a hint, never an auto-open', async () => {
    const port = new FakePort();
    const serial = fakeSerial([port]);
    const client = new WebSerialGatewayClient({ serial, timeoutMs: 100 });
    const authorized = await client.hasAuthorizedPort();
    check('a previously used port is reported, but nothing opened', () => {
      assert.strictEqual(authorized, true);
      assert.strictEqual(port.opened, false);
      assert.strictEqual(serial.requested, 0);
      assert.strictEqual(client.snapshot().state, 'disconnected');
    });
  });

  // ---- 11. the board resets as the port opens ---------------------------
  section('11. one bounded recovery when a board resets on open');
  await checkAsync('device_lost during the handshake reopens the same port once', async () => {
    const port = new FakePort();
    const { client, serial } = makeClient(port);
    port.onWrite = (count, target) => {
      if (count === 1) {
        target.reset();  // the chip rebooted: this request never landed
        return;
      }
      target.reply(INFO_LINE);
    };
    const info = await client.connect();
    check('the retry happened on the already-authorised port', () => {
      assert.strictEqual(info.gateway_id, 'gw-esp32-a1b2c3');
      assert.strictEqual(serial.requested, 1, 'no second port picker');
      assert.strictEqual(port.opens, 2, 'the same port was reopened exactly once');
      assert.strictEqual(client.snapshot().state, 'connected');
    });
    await client.disconnect();
  });

  await checkAsync('a device that never comes back fails instead of looping', async () => {
    const port = new FakePort();
    const { client } = makeClient(port, { timeoutMs: 60, maxAttempts: 1 });
    port.onWrite = (count, target) => target.reset();
    await expectReject(client.connect(), 'device_lost');
    check('at most two opens, then it stops and releases the port', () => {
      assert.ok(port.opens <= 2, `opens=${port.opens}`);
      assert.strictEqual(client.snapshot().state, 'error');
      assert.strictEqual(port.closed, true);
      assert.strictEqual(client.port, null);
    });
  });

  await checkAsync('recovery can be turned off', async () => {
    const port = new FakePort();
    const { client } = makeClient(port, { recoverOnDeviceLost: false });
    port.onWrite = (count, target) => target.reset();
    await expectReject(client.connect(), 'device_lost');
    check('exactly one open when the caller opts out', () => {
      assert.strictEqual(port.opens, 1);
    });
  });

  console.log(`\n=== ${checks} checks, ${failures} failures ===`);
  return failures === 0 ? 0 : 1;
}

main().then(
  (code) => process.exit(code),
  (error) => {
    console.error(error);
    process.exit(1);
  }
);
