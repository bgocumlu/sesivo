#!/usr/bin/env node

import fs from "node:fs";
import os from "node:os";
import path from "node:path";

const DEFAULT_SAMPLE_RATE = 48000;

function usage(exitCode = 0) {
  const text = [
    "usage:",
    "  node tools/latency-measurement.mjs click --out click.wav [--count 5]",
    "  node tools/latency-measurement.mjs loopback --input recording.wav --reference-channel 1 --received-channel 2 [--in-app-e2e-ms 18.4] [--out report.json]",
    "  node tools/latency-measurement.mjs diagnostics --log client.log [--assert] [threshold flags] [--out report.json]",
    "  node tools/latency-measurement.mjs matrix --manifest latency-matrix.json [--out summary.json]",
    "  node tools/latency-measurement.mjs self-test",
    "",
    "threshold flags:",
    "  --max-e2e-avg-ms N --max-e2e-peak-ms N --max-plc-frames N --max-underruns N",
    "  --max-age-limit-drops N --max-sequence-unresolved-gaps N --max-callback-over-deadline N",
    "  --max-callback-deadline-ms N --max-callback-max-ms N",
    "  --max-drift-ppm-abs N --max-queue-drift-packets N --min-e2e-samples N",
  ].join("\n");
  const stream = exitCode === 0 ? process.stdout : process.stderr;
  stream.write(`${text}\n`);
  process.exit(exitCode);
}

function parseArgs(argv) {
  const options = { _: [] };
  for (let i = 0; i < argv.length; i += 1) {
    const arg = argv[i];
    if (!arg.startsWith("--")) {
      options._.push(arg);
      continue;
    }
    const eq = arg.indexOf("=");
    if (eq >= 0) {
      options[arg.slice(2, eq)] = arg.slice(eq + 1);
      continue;
    }
    const key = arg.slice(2);
    const next = argv[i + 1];
    if (next === undefined || next.startsWith("--")) {
      options[key] = true;
    } else {
      options[key] = next;
      i += 1;
    }
  }
  return options;
}

function numberOption(options, key, fallback) {
  if (options[key] === undefined || options[key] === true) return fallback;
  const value = Number(options[key]);
  if (!Number.isFinite(value)) {
    throw new Error(`--${key} must be a number`);
  }
  return value;
}

function intOption(options, key, fallback) {
  return Math.trunc(numberOption(options, key, fallback));
}

function requirePath(options, key) {
  if (!options[key] || options[key] === true) {
    throw new Error(`missing --${key}`);
  }
  return String(options[key]);
}

function ensureParentDir(filePath) {
  const dir = path.dirname(path.resolve(filePath));
  fs.mkdirSync(dir, { recursive: true });
}

function writeJsonMaybe(report, outPath) {
  const json = `${JSON.stringify(report, null, 2)}\n`;
  if (outPath) {
    ensureParentDir(outPath);
    fs.writeFileSync(outPath, json);
  } else {
    process.stdout.write(json);
  }
}

function writeWav16(filePath, sampleRate, channels, frames) {
  const frameCount = frames.length;
  const blockAlign = channels * 2;
  const dataSize = frameCount * blockAlign;
  const buffer = Buffer.alloc(44 + dataSize);
  buffer.write("RIFF", 0, "ascii");
  buffer.writeUInt32LE(36 + dataSize, 4);
  buffer.write("WAVE", 8, "ascii");
  buffer.write("fmt ", 12, "ascii");
  buffer.writeUInt32LE(16, 16);
  buffer.writeUInt16LE(1, 20);
  buffer.writeUInt16LE(channels, 22);
  buffer.writeUInt32LE(sampleRate, 24);
  buffer.writeUInt32LE(sampleRate * blockAlign, 28);
  buffer.writeUInt16LE(blockAlign, 32);
  buffer.writeUInt16LE(16, 34);
  buffer.write("data", 36, "ascii");
  buffer.writeUInt32LE(dataSize, 40);

  let offset = 44;
  for (const frame of frames) {
    for (let ch = 0; ch < channels; ch += 1) {
      const clamped = Math.max(-1, Math.min(1, frame[ch] ?? 0));
      const sample = Math.round(clamped * 32767);
      buffer.writeInt16LE(sample, offset);
      offset += 2;
    }
  }

  ensureParentDir(filePath);
  fs.writeFileSync(filePath, buffer);
}

