# Updates Wii Party to the newest version on GitHub and rebuilds it from the player's own disc.
# Started by the launcher's Update button (from a copy in work\, since this file gets replaced).
# -Base is the folder that holds tools\, work\ and "Wii Party\": the repository downloaded from GitHub,
# or the build kit (whose source is in source\). Only the source is replaced; the game folder, the
# tools, the translated code and settings stay, so an update usually only recompiles what changed.
# This file must stay ASCII (Windows PowerShell 5.1).
param([Parameter(Mandatory = $true)][string]$Base)
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
Add-Type -AssemblyName System.Windows.Forms

$repo = 'bennoekart2-del/WPnative'
$kitLayout = Test-Path (Join-Path $Base 'source\CMakeLists.txt')
$src = if ($kitLayout) { Join-Path $Base 'source' } else { $Base }
$work = Join-Path $Base 'work'
$game = Join-Path $Base 'Wii Party'
$versionFile = Join-Path $Base 'version.txt'

function Stop-Update([string]$text) {
    Write-Host ''; Write-Host $text -ForegroundColor Red
    [void][System.Windows.Forms.MessageBox]::Show($text, 'Update Wii Party', 'OK', 'Error')
    exit 1
}
function Start-Launcher {
    $bat = Join-Path $game 'Wii Party.bat'
    if (Test-Path $bat) { Start-Process -FilePath $bat -WorkingDirectory $game }
}

Write-Host 'Update Wii Party' -ForegroundColor Green

# ------------------------------------------------------------------ newest version
Write-Host 'Looking for the newest version...'
try {
    $wc = New-Object System.Net.WebClient
    $wc.Headers.Add('User-Agent', 'WiiPartyUpdate'); $wc.Headers.Add('Accept', 'application/vnd.github.sha')
    $sha = $wc.DownloadString("https://api.github.com/repos/$repo/commits/main").Trim()
} catch { Stop-Update "Couldn't reach GitHub. Check the internet connection and try again.`r`n`r`n$($_.Exception.Message)" }
if ($sha -notmatch '^[0-9a-f]{40}$') { Stop-Update "GitHub gave an unexpected answer:`r`n$sha" }
$have = if (Test-Path $versionFile) { (Get-Content $versionFile -Raw).Trim() } else { '' }
if ($have -eq $sha) {
    [void][System.Windows.Forms.MessageBox]::Show('You already have the newest version.', 'Update Wii Party')
    Start-Launcher
    exit 0
}

# the game can't be replaced while it runs
while (Get-Process wiiparty-native -ErrorAction SilentlyContinue | Where-Object { $_.Path -and $_.Path.StartsWith($game) }) {
    $r = [System.Windows.Forms.MessageBox]::Show('Close the Wii Party game window first, then press OK.', 'Update Wii Party', 'OKCancel', 'Warning')
    if ($r -ne 'OK') { exit 1 }
}

# ------------------------------------------------------------------ download
Write-Host "Downloading version $($sha.Substring(0, 7))..."
$zip = Join-Path $work 'update.zip'
$unpacked = Join-Path $work 'update'
New-Item -ItemType Directory -Force $work | Out-Null
try { Invoke-WebRequest -UseBasicParsing -Uri "https://github.com/$repo/archive/$sha.zip" -OutFile $zip }
catch { Stop-Update "Downloading the new version failed. Check the internet connection and try again.`r`n`r`n$($_.Exception.Message)" }
if (Test-Path $unpacked) { Remove-Item -Recurse -Force $unpacked }
Expand-Archive -Force $zip $unpacked
$new = Get-ChildItem $unpacked -Directory | Select-Object -First 1  # GitHub puts everything in WPnative-<sha>\
if (-not $new -or -not (Test-Path (Join-Path $new.FullName 'kit\build-kit.ps1'))) { Stop-Update 'The download looks broken. Try again later.' }

# ------------------------------------------------------------------ replace the source
Write-Host 'Replacing the source...'
$keep = @('tools', 'work', 'Wii Party', 'game-folder.txt', 'version.txt')  # never touched
New-Item -ItemType Directory -Force $src | Out-Null
foreach ($item in Get-ChildItem -Force $new.FullName) {
    if ($keep -contains $item.Name) { continue }
    $dest = Join-Path $src $item.Name
    if (Test-Path -LiteralPath $dest) { Remove-Item -Recurse -Force -LiteralPath $dest }
    Copy-Item -Recurse -Force -LiteralPath $item.FullName $dest
}
if ($kitLayout) {  # the kit's own files sit next to source\
    foreach ($f in 'build-kit.ps1', 'Build Wii Party.bat', 'READ ME.txt') {
        $from = Join-Path $src "kit\$f"
        if (Test-Path -LiteralPath $from) { Copy-Item -Force -LiteralPath $from (Join-Path $Base $f) }
    }
}
Remove-Item -Recurse -Force $unpacked, $zip

# ------------------------------------------------------------------ rebuild
$buildScript = if ($kitLayout) { Join-Path $Base 'build-kit.ps1' } else { Join-Path $src 'kit\build-kit.ps1' }
& powershell -NoProfile -ExecutionPolicy Bypass -File $buildScript -Update
if ($LASTEXITCODE -ne 0) { exit 1 }  # build-kit already said what went wrong
Set-Content -Encoding ASCII $versionFile $sha
Write-Host ''
Write-Host 'Wii Party is updated!' -ForegroundColor Green
[void][System.Windows.Forms.MessageBox]::Show("Wii Party is updated to the newest version.`r`n`r`nThe launcher opens again now.", 'Update Wii Party')
Start-Launcher
