# Builds Wii Party for this PC from the player's own disc (started by "Build Wii Party.bat").
# Downloads a C compiler (w64devkit), CMake and Python into tools\, translates the game's code from
# the disc folder into C (work\gen), compiles it, and puts the finished game in "Wii Party\".
# Nothing from the disc leaves this PC; the result is for this player only.
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
Add-Type -AssemblyName System.Windows.Forms

$kit = Split-Path -Parent $MyInvocation.MyCommand.Path
$src = Join-Path $kit 'source'
$tools = Join-Path $kit 'tools'
$work = Join-Path $kit 'work'
$out = Join-Path $kit 'Wii Party'

$downloads = @{
    python = @('https://www.python.org/ftp/python/3.12.7/python-3.12.7-embed-amd64.zip', 'python.zip')
    gcc    = @('https://github.com/skeeto/w64devkit/releases/download/v2.0.0/w64devkit-x64-2.0.0.exe', 'w64devkit.exe')
    cmake  = @('https://github.com/Kitware/CMake/releases/download/v3.30.5/cmake-3.30.5-windows-x86_64.zip', 'cmake.zip')
}

function Step([string]$text) { Write-Host ''; Write-Host "== $text" -ForegroundColor Cyan }
function Stop-Build([string]$text) {
    Write-Host ''; Write-Host $text -ForegroundColor Red
    [void][System.Windows.Forms.MessageBox]::Show($text, 'Build Wii Party', 'OK', 'Error')
    exit 1
}

function Get-Download([string]$name) {
    $url, $file = $downloads[$name]
    $dest = Join-Path $tools $file
    if (Test-Path $dest) { return $dest }
    Write-Host "Downloading $file..."
    try { Invoke-WebRequest -Uri $url -OutFile "$dest.part" -UseBasicParsing }
    catch { Stop-Build "Downloading $file failed. Check the internet connection and run this again.`r`n`r`n$url`r`n$($_.Exception.Message)" }
    Move-Item -Force "$dest.part" $dest
    return $dest
}

function Test-GameFolder([string]$p) {
    return $p -and (Test-Path -LiteralPath (Join-Path $p 'sys\main.dol')) -and (Test-Path -LiteralPath (Join-Path $p 'files\rel'))
}
function Resolve-GameFolder([string]$p) {
    if (Test-GameFolder $p) { return $p }
    foreach ($sub in 'DATA', 'P-SUPE', 'P-SUPP', 'P-SUPJ') { if (Test-GameFolder (Join-Path $p $sub)) { return (Join-Path $p $sub) } }
    return $null
}

if ($kit -match '\s') {  # the compiler tools trip over spaces in folder names
    Stop-Build "Please move this kit to a folder without spaces in its name, for example C:\WiiPartyKit, and start it again from there.`r`n`r`nNow it is in:`r`n$kit"
}

Write-Host 'Build Wii Party' -ForegroundColor Green
$started = Get-Date
Write-Host 'This makes the game from your own Wii Party disc. It takes a while (about 10-40 minutes) and'
Write-Host 'needs about 6 GB of free disc space. You can keep using your PC meanwhile.'

# ------------------------------------------------------------------ the game folder
Step 'Finding your game folder'
$saved = Join-Path $kit 'game-folder.txt'
$game = if (Test-Path $saved) { Resolve-GameFolder (Get-Content $saved -Raw).Trim() } else { $null }
if (-not $game) {
    [void][System.Windows.Forms.MessageBox]::Show(("Choose the folder with your Wii Party disc in it.`r`n`r`n" +
        "Make it once with the free Dolphin emulator: right-click Wii Party > Properties > Filesystem > " +
        "right-click the disc > Extract Entire Disc. The right folder has 'files' and 'sys' inside it."), 'Build Wii Party')
    $dlg = New-Object System.Windows.Forms.FolderBrowserDialog
    $dlg.Description = "Your Wii Party folder (with 'files' and 'sys' inside)"
    if ($dlg.ShowDialog() -ne 'OK') { exit 1 }
    $game = Resolve-GameFolder $dlg.SelectedPath
    if (-not $game) { Stop-Build "That folder doesn't hold Wii Party. The right folder has two folders inside it, called 'files' and 'sys'." }
    Set-Content -Encoding UTF8 $saved $game
}
$id = [System.Text.Encoding]::ASCII.GetString([System.IO.File]::ReadAllBytes((Join-Path $game 'sys\boot.bin'))[0..3])
if ($id -notlike 'SUP*') { Stop-Build "That folder holds a different game ($id), not Wii Party." }
Write-Host "Game: $game ($id)"

# ------------------------------------------------------------------ tools
Step 'Getting the tools (only the first time)'
New-Item -ItemType Directory -Force $tools | Out-Null
$py = Join-Path $tools 'python\python.exe'
if (-not (Test-Path $py)) { Expand-Archive -Force (Get-Download 'python') (Join-Path $tools 'python') }
$gccBin = Join-Path $tools 'w64devkit\bin'
if (-not (Test-Path (Join-Path $gccBin 'gcc.exe'))) {
    $sfx = Get-Download 'gcc'
    & $sfx "-o$tools" -y | Out-Null
    if (-not (Test-Path (Join-Path $gccBin 'gcc.exe'))) { Stop-Build 'Unpacking the compiler (w64devkit) failed.' }
}
$cmakeBin = Join-Path $tools 'cmake-3.30.5-windows-x86_64\bin'
if (-not (Test-Path (Join-Path $cmakeBin 'cmake.exe'))) { Expand-Archive -Force (Get-Download 'cmake') $tools }
$env:PATH = "$gccBin;$cmakeBin;$env:PATH"

