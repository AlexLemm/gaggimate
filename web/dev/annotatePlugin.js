// Dev-server-only design notes: right-click the web UI, type what it should do, and the note lands in
// annotations/ANNOTATIONS.md next to the simulator's. `apply: 'serve'` keeps all of this out of the
// firmware bundle, which comes from `vite build`.
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const HERE = path.dirname(fileURLToPath(import.meta.url));
const NOTES_DIR = path.resolve(HERE, '../../annotations');
const NOTES_FILE = path.join(NOTES_DIR, 'ANNOTATIONS.md');
const MAX_BODY_BYTES = 64 * 1024;

const inline = value => String(value ?? '').replace(/\s+/g, ' ').trim() || '(none)';

function formatNote(n) {
  // UTC, matching the simulator's notes so the file stays in time order.
  const when = `${new Date().toISOString().slice(0, 16).replace('T', ' ')} UTC`;
  return [
    `\n## ${when} - Web UI - ${inline(n.route)}\n`,
    `- **Where:** x=${Number(n.x)}, y=${Number(n.y)} in a ${Number(n.viewportWidth)}x${Number(n.viewportHeight)} viewport`,
    `- **Element:** \`${inline(n.element)}\``,
    `- **Element text:** ${inline(n.text)}`,
    `- **Section:** ${inline(n.section)}`,
    `\n**Note:** ${String(n.note).trim()}\n`,
  ].join('\n');
}

function readBody(req) {
  return new Promise((resolve, reject) => {
    let body = '';
    req.setEncoding('utf8');
    req.on('data', chunk => {
      body += chunk;
      if (Buffer.byteLength(body) > MAX_BODY_BYTES) {
        reject(new Error('Note too large'));
        req.destroy();
      }
    });
    req.on('end', () => resolve(body));
    req.on('error', reject);
  });
}

export default function annotatePlugin() {
  return {
    name: 'gaggimate-annotate',
    apply: 'serve',
    transformIndexHtml() {
      return [
        {
          tag: 'script',
          attrs: { type: 'module' },
          children: fs.readFileSync(path.join(HERE, 'annotator.js'), 'utf8'),
          injectTo: 'body',
        },
      ];
    },
    configureServer(server) {
      server.middlewares.use('/__annotate', async (req, res) => {
        // JSON content type forces a CORS preflight, so other sites cannot post notes here.
        if (req.method !== 'POST' || !req.headers['content-type']?.startsWith('application/json')) {
          res.statusCode = 405;
          res.end();
          return;
        }
        try {
          const note = JSON.parse(await readBody(req));
          if (!String(note.note ?? '').trim()) throw new Error('Empty note');
          fs.mkdirSync(NOTES_DIR, { recursive: true });
          if (!fs.existsSync(NOTES_FILE)) {
            fs.writeFileSync(
              NOTES_FILE,
              '# GaggiMate design notes\n\nWritten by right-clicking the simulator or the web UI dev server.\n',
            );
          }
          fs.appendFileSync(NOTES_FILE, formatNote(note));
          res.statusCode = 204;
          res.end();
        } catch (err) {
          res.statusCode = 400;
          res.end(err.message);
        }
      });
    },
  };
}
