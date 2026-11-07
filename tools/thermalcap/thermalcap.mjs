// thermalcap -- records the machine's live telemetry to a CSV so the boiler's parameters can be identified from
// something other than a shot.
//
// WHY THIS EXISTS. shotsim fits the boiler to .slog files, and a shot log has two gaps that no amount of fitting can
// close. It does not record heater duty, so the element's contribution has to be reconstructed from the control law;
// and a shot is only ever thirty seconds of a boiler being disturbed, which cannot separate the standing loss from
// the mixing between the two halves -- their ratio IS the idle gradient, so a shot sees only the ratio.
//
// Both gaps close with a longer, duller recording. The websocket already publishes everything needed, twice a second:
//
//   hp   heater power, percent -- the duty, MEASURED, so nothing has to be inferred
//   ct   the thermocouple
//   tt   the profile target, BEFORE the temperature offset and before either compensation
//   tro  the offset thermal recovery is asking for; tbo the same for banking
//   fl   pump flow
//   mt/mbt/mnp  what the firmware's own live model believes, for comparison
//
// So: leave this running over a cooldown with the heater off and the standing loss is the only term left, which pins
// UA. Leave it running over a warm-up from cold and the duty is a known 100 % the whole way up, which pins the total
// heat capacity. The two together pin both, and only then does the split between the halves mean anything.
//
// Usage:
//   node tools/thermalcap/thermalcap.mjs --host 192.168.1.177 --out cooldown.csv --minutes 30 --note "heater off"
//   node tools/thermalcap/thermalcap.mjs --out warmup.csv --note "from cold, standby -> 93"
//
// Ctrl-C stops and flushes, so --minutes is a backstop rather than the plan. Rows are appended as they arrive and the
// file is valid at every moment, because a capture that is lost when the run is interrupted is a capture nobody makes.
//
// What it does NOT do: it never writes a setting and never starts a process. Turning the heater on or off, and
// deciding when the machine is cold, are the operator's business -- this only watches.
import fs from 'node:fs';
import process from 'node:process';

// Fields copied into the header so a capture is self-describing: the real setpoint is tt + offset + max(tbo, tro),
// and that arithmetic needs the offset. Deliberately an allowlist -- /api/settings also returns the wifi credentials,
// which have no business in a file that gets passed around.
const SETTING_KEYS = [
  'pid',
  'temperatureOffset',
  'inletWaterTemp',
  'boilerHeatCapacity',
  'maxThermalBank',
  'maxThermalRecovery',
  'targetWaterTemp',
];

// Columns, in order. Kept short and fixed so the C++ side can read it by name without a CSV library.
const COLUMNS = ['t_s', 'ct', 'tt', 'fl', 'hp', 'tro', 'tbo', 'trd', 'pr', 'mt', 'mbt', 'mnp', 'ew'];