function commandClick(options) {
  const out = requirePath(options, "out");
  const sampleRate = intOption(options, "sample-rate", DEFAULT_SAMPLE_RATE);
  const durationMs = numberOption(options, "duration-ms", 3000);
  const startMs = numberOption(options, "start-ms", 250);
  const spacingMs = numberOption(options, "spacing-ms", 500);
  const count = intOption(options, "count", 5);
  const clickMs = numberOption(options, "click-ms", 1.0);
  const amplitude = numberOption(options, "amplitude", 0.9);
  const totalFrames = Math.max(1, Math.round((durationMs * sampleRate) / 1000));
  const pulseFrames = Math.max(1, Math.round((clickMs * sampleRate) / 1000));
  const frames = Array.from({ length: totalFrames }, () => [0]);

  for (let click = 0; click < count; click += 1) {
    const startFrame = Math.round(((startMs + spacingMs * click) * sampleRate) / 1000);
    for (let n = 0; n < pulseFrames && startFrame + n < totalFrames; n += 1) {
      const envelope =
        pulseFrames === 1 ? 1 : 0.5 - 0.5 * Math.cos((2 * Math.PI * n) / (pulseFrames - 1));
      const sign = n % 2 === 0 ? 1 : -1;
      frames[startFrame + n][0] += amplitude * envelope * sign;
    }
  }

  writeWav16(out, sampleRate, 1, frames);
  const outJson = options["out-json"];
  writeJsonMaybe({
    schema: "sesivo_latency_measurement_click_v1",
    out,
    sampleRate,
    durationMs,
    startMs,
    spacingMs,
    count,
    clickMs,
  }, outJson && outJson !== true ? String(outJson) : undefined);
}

function readRiffWav(filePath) {
  const buffer = fs.readFileSync(filePath);
  if (buffer.length < 44 || buffer.toString("ascii", 0, 4) !== "RIFF" ||
      buffer.toString("ascii", 8, 12) !== "WAVE") {
    throw new Error(`${filePath} is not a RIFF/WAVE file`);
  }

  let fmt = null;
  let data = null;
  let offset = 12;
  while (offset + 8 <= buffer.length) {
    const id = buffer.toString("ascii", offset, offset + 4);
    const size = buffer.readUInt32LE(offset + 4);
    const start = offset + 8;
    const end = start + size;
    if (end > buffer.length) {
      throw new Error(`truncated WAV chunk ${id}`);
    }
    if (id === "fmt ") {
      fmt = {
        audioFormat: buffer.readUInt16LE(start),
        channels: buffer.readUInt16LE(start + 2),
        sampleRate: buffer.readUInt32LE(start + 4),
        byteRate: buffer.readUInt32LE(start + 8),
        blockAlign: buffer.readUInt16LE(start + 12),
        bitsPerSample: buffer.readUInt16LE(start + 14),
      };
    } else if (id === "data") {
      data = { offset: start, size };
    }
    offset = end + (size % 2);
  }

  if (!fmt || !data) {
    throw new Error(`${filePath} is missing fmt or data chunk`);
  }
  if (![1, 3].includes(fmt.audioFormat)) {
    throw new Error(`unsupported WAV format ${fmt.audioFormat}; use PCM or IEEE float`);
  }
  if (fmt.channels < 1) {
    throw new Error("WAV must have at least one channel");
  }
  if (fmt.bitsPerSample % 8 !== 0) {
    throw new Error("WAV bits per sample must be byte-aligned");
  }

  const bytesPerSample = fmt.bitsPerSample / 8;
  const frameCount = Math.floor(data.size / fmt.blockAlign);

  function sampleAt(frame, channel) {
    if (frame < 0 || frame >= frameCount || channel < 0 || channel >= fmt.channels) {
      return 0;
    }
    const sampleOffset = data.offset + frame * fmt.blockAlign + channel * bytesPerSample;
    if (fmt.audioFormat === 3 && fmt.bitsPerSample === 32) {
      return buffer.readFloatLE(sampleOffset);
    }
    if (fmt.audioFormat !== 1) {
      throw new Error("only 32-bit float WAV is supported for IEEE float");
    }
    if (fmt.bitsPerSample === 8) {
      return (buffer.readUInt8(sampleOffset) - 128) / 128;
    }
    if (fmt.bitsPerSample === 16) {
      return buffer.readInt16LE(sampleOffset) / 32768;
    }
    if (fmt.bitsPerSample === 24) {
      let value = buffer.readUIntLE(sampleOffset, 3);
      if (value & 0x800000) value |= 0xff000000;
      return (value << 0) / 8388608;
    }
    if (fmt.bitsPerSample === 32) {
      return buffer.readInt32LE(sampleOffset) / 2147483648;
    }
    throw new Error(`unsupported PCM bit depth ${fmt.bitsPerSample}`);
  }

  return { ...fmt, frameCount, sampleAt };
}

