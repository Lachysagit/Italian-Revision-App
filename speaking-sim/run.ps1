# Loads .env into the process environment and starts the server.
#
# The server reads its settings from environment variables, not from .env
# itself, so something has to export them first. The README's
# `set -a && . ./.env && set +a` is bash; this is the PowerShell equivalent.
#
#   .\run.ps1
#
# Run it from this directory. speaking-sim.exe opens web/index.html and
# prompts/examiner_system.txt by relative path, so starting it from anywhere
# else serves a 404 for the page and silently falls back to a built-in
# one-line system prompt.

$ErrorActionPreference = "Stop"

# Anchor to this script's own directory, so the relative paths the server
# expects hold no matter where the shell happens to be.
Set-Location -Path $PSScriptRoot

$envFile = Join-Path $PSScriptRoot ".env"
if (-not (Test-Path $envFile)) {
    Write-Error "No .env found. Copy .env.example to .env and fill it in."
}

Get-Content $envFile | ForEach-Object {
    $line = $_.Trim()
    if ($line -eq "" -or $line.StartsWith("#")) { return }

    # Split on the FIRST '=' only: values contain '=' (API keys end in padding,
    # URLs carry query strings), and splitting on all of them truncates those.
    $i = $line.IndexOf("=")
    if ($i -lt 1) { return }

    $name = $line.Substring(0, $i).Trim()
    $value = $line.Substring($i + 1).Trim()

    # Strip one layer of matching quotes, the way `set -a` sourcing would.
    if ($value.Length -ge 2 -and
        (($value.StartsWith('"') -and $value.EndsWith('"')) -or
         ($value.StartsWith("'") -and $value.EndsWith("'")))) {
        $value = $value.Substring(1, $value.Length - 2)
    }

    Set-Item -Path "Env:$name" -Value $value
}

# Fail loudly here rather than letting the server start half-configured. Both
# of these degrade silently otherwise: an empty key means every examiner turn
# comes back as an error frame, and an empty model path disables speech to text
# entirely while the server still looks healthy.
if (-not $env:GEMINI_API_KEY -and $env:EXAMINER_BACKEND -ne "hailo") {
    Write-Error "GEMINI_API_KEY is empty in .env, and EXAMINER_BACKEND is not hailo."
}
foreach ($pair in @(@("WHISPER_MODEL_PATH", $env:WHISPER_MODEL_PATH),
                    @("PIPER_MODEL_PATH", $env:PIPER_MODEL_PATH))) {
    $name, $path = $pair
    if ($path -and -not (Test-Path $path)) {
        Write-Error "$name points at '$path', which does not exist."
    }
}

$port = if ($env:PORT) { $env:PORT } else { "8080" }
Write-Host "Starting speaking-sim on http://localhost:$port" -ForegroundColor Green

& (Join-Path $PSScriptRoot "build\speaking-sim.exe")
