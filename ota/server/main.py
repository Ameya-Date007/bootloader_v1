import asyncio
import json
import os
import struct
import subprocess
import uuid
from pathlib import Path
from typing import AsyncGenerator

from fastapi import FastAPI, File, Form, Header, HTTPException, UploadFile
from fastapi.middleware.cors import CORSMiddleware
from fastapi.responses import HTMLResponse, StreamingResponse
from fastapi import Query

# ---------------------------------------------------------------------------
# Configuration
# ---------------------------------------------------------------------------
RAW_DIR      = Path(os.getenv("RAW_DIR",      "/home/ameya/ota/uploads/raw"))
SIGNED_DIR   = Path(os.getenv("SIGNED_DIR",   "/home/ameya/ota/uploads/signed"))
FLASHER_DIR  = Path(os.getenv("FLASHER_DIR",  "/home/ameya/ota/flasher"))
API_KEY      = os.getenv("API_KEY",      "test001")
SIGNING_KEY  = os.getenv("SIGNING_KEY",  "000102030405060708090a0b0c0d0e0f")
ZEROED_IV    = "00000000000000000000000000000000"

# Firmware layout constants - must match the C bootloader and Python signer
AES_BLOCK_SIZE        = 16
BOOTLOADER_SIZE       = 0x8000
FWINFO_OFFSET         = 0x01B0
SIGNATURE_OFFSET      = FWINFO_OFFSET + AES_BLOCK_SIZE
FWINFO_VERSION_OFFSET = 8
FWINFO_LENGTH_OFFSET  = 12

# ---------------------------------------------------------------------------
# FastAPI app
# ---------------------------------------------------------------------------
app = FastAPI(title="OTA Firmware Server")
app.add_middleware(
    CORSMiddleware,
    allow_origins=["*"],
    allow_methods=["*"],
    allow_headers=["*"],
)

RAW_DIR.mkdir(parents=True, exist_ok=True)
SIGNED_DIR.mkdir(parents=True, exist_ok=True)

# ---------------------------------------------------------------------------
# State
# ---------------------------------------------------------------------------
# Queue holds at most one pending job so uploads are rejected while busy
_job_queue: asyncio.Queue   = asyncio.Queue(maxsize=1)
_subscribers: list[asyncio.Queue] = []   # one queue per connected SSE client
_is_busy: bool = False


# ---------------------------------------------------------------------------
# Auth
# ---------------------------------------------------------------------------
def _require_api_key(x_api_key: str) -> None:
    if x_api_key != API_KEY:
        raise HTTPException(status_code=401, detail="Invalid API key")


# ---------------------------------------------------------------------------
# SSE broadcast helpers
# ---------------------------------------------------------------------------
async def _broadcast(event_type: str, message: str, level: str = "info") -> None:
    payload = json.dumps({"type": event_type, "level": level, "message": message})
    dead = []
    for q in _subscribers:
        try:
            q.put_nowait(payload)
        except asyncio.QueueFull:
            dead.append(q)
    for q in dead:
        _subscribers.remove(q)


async def _log(message: str, level: str = "info") -> None:
    print(f"[{level.upper()}] {message}")
    await _broadcast("log", message, level)