function detectTransient(wav, channel, options = {}) {
  if (channel < 0 || channel >= wav.channels) {
    throw new Error(`channel ${channel + 1} is outside WAV channel count ${wav.channels}`);
  }
  const minThreshold = options.minThreshold ?? 0.02;
  const thresholdRatio = options.thresholdRatio ?? 0.35;
  const baselineFrames = Math.min(
    wav.frameCount,
    Math.max(1, Math.round((50 * wav.sampleRate) / 1000)),
  );

  let baselineEnergy = 0;
  for (let i = 0; i < baselineFrames; i += 1) {
    const sample = wav.sampleAt(i, channel);
    baselineEnergy += sample * sample;
  }
  const baselineRms = Math.sqrt(baselineEnergy / baselineFrames);

  let peakAbs = 0;
  let peakFrame = 0;
  for (let i = 0; i < wav.frameCount; i += 1) {
    const abs = Math.abs(wav.sampleAt(i, channel));
    if (abs > peakAbs) {
      peakAbs = abs;
      peakFrame = i;
    }
  }

  const threshold = Math.max(minThreshold, baselineRms * 12, peakAbs * thresholdRatio);
  for (let i = 0; i < wav.frameCount; i += 1) {
    if (Math.abs(wav.sampleAt(i, channel)) >= threshold) {
      return {
        channel: channel + 1,
        onsetFrame: i,
        onsetMs: (i * 1000) / wav.sampleRate,
        peakFrame,
        peakMs: (peakFrame * 1000) / wav.sampleRate,
        peakAbs,
        baselineRms,
        threshold,
      };
    }
  }
  throw new Error(`no transient detected on channel ${channel + 1}`);
}

function buildLoopbackReport(options) {
  const input = requirePath(options, "input");
  const wav = readRiffWav(input);
  const referenceChannel = intOption(options, "reference-channel", 1) - 1;
  const receivedChannel = intOption(options, "received-channel", 2) - 1;
  const detectOptions = {
    minThreshold: numberOption(options, "min-threshold", 0.02),
    thresholdRatio: numberOption(options, "threshold-ratio", 0.35),
  };
  const reference = detectTransient(wav, referenceChannel, detectOptions);
  const received = detectTransient(wav, receivedChannel, detectOptions);
  const measuredMs = received.onsetMs - reference.onsetMs;
  const report = {
    schema: "sesivo_latency_measurement_loopback_v1",
    input,
    sampleRate: wav.sampleRate,
    channels: wav.channels,
    reference,
    received,
    measuredMs,
  };
  if (options["in-app-e2e-ms"] !== undefined) {
    const inAppMs = numberOption(options, "in-app-e2e-ms", 0);
    report.inAppE2eMs = inAppMs;
    report.inAppCalibrationDeltaMs = measuredMs - inAppMs;
  }
  if (options["max-measured-ms"] !== undefined) {
    const budget = numberOption(options, "max-measured-ms", 0);
    report.maxMeasuredMs = budget;
    report.ok = measuredMs <= budget;
    report.failures = report.ok ? [] : [`measuredMs ${measuredMs.toFixed(3)} > ${budget}`];
  } else {
    report.ok = measuredMs >= 0;
    report.failures = report.ok ? [] : ["received transient arrived before reference transient"];
  }
  return report;
}

