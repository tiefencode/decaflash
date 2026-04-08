# Decaflash Cloud Worker

Cloudflare Worker for the current Brain cloud contract.

Current routes:

- `POST /api/chattie`
- `POST /api/audd`
- `GET /api/health`
- `GET /api/debug/last.wav`
- `GET /api/debug/last.json`

## Local Development

```bash
cd /Users/tiefencode/Projekte/decaflash/workers/decaflash
cp .dev.vars.example .dev.vars
npm install
npm run dev
```

Required local secrets in `.dev.vars`:

- `BRAIN_SHARED_SECRET`
- `AUDD_API_TOKEN`
- `OPENAI_API_KEY`
- optional: `OPENAI_MODEL`

## Cloudflare Deploy

Use the existing Worker name `decaflash` so the current `workers.dev` URL stays stable.

Recommended pragmatic path:

1. Push this repo to GitHub.
2. In Cloudflare, open `Workers & Pages`.
3. Import the existing Worker from Git.
4. Point it at this folder: `workers/decaflash`
5. Keep the Worker name `decaflash`.
6. Verify secrets under `Settings -> Variables and Secrets`.

If Cloudflare creates a new Worker project or environment, set these secrets again there.

## Debug Artifacts

`/api/audd` stores the most recent decoded WAV plus metadata for debugging.

Required Cloudflare setup:

- create a KV namespace, e.g. `decaflash-debug-artifacts`
- bind it as `DEBUG_ARTIFACTS`
- keep the binding in `wrangler.jsonc` via `kv_namespaces`

After a fresh `/api/audd` run, download the latest debug audio:

```bash
curl -fL \
  -H 'Authorization: Bearer db3953d42370ed9d7c329704988ecaa6a84a693ea84d76e8f3627d05d9f353f5' \
  'https://decaflash.tiefencode.workers.dev/api/debug/last.wav' \
  -o "$HOME/Downloads/decaflash-last.wav"
```

Download the matching metadata:

```bash
curl -fL \
  -H 'Authorization: Bearer db3953d42370ed9d7c329704988ecaa6a84a693ea84d76e8f3627d05d9f353f5' \
  'https://decaflash.tiefencode.workers.dev/api/debug/last.json' \
  -o "$HOME/Downloads/decaflash-last.json"
```

Quick sanity check on macOS:

```bash
file "$HOME/Downloads/decaflash-last.wav"
afplay "$HOME/Downloads/decaflash-last.wav"
```

## Brain Contract

`/api/chattie` expects JSON:

```json
{
  "instructions": "Prompt instructions",
  "input": "User input"
}
```

Response:

```json
{
  "text": "Kurzer Text"
}
```

`/api/audd` expects multipart form data with:

- `encoding=ima_adpcm`
- `sample_rate_hz`
- `sample_count`
- `container=decaflash_ima_adpcm`
- `file=<binary adpcm blob>`

Response:

```json
{
  "matched": true,
  "title": "Song Title",
  "artist": "Artist Name",
  "text": "Kurzer Matrix-Text"
}
```
