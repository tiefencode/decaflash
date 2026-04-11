const IMA_STEP_TABLE = [
  7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21,
  23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66,
  73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209,
  230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658,
  724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066,
  2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484,
  7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350,
  22385, 24623, 27086, 29794, 32767,
];

const IMA_INDEX_TABLE = [
  -1, -1, -1, -1, 2, 4, 6, 8,
  -1, -1, -1, -1, 2, 4, 6, 8,
];
const IMA_BLOCK_SAMPLE_COUNT = 256;
const DEBUG_WAV_KV_KEY = "debug:last:wav:v2";
const DEBUG_JSON_KV_KEY = "debug:last:json";
const CHATTIE_SONG_PROMPT =
  "Schreibe genau einen sehr kurzen kreativen deutschen Text fuer eine 5x5-LED-Matrix. " +
  "Nutze nur Songtitel und Artist als Inspiration. Keine Lyrics. Keine Zitate. " +
  "Keine Erklaerung. Keine Emojis. Maximal 8 Woerter.";

export default {
  async fetch(request, env) {
    const url = new URL(request.url);

    if (request.method === "OPTIONS") {
      return new Response(null, { status: 204 });
    }

    if (url.pathname === "/api/health") {
      return jsonResponse({ ok: true, worker: "decaflash" });
    }

    const authError = authorize(request, env);
    if (authError) {
      return authError;
    }

    try {
      if (url.pathname === "/api/debug/last.wav") {
        return await getDebugAsset(env, {
          kvKey: DEBUG_WAV_KV_KEY,
          type: "arrayBuffer",
          contentType: "audio/wav",
          contentDisposition: 'attachment; filename="decaflash-last.wav"',
        });
      }

      if (url.pathname === "/api/debug/last.json") {
        return await getDebugAsset(env, {
          kvKey: DEBUG_JSON_KV_KEY,
          type: "text",
          contentType: "application/json; charset=utf-8",
        });
      }

      if (url.pathname === "/api/chattie") {
        return await handleChattie(request, env);
      }

      if (url.pathname === "/api/audd") {
        return await handleAudd(request, env);
      }

      return jsonResponse({ error: "not_found" }, 404);
    } catch (error) {
      return jsonResponse(
        {
          error: "internal_error",
          detail: error instanceof Error ? error.message : String(error),
        },
        500,
      );
    }
  },
};

function authorize(request, env) {
  const expected = env.BRAIN_SHARED_SECRET?.trim();
  if (!expected) {
    return jsonResponse({ error: "missing_worker_secret" }, 500);
  }

  const actual = request.headers.get("authorization") || "";
  if (actual !== `Bearer ${expected}`) {
    return jsonResponse({ error: "unauthorized" }, 401);
  }

  return null;
}

async function handleChattie(request, env) {
  if (request.method !== "POST") {
    return jsonResponse({ error: "method_not_allowed" }, 405);
  }

  const body = await request.json();
  const input = typeof body?.input === "string" ? body.input.trim() : "";
  const instructions = typeof body?.instructions === "string" ? body.instructions.trim() : "";

  if (!input || !instructions) {
    return jsonResponse({ error: "invalid_request" }, 400);
  }

  const generated = await generateChattieText(env, input, instructions);
  if (generated instanceof Response) {
    return generated;
  }

  return jsonResponse({ text: generated });
}