function commandLoopback(options) {
  const report = buildLoopbackReport(options);
  writeJsonMaybe(report, options.out);
  if (report.failures.length > 0) {
    process.exit(1);
  }
}

function maybeNumber(value) {
  const parsed = Number(value);
  return Number.isFinite(parsed) ? parsed : 0;
}

function parseDiagnosticsLog(text) {
  const latencyRows = [];
  const participantRows = [];
  const playoutRows = [];

  for (const line of text.split(/\r?\n/u)) {
    const latency = /Latency diag: callback_ms last\/avg\/max\/deadline=([-0-9.]+)\/([-0-9.]+)\/([-0-9.]+)\/([-0-9.]+) over=(\d+).*?txq_ms opus=([-0-9.]+)\/([-0-9.]+)\/([-0-9.]+) opus_p99=([-0-9.]+)/u.exec(line);
    if (latency) {
      latencyRows.push({
        callbackLastMs: maybeNumber(latency[1]),
        callbackAvgMs: maybeNumber(latency[2]),
        callbackMaxMs: maybeNumber(latency[3]),
        callbackDeadlineMs: maybeNumber(latency[4]),
        callbackOverDeadline: maybeNumber(latency[5]),
        opusSendQueueLastMs: maybeNumber(latency[6]),
        opusSendQueueAvgMs: maybeNumber(latency[7]),
        opusSendQueueMaxMs: maybeNumber(latency[8]),
        opusSendQueueP99Ms: maybeNumber(latency[9]),
      });
      continue;
    }

    const participant = /Participant diag\s+(\d+):.*?q=(\d+) q_avg=(\d+) q_max=(\d+) q_drift=([-0-9.]+).*?jitter_buffer=(\d+) queue_limit=(\d+) frames pkt\/cb=(\d+)\/(\d+) decoded_frames=(\d+) decoded_packets=(\d+) age_avg_ms=([-0-9.]+) e2e_avg_ms=([-0-9.]+) e2e_max_ms=([-0-9.]+)(?: e2e_samples=(\d+))? drift_ppm last\/avg\/max=([-0-9.]+)\/([-0-9.]+)\/([-0-9.]+) underruns=(\d+)(?: plc=(\d+))? drops q\/age=(\d+)\/(\d+) drop_detail limit\/age\/overflow=(\d+)\/(\d+)\/(\d+) seq gap\/recovered\/unresolved\/late=(\d+)\/(\d+)\/(\d+)\/(\d+) target_trim=(\d+)/u.exec(line);
    if (participant) {
      participantRows.push({
        id: maybeNumber(participant[1]),
        queueSize: maybeNumber(participant[2]),
        queueAvg: maybeNumber(participant[3]),
        queueMax: maybeNumber(participant[4]),
        queueDriftPackets: maybeNumber(participant[5]),
        jitterBufferPackets: maybeNumber(participant[6]),
        queueLimitPackets: maybeNumber(participant[7]),
        packetFrames: maybeNumber(participant[8]),
        callbackFrames: maybeNumber(participant[9]),
        decodedFrames: maybeNumber(participant[10]),
        decodedPackets: maybeNumber(participant[11]),
        ageAvgMs: maybeNumber(participant[12]),
        e2eAvgMs: maybeNumber(participant[13]),
        e2eMaxMs: maybeNumber(participant[14]),
        e2eSamples: maybeNumber(participant[15]),
        driftPpmLast: maybeNumber(participant[16]),
        driftPpmAvg: maybeNumber(participant[17]),
        driftPpmAbsMax: maybeNumber(participant[18]),
        underruns: maybeNumber(participant[19]),
        plcFrames: maybeNumber(participant[20]),
        jitterDepthDrops: maybeNumber(participant[21]),
        jitterAgeDrops: maybeNumber(participant[22]),
        opusQueueLimitDrops: maybeNumber(participant[23]),
        opusAgeLimitDrops: maybeNumber(participant[24]),
        opusDecodeOverflowDrops: maybeNumber(participant[25]),
        sequenceGaps: maybeNumber(participant[26]),
        sequenceGapRecoveries: maybeNumber(participant[27]),
        sequenceUnresolvedGaps: maybeNumber(participant[28]),
        sequenceLateOrReordered: maybeNumber(participant[29]),
        opusTargetTrimDrops: maybeNumber(participant[30]),
      });
      continue;
    }

    const playout = /Participant playout rates\s+(\d+):.*?ratio=([-0-9.]+) correction_callbacks=(\d+) drops limit\/age\/overflow\/target=([-0-9.]+)\/([-0-9.]+)\/([-0-9.]+)\/([-0-9.]+)\/s/u.exec(line);
    if (playout) {
      playoutRows.push({
        id: maybeNumber(playout[1]),
        ratio: maybeNumber(playout[2]),
        correctionCallbacks: maybeNumber(playout[3]),
        queueLimitDropRate: maybeNumber(playout[4]),
        ageLimitDropRate: maybeNumber(playout[5]),
        decodeOverflowDropRate: maybeNumber(playout[6]),
        targetTrimDropRate: maybeNumber(playout[7]),
      });
    }
  }

  const latestParticipants = new Map();
  for (const row of participantRows) {
    latestParticipants.set(row.id, row);
  }
  const latestPlayout = new Map();
  for (const row of playoutRows) {
    latestPlayout.set(row.id, row);
  }

  const participants = [...latestParticipants.values()];
  const latencyLatest = latencyRows.at(-1) ?? null;

  const aggregate = {
    participantCount: participants.length,
    latencySamples: latencyRows.length,
    participantSamples: participantRows.length,
    callbackDeadlineMs: Math.max(0, ...latencyRows.map((row) => row.callbackDeadlineMs)),
    callbackOverDeadline: Math.max(0, ...latencyRows.map((row) => row.callbackOverDeadline)),
    callbackMaxMs: Math.max(0, ...latencyRows.map((row) => row.callbackMaxMs)),
    e2eAvgMsMax: Math.max(0, ...participants.map((row) => row.e2eAvgMs)),
    e2ePeakMsMax: Math.max(0, ...participants.map((row) => row.e2eMaxMs)),
    e2eSamples: participants.reduce((sum, row) => sum + row.e2eSamples, 0),
    plcFrames: participants.reduce((sum, row) => sum + row.plcFrames, 0),
    underruns: participants.reduce((sum, row) => sum + row.underruns, 0),
    ageLimitDrops: participants.reduce((sum, row) => sum + row.opusAgeLimitDrops, 0),
    sequenceUnresolvedGaps: participants.reduce((sum, row) => sum + row.sequenceUnresolvedGaps, 0),
    driftPpmAbsMax: Math.max(0, ...participants.map((row) => Math.abs(row.driftPpmAbsMax))),
    queueDriftPacketsAbsMax: Math.max(0, ...participants.map((row) => Math.abs(row.queueDriftPackets))),
  };

  return {
    schema: "sesivo_latency_measurement_diagnostics_v1",
    aggregate,
    latencyLatest,
    participants,
    playout: [...latestPlayout.values()],
  };
}

