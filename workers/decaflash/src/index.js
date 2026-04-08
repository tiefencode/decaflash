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

  if (!env.OPENAI_API_KEY) {
    return jsonResponse({ error: "missing_openai_api_key" }, 500);
  }

  const body = await request.json();
  const input = typeof body?.input === "string" ? body.input.trim() : "";
  const instructions = typeof body?.instructions === "string" ? body.instructions.trim() : "";

  if (!input || !instructions) {
    return jsonResponse({ error: "invalid_request" }, 400);
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

  return jsonResponse({ text });
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

  if (encoding) {
    const sampleRateHz = integerField(form.get("sample_rate_hz"));
    const sampleCount = integerField(form.get("sample_count"));
    const container = stringField(form.get("container"));

    if (encoding !== "ima_adpcm" || container !== "decaflash_ima_adpcm") {
      return jsonResponse({ error: "unsupported_audio_encoding" }, 400);
    }
    if (!sampleRateHz || !sampleCount) {
      return jsonResponse({ error: "missing_audio_metadata" }, 400);
    }

    const compressed = new Uint8Array(await file.arrayBuffer());
    const pcmSamples = decodeDecaflashImaAdpcm(compressed, sampleCount);
    const wavBytes = buildMono16BitWav(pcmSamples, sampleRateHz);
    uploadFile = new File([wavBytes], "recording.wav", { type: "audio/wav" });
  }

  const auddForm = new FormData();
  auddForm.set("api_token", env.AUDD_API_TOKEN);
  auddForm.set("file", uploadFile);

  const response = await fetch("https://api.audd.io/", {
    method: "POST",
    body: auddForm,
  });

  if (!response.ok) {
    return await upstreamJsonError("audd_failed", response);
  }

  const data = await response.json();
  const result = data?.result;
  if (!result) {
    return jsonResponse({ matched: false });
  }

  return jsonResponse({
    matched: true,
    title: typeof result.title === "string" ? result.title : "",
    artist: typeof result.artist === "string" ? result.artist : "",
    album: typeof result.album === "string" ? result.album : "",
    release_date: typeof result.release_date === "string" ? result.release_date : "",
  });
}

function stringField(value) {
  return typeof value === "string" ? value.trim() : "";
}

function integerField(value) {
  const parsed = Number.parseInt(stringField(value), 10);
  return Number.isFinite(parsed) && parsed > 0 ? parsed : 0;
}

function clamp(value, min, max) {
  return Math.min(Math.max(value, min), max);
}

function decodeDecaflashImaAdpcm(bytes, sampleCount) {
  if (!(bytes instanceof Uint8Array) || bytes.length < 4) {
    throw new Error("invalid_ima_adpcm_payload");
  }

  const predictor = (bytes[0] | (bytes[1] << 8));
  let statePredictor = predictor > 0x7fff ? predictor - 0x10000 : predictor;
  let stateIndex = clamp(bytes[2], 0, 88);

  const totalSamples = sampleCount || (1 + Math.max(0, bytes.length - 4) * 2);
  const output = new Int16Array(totalSamples);
  output[0] = statePredictor;

  let outputIndex = 1;
  for (let i = 4; i < bytes.length && outputIndex < totalSamples; i += 1) {
    const packed = bytes[i];
    output[outputIndex++] = decodeNibble(packed & 0x0f);
    if (outputIndex < totalSamples) {
      output[outputIndex++] = decodeNibble((packed >> 4) & 0x0f);
    }
  }

  return output;

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