async function handleAudd(request, env) {
  if (request.method !== "POST") {
    return jsonResponse({ error: "method_not_allowed" }, 405);
  }

  if (!env.AUDD_API_TOKEN) {
    return jsonResponse({ error: "missing_audd_api_token" }, 500);
  }

  const form = await request.formData();
  const file = form.get("file");
  if (!(file instanceof File)) {
    return jsonResponse({ error: "missing_file" }, 400);
  }

  const encoding = stringField(form.get("encoding"));
  let uploadFile = file;
  let debugWavBytes = null;
  const requestInfo = {
    encoding: encoding || "passthrough",
    original_file_bytes: file.size,
  };

  if (encoding) {
    const sampleRateHz = integerField(form.get("sample_rate_hz"));
    const sampleCount = integerField(form.get("sample_count"));
    const container = stringField(form.get("container"));
    if (!sampleRateHz || !sampleCount) {
      return jsonResponse({ error: "missing_audio_metadata" }, 400);
    }

    const compressed = new Uint8Array(await file.arrayBuffer());
    let pcmSamples;

    if (encoding === "ima_adpcm" && container === "decaflash_ima_adpcm") {
      pcmSamples = decodeDecaflashImaAdpcm(compressed, sampleCount);
    } else if (encoding === "mulaw" && container === "decaflash_mulaw") {
      pcmSamples = decodeDecaflashMuLaw(compressed, sampleCount);
    } else {
      return jsonResponse({ error: "unsupported_audio_encoding" }, 400);
    }

    const pcmSummary = summarizePcm(pcmSamples);
    const wavBytes = buildMono16BitWav(pcmSamples, sampleRateHz);
    debugWavBytes = wavBytes;
    uploadFile = new File([wavBytes], "recording.wav", { type: "audio/wav" });

    requestInfo.container = container;
    requestInfo.sample_rate_hz = sampleRateHz;
    requestInfo.sample_count = sampleCount;
    requestInfo.decoded_samples = pcmSamples.length;
    requestInfo.wav_bytes = wavBytes.byteLength;
    requestInfo.pcm_min = pcmSummary.min;
    requestInfo.pcm_max = pcmSummary.max;
    requestInfo.pcm_abs_peak = pcmSummary.absPeak;
    requestInfo.pcm_avg_abs = pcmSummary.avgAbs;
  }

  const auddForm = new FormData();
  auddForm.set("api_token", env.AUDD_API_TOKEN);
  auddForm.set("file", uploadFile);

  const response = await fetch("https://api.audd.io/", {
    method: "POST",
    body: auddForm,
  });

  if (!response.ok) {
    console.error(`audd=upstream_http_error status=${response.status}`);
    await storeDebugArtifacts(env, requestInfo, debugWavBytes, {
      upstream_http_status: response.status,
      matched: false,
    });
    return await upstreamJsonError("audd_failed", response);
  }

  const data = await response.json();
  const result = data?.result;
  const upstreamInfo = {
    status: data?.status ?? "",
    matched: Boolean(result),
    error: data?.error ?? null,
    title: typeof result?.title === "string" ? result.title : "",
    artist: typeof result?.artist === "string" ? result.artist : "",
  };

  if (!result) {
    await storeDebugArtifacts(env, requestInfo, debugWavBytes, upstreamInfo);
    return jsonResponse({ matched: false });
  }

  const title = typeof result.title === "string" ? result.title : "";
  const artist = typeof result.artist === "string" ? result.artist : "";
  const generated = await generateChattieText(
    env,
    buildSongTextInput(title, artist),
    CHATTIE_SONG_PROMPT,
  );
  if (generated instanceof Response) {
    upstreamInfo.text = "";
    await storeDebugArtifacts(env, requestInfo, debugWavBytes, upstreamInfo);
    return generated;
  }

  upstreamInfo.text = generated;
  await storeDebugArtifacts(env, requestInfo, debugWavBytes, upstreamInfo);

  return jsonResponse({
    matched: true,
    title,
    artist,
    album: typeof result.album === "string" ? result.album : "",
    release_date: typeof result.release_date === "string" ? result.release_date : "",
    text: generated,
  });
}

function buildSongTextInput(title, artist) {
  return `Titel: ${title}\nArtist: ${artist}`;
}

async function generateChattieText(env, input, instructions) {
  if (!env.OPENAI_API_KEY) {
    return jsonResponse({ error: "missing_openai_api_key" }, 500);
  }

  const response = await fetch("https://api.openai.com/v1/responses", {
    method: "POST",
    headers: {
      Authorization: `Bearer ${env.OPENAI_API_KEY}`,
      "Content-Type": "application/json",
    },
    body: JSON.stringify({
      model: env.OPENAI_MODEL || "gpt-4.1-mini",
      instructions,
      input,
      max_output_tokens: 80,
    }),
  });

  if (!response.ok) {
    return await upstreamJsonError("openai_failed", response);
  }

  const data = await response.json();
  const text = extractResponseText(data).trim();
  if (!text) {
    return jsonResponse({ error: "openai_empty_text" }, 502);
  }

  return text;
}