const THRESHOLDS = {
  "max-e2e-avg-ms": ["max", "e2eAvgMsMax"],
  "max-e2e-peak-ms": ["max", "e2ePeakMsMax"],
  "max-plc-frames": ["max", "plcFrames"],
  "max-underruns": ["max", "underruns"],
  "max-age-limit-drops": ["max", "ageLimitDrops"],
  "max-sequence-unresolved-gaps": ["max", "sequenceUnresolvedGaps"],
  "max-callback-over-deadline": ["max", "callbackOverDeadline"],
  "max-callback-deadline-ms": ["max", "callbackDeadlineMs"],
  "max-drift-ppm-abs": ["max", "driftPpmAbsMax"],
  "max-queue-drift-packets": ["max", "queueDriftPacketsAbsMax"],
  "max-callback-max-ms": ["max", "callbackMaxMs"],
  "min-e2e-samples": ["min", "e2eSamples"],
  "min-participant-samples": ["min", "participantSamples"],
  "min-latency-samples": ["min", "latencySamples"],
};

function thresholdValues(options, extra = {}) {
  const values = { ...extra };
  for (const key of Object.keys(THRESHOLDS)) {
    if (options[key] !== undefined) {
      values[key] = numberOption(options, key, 0);
    }
  }
  return values;
}

