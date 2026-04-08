# Decaflash Cloud Worker

Cloudflare Worker for the current Brain cloud contract.

Current routes:

- `POST /api/chattie`
- `POST /api/audd`
- `GET /api/health`

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
  "artist": "Artist Name"
}
```