function stringField(value) {
  return typeof value === "string" ? value.trim() : "";
}

function integerField(value) {
  const parsed = Number.parseInt(stringField(value), 10);
  return Number.isFinite(parsed) && parsed > 0 ? parsed : 0;
}

async function getDebugAsset(env, options) {
  const debugStore = getDebugStore(env);
  if (!debugStore) {
    return jsonResponse({ error: "debug_store_unconfigured" }, 500);
  }

  const stored = await debugStore.get(options.kvKey, options.type);
  if (stored === null) {
    return jsonResponse({ error: "debug_asset_not_found" }, 404);
  }

  const headers = new Headers({
    "cache-control": "no-store",
    "content-type": options.contentType,
  });
  if (options.contentDisposition) {
    headers.set("content-disposition", options.contentDisposition);
  }

  return new Response(stored, { headers });
}

async function storeDebugArtifacts(env, requestInfo, wavBytes, upstreamInfo) {
  if (!(wavBytes instanceof Uint8Array) || wavBytes.byteLength === 0) {
    return;
  }

  const metadata = {
    ...requestInfo,
    upstream: upstreamInfo,
    stored_at: new Date().toISOString(),
  };

  const debugStore = getDebugStore(env);
  if (!debugStore) {
    console.error("debug=store_unconfigured");
    return;
  }

  const wavBuffer = wavBytes.buffer.slice(
    wavBytes.byteOffset,
    wavBytes.byteOffset + wavBytes.byteLength,
  );

  await debugStore.put(DEBUG_WAV_KV_KEY, wavBuffer);
  await debugStore.put(DEBUG_JSON_KV_KEY, JSON.stringify(metadata, null, 2));

  console.log(
    `debug=stored wav_bytes=${wavBytes.byteLength} sample_count=${requestInfo.sample_count || 0} matched=${Boolean(upstreamInfo?.matched)}`,
  );
}

function getDebugStore(env) {
  return env?.DEBUG_ARTIFACTS &&
    typeof env.DEBUG_ARTIFACTS.get === "function" &&
    typeof env.DEBUG_ARTIFACTS.put === "function"
    ? env.DEBUG_ARTIFACTS
    : null;
}

function clamp(value, min, max) {
  return Math.min(Math.max(value, min), max);
}

function summarizePcm(samples) {
  let min = 32767;
  let max = -32768;
  let absPeak = 0;
  let absSum = 0;

  for (let i = 0; i < samples.length; i += 1) {
    const sample = samples[i];
    if (sample < min) {
      min = sample;
    }
    if (sample > max) {
      max = sample;
    }
    const abs = Math.abs(sample);
    if (abs > absPeak) {
      absPeak = abs;
    }
    absSum += abs;
  }

  return {
    min,
    max,
    absPeak,
    avgAbs: samples.length > 0 ? Math.round(absSum / samples.length) : 0,
  };
}

function decodeDecaflashMuLaw(bytes, sampleCount) {
  if (!(bytes instanceof Uint8Array) || bytes.length === 0) {
    throw new Error("invalid_mulaw_payload");
  }

  const totalSamples = sampleCount || bytes.length;
  const output = new Int16Array(totalSamples);
  const limit = Math.min(bytes.length, totalSamples);
  for (let i = 0; i < limit; i += 1) {
    output[i] = decodeMuLawByte(bytes[i]);
  }
  return output;
}

function decodeMuLawByte(value) {
  value = (~value) & 0xff;
  const sign = value & 0x80;
  const exponent = (value >> 4) & 0x07;
  const mantissa = value & 0x0f;
  let magnitude = ((mantissa << 3) + 0x84) << exponent;
  magnitude -= 0x84;
  return sign ? -magnitude : magnitude;
}