function applyThresholds(report, thresholds) {
  const failures = [];
  for (const [key, value] of Object.entries(thresholds)) {
    const rule = THRESHOLDS[key];
    if (!rule) {
      failures.push(`unknown threshold '${key}'`);
      continue;
    }
    const [direction, field] = rule;
    const actual = report.aggregate[field];
    if (!Number.isFinite(actual)) {
      failures.push(`missing aggregate field '${field}'`);
    } else if (direction === "max" && actual > value) {
      failures.push(`${field} ${actual} > ${value}`);
    } else if (direction === "min" && actual < value) {
      failures.push(`${field} ${actual} < ${value}`);
    }
  }
  return failures;
}

function buildDiagnosticsReport(logPath, thresholds) {
  const text = fs.readFileSync(logPath, "utf8");
  const report = parseDiagnosticsLog(text);
  report.log = logPath;
  report.thresholds = thresholds;
  report.failures = applyThresholds(report, thresholds);
  report.ok = report.failures.length === 0;
  return report;
}

function commandDiagnostics(options) {
  const logPath = requirePath(options, "log");
  let thresholds = {};
  if (options.thresholds && options.thresholds !== true) {
    thresholds = JSON.parse(fs.readFileSync(String(options.thresholds), "utf8"));
  }
  thresholds = thresholdValues(options, thresholds);
  const report = buildDiagnosticsReport(logPath, thresholds);
  writeJsonMaybe(report, options.out);
  if (options.assert && !report.ok) {
    process.exit(1);
  }
}

function resolveManifestPath(manifestPath, value) {
  if (path.isAbsolute(value)) return value;
  return path.resolve(path.dirname(manifestPath), value);
}

function commandMatrix(options) {
  const manifestPath = requirePath(options, "manifest");
  const manifest = JSON.parse(fs.readFileSync(manifestPath, "utf8"));
  if (!Array.isArray(manifest.runs)) {
    throw new Error("matrix manifest must contain a runs array");
  }

  const runs = manifest.runs.map((run) => {
    if (!run.name) throw new Error("matrix run is missing name");
    if (!run.log) throw new Error(`matrix run '${run.name}' is missing log`);
    const logPath = resolveManifestPath(manifestPath, run.log);
    const report = buildDiagnosticsReport(logPath, run.thresholds ?? {});
    return {
      name: run.name,
      profile: run.profile ?? "",
      impairment: run.impairment ?? {},
      log: logPath,
      ok: report.ok,
      failures: report.failures,
      aggregate: report.aggregate,
      thresholds: report.thresholds,
    };
  });

  const summary = {
    schema: "sesivo_latency_measurement_matrix_v1",
    manifest: manifestPath,
    ok: runs.every((run) => run.ok),
    runs,
  };
  writeJsonMaybe(summary, options.out);
  if (!summary.ok) {
    process.exit(1);
  }
}

function assert(condition, message) {
  if (!condition) {
    throw new Error(`self-test failed: ${message}`);
  }
}