# ---------------------------------------------------------------------------
# Firmware signing (refactored from the original main.py signer)
# ---------------------------------------------------------------------------
def _sign_firmware(raw_path: Path, version_hex: str) -> Path:
    """
    Reads raw firmware, injects version and length, computes an
    AES-CBC-MAC style signature using openssl, writes signed binary
    to SIGNED_DIR/firmware.bin and returns that path.
    """
    with open(raw_path, "rb") as f:
        f.seek(BOOTLOADER_SIZE)
        fw_image = bytearray(f.read())
        f.close()

    version_value = int(version_hex, 16)
    struct.pack_into("<I", fw_image, FWINFO_OFFSET + FWINFO_LENGTH_OFFSET, len(fw_image))
    struct.pack_into("<I", fw_image, FWINFO_OFFSET + FWINFO_VERSION_OFFSET, version_value)

    # Build the image that will be encrypted to produce the signature.
    # Layout: [fwinfo block (16 bytes)] + [everything before fwinfo] + [everything after fwinfo+signature area]
    signing_image  = bytes(fw_image[FWINFO_OFFSET : FWINFO_OFFSET + AES_BLOCK_SIZE])
    signing_image += bytes(fw_image[:FWINFO_OFFSET])
    signing_image += bytes(fw_image[FWINFO_OFFSET + AES_BLOCK_SIZE * 2:])

    tmp          = Path("/tmp")
    sign_path    = tmp / f"to_sign_{uuid.uuid4().hex}.bin"
    enc_path     = tmp / f"encrypted_{uuid.uuid4().hex}.bin"

    try:
        sign_path.write_bytes(signing_image)

        cmd = (
            f"openssl enc -aes-128-cbc -nosalt "
            f"-K {SIGNING_KEY} -iv {ZEROED_IV} "
            f"-in {sign_path} -out {enc_path}"
        )
        result = subprocess.run(cmd.split(), capture_output=True)
        if result.returncode != 0:
            raise RuntimeError(
                f"openssl failed: {result.stderr.decode().strip()}"
            )

        # Signature is the last AES block of the CBC-encrypted output
        enc_data  = enc_path.read_bytes()
        signature = enc_data[-AES_BLOCK_SIZE:]
    finally:
        sign_path.unlink(missing_ok=True)
        enc_path.unlink(missing_ok=True)

    fw_image[SIGNATURE_OFFSET : SIGNATURE_OFFSET + AES_BLOCK_SIZE] = signature

    signed_path = SIGNED_DIR / "signed.bin"
    signed_path.write_bytes(fw_image)
    return signed_path


# ---------------------------------------------------------------------------
# TypeScript flasher runner
# ---------------------------------------------------------------------------
async def _run_flasher() -> int:
    """
    Runs the compiled TypeScript flasher as a subprocess and streams
    every output line to all connected SSE clients.
    Returns the process exit code.
    """

    await asyncio.sleep(1)

    process = await asyncio.create_subprocess_exec(
       "/home/ameya/.nvm/versions/node/v24.15.0/bin/node",
        "src/index.ts",
        cwd=str(FLASHER_DIR),
        stdout=asyncio.subprocess.PIPE,
        stderr=asyncio.subprocess.STDOUT,
    )


    assert process.stdout is not None
    async for raw_line in process.stdout:
        line = raw_line.decode(errors="replace").rstrip()
        if not line:
            continue
        # Map the TypeScript logger prefixes to log levels
        if line.startswith("[$]"):
            await _log(line, level="success")
        elif line.startswith("[!]"):
            await _log(line, level="error")
        else:
            await _log(line, level="info")

    await process.wait()
    return process.returncode


# ---------------------------------------------------------------------------
# Background job worker
# ---------------------------------------------------------------------------
async def _job_worker() -> None:
    """
    Processes one job at a time from the queue.
    Each job is a tuple of (raw_path, version_hex).
    """
    global _is_busy
    while True:
        raw_path, version_hex = await _job_queue.get()
        print(raw_path)
        _is_busy = True

        try:
            # --- Step 1: Sign ---
            await _log("Signing firmware...", level="info")
            loop = asyncio.get_event_loop()
            signed_path = await loop.run_in_executor(
                None, _sign_firmware, raw_path, version_hex
            )
            sig_hex = signed_path.read_bytes()[
                SIGNATURE_OFFSET : SIGNATURE_OFFSET + AES_BLOCK_SIZE
            ].hex()
            await _log(f"Firmware signed  version={version_hex}  sig={sig_hex}", level="success")

            # --- Step 2: Flash ---
            await _log("Starting flash sequence...", level="info")
            rc = await _run_flasher()

            if rc == 0:
                await _broadcast("done", "Firmware update complete!", level="success")
            else:
                await _broadcast(
                    "done",
                    f"Flasher exited with code {rc}. Check logs above.",
                    level="error",
                )

        except Exception as exc:
            await _broadcast("done", f"Job failed: {exc}", level="error")

        finally:
            _is_busy = False
            _job_queue.task_done()
            # Remove the raw file after processing to keep the directory clean
            """
            try:
                raw_path.unlink(missing_ok=True)
            except Exception:
                pass
            """


