$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

function Get-EnvOrDefault {
    param(
        [Parameter(Mandatory = $true)][string]$Name,
        [Parameter(Mandatory = $true)][string]$Default
    )

    $value = [Environment]::GetEnvironmentVariable($Name)
    if ([string]::IsNullOrWhiteSpace($value)) {
        return $Default
    }
    return $value
}

function Get-ProjectVersion {
    $repoRoot = Resolve-Path -LiteralPath (Join-Path $PSScriptRoot "..")
    $cmakeLists = Join-Path $repoRoot "CMakeLists.txt"
    $match = Select-String -Path $cmakeLists `
        -Pattern '^\s*set\s*\(\s*SESIVO_VERSION\s+"([^"]+)"\s*\)' |
        Select-Object -First 1
    if (-not $match) {
        throw "could not read SESIVO_VERSION from CMakeLists.txt"
    }
    return $match.Matches[0].Groups[1].Value
}

function Require-Command {
    param([Parameter(Mandatory = $true)][string]$Name)

    $command = Get-Command $Name -ErrorAction SilentlyContinue
    if (-not $command) {
        throw "missing required command: $Name"
    }
    return $command.Source
}

function Find-SignTool {
    $explicit = [Environment]::GetEnvironmentVariable("SIGNTOOL")
    if (-not [string]::IsNullOrWhiteSpace($explicit)) {
        if (Test-Path -LiteralPath $explicit) {
            return (Resolve-Path -LiteralPath $explicit).Path
        }
        $command = Get-Command $explicit -ErrorAction SilentlyContinue
        if ($command) {
            return $command.Source
        }
        throw "SIGNTOOL does not point to an existing file or command: $explicit"
    }

    $onPath = Get-Command "signtool.exe" -ErrorAction SilentlyContinue
    if ($onPath) {
        return $onPath.Source
    }

    $kitsRoot = Join-Path ${env:ProgramFiles(x86)} "Windows Kits\10\bin"
    if (Test-Path -LiteralPath $kitsRoot) {
        $candidate = Get-ChildItem -Path $kitsRoot -Recurse -Filter "signtool.exe" |
            Where-Object { $_.FullName -match "\\x64\\signtool\.exe$" } |
            Sort-Object FullName -Descending |
            Select-Object -First 1
        if ($candidate) {
            return $candidate.FullName
        }
    }

    throw "missing required command: signtool.exe. Install the Windows SDK or set SIGNTOOL."
}

function Find-ClientExe {
    param(
        [Parameter(Mandatory = $true)][string]$BuildDir,
        [Parameter(Mandatory = $true)][string]$Config
    )

    $candidates = @(
        (Join-Path $BuildDir "$Config\sesivo.exe"),
        (Join-Path $BuildDir "sesivo.exe")
    )

    foreach ($candidate in $candidates) {
        if (Test-Path -LiteralPath $candidate) {
            return (Resolve-Path -LiteralPath $candidate).Path
        }
    }

    $found = Get-ChildItem -Path $BuildDir -Recurse -Filter "sesivo.exe" |
        Sort-Object FullName |
        Select-Object -First 1
    if ($found) {
        return $found.FullName
    }

    throw "could not find sesivo.exe under build directory: $BuildDir"
}

function Get-OrCreate-SelfSignedCodeSigningCert {
    param([Parameter(Mandatory = $true)][string]$Subject)

    $minExpiry = (Get-Date).AddDays(30)
    $existing = Get-ChildItem -Path Cert:\CurrentUser\My -CodeSigningCert |
        Where-Object { $_.Subject -eq $Subject -and $_.NotAfter -gt $minExpiry } |
        Sort-Object NotAfter -Descending |
        Select-Object -First 1
    if ($existing) {
        return $existing
    }

    return New-SelfSignedCertificate `
        -Type CodeSigningCert `
        -Subject $Subject `
        -CertStoreLocation Cert:\CurrentUser\My `
        -KeyUsage DigitalSignature `
        -KeyAlgorithm RSA `
        -KeyLength 3072 `
        -HashAlgorithm SHA256 `
        -NotAfter (Get-Date).AddYears(3)
}

function Add-ZipEntry {
    param(
        [Parameter(Mandatory = $true)][System.IO.Compression.ZipArchive]$Zip,
        [Parameter(Mandatory = $true)][string]$SourcePath,
        [Parameter(Mandatory = $true)][string]$EntryName
    )

    [System.IO.Compression.ZipFileExtensions]::CreateEntryFromFile(
        $Zip,
        $SourcePath,
        $EntryName,
        [System.IO.Compression.CompressionLevel]::Optimal
    ) | Out-Null
}

$buildDir = Get-EnvOrDefault "BUILD_DIR" "build"
$config = Get-EnvOrDefault "CONFIG" "Release"
$packageDir = Get-EnvOrDefault "PACKAGE_DIR" (Join-Path $buildDir "package")
$appVersion = Get-EnvOrDefault "APP_VERSION" (Get-ProjectVersion)
$signWindows = Get-EnvOrDefault "SIGN_WINDOWS" "1"
$timestampUrl = Get-EnvOrDefault "WINDOWS_TIMESTAMP_URL" "http://timestamp.digicert.com"
$signDescription = Get-EnvOrDefault "WINDOWS_SIGN_DESCRIPTION" "sesivo"
$selfSignSubject = Get-EnvOrDefault "WINDOWS_SELF_SIGN_SUBJECT" "CN=sesivo, E=hello@sesivo.app"
$signUrl = [Environment]::GetEnvironmentVariable("WINDOWS_SIGN_URL")

$cmake = Require-Command "cmake"

& $cmake -S . -B $buildDir "-DCMAKE_BUILD_TYPE=$config" "-DJAM_BUILD_CLIENT=ON"
& $cmake --build $buildDir --target client --config $config

$exePath = Find-ClientExe $buildDir $config

if ($signWindows -eq "1") {
    $signTool = Find-SignTool
    $usingSelfSigned = $false
    $signArgs = @(
        "sign",
        "/fd",
        "SHA256",
        "/tr",
        $timestampUrl,
        "/td",
        "SHA256",
        "/d",
        $signDescription
    )
    if (-not [string]::IsNullOrWhiteSpace($signUrl)) {
        $signArgs += @("/du", $signUrl)
    }

    $pfxPath = [Environment]::GetEnvironmentVariable("WINDOWS_CODESIGN_PFX")
    $pfxPassword = [Environment]::GetEnvironmentVariable("WINDOWS_CODESIGN_PASSWORD")
    $subject = [Environment]::GetEnvironmentVariable("WINDOWS_CODESIGN_SUBJECT")

    if (-not [string]::IsNullOrWhiteSpace($pfxPath)) {
        if (-not (Test-Path -LiteralPath $pfxPath)) {
            throw "WINDOWS_CODESIGN_PFX does not point to a file: $pfxPath"
        }
        $signArgs += @("/f", (Resolve-Path -LiteralPath $pfxPath).Path)
        if (-not [string]::IsNullOrWhiteSpace($pfxPassword)) {
            $signArgs += @("/p", $pfxPassword)
        }
    } elseif (-not [string]::IsNullOrWhiteSpace($subject)) {
        $signArgs += @("/n", $subject)
    } else {
        $cert = Get-OrCreate-SelfSignedCodeSigningCert $selfSignSubject
        $usingSelfSigned = $true
        $signArgs += @("/sha1", $cert.Thumbprint)
    }

    $signArgs += $exePath
    & $signTool @signArgs
    if ($usingSelfSigned) {
        $signature = Get-AuthenticodeSignature -FilePath $exePath
        if (-not $signature.SignerCertificate) {
            throw "signtool completed, but no Authenticode signer certificate was found on $exePath"
        }
        Write-Host "Signed with self-signed certificate: $($signature.SignerCertificate.Subject)"
        Write-Host "Signature status: $($signature.Status)"
    } else {
        & $signTool verify /pa /v $exePath
    }
} else {
    Write-Host "SIGN_WINDOWS is 0; packaging unsigned exe."
}

New-Item -ItemType Directory -Force -Path $packageDir | Out-Null

$zipPath = Join-Path $packageDir "sesivo-$appVersion-windows-x64-portable.zip"
if (Test-Path -LiteralPath $zipPath) {
    Remove-Item -LiteralPath $zipPath -Force
}

Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem
$zip = [System.IO.Compression.ZipFile]::Open($zipPath, [System.IO.Compression.ZipArchiveMode]::Create)
try {
    Add-ZipEntry $zip $exePath "sesivo.exe"
    Add-ZipEntry $zip (Resolve-Path -LiteralPath "LICENSE").Path "LICENSE"
    Add-ZipEntry $zip (Resolve-Path -LiteralPath "THIRD_PARTY_NOTICES.md").Path "THIRD_PARTY_NOTICES.md"
} finally {
    $zip.Dispose()
}

Write-Host "Packaged $zipPath"
