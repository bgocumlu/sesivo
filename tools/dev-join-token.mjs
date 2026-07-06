#!/usr/bin/env node

import crypto from "node:crypto";

const defaults = {
  server: "127.0.0.1",
  port: "9999",
  serverId: "local-dev",
  codec: "opus",
  frames: "120",
  ttlMs: "120000",
  roomInstance: "",
  accessEpoch: "0",
  client: "./build/Debug/client.exe",
};

function parseArgs(argv) {
  const options = { ...defaults };
  for (let i = 0; i < argv.length; i += 1) {
    const arg = argv[i];
    const next = argv[i + 1];
    if (arg === "--secret" && next) options.secret = argv[++i];
    else if (arg === "--server-id" && next) options.serverId = argv[++i];
    else if (arg === "--server" && next) options.server = argv[++i];
    else if (arg === "--port" && next) options.port = argv[++i];
    else if (arg === "--room" && next) options.room = argv[++i];
    else if ((arg === "--user" || arg === "--user-id") && next) options.user = argv[++i];
    else if (arg === "--display-name" && next) options.displayName = argv[++i];
    else if (arg === "--codec" && next) options.codec = argv[++i];
    else if (arg === "--frames" && next) options.frames = argv[++i];
    else if (arg === "--ttl-ms" && next) options.ttlMs = argv[++i];
    else if (arg === "--room-instance" && next) options.roomInstance = argv[++i];
    else if (arg === "--access-epoch" && next) options.accessEpoch = argv[++i];
    else if (arg === "--client" && next) options.client = argv[++i];
  }
  return options;
}

function usage() {
  console.error(
    [
      "usage:",
      "  node tools/dev-join-token.mjs --secret <secret> --room <room> --user <user> --display-name <name>",
      "",
      "optional:",
      "  --server-id local-dev --server 127.0.0.1 --port 9999 --codec opus --frames 120 --ttl-ms 120000",
      "  --room-instance <id> --access-epoch <n>",
    ].join("\n"),
  );
}

function shellQuote(value) {
  if (/^[A-Za-z0-9_./:=+-]+$/.test(value)) return value;
  return `"${value.replaceAll("\\", "\\\\").replaceAll('"', '\\"')}"`;
}

function claimField(value) {
  const text = String(value);
  return `${Buffer.byteLength(text, "utf8")}:${text}`;
}

function tokenPayload(fields) {
  return fields.map(claimField).join("");
}

function base64Url(value) {
  return Buffer.from(value, "utf8")
    .toString("base64")
    .replaceAll("+", "-")
    .replaceAll("/", "_")
    .replace(/=+$/u, "");
}

const options = parseArgs(process.argv.slice(2));
if (!options.secret || !options.room || !options.user) {
  usage();
  process.exit(2);
}

const displayName = options.displayName ?? options.user;
const expiresAtMs = Date.now() + Number.parseInt(options.ttlMs, 10);
const nonce = crypto.randomBytes(16).toString("hex");
const payload = [
  expiresAtMs,
  options.serverId,
  options.room,
  options.user,
  options.roomInstance,
  options.accessEpoch,
  nonce,
];
const encodedPayload = tokenPayload(payload);
const signature = crypto
  .createHmac("sha256", options.secret)
  .update(`v2|${encodedPayload}`)
  .digest("hex");
const token = ["v2", base64Url(encodedPayload), signature].join(".");

const command = [
  options.client,
  "--server",
  options.server,
  "--port",
  options.port,
  "--room",
  options.room,
  "--user-id",
  options.user,
  "--display-name",
  displayName,
  "--join-token",
  token,
  "--codec",
  options.codec,
  "--frames",
  options.frames,
].map(shellQuote).join(" ");

console.log("Token:");
console.log(token);
console.log("");
console.log("Client command:");
console.log(command);
