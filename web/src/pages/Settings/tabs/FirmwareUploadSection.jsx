import { useCallback, useEffect, useRef, useState } from 'preact/hooks';
import { Trans, useLingui } from '@lingui/react/macro';
import Section from '../../../components/Card.jsx';

const RESTART_POLL_MS = 2000;
const RESTART_FIRST_POLL_MS = 5000;
const RESTART_TIMEOUT_MS = 90000;

export function FirmwareUploadSection() {
  const { t } = useLingui();
  const [file, setFile] = useState(null);
  const [state, setState] = useState('idle'); // idle | uploading | restarting | timeout
  const [progress, setProgress] = useState(0);
  const [error, setError] = useState('');
  const inputRef = useRef();
  const timerRef = useRef();
  const xhrRef = useRef();

  useEffect(
    () => () => {
      clearTimeout(timerRef.current);
      xhrRef.current?.abort();
    },
    [],
  );

  // The machine is unreachable while it restarts; reload once it answers so the new UI loads.
  const waitForRestart = useCallback(() => {
    setState('restarting');
    const deadline = Date.now() + RESTART_TIMEOUT_MS;
    const poll = async () => {
      try {
        const response = await fetch('/api/status', { cache: 'no-store' });
        if (response.ok) {
          location.reload();
          return;
        }
      } catch {
        // Still restarting.
      }
      if (Date.now() > deadline) {
        setState('timeout');
        return;
      }
      timerRef.current = setTimeout(poll, RESTART_POLL_MS);
    };
    timerRef.current = setTimeout(poll, RESTART_FIRST_POLL_MS);
  }, []);

  const install = useCallback(() => {
    setError('');
    setProgress(0);
    setState('uploading');

    // XMLHttpRequest because fetch cannot report upload progress.
    const xhr = new XMLHttpRequest();
    xhrRef.current = xhr;
    xhr.open('POST', '/api/firmware/upload');
    // A custom header makes browsers ask permission first, which stops other websites posting firmware here.
    xhr.setRequestHeader('X-GaggiMate-Firmware', '1');
    xhr.setRequestHeader('Content-Type', 'application/octet-stream');
    xhr.upload.onprogress = e => {
      if (e.lengthComputable) setProgress(Math.round((e.loaded * 100) / e.total));
    };
    xhr.onload = () => {
      if (xhr.status === 200) {
        waitForRestart();
      } else {
        setError(xhr.responseText || t`The machine rejected this file.`);
        setState('idle');
      }
    };
    xhr.onerror = () => {
      setError(t`The connection was lost during the upload. The machine kept its current firmware.`);
      setState('idle');
    };
    xhr.send(file);
  }, [file, t, waitForRestart]);

  const busy = state !== 'idle';

  return (
    <Section title={t`Install Firmware File`} className='h-full'>
      <div className='space-y-4'>
        <p className='text-base-content/70 text-sm'>
          <Trans>
            Install a display firmware file (.bin) from this computer over Wi-Fi. This also lets you
            go back to an earlier version. Controller firmware cannot be installed here.
          </Trans>
        </p>

        <input
          ref={inputRef}
          type='file'
          accept='.bin'
          aria-label={t`Firmware file`}
          className='file-input file-input-bordered w-full'
          disabled={busy}
          onChange={e => {
            setError('');
            setFile(e.target.files[0] ?? null);
          }}
        />

        {file && state === 'idle' && (
          <div className='text-base-content/70 text-sm'>
            {file.name} ({(file.size / (1024 * 1024)).toFixed(2)} MB)
          </div>
        )}

        {state === 'uploading' && (
          <div className='space-y-1' role='status'>
            <progress className='progress progress-primary w-full' value={progress} max='100' />
            <div className='text-sm'>
              <Trans>Uploading: {progress}%. Keep this page open.</Trans>
            </div>
          </div>
        )}
        {state === 'restarting' && (
          <div className='alert alert-success' role='status'>
            <span>
              <Trans>Firmware installed. Waiting for the machine to restart...</Trans>
            </span>
          </div>
        )}
        {state === 'timeout' && (
          <div className='alert alert-warning' role='alert'>
            <span>
              <Trans>
                The firmware was installed, but the machine has not come back yet. If it stays
                unreachable, check its screen, then reinstall over USB.
              </Trans>
            </span>
          </div>
        )}
        {error && (
          <div className='alert alert-error' role='alert'>
            <span>{error}</span>
          </div>
        )}

        <div className='alert alert-warning'>
          <span>
            <Trans>
              Back up your profiles first. Older firmware may not understand settings saved by a
              newer version, and a firmware that cannot start needs reinstalling over USB.
            </Trans>
          </span>
        </div>

        <button
          type='button'
          className='btn btn-secondary btn-sm w-full'
          disabled={!file || busy}
          onClick={install}
        >
          <Trans>Install File</Trans>
        </button>
      </div>
    </Section>
  );
}