function parseArgs(argv) {
  const opt = { host: '192.168.1.177', out: null, minutes: 0, note: '', quiet: false };
  for (let i = 2; i < argv.length; i++) {
    const a = argv[i];
    const next = () => {
      if (i + 1 >= argv.length) throw new Error(`${a} needs a value`);
      return argv[++i];
    };
    if (a === '--host') opt.host = next().replace(/^https?:\/\//, '').replace(/\/$/, '');
    else if (a === '--out' || a === '-o') opt.out = next();
    else if (a === '--minutes') opt.minutes = Number(next());
    else if (a === '--note') opt.note = next();
    else if (a === '--quiet') opt.quiet = true;
    else if (a === '--help' || a === '-h') return null;
    else throw new Error(`unknown option ${a}`);
  }
  if (!opt.out) throw new Error('--out is required');
  return opt;
}

function usage() {
  console.log(`thermalcap -- record live telemetry to a CSV for shotsim --capture

  --host <addr>    machine address (default 192.168.1.177)
  --out <file>     CSV to write (required)
  --minutes <n>    stop after n minutes (default: run until Ctrl-C)
  --note <text>    free text stored in the header; say what the machine was doing
  --quiet          no progress line

What to capture, and why:

  cooldown   heater off, machine hot, no flow. Pins the standing loss, which a shot cannot.
  warm-up    from cold to the working temperature. Duty is a known 100 %, which pins the heat capacity.
  idle       sitting at temperature, doing nothing. The duty here IS the standing loss, a second reading of it.
  flush      a draw with no basket, logged well past the end. Shows the delayed sag a shot log cuts off.

Then: .pio/build/shotsim/program --capture cooldown.csv --capture warmup.csv`);
}

async function fetchSettings(host) {
  try {
    const r = await fetch(`http://${host}/api/settings`, { signal: AbortSignal.timeout(10000) });
    if (!r.ok) return { error: `HTTP ${r.status}` };
    const all = await r.json();
    const kept = {};
    for (const k of SETTING_KEYS) if (k in all) kept[k] = all[k];
    return kept;
  } catch (e) {
    return { error: String(e.message || e) };
  }
}

function main() {
  let opt;
  try {
    opt = parseArgs(process.argv);
  } catch (e) {
    console.error(`thermalcap: ${e.message}`);
    process.exit(2);
  }
  if (!opt) {
    usage();
    return;
  }

  fetchSettings(opt.host).then((settings) => {
    const out = fs.createWriteStream(opt.out, { flags: 'w' });
    out.write('# thermalcap 1\n');
    out.write(`# host=${opt.host}\n`);
    out.write(`# started=${new Date().toISOString()}\n`);
    if (opt.note) out.write(`# note=${opt.note.replace(/[\r\n]+/g, ' ')}\n`);
    for (const [k, v] of Object.entries(settings)) out.write(`# ${k}=${v}\n`);
    out.write(`${COLUMNS.join(',')}\n`);

    const url = `ws://${opt.host}/ws`;
    const t0 = Date.now();
    let rows = 0;
    let drops = 0;
    let lastReport = 0;
    let lastRowAt = t0;
    let worstGapS = 0;
    let stopping = false;
    let ws = null;
    let retryDelayMs = 250;

    const finish = (code, why) => {
      if (stopping) return;
      stopping = true;
      try {
        ws?.close();
      } catch {
        // Already closed; the stream still needs flushing.
      }
      const span = (Date.now() - t0) / 1000;
      out.write(`# ended=${new Date().toISOString()} reason=${why}\n`);
      out.write(`# rows=${rows} spanS=${span.toFixed(1)} reconnects=${drops} worstGapS=${worstGapS.toFixed(1)}\n`);
      out.end(() => {
        console.error(
          `\nthermalcap: ${why} -- ${rows} rows over ${span.toFixed(0)} s, ` +
            `${drops} reconnect${drops === 1 ? '' : 's'}, worst gap ${worstGapS.toFixed(1)} s -> ${opt.out}`
        );
        process.exit(code);
      });
    };

    const onMessage = (ev) => {
      let msg;
      try {
        msg = JSON.parse(typeof ev.data === 'string' ? ev.data : Buffer.from(ev.data).toString('utf8'));
      } catch {
        return;
      }
      // Both the fast telemetry and the slow state snapshot are published as evt:status; only the fast one carries a
      // temperature, and a snapshot written as a row would read as a boiler at 0 C.
      if (msg?.tp !== 'evt:status' || typeof msg.ct !== 'number') return;
      const now = Date.now();
      const t = (now - t0) / 1000;
      const gap = (now - lastRowAt) / 1000;
      if (rows > 0 && gap > worstGapS) worstGapS = gap;
      lastRowAt = now;
      // A missing field is written as an empty cell rather than a zero: a firmware without the experimental observers
      // publishes no mt, and a zero there would read as a boiler at 0 C.
      const cells = COLUMNS.map((c) => {
        if (c === 't_s') return t.toFixed(3);
        const v = msg[c];
        return typeof v === 'number' && Number.isFinite(v) ? String(v) : '';
      });
      out.write(`${cells.join(',')}\n`);
      rows++;

      if (!opt.quiet && now - lastReport > 5000) {
        lastReport = now;
        const f = (v, d = 1) => (typeof v === 'number' ? v.toFixed(d) : '--');
        process.stderr.write(
          `\r  ${t.toFixed(0).padStart(5)}s  ct ${f(msg.ct, 2)}  tt ${f(msg.tt)}  duty ${f(msg.hp)}%` +
            `  flow ${f(msg.fl, 2)}  recovery ${f(msg.tro, 2)}C  rows ${rows}  gaps ${drops}   `
        );
      }
      if (opt.minutes > 0 && t >= opt.minutes * 60) finish(0, 'reached --minutes');
    };

    // The machine drops a websocket client that falls behind its send queue, which it does to a listener that only
    // listens -- roughly every six seconds here. The web UI copes by reconnecting with backoff and never mentioning
    // it, so a capture has to do the same or it records six seconds and stops. Each reconnect costs a sample or two;
    // the gaps are counted and the worst one is recorded in the footer, because a fit over a capture with a
    // half-minute hole in it should say so rather than interpolate across it.
    const open = () => {
      if (stopping) return;
      ws = new WebSocket(url);
      ws.addEventListener('open', () => {
        retryDelayMs = 250;
        if (!opt.quiet && rows === 0) {
          console.error(`thermalcap: connected to ${url}, recording to ${opt.out} (Ctrl-C to stop)`);
        }
      });
      ws.addEventListener('message', onMessage);
      ws.addEventListener('error', () => {
        // The event carries no useful detail in Node; the close that follows is where the retry is scheduled.
      });
      ws.addEventListener('close', () => {
        if (stopping) return;
        drops++;
        // Reconnecting forever is right once a capture is under way, but not before the first row: a wrong --host
        // would otherwise sit there retrying in silence instead of saying so.
        if (rows === 0 && drops >= 5) {
          finish(1, `could not record from ${url}`);
          return;
        }
        setTimeout(open, retryDelayMs);
        retryDelayMs = Math.min(retryDelayMs * 2, 5000);
      });
    };
    open();

    process.on('SIGINT', () => finish(0, 'stopped'));
    process.on('SIGTERM', () => finish(0, 'stopped'));
  });
}

main();
