// Injected only by the dev server (see annotatePlugin.js). Right-click opens a note box;
// Shift+right-click keeps the browser's normal menu.
let dialog = null;
let highlighted = null;
let previousOutline = '';

function describeElement(el) {
  const parts = [];
  for (let node = el; node && node !== document.body && parts.length < 5; node = node.parentElement) {
    let part = node.tagName.toLowerCase();
    if (node.id) part += `#${node.id}`;
    const name = node.getAttribute('aria-label') || node.getAttribute('name');
    if (name) part += `[${name}]`;
    parts.unshift(part);
  }
  return parts.join(' > ');
}

function sectionOf(el) {
  const card = el.closest('section, article, form, [class*="card"], nav, aside, main');
  const heading = card?.querySelector('h1, h2, h3, h4, legend, label');
  return heading?.innerText?.slice(0, 80) ?? '';
}

function setHighlight(el) {
  if (highlighted) highlighted.style.outline = previousOutline;
  highlighted = el;
  if (el) {
    previousOutline = el.style.outline;
    el.style.outline = '3px solid #ff3030';
  }
}

function closeDialog() {
  dialog?.remove();
  dialog = null;
  setHighlight(null);
}

function toast(message, ok) {
  const el = document.createElement('div');
  el.textContent = message;
  el.style.cssText = `position:fixed;top:16px;left:50%;transform:translateX(-50%);z-index:2147483647;
    padding:8px 14px;border-radius:6px;color:#fff;font:14px sans-serif;background:${ok ? '#1e7d3c' : '#b3261e'}`;
  document.body.append(el);
  setTimeout(() => el.remove(), 2000);
}

async function save(context, note) {
  try {
    const response = await fetch('/__annotate', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ ...context, note }),
    });
    toast(response.ok ? 'Note saved' : 'Could not save note', response.ok);
  } catch {
    toast('Could not save note', false);
  }
}

function openDialog(event) {
  closeDialog();
  const target = event.target;
  const context = {
    route: location.pathname,
    x: event.clientX,
    y: event.clientY,
    viewportWidth: innerWidth,
    viewportHeight: innerHeight,
    element: describeElement(target),
    text: (target.innerText || target.value || '').slice(0, 120),
    section: sectionOf(target),
  };
  setHighlight(target);

  dialog = document.createElement('div');
  dialog.id = 'gm-annotator';
  const width = 340;
  const height = 190;
  const left = Math.min(event.clientX + 8, innerWidth - width - 8);
  const top = Math.min(event.clientY + 8, innerHeight - height - 8);
  dialog.style.cssText = `position:fixed;left:${Math.max(8, left)}px;top:${Math.max(8, top)}px;width:${width}px;
    z-index:2147483647;padding:10px;border-radius:8px;background:#202020;border:2px solid #ff3030;
    color:#fff;font:14px sans-serif;box-sizing:border-box`;

  const title = document.createElement('div');
  title.textContent = `Note: ${context.route}`;
  title.style.marginBottom = '6px';

  const textarea = document.createElement('textarea');
  textarea.placeholder = 'What should this do, and how?';
  textarea.style.cssText = `display:block;width:100%;height:104px;box-sizing:border-box;padding:6px;border-radius:4px;
    background:#101010;color:#fff;border:1px solid #555;font:14px sans-serif;resize:none`;

  const hint = document.createElement('div');
  hint.textContent = 'Enter save · Shift+Enter new line · Esc cancel';
  hint.style.cssText = 'margin-top:6px;font-size:12px;color:#b0b0b0';

  textarea.addEventListener('keydown', e => {
    e.stopPropagation();
    if (e.key === 'Escape') {
      closeDialog();
    } else if (e.key === 'Enter' && !e.shiftKey) {
      e.preventDefault();
      const note = textarea.value.trim();
      closeDialog();
      if (note) save(context, note);
    }
  });

  dialog.append(title, textarea, hint);
  document.body.append(dialog);
  textarea.focus();
}

document.addEventListener(
  'contextmenu',
  event => {
    if (event.shiftKey || event.target.closest?.('#gm-annotator')) return;
    event.preventDefault();
    openDialog(event);
  },
  true,
);
