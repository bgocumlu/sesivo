import { spawnSync } from "node:child_process";
import { existsSync, readdirSync, readFileSync, rmSync, statSync } from "node:fs";
import { join, resolve } from "node:path";
import { fileURLToPath } from "node:url";

const targetArg = process.argv[2];
const targetAliases = new Map([
  ["mac", "mac"],
  ["macos", "mac"],
  ["windows", "windows"],
  ["win", "windows"],
]);
const target = targetAliases.get(targetArg ?? "");

if (!target) {
  console.error("Usage: node tools/gh-release.mjs <mac|windows>");
  process.exit(1);
}

const repoDir = resolve(fileURLToPath(new URL("..", import.meta.url)));
const buildDir = resolveFromRepo(process.env.BUILD_DIR ?? "build");
const packageDir = resolveFromRepo(process.env.PACKAGE_DIR ?? join(buildDir, "package"));
const version = process.env.APP_VERSION ?? readProjectVersion();
const tag = `v${version}`;
const releaseTitle = tag;
const releaseNotes = `sesivo ${tag}`;

function resolveFromRepo(path) {
  return resolve(repoDir, path);
}

function readProjectVersion() {
  const cmakePath = join(repoDir, "CMakeLists.txt");
  const text = readFileSync(cmakePath, "utf8");
  const match = text.match(/set\s*\(\s*SESIVO_VERSION\s+"([^"]+)"/);
  if (!match) {
    console.error("Could not read SESIVO_VERSION from CMakeLists.txt.");
    process.exit(1);
  }
  return match[1];
}

function commandName(command) {
  if (process.platform !== "win32") {
    return command;
  }
  if (command === "gh") {
    const githubCliPath = join(
      process.env.ProgramFiles ?? "C:\\Program Files",
      "GitHub CLI",
      "gh.exe",
    );
    if (existsSync(githubCliPath)) {
      return githubCliPath;
    }
  }
  if (command === "powershell") {
    return "powershell.exe";
  }
  return command;
}

function run(command, args, options = {}) {
  const result = spawnSync(command, args, {
    cwd: repoDir,
    stdio: "inherit",
    ...options,
  });
  if (result.error) {
    console.error(result.error.message);
    process.exit(1);
  }
  if (result.status !== 0) {
    process.exit(result.status ?? 1);
  }
}

function probe(command, args) {
  return (
    spawnSync(command, args, {
      cwd: repoDir,
      stdio: "ignore",
    }).status === 0
  );
}

function runPackageScript() {
  rmSync(packageDir, { recursive: true, force: true });

  const env = {
    ...process.env,
    APP_VERSION: version,
    BUILD_DIR: buildDir,
    PACKAGE_DIR: packageDir,
  };

  if (target === "mac") {
    if (process.platform !== "darwin") {
      console.error("mac releases must be built on macOS.");
      process.exit(1);
    }
    run("bash", ["tools/release-macos.sh"], { env });
    return;
  }

  if (process.platform !== "win32") {
    console.error("Windows releases must be built on Windows.");
    process.exit(1);
  }
  run(commandName("powershell"), [
    "-NoProfile",
    "-ExecutionPolicy",
    "Bypass",
    "-File",
    "tools\\package-windows.ps1",
  ], { env });
}

function listArtifacts() {
  if (!existsSync(packageDir)) {
    console.error(`No package directory found: ${packageDir}`);
    process.exit(1);
  }

  const allowedExtensions =
    target === "mac" ? [".dmg"] : [".zip", ".exe"];

  return readdirSync(packageDir)
    .map((name) => join(packageDir, name))
    .filter((path) => statSync(path).isFile())
    .filter((path) => {
      const lower = path.toLowerCase();
      return allowedExtensions.some((extension) => lower.endsWith(extension));
    });
}

runPackageScript();

const artifacts = listArtifacts();
if (artifacts.length === 0) {
  console.error(`No ${target} release artifacts found in ${packageDir}.`);
  process.exit(1);
}

const gh = commandName("gh");
const releaseExists = probe(gh, ["release", "view", tag]);

if (releaseExists) {
  run(gh, ["release", "upload", tag, ...artifacts, "--clobber"]);
} else {
  run(gh, [
    "release",
    "create",
    tag,
    ...artifacts,
    "--title",
    releaseTitle,
    "--notes",
    releaseNotes,
  ]);
}

run(gh, ["workflow", "run", "update-pages-downloads.yml", "--ref", "main"]);

console.log(`Published ${artifacts.length} artifact(s) to ${tag}.`);
