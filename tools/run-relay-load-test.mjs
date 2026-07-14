#!/usr/bin/env node

import { spawn } from "node:child_process";
import { existsSync, readFileSync, rmSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";

function value(name, fallback) {
  const index = process.argv.indexOf(name);
  return index >= 0 && index + 1 < process.argv.length ? process.argv[index + 1] : fallback;
}

function percentileUpperBound(histogram, quantile) {
  const target = Math.ceil(histogram.samples * quantile);
  let cumulative = 0;
  for (let index = 0; index < histogram.counts.length; ++index) {
    cumulative += histogram.counts[index];
    if (cumulative >= target) {
      return histogram.upper_bounds_us[index] ?? Number.POSITIVE_INFINITY;
    }
  }
  return Number.POSITIVE_INFINITY;
}

const serverExe = value("--server-exe", "build/Release/sesivo-server.exe");
const probeExe = value("--probe-exe", "build/Release/relay_load_probe.exe");
const port = value("--port", "19999");
const secret = value("--secret", "load-secret");
const serverLog = join(tmpdir(), `sesivo-relay-load-${port}.log`);
const metricsFile = join(tmpdir(), `sesivo-relay-load-metrics-${port}.jsonl`);
const secretFile = join(tmpdir(), `sesivo-relay-load-secret-${port}.txt`);
rmSync(serverLog, { force: true });
rmSync(metricsFile, { force: true });
rmSync(secretFile, { force: true });
writeFileSync(secretFile, secret, { encoding: "utf8", mode: 0o600 });
const clientCount = Number.parseInt(value("--clients", "8"), 10);
const senderCount = Number.parseInt(value("--senders", `${clientCount}`), 10);
const durationMs = Number.parseInt(value("--duration-ms", "500"), 10);
const frameCount = Number.parseInt(value("--frames", "120"), 10);
const maximumRelayDwellP999Us = Number.parseInt(
  value("--max-relay-dwell-p999-us", "2500"), 10,
);
const rounds = Math.max(1, Math.floor(durationMs * 48 / frameCount));
if (process.argv.includes("--shards")) {
  throw new Error(
    "--shards was removed because multiple probe processes add scheduler latency; " +
    "one probe owns the complete room",
  );
}
const server = spawn(serverExe, [
  "--port", port,
  "--server-id", "load-test",
  "--join-secret-file", secretFile,
  "--log-file", serverLog,
  "--metrics-jsonl", metricsFile,
], {
  stdio: ["ignore", "pipe", "pipe"],
});

let serverOutput = "";
server.stdout.on("data", (chunk) => { serverOutput += chunk.toString(); });
server.stderr.on("data", (chunk) => { serverOutput += chunk.toString(); });

function latestMetricsSnapshot() {
  if (!existsSync(metricsFile)) {
    return null;
  }
  const rows = readFileSync(metricsFile, "utf8").trim().split(/\r?\n/).filter(Boolean);
  for (let index = rows.length - 1; index >= 0; --index) {
    try {
      return JSON.parse(rows[index]);
    } catch {
      // The async exporter may still be appending the newest row; try an older complete row.
    }
  }
  return null;
}

async function waitForRelayMetrics() {
  const deadline = Date.now() + 6500;
  while (Date.now() < deadline) {
    const metrics = latestMetricsSnapshot();
    if (metrics?.schema === "jam_server_metrics_v2" &&
        Number(metrics?.relay_dwell_us?.samples) > 0) {
      return metrics;
    }
    await new Promise((resolve) => setTimeout(resolve, 50));
  }
  return latestMetricsSnapshot();
}

let exitCode = 1;
let finalMetrics = null;
try {
  await new Promise((resolve, reject) => {
    const deadline = Date.now() + 3000;
    const poll = setInterval(() => {
      if (serverOutput.includes("SFU server ready")) {
        clearInterval(poll);
        resolve();
      } else if (Date.now() >= deadline) {
        clearInterval(poll);
        reject(new Error(`server readiness timeout\n${serverOutput}`));
      }
    }, 10);
    server.once("exit", (code) => {
      clearInterval(poll);
      reject(new Error(`server exited ${code}\n${serverOutput}`));
    });
  });

  const consumed = [
    "--server-exe", "--probe-exe", "--port", "--secret", "--clients", "--senders",
    "--max-relay-dwell-p999-us",
  ];
  const passthrough = process.argv.slice(2).filter((argument, index, all) => {
    const prior = all[index - 1];
    return !consumed.includes(argument) && !consumed.includes(prior);
  });
  const probe = spawn(probeExe, [
    "--port", port,
    "--secret", secret,
    "--clients", `${clientCount}`,
    "--senders", `${senderCount}`,
    "--room-participants", `${clientCount}`,
    "--total-senders", `${senderCount}`,
    "--profile-offset", "0",
    "--start-delay-ms", "0",
    "--rounds", `${rounds}`,
    ...passthrough,
  ], { stdio: "inherit" });
  const probeExitCode = await new Promise((resolve) => probe.once("exit", resolve));
  exitCode = probeExitCode === 0 ? 0 : 1;
  finalMetrics = await waitForRelayMetrics();
} finally {
  server.kill();
  await new Promise((resolve) => {
    server.once("exit", resolve);
    setTimeout(resolve, 1000);
  });
  finalMetrics ??= latestMetricsSnapshot();
  if (finalMetrics?.schema !== "jam_server_metrics_v2" ||
      !Number.isFinite(Number(finalMetrics?.relay_dwell_us?.samples)) ||
      Number(finalMetrics.relay_dwell_us.samples) <= 0) {
    console.error("relay validation requires a v2 metrics snapshot with relay samples");
    exitCode = 1;
  } else {
    const relayDwellP999Us = percentileUpperBound(finalMetrics.relay_dwell_us, 0.999);
    console.log(JSON.stringify({ server_metrics: {
      schema: finalMetrics.schema,
      connected_clients: finalMetrics.connected_clients,
      drops: finalMetrics.drops,
      receive_handler_us: finalMetrics.receive_handler_us,
      relay_dwell_us: finalMetrics.relay_dwell_us,
      relay_dwell_p999_upper_us: relayDwellP999Us,
    } }));
    if (relayDwellP999Us > maximumRelayDwellP999Us) {
      exitCode = 1;
    }
  }
  rmSync(serverLog, { force: true });
  rmSync(metricsFile, { force: true });
  rmSync(secretFile, { force: true });
}
process.exit(exitCode ?? 1);
