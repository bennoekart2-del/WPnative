# Wii Party (native) launcher. Started by "Wii Party.bat". Four tabs: Play (find the game, press
# Play), Settings, Controls (player 1 keys) and Help. Settings are kept in settings.ini next to it.
Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing
[System.Windows.Forms.Application]::EnableVisualStyles()

$dir = Split-Path -Parent $MyInvocation.MyCommand.Path
Set-Location $dir
$ini = Join-Path $dir 'settings.ini'
$exe = Join-Path $dir 'wiiparty-native.exe'

function Show-Message([string]$text, [string]$icon = 'Information') {
    [void][System.Windows.Forms.MessageBox]::Show($text, 'Wii Party', 'OK', $icon)
}

try {

# action id (WPR_KEY_<id>), label, default SDL key name
$actions = @(
    @('UP', 'Up', 'Up'), @('DOWN', 'Down', 'Down'), @('LEFT', 'Left', 'Left'), @('RIGHT', 'Right', 'Right'),
    @('UP2', 'Up (2nd key)', 'W'), @('DOWN2', 'Down (2nd key)', 'S'), @('LEFT2', 'Left (2nd key)', 'A'), @('RIGHT2', 'Right (2nd key)', 'D'),
    @('A', 'A', 'Return'), @('A2', 'A (2nd key)', 'Space'), @('B', 'B', 'Backspace'), @('1', '1', '1'), @('2', '2', '2'),
    @('PLUS', '+', '='), @('MINUS', '-', '-'), @('HOME', 'HOME', 'H'), @('SHAKE', 'Shake', 'Left Shift'),
    @('SWINGUP', 'Swing up', 'T'), @('SWINGDOWN', 'Swing down', 'G'), @('TILTFWD', 'Tilt forward', 'R'),
    @('TILTBACK', 'Tilt back', 'F'), @('TILTLEFT', 'Turn left', 'Q'), @('TILTRIGHT', 'Turn right', 'E'),
    @('GRIP', 'Upright / sideways', 'F1')
)

$cfg = @{ disc = ''; scale = 'Auto'; aspect = '16:9'; fullscreen = '1'; sideways = '0' }
foreach ($a in $actions) { $cfg['key_' + $a[0]] = $a[2] }
if (Test-Path $ini) {
    foreach ($line in Get-Content $ini) {
        if ($line -match '^\s*([^=#]+?)\s*=\s*(.*)$') { $cfg[$matches[1]] = $matches[2] }
    }
}

function Save-Settings {
    $cfg.Keys | Sort-Object | ForEach-Object { "$_=$($cfg[$_])" } | Set-Content -Encoding ASCII $ini
}

# ------------------------------------------------------------------ finding the game
# A usable game folder is a Wii Party disc extracted with Dolphin: it has sys\main.dol and files\,
# and sys\boot.bin starts with the game's ID (SUP...).
function Test-GameFolder([string]$p) {
    if (-not $p -or -not (Test-Path -LiteralPath (Join-Path $p 'sys\main.dol'))) { return $false }
    if (-not (Test-Path -LiteralPath (Join-Path $p 'files'))) { return $false }
    $boot = Join-Path $p 'sys\boot.bin'
    if (Test-Path -LiteralPath $boot) {
        $id = [System.Text.Encoding]::ASCII.GetString([System.IO.File]::ReadAllBytes($boot), 0, 3)
        if ($id -ne 'SUP') { return $false }
    }
    return $true
}

# Dolphin's "Extract Entire Disc" can put the files in the chosen folder or in a DATA subfolder.
function Resolve-GameFolder([string]$p) {
    if (Test-GameFolder $p) { return $p }
    foreach ($sub in 'DATA', 'P-SUPE', 'P-SUPP', 'P-SUPJ') {
        $q = Join-Path $p $sub
        if (Test-GameFolder $q) { return $q }
    }
    return $null
}

function Get-SearchRoots {
    $h = $env:USERPROFILE
    $roots = @($dir, (Split-Path -Parent $dir), "$h\Downloads", "$h\Desktop", "$h\Documents", "$h\Games",
               "$h\OneDrive\Desktop", "$h\OneDrive\Documents", 'C:\Games')
    foreach ($d in [System.IO.DriveInfo]::GetDrives()) {
        if ($d.IsReady -and $d.DriveType -eq 'Fixed' -and $d.Name -ne 'C:\') { $roots += $d.Name }
    }
    return $roots | Where-Object { $_ -and (Test-Path -LiteralPath $_) } | Select-Object -Unique
}

function Find-GameFolder {
    foreach ($root in Get-SearchRoots) {
        $hit = Get-ChildItem -LiteralPath $root -Directory -Recurse -Depth 3 -Filter 'sys' -ErrorAction SilentlyContinue |
            ForEach-Object { Split-Path -Parent $_.FullName } |
            Where-Object { Test-GameFolder $_ } | Select-Object -First 1
        if ($hit) { return $hit }
    }
    return $null
}

function Find-DiscImage {
    foreach ($root in Get-SearchRoots) {
        $hit = Get-ChildItem -LiteralPath $root -File -Recurse -Depth 2 -Include '*.iso', '*.wbfs', '*.rvz', '*.ciso', '*.wia', '*.gcz' -ErrorAction SilentlyContinue |
            Where-Object { $_.Name -match 'party|SUP' } | Select-Object -First 1
        if ($hit) { return $hit.FullName }
    }
    return $null
}

$extractHelp = "You need your own Wii Party disc, copied to this PC as a folder. This is done once, with the free Dolphin emulator (dolphin-emu.org):`r`n`r`n" +
    "1. Make a copy of your Wii Party disc (for example with a Wii and CleanRip), so you have a .iso, .wbfs or .rvz file.`r`n" +
    "2. Open Dolphin and add the folder with that file (Options > Configuration > Paths).`r`n" +
    "3. In Dolphin's game list, right-click Wii Party > Properties > Filesystem.`r`n" +
    "4. Right-click the disc at the top of the list > Extract Entire Disc..., and pick an empty folder.`r`n" +
    "5. Back here, press 'Find my game' (or 'Choose folder' and pick that folder)."

# ------------------------------------------------------------------ keys
Add-Type -Namespace W -Name K -MemberDefinition '[DllImport("user32.dll")] public static extern short GetAsyncKeyState(int v);'
function Get-KeyState([int]$vk) { return ([W.K]::GetAsyncKeyState($vk) -band 0x8000) -ne 0 }

# Windows key -> SDL key name (what the game understands)
function Get-SdlName([System.Windows.Forms.KeyEventArgs]$e) {
    $n = $e.KeyCode.ToString()
    if ($n -match '^[A-Z]$') { return $n }
    if ($n -match '^D([0-9])$') { return $matches[1] }
    if ($n -match '^NumPad([0-9])$') { return 'Keypad ' + $matches[1] }
    if ($n -match '^F([0-9]+)$') { return $n }
    switch ($n) {
        'Return' { return 'Return' } 'Space' { return 'Space' } 'Back' { return 'Backspace' } 'Tab' { return 'Tab' }
        'Escape' { return 'Escape' } 'Up' { return 'Up' } 'Down' { return 'Down' } 'Left' { return 'Left' } 'Right' { return 'Right' }
        'Oemplus' { return '=' } 'OemMinus' { return '-' } 'Oemcomma' { return ',' } 'OemPeriod' { return '.' }
        'OemQuestion' { return '/' } 'OemSemicolon' { return ';' } 'OemQuotes' { return "'" } 'OemOpenBrackets' { return '[' }
        'OemCloseBrackets' { return ']' } 'OemPipe' { return '\' } 'Oemtilde' { return '`' }
        'Insert' { return 'Insert' } 'Delete' { return 'Delete' } 'Home' { return 'Home' } 'End' { return 'End' }
        'PageUp' { return 'PageUp' } 'Next' { return 'PageDown' } 'PageDown' { return 'PageDown' }
        'Add' { return 'Keypad +' } 'Subtract' { return 'Keypad -' } 'Multiply' { return 'Keypad *' } 'Divide' { return 'Keypad /' }
    }
    if ($n -eq 'ShiftKey') { if (Get-KeyState 0xA1) { return 'Right Shift' } else { return 'Left Shift' } }
    if ($n -eq 'ControlKey') { if (Get-KeyState 0xA3) { return 'Right Ctrl' } else { return 'Left Ctrl' } }
    if ($n -eq 'Menu') { if (Get-KeyState 0xA5) { return 'Right Alt' } else { return 'Left Alt' } }
    return $null
}

# ------------------------------------------------------------------ window
$font = New-Object System.Drawing.Font('Segoe UI', 10)
$bold = New-Object System.Drawing.Font('Segoe UI', 11, [System.Drawing.FontStyle]::Bold)
$form = New-Object System.Windows.Forms.Form
$form.Text = 'Wii Party'
$form.FormBorderStyle = 'FixedDialog'
$form.MaximizeBox = $false
$form.StartPosition = 'CenterScreen'
$form.Font = $font
$form.ClientSize = New-Object System.Drawing.Size(560, 560)
$form.KeyPreview = $true

$tabs = New-Object System.Windows.Forms.TabControl
$tabs.Dock = 'Fill'
$form.Controls.Add($tabs)
function New-Tab([string]$name) {
    $t = New-Object System.Windows.Forms.TabPage
    $t.Text = $name; $t.Padding = New-Object System.Windows.Forms.Padding(12); $t.BackColor = [System.Drawing.Color]::White
    $tabs.TabPages.Add($t); return $t
}
function Add-Label($parent, [string]$text, $x, $y, $w, $h = 24, $f = $null) {
    $l = New-Object System.Windows.Forms.Label
    $l.Text = $text; $l.Location = New-Object System.Drawing.Point($x, $y); $l.Size = New-Object System.Drawing.Size($w, $h)
    if ($f) { $l.Font = $f }
    $parent.Controls.Add($l); return $l
}
function Add-Button($parent, [string]$text, $x, $y, $w, $h = 32) {
    $b = New-Object System.Windows.Forms.Button
    $b.Text = $text; $b.Location = New-Object System.Drawing.Point($x, $y); $b.Size = New-Object System.Drawing.Size($w, $h)
    $parent.Controls.Add($b); return $b
}

# ---- Play tab
$tPlay = New-Tab 'Play'
Add-Label $tPlay 'Wii Party' 16 12 400 44 (New-Object System.Drawing.Font('Segoe UI', 22, [System.Drawing.FontStyle]::Bold)) | Out-Null
Add-Label $tPlay 'Step 1: your game' 16 70 400 26 $bold | Out-Null
$gameStatus = Add-Label $tPlay '' 16 98 520 44
$findBtn = Add-Button $tPlay 'Find my game' 16 146 160
$chooseBtn = Add-Button $tPlay 'Choose folder...' 186 146 160
$noGameBtn = Add-Button $tPlay "I don't have it yet" 356 146 170
Add-Label $tPlay 'Step 2: controllers' 16 196 400 26 $bold | Out-Null
Add-Label $tPlay ("Player 1 uses the keyboard and mouse: move the mouse to point, left click = A, right click = B.`r`n" +
    "Players 2, 3 and 4: plug in gamepads (Xbox, PlayStation, Switch) before you press Play.") 16 224 520 64 | Out-Null
Add-Label $tPlay 'Step 3: play' 16 296 400 26 $bold | Out-Null
$play = Add-Button $tPlay 'PLAY' 16 326 510 64
$play.Font = New-Object System.Drawing.Font('Segoe UI', 20, [System.Drawing.FontStyle]::Bold)
$play.BackColor = [System.Drawing.Color]::FromArgb(0, 150, 220); $play.ForeColor = [System.Drawing.Color]::White
$play.FlatStyle = 'Flat'
$playStatus = Add-Label $tPlay '' 16 398 520 24
Add-Label $tPlay "In the game: F11 = full screen on/off, F1 = hold the Remote sideways or upright. Close the game window to quit." 16 430 520 44 | Out-Null
Add-Label $tPlay 'Questions? Look at the Help tab.' 16 480 520 24 | Out-Null

function Update-GameStatus {
    $p = Resolve-GameFolder $cfg.disc
    if ($p) {
        $cfg.disc = $p
        $gameStatus.ForeColor = [System.Drawing.Color]::FromArgb(0, 130, 0)
        $gameStatus.Text = "Found your game:`r`n$p"
        return $true
    }
    $gameStatus.ForeColor = [System.Drawing.Color]::FromArgb(190, 0, 0)
    $gameStatus.Text = "The game hasn't been found yet. Press 'Find my game'.`r`nNo copy of your disc yet? Press 'I don't have it yet'."
    return $false
}

$findBtn.Add_Click({
    $form.Cursor = [System.Windows.Forms.Cursors]::WaitCursor; $gameStatus.ForeColor = [System.Drawing.Color]::Black
    $gameStatus.Text = 'Looking on this PC... this can take a minute.'; $form.Refresh()
    $p = Find-GameFolder
    $form.Cursor = [System.Windows.Forms.Cursors]::Default
    if ($p) { $cfg.disc = $p; Save-Settings; [void](Update-GameStatus); return }
    [void](Update-GameStatus)
    $img = Find-DiscImage
    if ($img) {
        Show-Message ("I found a disc file:`r`n$img`r`n`r`nIt still has to be turned into a folder once. Here's how:`r`n`r`n" + $extractHelp)
    } else {
        Show-Message ("I couldn't find Wii Party on this PC.`r`n`r`n" + $extractHelp)
    }
})
$chooseBtn.Add_Click({
    $d = New-Object System.Windows.Forms.FolderBrowserDialog
    $d.Description = 'Pick the folder you extracted Wii Party into (it has folders called "files" and "sys" inside).'
    if ($d.ShowDialog() -ne 'OK') { return }
    $p = Resolve-GameFolder $d.SelectedPath
    if ($p) { $cfg.disc = $p; Save-Settings; [void](Update-GameStatus); return }
    Show-Message ("That folder doesn't hold Wii Party.`r`nThe right folder has two folders inside it, called 'files' and 'sys'.`r`n`r`n" + $extractHelp) 'Warning'
})
$noGameBtn.Add_Click({ Show-Message $extractHelp })

# ---- Settings tab
$tSet = New-Tab 'Settings'
Add-Label $tSet 'Picture quality' 16 20 160 | Out-Null
$scale = New-Object System.Windows.Forms.ComboBox
$scale.DropDownStyle = 'DropDownList'; $scale.Location = New-Object System.Drawing.Point(190, 16); $scale.Size = New-Object System.Drawing.Size(340, 24)
[void]$scale.Items.AddRange(@('Automatic (recommended)', 'Original (fastest, blurry)', 'Sharp (2x)', 'Sharper (3x, good for 1080p)',
    'Very sharp (4x)', 'Ultra (5x, good for 4K)', 'Maximum (6x, needs a strong PC)'))
$scale.SelectedIndex = if ($cfg.scale -match '^[1-6]$') { [int]$cfg.scale } else { 0 }
$tSet.Controls.Add($scale)
Add-Label $tSet 'Screen shape' 16 60 160 | Out-Null
$aspect = New-Object System.Windows.Forms.ComboBox
$aspect.DropDownStyle = 'DropDownList'; $aspect.Location = New-Object System.Drawing.Point(190, 56); $aspect.Size = New-Object System.Drawing.Size(340, 24)
[void]$aspect.Items.AddRange(@('Widescreen 16:9 (recommended)', 'Old TV 4:3'))
$aspect.SelectedIndex = if ($cfg.aspect -eq '4:3') { 1 } else { 0 }
$tSet.Controls.Add($aspect)
$full = New-Object System.Windows.Forms.CheckBox
$full.Text = 'Start in full screen (F11 switches in the game)'; $full.Location = New-Object System.Drawing.Point(190, 96); $full.Size = New-Object System.Drawing.Size(340, 24)
$full.Checked = $cfg.fullscreen -eq '1'; $tSet.Controls.Add($full)
$side = New-Object System.Windows.Forms.CheckBox
$side.Text = 'Start with the Remote held sideways (F1 switches)'; $side.Location = New-Object System.Drawing.Point(190, 126); $side.Size = New-Object System.Drawing.Size(340, 24)
$side.Checked = $cfg.sideways -eq '1'; $tSet.Controls.Add($side)
Add-Label $tSet 'If the game is slow, pick a lower picture quality.' 16 176 520 24 | Out-Null
$resetAll = Add-Button $tSet 'Back to recommended settings' 16 210 260
$resetAll.Add_Click({ $scale.SelectedIndex = 0; $aspect.SelectedIndex = 0; $full.Checked = $true; $side.Checked = $false })

# ---- Controls tab
$tKeys = New-Tab 'Controls'
Add-Label $tKeys 'Player 1 keys: click a button, then press the key you want.' 12 8 520 24 | Out-Null
$keyPanel = New-Object System.Windows.Forms.Panel
$keyPanel.Location = New-Object System.Drawing.Point(8, 34); $keyPanel.Size = New-Object System.Drawing.Size(530, 400); $keyPanel.AutoScroll = $true
$tKeys.Controls.Add($keyPanel)
$waiting = $null
$keyButtons = @{}
$col = 0; $y = 0
foreach ($a in $actions) {
    $x = if ($col -eq 0) { 0 } else { 262 }
    Add-Label $keyPanel $a[1] $x ($y + 5) 118 | Out-Null
    $b = Add-Button $keyPanel $cfg['key_' + $a[0]] ($x + 120) $y 128 30
    $b.Tag = $a[0]
    $b.Add_Click({ param($s, $e) $script:waiting = $s; $s.Text = 'press a key...' })
    $b.Add_PreviewKeyDown({ param($s, $e) if ($script:waiting) { $e.IsInputKey = $true } })
    $keyButtons[$a[0]] = $b
    if ($col -eq 1) { $y += 34 }
    $col = 1 - $col
}
$resetKeys = Add-Button $tKeys 'Default keys' 12 440 140
$resetKeys.Add_Click({ foreach ($a in $actions) { $cfg['key_' + $a[0]] = $a[2]; $keyButtons[$a[0]].Text = $a[2] } })
Add-Label $tKeys "Mouse: point, left click = A, right click = B, middle click = shake, wheel = swing.`r`nGamepads: A/B/X/Y buttons, right stick points, left stick tilts." 12 476 520 44 | Out-Null
$form.Add_KeyDown({
    param($s, $e)
    if (-not $script:waiting) { return }
    $name = Get-SdlName $e
    if ($name) {
        $script:waiting.Text = $name
        $cfg['key_' + $script:waiting.Tag] = $name
        $script:waiting = $null
        $form.ActiveControl = $null  # so the key's release does not click a button
    }
    $e.Handled = $true; $e.SuppressKeyPress = $true
})

# ---- Help tab
$tHelp = New-Tab 'Help'
$help = New-Object System.Windows.Forms.TextBox
$help.Multiline = $true; $help.ReadOnly = $true; $help.ScrollBars = 'Vertical'; $help.BackColor = [System.Drawing.Color]::White
$help.Location = New-Object System.Drawing.Point(8, 8); $help.Size = New-Object System.Drawing.Size(530, 500)
$help.Text = ("HOW TO START`r`n" +
    "1. On the Play tab, press 'Find my game'.`r`n2. Plug in any gamepads.`r`n3. Press PLAY.`r`n`r`n" +
    "GETTING THE GAME FOLDER`r`n" + $extractHelp + "`r`n`r`n" +
    "IN THE GAME`r`n" +
    "Mouse = pointer. Left click = A, right click = B.`r`n" +
    "Arrow keys or W A S D = the Remote's + pad. Left Shift or middle click = shake.`r`n" +
    "Q / E = turn the Remote left / right, R / F = tilt it forward / back.`r`n" +
    "F1 = hold the Remote sideways or upright (the window title shows which). Most minigames want it sideways.`r`n" +
    "F11 = full screen on/off. Close the window to quit.`r`n`r`n" +
    "SOMETHING WRONG?`r`n" +
    "- The game is slow: Settings tab > Picture quality > Original.`r`n" +
    "- A gamepad does nothing: plug it in before pressing PLAY.`r`n" +
    "- The game closed by itself: send the file log.txt from this folder to the person who helps you.`r`n" +
    "- Windows said 'Windows protected your PC': press 'More info' and then 'Run anyway'.")
$help.Select(0, 0)
$tHelp.Controls.Add($help)

# ------------------------------------------------------------------ play
function Read-Settings {
    $cfg.scale = if ($scale.SelectedIndex -le 0) { 'Auto' } else { [string]$scale.SelectedIndex }
    $cfg.aspect = if ($aspect.SelectedIndex -eq 1) { '4:3' } else { '16:9' }
    $cfg.fullscreen = if ($full.Checked) { '1' } else { '0' }
    $cfg.sideways = if ($side.Checked) { '1' } else { '0' }
}

$play.Add_Click({
    Read-Settings
    if (-not (Update-GameStatus)) {
        $tabs.SelectedTab = $tPlay
        Show-Message "First the game has to be found: press 'Find my game'." 'Warning'
        return
    }
    Save-Settings
    if (Test-Path 'wp.zip.001') {
        $playStatus.Text = 'Unpacking the new version, one moment...'; $form.Cursor = [System.Windows.Forms.Cursors]::WaitCursor; $form.Refresh()
        $parts = Get-ChildItem 'wp.zip.0*' | Sort-Object Name
        $out = [System.IO.File]::Create((Join-Path $dir 'wp.zip'))
        foreach ($p in $parts) { $bytes = [System.IO.File]::ReadAllBytes($p.FullName); $out.Write($bytes, 0, $bytes.Length) }
        $out.Close()
        tar -xf wp.zip
        $form.Cursor = [System.Windows.Forms.Cursors]::Default
        if ($LASTEXITCODE -ne 0) { Show-Message 'Unpacking the new version failed. Is the disc full?' 'Error'; return }
        Remove-Item wp.zip; $parts | Remove-Item
        $playStatus.Text = ''
    }
    if (-not (Test-Path -LiteralPath $exe)) {
        Show-Message "The game program (wiiparty-native.exe) isn't in this folder:`r`n$dir`r`n`r`nThis launcher has to be in the same folder as the game program." 'Error'
        return
    }
    if (Test-Path 'log.txt') { Move-Item -Force 'log.txt' 'log-previous.txt' }
    $env:WPR_ASPECT = $cfg.aspect
    if ($cfg.scale -ne 'Auto') { $env:WPR_SCALE = $cfg.scale } else { Remove-Item Env:WPR_SCALE -ErrorAction SilentlyContinue }
    $env:WPR_FULLSCREEN = $cfg.fullscreen
    $env:WPR_SIDEWAYS = $cfg.sideways
    foreach ($a in $actions) { Set-Item ('Env:WPR_KEY_' + $a[0]) $cfg['key_' + $a[0]] }
    $started = Get-Date
    $proc = Start-Process -FilePath $exe -ArgumentList ('"' + $cfg.disc + '"') -WorkingDirectory $dir `
        -RedirectStandardError (Join-Path $dir 'log.txt') -NoNewWindow -PassThru
    $null = $proc.Handle  # keeps the exit code readable after it ends
    $form.Hide()
    $proc.WaitForExit()
    if ($proc.ExitCode -ne 0) {
        Show-Message ("The game stopped unexpectedly.`r`n`r`nPlease send the file log.txt from this folder to the person who helps you:`r`n$dir") 'Error'
    } elseif (((Get-Date) - $started).TotalSeconds -lt 5) {
        Show-Message ("The game closed right away.`r`n`r`nPlease send the file log.txt from this folder to the person who helps you:`r`n$dir") 'Warning'
    }
    $form.Close()
})

$form.Add_FormClosing({ Read-Settings; Save-Settings })
[void](Update-GameStatus)
[void]$form.ShowDialog()

} catch {
    $msg = "The launcher ran into a problem:`r`n$($_.Exception.Message)`r`n(line $($_.InvocationInfo.ScriptLineNumber))"
    $msg | Out-File -Encoding ASCII (Join-Path $dir 'launcher-error.txt')
    Show-Message $msg 'Error'
}