function commandSelfTest() {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), "sesivo-latency-measurement-"));
  const wavPath = path.join(dir, "loopback.wav");
  const sampleRate = DEFAULT_SAMPLE_RATE;
  const frames = Array.from({ length: sampleRate }, () => [0, 0]);
  const referenceFrame = Math.round(0.1 * sampleRate);
  const receivedFrame = referenceFrame + Math.round(0.0235 * sampleRate);
  frames[referenceFrame][0] = 0.9;
  frames[receivedFrame][1] = 0.9;
  writeWav16(wavPath, sampleRate, 2, frames);

  const loopback = buildLoopbackReport({
    input: wavPath,
    "reference-channel": "1",
    "received-channel": "2",
    "in-app-e2e-ms": "20",
  });
  assert(Math.abs(loopback.measuredMs - 23.5) < 0.05, "loopback delta should be measured");
  assert(Math.abs(loopback.inAppCalibrationDeltaMs - 3.5) < 0.05,
         "in-app calibration delta should be measured");

  const logPath = path.join(dir, "client.log");
  fs.writeFileSync(logPath, [
    "Latency diag: callback_ms last/avg/max/deadline=1.000/1.200/2.000/5.000 over=0 txq_ms opus=0.100/0.200/0.300 opus_p99=0.250 encode_ms=0.100/0.100/0.200 send_pace_ms=2.500/2.500/2.700 rx_decode_ms=0.100/0.100/0.200 rx_playout_ms=0.050/0.060/0.080",
    "Participant diag 7: ready=1 q=4 q_avg=3 q_max=5 q_drift=0.20 jitter_buffer=4 queue_limit=24 frames pkt/cb=120/120 decoded_frames=120 decoded_packets=500 age_avg_ms=4.0 e2e_avg_ms=14.5 e2e_max_ms=19.0 e2e_samples=500 drift_ppm last/avg/max=1.0/0.5/2.0 underruns=0 plc=0 drops q/age=0/0 drop_detail limit/age/overflow=0/0/0 seq gap/recovered/unresolved/late=0/0/0/0 target_trim=0 drop_rate opus/q/age=0.0/0.0/0.0/s",
    "Participant playout rates 7: decoded_packets=100.0/s ratio=1.0000 correction_callbacks=0 drops limit/age/overflow/target=0.0/0.0/0.0/0.0/s",
    "",
  ].join("\n"));

  const diagnostics = buildDiagnosticsReport(logPath, {
    "max-plc-frames": 0,
    "max-underruns": 0,
    "max-age-limit-drops": 0,
    "max-sequence-unresolved-gaps": 0,
    "max-callback-over-deadline": 0,
    "max-callback-deadline-ms": 5,
    "max-e2e-avg-ms": 20,
    "min-e2e-samples": 100,
  });
  assert(diagnostics.ok, "diagnostics thresholds should pass");
  assert(diagnostics.aggregate.e2eSamples === 500, "e2e samples should parse");
  assert(diagnostics.aggregate.callbackDeadlineMs === 5,
         "callback deadline should parse");

  const deadlineFailure = buildDiagnosticsReport(logPath, {
    "max-callback-deadline-ms": 4,
  });
  assert(!deadlineFailure.ok, "callback deadline threshold should fail");

  const manifestPath = path.join(dir, "matrix.json");
  fs.writeFileSync(manifestPath, `${JSON.stringify({
    runs: [{
      name: "low-clean",
      profile: "low",
      log: "client.log",
      thresholds: { "max-plc-frames": 0, "max-underruns": 0 },
    }],
  })}\n`);
  const matrixSummaryPath = path.join(dir, "summary.json");
  commandMatrix({ manifest: manifestPath, out: matrixSummaryPath });
  const matrixSummary = JSON.parse(fs.readFileSync(matrixSummaryPath, "utf8"));
  assert(matrixSummary.ok, "matrix summary should pass");

  fs.rmSync(dir, { recursive: true, force: true });
  process.stdout.write("latency measurement tool self-test passed\n");
}

function main() {
  const [command, ...rest] = process.argv.slice(2);
  if (!command || command === "help" || command === "--help" || command === "-h") {
    usage(0);
  }
  const options = parseArgs(rest);
  if (command === "click") commandClick(options);
  else if (command === "loopback") commandLoopback(options);
  else if (command === "diagnostics") commandDiagnostics(options);
  else if (command === "matrix") commandMatrix(options);
  else if (command === "self-test") commandSelfTest();
  else usage(2);
}

try {
  main();
} catch (error) {
  process.stderr.write(`${error.message}\n`);
  process.exit(1);
}