# ---------------------------------------------------------------------------
# Startup: launch the background worker
# ---------------------------------------------------------------------------
@app.on_event("startup")
async def _startup() -> None:
    asyncio.create_task(_job_worker())


# ---------------------------------------------------------------------------
# Routes
# ---------------------------------------------------------------------------

@app.get("/status")
async def get_status() -> dict:
    """Returns whether a flash job is currently running."""
    return {"busy": _is_busy, "queued": _job_queue.qsize()}


@app.post("/upload")
async def upload_firmware(
    file:      UploadFile = File(...),
    version:   str        = Form(...),
    x_api_key: str        = Header(...),
) -> dict:
    """
    Accepts a raw firmware .bin file and a version string (hex, e.g. 0x00000001).
    Saves it to RAW_DIR, then enqueues a sign-and-flash job.
    Rejects the upload if a job is already running or queued.
    """
    _require_api_key(x_api_key)

    if _is_busy or not _job_queue.empty():
        raise HTTPException(
            status_code=409,
            detail="A flash job is already in progress. Try again once it completes.",
        )

    content = await file.read()
    print(f"DEBUG: received {len(content)} bytes")         # ? add this
    print(f"DEBUG: RAW_DIR = {RAW_DIR}")                   # ? add this
    print(f"DEBUG: RAW_DIR exists = {RAW_DIR.exists()}")   # ? add this

    # Basic firmware validation before touching the hardware
    if len(content) <= BOOTLOADER_SIZE:
        raise HTTPException(
            status_code=400,
            detail=(
                f"File too small ({len(content)} bytes). "
                f"Must be larger than {BOOTLOADER_SIZE} bytes (bootloader size)."
            ),
        )

    # Validate version string
    try:
        int(version, 16)
    except ValueError:
        raise HTTPException(
            status_code=400,
            detail=f"Invalid version '{version}'. Must be a hex string, e.g. '00000001'.",
        )

    raw_path = RAW_DIR / f"{Path(file.filename or 'firmware').name}"
    print(f"DEBUG: writing to {raw_path}")                 # ? add this
    raw_path.write_bytes(content)

    await _job_queue.put((raw_path, version))
    await _log(
        f"Job queued  file={file.filename}  version={version}  size={len(content)} bytes",
        level="info",
    )

    return {
        "queued":   True,
        "filename": file.filename,
        "version":  version,
        "size":     len(content),
    }


@app.get("/events")
async def sse_events(x_api_key: str = Query(...)) -> StreamingResponse:
    """ 
    Server-Sent Events endpoint.
    Each event is a JSON object: { type, level, message }
    Types: log | done
    Levels: info | success | error
    """
    _require_api_key(x_api_key)

    client_queue: asyncio.Queue = asyncio.Queue(maxsize=200)
    _subscribers.append(client_queue)

    async def generator() -> AsyncGenerator[str, None]:
        # Send a keep-alive comment every 15 seconds so proxies don't close the connection
        try:
            while True:
                try:
                    payload = await asyncio.wait_for(client_queue.get(), timeout=15)
                    yield f"data: {payload}\n\n"
                except asyncio.TimeoutError:
                    yield ": keep-alive\n\n"
        except asyncio.CancelledError:
            pass
        finally:
            if client_queue in _subscribers:
                _subscribers.remove(client_queue)

    return StreamingResponse(
        generator(),
        media_type="text/event-stream",
        headers={
            "Cache-Control": "no-cache",
            "X-Accel-Buffering": "no",   # disables Nginx response buffering
        },
    )


