#!/usr/bin/env node

import { spawn } from "node:child_process";
import { fileURLToPath } from "node:url";

const runner = fileURLToPath(new URL("./run-relay-load-test.mjs", import.meta.url));
const cases = [];
for (const frames of [120, 240, 480, 960]) {
  for (const clients of [2, 8, 16, 32]) {
    cases.push({ frames, clients, senders: clients === 2 ? 1 : clients });
  }
}

let failed = false;
for (let index = 0; index < cases.length; ++index) {
  const test = cases[index];
  const supported = true;
  const port = 22000 + index;
  const args = [
    runner,
    "--port", `${port}`,
    "--clients", `${test.clients}`,
    "--senders", `${test.senders}`,
    "--duration-ms", "1000",
    "--frames", `${test.frames}`,
    "--min-delivery", supported ? "0.995" : "0",
    "--max-receive-age-p999-ms", supported ? "25" : "10000",
    ...process.argv.slice(2),
  ];
  console.log(JSON.stringify({ capacity_case: { ...test, supported } }));
  const code = await new Promise((resolve) => {
    const child = spawn(process.execPath, args, { stdio: "inherit" });
    child.once("exit", resolve);
  });
  if (supported && code !== 0) {
    failed = true;
  }
}

process.exit(failed ? 1 : 0);
