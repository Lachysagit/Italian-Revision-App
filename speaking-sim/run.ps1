# Loads .env into the process environment and starts the server; the PowerShell
# equivalent of the README's `set -a`. Run it from this directory: paths are relative.

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

# Fail loudly rather than start half-configured: an empty key errors every
# examiner turn, and an empty model path silently disables speech to text.
# Either key variable will do: config.cpp falls back to the first entry of the
# GEMINI_API_KEYS pool when the single GEMINI_API_KEY is unset, so requiring
# GEMINI_API_KEY here would reject a perfectly valid pool-only .env.
if (-not $env:GEMINI_API_KEY -and -not $env:GEMINI_API_KEYS -and $env:EXAMINER_BACKEND -ne "hailo") {
    Write-Error "Both GEMINI_API_KEY and GEMINI_API_KEYS are empty in .env, and EXAMINER_BACKEND is not hailo."
}
foreach ($pair in @(@("WHISPER_MODEL_PATH", $env:WHISPER_MODEL_PATH),
                    @("PIPER_MODEL_PATH", $env:PIPER_MODEL_PATH))) {
    $name, $path = $pair
    if ($path -and -not (Test-Path $path)) {
        Write-Error "$name points at '$path', which does not exist."
    }
}

# Same check for each voice named in LANGUAGE_VOICES ("id=path,id=path"). A
# missing voice is not fatal to the server - that language simply falls silent -
# but it is always a mistake, and finding out here beats finding out mid-exam.
if ($env:LANGUAGE_VOICES) {
    foreach ($entry in $env:LANGUAGE_VOICES.Split(",")) {
        $i = $entry.IndexOf("=")
        if ($i -lt 1) { continue }
        $id = $entry.Substring(0, $i).Trim()
        $voice = $entry.Substring($i + 1).Trim()
        if ($voice -and -not (Test-Path $voice)) {
            Write-Error "LANGUAGE_VOICES entry '$id' points at '$voice', which does not exist."
        }
        # piper reads the sample rate from the voice's .onnx.json, and without
        # it that language plays back at piper's 22050 default instead.
        if ($voice -and (Test-Path $voice) -and -not (Test-Path "$voice.json")) {
            Write-Error "LANGUAGE_VOICES entry '$id' has no '$voice.json' beside it."
        }
    }
}

# Sign-in: the redirect URI Google is given is built from PUBLIC_ORIGIN, and
# Google compares it to its allowlist as a literal string. A PUBLIC_ORIGIN whose
# port disagrees with PORT is the mismatch that is hardest to spot, because both
# values look right on their own.
if ($env:GOOGLE_CLIENT_ID -or $env:GOOGLE_CLIENT_SECRET) {
    if (-not $env:GOOGLE_CLIENT_ID -or -not $env:GOOGLE_CLIENT_SECRET) {
        Write-Error "Set both GOOGLE_CLIENT_ID and GOOGLE_CLIENT_SECRET, or neither."
    }

    $origin = if ($env:PUBLIC_ORIGIN) { $env:PUBLIC_ORIGIN.TrimEnd("/") } else { "http://localhost:8080" }
    if ($origin -notmatch "^https?://") {
        Write-Error "PUBLIC_ORIGIN must start with http:// or https://, got '$origin'."
    }

    $originPort = if ($origin -match ":(\d+)$") { $matches[1] }
                  elseif ($origin.StartsWith("https://")) { "443" }
                  else { "80" }
    $serverPort = if ($env:PORT) { $env:PORT } else { "8080" }
    if ($originPort -ne $serverPort) {
        Write-Warning "PUBLIC_ORIGIN is '$origin' but PORT is $serverPort. Google will be sent a redirect_uri on port $originPort."
    }

    Write-Host "Google sign-in enabled. This exact URI must be an authorised redirect URI:" -ForegroundColor Cyan
    Write-Host "    $origin/auth/callback" -ForegroundColor Cyan
}

$port = if ($env:PORT) { $env:PORT } else { "8080" }
Write-Host "Starting speaking-sim on http://localhost:$port" -ForegroundColor Green

& (Join-Path $PSScriptRoot "build\speaking-sim.exe")