# ---------------------------------------------------------------------------
# Minimal HTML UI
# ---------------------------------------------------------------------------
HTML = """<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>OTA Flash Tool</title>
<style>
  @import url('https://fonts.googleapis.com/css2?family=JetBrains+Mono:wght@400;600;700&family=Syne:wght@700;800&display=swap');

  :root {
    --bg:      #0d0f12;
    --surface: #13161b;
    --border:  #1f242d;
    --accent:  #00e5a0;
    --warn:    #ff5f57;
    --muted:   #3a4150;
    --text:    #c8d0de;
    --bright:  #eef1f7;
  }

  *, *::before, *::after { box-sizing: border-box; margin: 0; padding: 0; }

  body {
    background: var(--bg);
    color: var(--text);
    font-family: 'JetBrains Mono', monospace;
    min-height: 100vh;
    display: flex;
    flex-direction: column;
    align-items: center;
    padding: 3rem 1.5rem;
    gap: 2rem;
  }

  header {
    text-align: center;
  }

  header h1 {
    font-family: 'Syne', sans-serif;
    font-size: clamp(1.8rem, 5vw, 2.8rem);
    font-weight: 800;
    color: var(--bright);
    letter-spacing: -0.02em;
  }

  header h1 span { color: var(--accent); }

  header p {
    margin-top: 0.5rem;
    font-size: 0.8rem;
    color: var(--muted);
    letter-spacing: 0.08em;
    text-transform: uppercase;
  }

  .card {
    background: var(--surface);
    border: 1px solid var(--border);
    border-radius: 12px;
    padding: 2rem;
    width: 100%;
    max-width: 560px;
  }

  .card h2 {
    font-family: 'Syne', sans-serif;
    font-size: 0.85rem;
    font-weight: 700;
    letter-spacing: 0.12em;
    text-transform: uppercase;
    color: var(--muted);
    margin-bottom: 1.5rem;
  }

  /* Drop zone */
  .drop-zone {
    border: 2px dashed var(--border);
    border-radius: 8px;
    padding: 2.5rem 1rem;
    text-align: center;
    cursor: pointer;
    transition: border-color 0.2s, background 0.2s;
    position: relative;
  }

  .drop-zone:hover, .drop-zone.drag-over {
    border-color: var(--accent);
    background: rgba(0, 229, 160, 0.04);
  }

  .drop-zone input[type="file"] {
    position: absolute; inset: 0; opacity: 0; cursor: pointer;
  }

  .drop-zone .icon {
    font-size: 2rem;
    margin-bottom: 0.75rem;
    display: block;
  }

  .drop-zone .label {
    font-size: 0.85rem;
    color: var(--text);
  }

  .drop-zone .label strong { color: var(--accent); }

  .drop-zone .filename {
    margin-top: 0.75rem;
    font-size: 0.75rem;
    color: var(--accent);
    min-height: 1em;
  }

  .field {
    margin-top: 1.25rem;
    display: flex;
    flex-direction: column;
    gap: 0.4rem;
  }

  .field label {
    font-size: 0.72rem;
    letter-spacing: 0.1em;
    text-transform: uppercase;
    color: var(--muted);
  }

  .field input {
    background: var(--bg);
    border: 1px solid var(--border);
    border-radius: 6px;
    color: var(--bright);
    font-family: 'JetBrains Mono', monospace;
    font-size: 0.9rem;
    padding: 0.65rem 0.9rem;
    outline: none;
    transition: border-color 0.2s;
  }

  .field input:focus { border-color: var(--accent); }

  button.flash-btn {
    margin-top: 1.75rem;
    width: 100%;
    background: var(--accent);
    color: #000;
    border: none;
    border-radius: 8px;
    font-family: 'Syne', sans-serif;
    font-size: 1rem;
    font-weight: 700;
    letter-spacing: 0.04em;
    padding: 0.9rem;
    cursor: pointer;
    transition: opacity 0.2s, transform 0.1s;
  }

  button.flash-btn:hover:not(:disabled) { opacity: 0.88; }
  button.flash-btn:active:not(:disabled) { transform: scale(0.99); }
  button.flash-btn:disabled { opacity: 0.35; cursor: not-allowed; }

  /* Status pill */
  .status-bar {
    display: flex;
    align-items: center;
    gap: 0.6rem;
    font-size: 0.78rem;
    color: var(--muted);
    margin-top: 1rem;
  }

  .dot {
    width: 8px; height: 8px;
    border-radius: 50%;
    background: var(--muted);
    flex-shrink: 0;
    transition: background 0.3s;
  }
  .dot.idle    { background: var(--muted); }
  .dot.busy    { background: var(--accent); animation: pulse 1s infinite; }
  .dot.success { background: var(--accent); }
  .dot.error   { background: var(--warn); }

  @keyframes pulse {
    0%, 100% { opacity: 1; }
    50%       { opacity: 0.35; }
  }

  /* Log terminal */
  .log-card { max-width: 560px; }

  .log-terminal {
    background: #090b0e;
    border: 1px solid var(--border);
    border-radius: 8px;
    padding: 1rem 1.25rem;
    height: 320px;
    overflow-y: auto;
    font-size: 0.78rem;
    line-height: 1.7;
    display: flex;
    flex-direction: column;
    gap: 0;
  }

  .log-terminal::-webkit-scrollbar { width: 4px; }
  .log-terminal::-webkit-scrollbar-thumb { background: var(--border); border-radius: 4px; }

  .log-line { white-space: pre-wrap; word-break: break-all; }
  .log-line.info    { color: var(--text); }
  .log-line.success { color: var(--accent); }
  .log-line.error   { color: var(--warn); }

  .log-empty {
    color: var(--muted);
    font-size: 0.75rem;
    margin: auto;
    text-align: center;
  }
</style>
</head>
<body>

<header>
  <h1>OTA <span>Flash</span> Tool</h1>
  <p>STM32 Over-the-Air Firmware Updater</p>
</header>

<!-- Upload card -->
<div class="card">
  <h2>Upload Firmware</h2>

  <div class="drop-zone" id="dropZone">
    <input type="file" id="fileInput" accept=".bin">
    <span class="icon">&#128190;</span>
    <div class="label">Drop <strong>.bin</strong> file here or click to browse</div>
    <div class="filename" id="fileName"></div>
  </div>

  <div class="field">
    <label>API Key</label>
    <input type="password" id="apiKey" placeholder="your-api-key" autocomplete="off">
  </div>

  <div class="field">
    <label>Version (hex)</label>
    <input type="text" id="version" value="00000001" placeholder="e.g. 00000001">
  </div>

  <button class="flash-btn" id="flashBtn" disabled>Flash Firmware</button>

  <div class="status-bar">
    <div class="dot idle" id="statusDot"></div>
    <span id="statusText">Idle</span>
  </div>
</div>

<!-- Log card -->
<div class="card log-card">
  <h2>Flash Log</h2>
  <div class="log-terminal" id="logTerminal">
    <div class="log-empty" id="logEmpty">Waiting for a job to start...</div>
  </div>
</div>

<script>
  const fileInput   = document.getElementById('fileInput');
  const dropZone    = document.getElementById('dropZone');
  const fileName    = document.getElementById('fileName');
  const apiKeyInput = document.getElementById('apiKey');
  const versionInput= document.getElementById('version');
  const flashBtn    = document.getElementById('flashBtn');
  const statusDot   = document.getElementById('statusDot');
  const statusText  = document.getElementById('statusText');
  const logTerminal = document.getElementById('logTerminal');
  const logEmpty    = document.getElementById('logEmpty');

  let selectedFile = null;
  let eventSource  = null;

  // --- File selection ---
  fileInput.addEventListener('change', () => {
    selectedFile = fileInput.files[0] || null;
    fileName.textContent = selectedFile ? selectedFile.name : '';
    updateFlashBtn();
  });

  dropZone.addEventListener('dragover', e => { e.preventDefault(); dropZone.classList.add('drag-over'); });
  dropZone.addEventListener('dragleave', () => dropZone.classList.remove('drag-over'));
  dropZone.addEventListener('drop', e => {
    e.preventDefault();
    dropZone.classList.remove('drag-over');
    const file = e.dataTransfer.files[0];
    if (file) { selectedFile = file; fileName.textContent = file.name; updateFlashBtn(); }
  });

  apiKeyInput.addEventListener('input', updateFlashBtn);

  function updateFlashBtn() {
    flashBtn.disabled = !selectedFile || !apiKeyInput.value.trim();
  }

  // --- Set status ---
  function setStatus(state, text) {
    statusDot.className = `dot ${state}`;
    statusText.textContent = text;
  }

  // --- Append log line ---
  function appendLog(message, level = 'info') {
    if (logEmpty) logEmpty.remove();
    const line = document.createElement('div');
    line.className = `log-line ${level}`;
    const ts = new Date().toLocaleTimeString('en-GB', { hour12: false });
    line.textContent = `[${ts}] ${message}`;
    logTerminal.appendChild(line);
    logTerminal.scrollTop = logTerminal.scrollHeight;
  }

  // --- SSE subscription ---
  function subscribeToEvents(apiKey) {
    if (eventSource) { eventSource.close(); }

    eventSource = new EventSource(`/events?x_api_key=${encodeURIComponent(apiKey)}`);

    // Note: FastAPI Header alias converts X-Api-Key -> x_api_key query param workaround
    // For production use an auth token in a cookie instead
    eventSource.onmessage = e => {
      try {
        const msg = JSON.parse(e.data);
        if (msg.type === 'log') {
          appendLog(msg.message, msg.level);
        } else if (msg.type === 'done') {
          appendLog(msg.message, msg.level);
          setStatus(msg.level === 'success' ? 'success' : 'error',
                    msg.level === 'success' ? 'Complete' : 'Failed');
          flashBtn.disabled = false;
        }
      } catch (_) {}
    };

    eventSource.onerror = () => {
      // Suppress noise from keep-alive comments being parsed as errors
    };
  }

  // --- Flash button ---
  flashBtn.addEventListener('click', async () => {
    const apiKey  = apiKeyInput.value.trim();
    const version = versionInput.value.trim();

    if (!selectedFile || !apiKey) return;

    flashBtn.disabled = true;
    setStatus('busy', 'Uploading...');
    logTerminal.innerHTML = '';
    appendLog(`Uploading ${selectedFile.name} (${(selectedFile.size / 1024).toFixed(1)} KB)`, 'info');

    // Open SSE connection before upload so no log lines are missed
    subscribeToEvents(apiKey);

    const form = new FormData();
    form.append('file', selectedFile);
    form.append('version', version);

    try {
      const res = await fetch('/upload', {
        method: 'POST',
        headers: { 'x-api-key': apiKey },
        body: form,
      });

      const data = await res.json();

      if (!res.ok) {
        appendLog(`Upload failed: ${data.detail || res.statusText}`, 'error');
        setStatus('error', 'Upload failed');
        flashBtn.disabled = false;
        return;
      }

      setStatus('busy', 'Signing & flashing...');
      appendLog(`Job queued — version 0x${version}`, 'success');

    } catch (err) {
      appendLog(`Network error: ${err.message}`, 'error');
      setStatus('error', 'Network error');
      flashBtn.disabled = false;
    }
  });
</script>
</body>
</html>
"""


@app.get("/", response_class=HTMLResponse)
async def ui() -> HTMLResponse:
    return HTMLResponse(content=HTML)