function decodeDecaflashImaAdpcm(bytes, sampleCount) {
  if (!(bytes instanceof Uint8Array) || bytes.length < 4) {
    throw new Error("invalid_ima_adpcm_payload");
  }

  const totalSamples = sampleCount || estimateBlockDecodedSamples(bytes.length);
  const output = new Int16Array(totalSamples);

  let readIndex = 0;
  let outputIndex = 0;

  while ((readIndex + 4) <= bytes.length && outputIndex < totalSamples) {
    const predictor = (bytes[readIndex] | (bytes[readIndex + 1] << 8));
    let statePredictor = predictor > 0x7fff ? predictor - 0x10000 : predictor;
    let stateIndex = clamp(bytes[readIndex + 2], 0, 88);
    readIndex += 4;

    const blockSamples = Math.min(IMA_BLOCK_SAMPLE_COUNT, totalSamples - outputIndex);
    output[outputIndex++] = statePredictor;

    for (let blockIndex = 1; blockIndex < blockSamples; ) {
      if (readIndex >= bytes.length) {
        throw new Error("truncated_ima_adpcm_block");
      }

      const packed = bytes[readIndex++];
      output[outputIndex++] = decodeNibble(packed & 0x0f);
      blockIndex += 1;

      if (blockIndex < blockSamples) {
        output[outputIndex++] = decodeNibble((packed >> 4) & 0x0f);
        blockIndex += 1;
      }
    }

    function decodeNibble(nibble) {
      const step = IMA_STEP_TABLE[stateIndex];
      let diff = step >> 3;

      if ((nibble & 4) !== 0) {
        diff += step;
      }
      if ((nibble & 2) !== 0) {
        diff += step >> 1;
      }
      if ((nibble & 1) !== 0) {
        diff += step >> 2;
      }

      if ((nibble & 8) !== 0) {
        statePredictor -= diff;
      } else {
        statePredictor += diff;
      }

      statePredictor = clamp(statePredictor, -32768, 32767);
      stateIndex = clamp(stateIndex + IMA_INDEX_TABLE[nibble & 0x0f], 0, 88);
      return statePredictor;
    }
  }

  return output;
}

function estimateBlockDecodedSamples(byteLength) {
  let totalSamples = 0;
  let remaining = byteLength;
  while (remaining >= 4) {
    const encodedBytes = Math.min(128, remaining - 4);
    totalSamples += 1 + (encodedBytes * 2);
    remaining -= 4 + encodedBytes;
  }
  return totalSamples;
}

function buildMono16BitWav(samples, sampleRateHz) {
  const pcmByteLength = samples.length * 2;
  const totalLength = 44 + pcmByteLength;
  const buffer = new ArrayBuffer(totalLength);
  const view = new DataView(buffer);
  const bytes = new Uint8Array(buffer);

  writeAscii(bytes, 0, "RIFF");
  view.setUint32(4, 36 + pcmByteLength, true);
  writeAscii(bytes, 8, "WAVE");
  writeAscii(bytes, 12, "fmt ");
  view.setUint32(16, 16, true);
  view.setUint16(20, 1, true);
  view.setUint16(22, 1, true);
  view.setUint32(24, sampleRateHz, true);
  view.setUint32(28, sampleRateHz * 2, true);
  view.setUint16(32, 2, true);
  view.setUint16(34, 16, true);
  writeAscii(bytes, 36, "data");
  view.setUint32(40, pcmByteLength, true);

  let offset = 44;
  for (let i = 0; i < samples.length; i += 1) {
    view.setInt16(offset, samples[i], true);
    offset += 2;
  }

  return bytes;
}

function writeAscii(buffer, offset, text) {
  for (let i = 0; i < text.length; i += 1) {
    buffer[offset + i] = text.charCodeAt(i);
  }
}

function extractResponseText(data) {
  if (typeof data?.output_text === "string" && data.output_text) {
    return data.output_text;
  }

  const output = Array.isArray(data?.output) ? data.output : [];
  const parts = [];
  for (const item of output) {
    const content = Array.isArray(item?.content) ? item.content : [];
    for (const entry of content) {
      if (typeof entry?.text === "string" && entry.text) {
        parts.push(entry.text);
      }
    }
  }

  return parts.join("").trim();
}

async function upstreamJsonError(label, response) {
  let detail = "";
  try {
    detail = await response.text();
  } catch {
    detail = "";
  }

  return jsonResponse(
    {
      error: label,
      status: response.status,
      detail: detail.slice(0, 400),
    },
    502,
  );
}

function jsonResponse(body, status = 200) {
  return new Response(JSON.stringify(body), {
    status,
    headers: {
      "content-type": "application/json; charset=utf-8",
      "cache-control": "no-store",
    },
  });
}
