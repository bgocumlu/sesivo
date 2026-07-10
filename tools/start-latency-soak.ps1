param(
  [string]$Room = "phase1-soak",
  [string]$ServerId = "local-dev",
  [string]$ServerHost = "127.0.0.1",
  [int]$Port = 9999,
  [string]$LatencyProfile = "low",
  [int]$BufferFrames = 120,
  [int]$PacketFrames = 120,
  [string]$JoinSecret = "dev-secret",
  [string]$MediaSecret = "dev-media-secret",
  [int]$Seconds = 3600,
  [string]$RunRoot = "validation_logs\latency"
)

$ErrorActionPreference = "Stop"

function Resolve-RepoPath([string]$RelativePath) {
  return Join-Path $repo $RelativePath
}

function Stop-SoakProcesses {
  param(
    [System.Diagnostics.Process[]]$ClientProcesses,
    [System.Diagnostics.Process]$ServerProcess
  )

  $requestedClose = $false
  foreach ($process in $ClientProcesses) {
    if ($null -ne $process -and -not $process.HasExited) {
      try {
        $process.CloseMainWindow() | Out-Null
        $requestedClose = $true
      } catch {
      }
    }
  }

  if ($requestedClose) {
    Start-Sleep -Seconds 5
  }

  foreach ($process in @($ClientProcesses + $ServerProcess)) {
    if ($null -ne $process -and -not $process.HasExited) {
      Stop-Process -Id $process.Id -ErrorAction SilentlyContinue
    }
  }
}

$repo = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$run = Join-Path (Resolve-RepoPath $RunRoot) ("phase1-soak-" + (Get-Date -Format "yyyyMMdd-HHmmss"))

$server = Resolve-RepoPath "build\Release\sesivo-server.exe"
$client = Resolve-RepoPath "build\Release\sesivo.exe"
$latencyTool = Resolve-RepoPath "tools\latency-measurement.mjs"

if (-not (Test-Path -LiteralPath $server)) {
  throw "Missing server binary: $server"
}
if (-not (Test-Path -LiteralPath $client)) {
  throw "Missing client binary: $client"
}
if ($Seconds -lt 0) {
  throw "Seconds must be 0 or greater"
}

$aCfg = Join-Path $run "client-a-config"
$bCfg = Join-Path $run "client-b-config"
$clientBLog = Join-Path $run "client-b.log"
$report = Join-Path $run "soak-report.json"
$minE2eSamples = 100000
if ($Seconds -gt 0) {
  $expectedE2eSamples = [Math]::Floor($Seconds * 48000.0 / $PacketFrames * 0.8)
  $minE2eSamples = [Math]::Min($minE2eSamples, [Math]::Max(1, $expectedE2eSamples))
}
New-Item -ItemType Directory -Force -Path @($run, $aCfg, $bCfg) | Out-Null

$tokenTtlSeconds = 90 * 60
if ($Seconds -gt 0) {
  $tokenTtlSeconds = [Math]::Max($tokenTtlSeconds, $Seconds + (30 * 60))
}

$tokenScript = @'
const crypto = require("node:crypto");
const [user, room, serverId, secret, ttlSeconds] = process.argv.slice(1);
const expiresAtMs = Date.now() + Number(ttlSeconds) * 1000;
function field(v) {
  const s = String(v);
  return `${Buffer.byteLength(s, "utf8")}:${s}`;
}
const payload = [expiresAtMs, serverId, room, user, "", "0", crypto.randomBytes(16).toString("hex")].map(field).join("");
const body = Buffer.from(payload, "utf8").toString("base64").replaceAll("+", "-").replaceAll("/", "_").replace(/=+$/u, "");
const sig = crypto.createHmac("sha256", secret).update(`v2|${payload}`).digest("hex");
console.log(`v2.${body}.${sig}`);
'@

function New-JoinToken([string]$User) {
  return node -e $tokenScript $User $Room $ServerId $JoinSecret $tokenTtlSeconds
}

$tokenA = New-JoinToken "soak-a"
$tokenB = New-JoinToken "soak-b"

$serverProcess = Start-Process -FilePath $server -WorkingDirectory $repo -WindowStyle Hidden -PassThru -ArgumentList @(
  "--port", $Port,
  "--server-id", $ServerId,
  "--join-secret", $JoinSecret,
  "--log-file", (Join-Path $run "server.log")
)

Start-Sleep -Seconds 2

$common = @(
  "--server", $ServerHost,
  "--port", $Port,
  "--room", $Room,
  "--room-handle", $Room,
  "--media-secret", $MediaSecret,
  "--codec", "opus",
  "--frames", $BufferFrames,
  "--opus-packet-frames", $PacketFrames,
  "--latency-profile", $LatencyProfile
)

$clientAProcess = Start-Process -FilePath $client -WorkingDirectory $repo -PassThru -ArgumentList ($common + @(
  "--user-id", "soak-a",
  "--display-name", "SoakA",
  "--join-token", $tokenA,
  "--config-dir", $aCfg,
  "--log-file", (Join-Path $run "client-a.log")
))

$clientBProcess = Start-Process -FilePath $client -WorkingDirectory $repo -PassThru -ArgumentList ($common + @(
  "--user-id", "soak-b",
  "--display-name", "SoakB",
  "--join-token", $tokenB,
  "--config-dir", $bCfg,
  "--log-file", $clientBLog
))

Write-Host "RUN_DIR=$run"
Write-Host "SERVER_PID=$($serverProcess.Id)"
Write-Host "CLIENT_A_PID=$($clientAProcess.Id)"
Write-Host "CLIENT_B_PID=$($clientBProcess.Id)"

try {
  if ($Seconds -eq 0) {
    Write-Host ""
    Write-Host "Started without auto-stop because -Seconds 0 was used."
    Write-Host "Diagnostics command:"
    Write-Host "node tools\latency-measurement.mjs diagnostics --log `"$clientBLog`" --assert --min-e2e-samples $minE2eSamples --max-drift-ppm-abs 5000 --max-plc-frames 0 --max-underruns 0 --max-callback-over-deadline 0 --out `"$report`""
    return
  }

  Write-Host ""
  Write-Host "Running soak for $Seconds seconds..."
  Start-Sleep -Seconds $Seconds

  Write-Host "Stopping clients and server..."
  Stop-SoakProcesses -ClientProcesses @($clientAProcess, $clientBProcess) -ServerProcess $serverProcess

  Write-Host "Running soak diagnostics..."
  & node $latencyTool diagnostics `
    --log $clientBLog `
    --assert `
    --min-e2e-samples $minE2eSamples `
    --max-drift-ppm-abs 5000 `
    --max-plc-frames 0 `
    --max-underruns 0 `
    --max-callback-over-deadline 0 `
    --out $report
  if ($LASTEXITCODE -ne 0) {
    throw "latency diagnostics failed with exit code $LASTEXITCODE"
  }

  Write-Host "SOAK_REPORT=$report"
} finally {
  if ($Seconds -gt 0) {
    Stop-SoakProcesses -ClientProcesses @($clientAProcess, $clientBProcess) -ServerProcess $serverProcess
  }
}