# ------------------------------------------------------------------ translate
$gen = Join-Path $work 'gen'
$stamp = Join-Path $gen 'done.txt'
if (-not ((Test-Path $stamp) -and ((Get-Content $stamp -Raw).Trim() -eq $game))) {
    Step 'Translating the game code to C (a few minutes)'
    if (Test-Path $gen) { Remove-Item -Recurse -Force $gen }
    $hle = Join-Path $src 'recomp\hle_SUPP01.json'
    & $py (Join-Path $src 'recomp\recompile.py') --dol (Join-Path $game 'sys\main.dol') --hle $hle --out $gen
    if ($LASTEXITCODE -ne 0) { Stop-Build 'Translating the main program failed. Send the text in this window to whoever helps you.' }
    # the minigames, several at once (one Python per core), biggest first so they finish together
    $rels = @(Get-ChildItem (Join-Path $game 'files\rel\*.rel.lz') | Sort-Object Length -Descending | ForEach-Object { $_.FullName })
    $workers = [Math]::Max(1, [Math]::Min([int]$env:NUMBER_OF_PROCESSORS, 12))
    $groups = @(for ($w = 0; $w -lt $workers; $w++) { , @(for ($i = $w; $i -lt $rels.Count; $i += $workers) { $rels[$i] }) })
    $procs = @()
    for ($w = 0; $w -lt $workers; $w++) {
        if (-not $groups[$w]) { continue }
        $argList = @("`"$(Join-Path $src 'recomp\rel.py')`"", '--dol-gen', "`"$gen`"", '--out', "`"$gen`"", '--hle', "`"$hle`"", '--rel') +
            @($groups[$w] | ForEach-Object { "`"$_`"" })
        $proc = Start-Process -FilePath $py -ArgumentList $argList -NoNewWindow -PassThru `
            -RedirectStandardOutput (Join-Path $work "translate-$w.txt") -RedirectStandardError (Join-Path $work "translate-$w-errors.txt")
        $null = $proc.Handle  # without this, PowerShell forgets the exit code
        $procs += $proc
    }
    $procs | Wait-Process
    foreach ($p in $procs) {
        if ($p.ExitCode -ne 0) {
            Get-ChildItem (Join-Path $work 'translate-*-errors.txt') | Get-Content | Write-Host
            Stop-Build 'Translating the minigames failed. Send the text in this window to whoever helps you.'
        }
    }
    & $py (Join-Path $src 'recomp\rel.py') --dol-gen $gen --out $gen --hle $hle --rel  # the list of all of them
    if ($LASTEXITCODE -ne 0) { Stop-Build 'Translating the minigames failed. Send the text in this window to whoever helps you.' }
    Set-Content -Encoding UTF8 $stamp $game
}

# ------------------------------------------------------------------ compile
Step 'Compiling (the longest part: 5-30 minutes, depending on the PC)'
$build = Join-Path $work 'build'
$cores = [int]$env:NUMBER_OF_PROCESSORS
$ramGB = [int]((Get-CimInstance Win32_ComputerSystem).TotalPhysicalMemory / 1GB)
$jobs = [Math]::Max(1, [Math]::Min($cores, [int][Math]::Floor(($ramGB - 2) / 1.2)))  # the biggest files need about 1 GB each
Write-Host "Using $jobs of $cores processor cores ($ramGB GB of memory)."
& cmake -S $src -B $build -G 'MinGW Makefiles' -DCMAKE_BUILD_TYPE=Release "-DWPR_GEN_DIR=$gen" `
    "-DCMAKE_MAKE_PROGRAM=$(Join-Path $gccBin 'make.exe')" -DCMAKE_C_COMPILER=gcc -DCMAKE_CXX_COMPILER=g++ -DCMAKE_SH=CMAKE_SH-NOTFOUND
if ($LASTEXITCODE -ne 0) { Stop-Build 'Setting up the build failed. Send the text in this window to whoever helps you.' }
& cmake --build $build -j $jobs
if ($LASTEXITCODE -ne 0) { Stop-Build 'Compiling failed. Send the text in this window to whoever helps you.' }

# ------------------------------------------------------------------ the finished game
Step 'Putting the game together'
New-Item -ItemType Directory -Force $out | Out-Null
& strip -o (Join-Path $out 'wiiparty-native.exe') (Join-Path $build 'wiiparty-native.exe')
if ($LASTEXITCODE -ne 0) { Copy-Item -Force (Join-Path $build 'wiiparty-native.exe') $out }
Copy-Item -Force (Join-Path $src 'dist\*') $out
$ini = Join-Path $out 'settings.ini'
if (-not (Test-Path $ini)) { Set-Content -Encoding ASCII $ini "disc=$game" }

Write-Host ''
Write-Host "Done in $([int]((Get-Date) - $started).TotalMinutes) minutes! The game is in: $out" -ForegroundColor Green
Write-Host 'Open that folder and double-click "Wii Party.bat". (You can delete the work and tools folders now.)'
[void][System.Windows.Forms.MessageBox]::Show("Wii Party is ready!`r`n`r`nOpen the folder 'Wii Party' next to this kit and double-click 'Wii Party.bat'.", 'Build Wii Party')
Start-Process explorer.exe $out
