#!/usr/bin/env node

import crypto from "node:crypto";
import fs from "node:fs";
import { spawn } from "node:child_process";
import path from "node:path";
import { fileURLToPath } from "node:url";

const repoRoot = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..");

function abs(relativePath) {
  return path.resolve(repoRoot, relativePath);
}

function firstExisting(candidates) {
  return candidates.find((candidate) => fs.existsSync(abs(candidate))) ?? candidates[0];
}

function defaultClientExe() {
  if (process.platform === "win32") {
    return firstExisting(["build/Debug/sesivo.exe", "build/Release/sesivo.exe"]);
  }
  if (process.platform === "darwin") {
    return firstExisting([
      "build/sesivo.app/Contents/MacOS/sesivo",
      "build/Debug/sesivo.app/Contents/MacOS/sesivo",
      "build/Release/sesivo.app/Contents/MacOS/sesivo",
    ]);
  }
  return firstExisting(["build/sesivo", "build/Debug/sesivo", "build/Release/sesivo"]);
}

function defaultServerExe() {
  if (process.platform === "win32") {
    return firstExisting(["build/Debug/sesivo-server.exe", "build/Release/sesivo-server.exe"]);
  }
  return firstExisting(["build/sesivo-server", "build/Debug/sesivo-server", "build/Release/sesivo-server"]);
}

// Edit these defaults for your local dev loop.
const DEV = {
  serverHost: "127.0.0.1",
  port: process.env.JAM_DEV_PORT ?? "9999",
  serverId: "local-dev",
  secret: "dev-secret",
  mediaSecret: process.env.JAM_MEDIA_SECRET ?? "dev-media-secret",
  codec: "opus",
  frames: "120",
  ttlMs: 10 * 60 * 1000,
  roomInstance: "",
  accessEpoch: "0",
  serverExe: process.env.JAM_SERVER_EXE ?? defaultServerExe(),
  clientExe: process.env.JAM_CLIENT_EXE ?? defaultClientExe(),
  clients: {
    a: { room: "room-a", user: "user-a1", displayName: "User A1" },
    b: { room: "room-a", user: "user-a2", displayName: "User A2" },
    c: { room: "room-b", user: "user-b1", displayName: "User B1" },
  },
};

function executablePath(value) {
  return path.isAbsolute(value) ? value : abs(value);
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

function tokenFor(client) {
  const expiresAtMs = Date.now() + DEV.ttlMs;
  const nonce = crypto.randomBytes(16).toString("hex");
  const payload = [
    expiresAtMs,
    DEV.serverId,
    client.room,
    client.user,
    DEV.roomInstance,
    DEV.accessEpoch,
    nonce,
  ];
  const encodedPayload = tokenPayload(payload);
  const signature = crypto
    .createHmac("sha256", DEV.secret)
    .update(`v2|${encodedPayload}`)
    .digest("hex");
  return ["v2", base64Url(encodedPayload), signature].join(".");
}

function run(command, args) {
  if (!fs.existsSync(command)) {
    console.error(`missing executable: ${command}`);
    console.error("build the target first, or set JAM_CLIENT_EXE / JAM_SERVER_EXE");
    process.exit(2);
  }
  console.log([command, ...args].join(" "));
  const child = spawn(command, args, {
    cwd: repoRoot,
    stdio: "inherit",
    windowsHide: false,
  });
  child.on("exit", (code, signal) => {
    if (signal) process.exit(1);
    process.exit(code ?? 0);
  });
}

function usage() {
  console.log(
    [
      "usage:",
      "  node tools/dev-jam.mjs server",
      "  node tools/dev-jam.mjs client a",
      "  node tools/dev-jam.mjs client b",
      "  node tools/dev-jam.mjs client c",
      "",
      "defaults live at the top of tools/dev-jam.mjs",
      "env overrides: JAM_CLIENT_EXE, JAM_SERVER_EXE, JAM_DEV_PORT, JAM_MEDIA_SECRET",
      "default rooms: a+b in room-a, c in room-b",
    ].join("\n"),
  );
}

const [command, id] = process.argv.slice(2);

if (command === "server") {
  run(executablePath(DEV.serverExe), [
    "--port",
    DEV.port,
    "--server-id",
    DEV.serverId,
    "--join-secret",
    DEV.secret,
  ]);
} else if (command === "client") {
  const client = DEV.clients[id];
  if (!client) {
    console.error(`unknown client '${id ?? ""}'`);
    usage();
    process.exit(2);
  }

  run(executablePath(DEV.clientExe), [
    "--server",
    DEV.serverHost,
    "--port",
    DEV.port,
    "--room",
    client.room,
    "--room-handle",
    client.room,
    "--user-id",
    client.user,
    "--display-name",
    client.displayName,
    "--join-token",
    tokenFor(client),
    "--media-secret",
    DEV.mediaSecret,
    "--codec",
    DEV.codec,
    "--frames",
    DEV.frames,
  ]);
} else {
  usage();
  process.exit(command ? 2 : 0);
}
