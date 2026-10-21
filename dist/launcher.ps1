# Wii Party (native) launcher: settings window, then starts the game with them.
# Settings are kept in settings.ini next to this file.
Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing
[System.Windows.Forms.Application]::EnableVisualStyles()

$dir = Split-Path -Parent $MyInvocation.MyCommand.Path
Set-Location $dir
$ini = Join-Path $dir 'settings.ini'

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

$cfg = @{ disc = ''; scale = 'Auto'; aspect = '16:9'; fullscreen = '0'; sideways = '0' }
foreach ($a in $actions) { $cfg['key_' + $a[0]] = $a[2] }
if (Test-Path $ini) {
    foreach ($line in Get-Content $ini) {
        if ($line -match '^\s*([^=#]+?)\s*=\s*(.*)$') { $cfg[$matches[1]] = $matches[2] }
    }
}
if (-not $cfg.disc -and (Test-Path (Join-Path $dir '..\DATA'))) { $cfg.disc = (Resolve-Path (Join-Path $dir '..\DATA')).Path }

function Save-Settings {
    $cfg.Keys | Sort-Object | ForEach-Object { "$_=$($cfg[$_])" } | Set-Content -Encoding ASCII $ini
}

# Windows key -> SDL key name (what the game understands)
function Get-SdlName([System.Windows.Forms.KeyEventArgs]$e) {
    $k = $e.KeyCode
    $n = $k.ToString()
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

Add-Type -Namespace W -Name K -MemberDefinition '[DllImport("user32.dll")] public static extern short GetAsyncKeyState(int v);'
function Get-KeyState([int]$vk) { return ([W.K]::GetAsyncKeyState($vk) -band 0x8000) -ne 0 }

$form = New-Object System.Windows.Forms.Form
$form.Text = 'Wii Party launcher'
$form.FormBorderStyle = 'FixedDialog'
$form.MaximizeBox = $false
$form.StartPosition = 'CenterScreen'
$form.Font = New-Object System.Drawing.Font('Segoe UI', 10)
$form.ClientSize = New-Object System.Drawing.Size(520, 760)
$form.KeyPreview = $true

function Add-Label($text, $x, $y, $w = 160) {
    $l = New-Object System.Windows.Forms.Label
    $l.Text = $text; $l.Location = New-Object System.Drawing.Point($x, $y); $l.Size = New-Object System.Drawing.Size($w, 24)
    $form.Controls.Add($l); return $l
}

$y = 16
Add-Label 'Game folder (your disc)' 16 ($y + 3) | Out-Null
$disc = New-Object System.Windows.Forms.TextBox
$disc.Location = New-Object System.Drawing.Point(180, $y); $disc.Size = New-Object System.Drawing.Size(240, 24); $disc.Text = $cfg.disc
$form.Controls.Add($disc)
$browse = New-Object System.Windows.Forms.Button
$browse.Text = 'Browse'; $browse.Location = New-Object System.Drawing.Point(426, ($y - 1)); $browse.Size = New-Object System.Drawing.Size(78, 28)
$browse.Add_Click({
    $d = New-Object System.Windows.Forms.FolderBrowserDialog
    $d.Description = 'Pick the extracted disc folder (the one with "files" and "sys" inside)'
    if ($d.ShowDialog() -eq 'OK') { $disc.Text = $d.SelectedPath }
})
$form.Controls.Add($browse)

$y += 40
Add-Label 'Upscale' 16 ($y + 3) | Out-Null
$scale = New-Object System.Windows.Forms.ComboBox
$scale.DropDownStyle = 'DropDownList'; $scale.Location = New-Object System.Drawing.Point(180, $y); $scale.Size = New-Object System.Drawing.Size(324, 24)
$scaleItems = @('Auto (fits your screen)', '1x (original, 640x528)', '2x (1280x1056)', '3x (1920x1584, good for 1080p/1440p)',
                '4x (2560x2112)', '5x (3200x2640, good for 4K)', '6x (3840x3168)')
$scale.Items.AddRange($scaleItems)
$scale.SelectedIndex = if ($cfg.scale -match '^[1-6]$') { [int]$cfg.scale } else { 0 }
$form.Controls.Add($scale)

$y += 40
Add-Label 'Screen shape' 16 ($y + 3) | Out-Null
$aspect = New-Object System.Windows.Forms.ComboBox
$aspect.DropDownStyle = 'DropDownList'; $aspect.Location = New-Object System.Drawing.Point(180, $y); $aspect.Size = New-Object System.Drawing.Size(324, 24)
$aspect.Items.AddRange(@('16:9 widescreen', '4:3 (original TV)'))
$aspect.SelectedIndex = if ($cfg.aspect -eq '4:3') { 1 } else { 0 }
$form.Controls.Add($aspect)

$y += 40
$full = New-Object System.Windows.Forms.CheckBox
$full.Text = 'Start in full screen (F11 switches)'; $full.Location = New-Object System.Drawing.Point(180, $y); $full.Size = New-Object System.Drawing.Size(324, 24)
$full.Checked = $cfg.fullscreen -eq '1'; $form.Controls.Add($full)
$y += 30
$side = New-Object System.Windows.Forms.CheckBox
$side.Text = 'Start with the Remote held sideways (F1 switches)'; $side.Location = New-Object System.Drawing.Point(180, $y); $side.Size = New-Object System.Drawing.Size(324, 24)
$side.Checked = $cfg.sideways -eq '1'; $form.Controls.Add($side)

$y += 40
$hdr = Add-Label 'Player 1 keys (click a button, then press a key)' 16 $y 488
$hdr.Font = New-Object System.Drawing.Font('Segoe UI', 10, [System.Drawing.FontStyle]::Bold)
$y += 28
$waiting = $null
$keyButtons = @{}
$col = 0
foreach ($a in $actions) {
    $x = if ($col -eq 0) { 16 } else { 268 }
    Add-Label $a[1] $x ($y + 3) 110 | Out-Null
    $b = New-Object System.Windows.Forms.Button
    $b.Text = $cfg['key_' + $a[0]]; $b.Tag = $a[0]
    $b.Location = New-Object System.Drawing.Point(($x + 112), $y); $b.Size = New-Object System.Drawing.Size(124, 28)
    $b.Add_Click({ param($s, $e) $script:waiting = $s; $s.Text = 'press a key...' })
    $form.Controls.Add($b)
    $keyButtons[$a[0]] = $b
    if ($col -eq 1) { $y += 34 }
    $col = 1 - $col
}
if ($col -eq 1) { $y += 34 }
$form.Add_KeyDown({
    param($s, $e)
    if ($script:waiting) {
        $name = Get-SdlName $e
        if ($name) {
            $script:waiting.Text = $name
            $cfg['key_' + $script:waiting.Tag] = $name
            $script:waiting = $null
            $form.ActiveControl = $null  # so the key's release does not click a button
        }
        $e.Handled = $true; $e.SuppressKeyPress = $true
    }
})
# Enter, arrows and Tab normally move between buttons; let them be bound instead
$form.Add_PreviewKeyDown({ param($s, $e) if ($script:waiting) { $e.IsInputKey = $true } })
foreach ($b in $keyButtons.Values) { $b.Add_PreviewKeyDown({ param($s, $e) if ($script:waiting) { $e.IsInputKey = $true } }) }

$reset = New-Object System.Windows.Forms.Button
$reset.Text = 'Default keys'; $reset.Location = New-Object System.Drawing.Point(16, ($y + 6)); $reset.Size = New-Object System.Drawing.Size(120, 30)
$reset.Add_Click({ foreach ($a in $actions) { $cfg['key_' + $a[0]] = $a[2]; $keyButtons[$a[0]].Text = $a[2] } })
$form.Controls.Add($reset)

$y += 44
$note = Add-Label 'Gamepads are players 2-4 (A/B/X/Y, sticks for pointer and tilt). Mouse: pointer, left click A, right click B, middle click shake, wheel swings.' 16 $y 488
$note.Size = New-Object System.Drawing.Size(488, 40)

$y += 50
$play = New-Object System.Windows.Forms.Button
$play.Text = 'Play'; $play.Font = New-Object System.Drawing.Font('Segoe UI', 12, [System.Drawing.FontStyle]::Bold)
$play.Location = New-Object System.Drawing.Point(304, $y); $play.Size = New-Object System.Drawing.Size(200, 44)
$form.AcceptButton = $null
$form.Controls.Add($play)
$status = Add-Label '' 16 ($y + 10) 280
$form.ClientSize = New-Object System.Drawing.Size(520, ($y + 60))

$play.Add_Click({
    $cfg.disc = $disc.Text.Trim('"', ' ')
    $cfg.scale = if ($scale.SelectedIndex -eq 0) { 'Auto' } else { [string]$scale.SelectedIndex }
    $cfg.aspect = if ($aspect.SelectedIndex -eq 1) { '4:3' } else { '16:9' }
    $cfg.fullscreen = if ($full.Checked) { '1' } else { '0' }
    $cfg.sideways = if ($side.Checked) { '1' } else { '0' }
    Save-Settings
    if (-not (Test-Path (Join-Path $cfg.disc 'sys\main.dol'))) {
        [System.Windows.Forms.MessageBox]::Show('That folder is not an extracted Wii Party disc (no sys\main.dol inside).', 'Wii Party') | Out-Null
        return
    }
    if (Test-Path 'wp.zip.001') {
        $status.Text = 'Unpacking the new version...'; $form.Refresh()
        $parts = Get-ChildItem 'wp.zip.0*' | Sort-Object Name
        $out = [System.IO.File]::Create((Join-Path $dir 'wp.zip'))
        foreach ($p in $parts) { $bytes = [System.IO.File]::ReadAllBytes($p.FullName); $out.Write($bytes, 0, $bytes.Length) }
        $out.Close()
        tar -xf wp.zip
        if ($LASTEXITCODE -ne 0) { [System.Windows.Forms.MessageBox]::Show('Unpacking failed.', 'Wii Party') | Out-Null; return }
        Remove-Item wp.zip; $parts | Remove-Item
    }
    if (Test-Path 'log.txt') { Move-Item -Force 'log.txt' 'log-previous.txt' }
    $env:WPR_ASPECT = $cfg.aspect
    if ($cfg.scale -ne 'Auto') { $env:WPR_SCALE = $cfg.scale } else { Remove-Item Env:WPR_SCALE -ErrorAction SilentlyContinue }
    $env:WPR_FULLSCREEN = $cfg.fullscreen
    $env:WPR_SIDEWAYS = $cfg.sideways
    foreach ($a in $actions) { Set-Item ('Env:WPR_KEY_' + $a[0]) $cfg['key_' + $a[0]] }
    Start-Process -FilePath (Join-Path $dir 'wiiparty-native.exe') -ArgumentList ('"' + $cfg.disc + '"') `
        -WorkingDirectory $dir -RedirectStandardError (Join-Path $dir 'log.txt')
    $form.Close()
})

[void]$form.ShowDialog()
