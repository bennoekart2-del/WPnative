# Wii Party (native) launcher. Started by "Wii Party.bat". A pink sidebar with five pages: Play (find the
# game, press Play), Online, Settings, Controls (player 1 keys) and Help. Settings are kept in settings.ini
# next to it. Pictures in the art folder (title.jpg, cover.jpg, case.png, host.png) decorate the pages;
# without them the pages are drawn in plain colours. This file must stay ASCII (Windows PowerShell 5.1).
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

# ------------------------------------------------------------------ drawing helpers (C# 5: Windows' own compiler)
Add-Type -ReferencedAssemblies System.Windows.Forms, System.Drawing -TypeDefinition @'
// BEGIN UI
using System;
using System.Drawing;
using System.Drawing.Drawing2D;
using System.Drawing.Imaging;
using System.IO;
using System.Runtime.InteropServices;
using System.Windows.Forms;

namespace WPR {
    public static class Native {
        [DllImport("user32.dll")] public static extern short GetAsyncKeyState(int v);
        [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
    }

    // A window that lets the launcher see every key first (also arrows, Tab and F1), for the key picker.
    public class KeyForm : Form {
        public Func<Keys, bool> KeyHook;
        protected override bool ProcessCmdKey(ref Message msg, Keys keyData) {
            if (KeyHook != null && (msg.Msg == 0x100 || msg.Msg == 0x104) && KeyHook(keyData)) return true;
            return base.ProcessCmdKey(ref msg, keyData);
        }
    }

    // A page: a panel whose pre-drawn background picture is painted without flicker.
    public class Page : Panel {
        public Page() { DoubleBuffered = true; }
    }

    // A rounded, glossy button that is drawn over the page's picture.
    public class Pill : Control {
        public Color Color1 = Color.FromArgb(255, 60, 160);
        public Color Color2 = Color.FromArgb(214, 0, 120);
        public Color Fore = Color.White;
        public Color Hover = Color.Empty;
        public Color Border = Color.Empty;
        public float Radius = -1f;
        public bool LeftAlign;
        public bool Shadow = true;
        public bool Gloss = true;
        public string Icon;
        public Font IconFont;
        bool hover, down;

        public Pill() {
            SetStyle(ControlStyles.SupportsTransparentBackColor | ControlStyles.OptimizedDoubleBuffer |
                     ControlStyles.AllPaintingInWmPaint | ControlStyles.UserPaint | ControlStyles.ResizeRedraw, true);
            SetStyle(ControlStyles.Selectable, false);
            BackColor = Color.Transparent;
            Cursor = Cursors.Hand;
        }

        protected override void OnMouseEnter(EventArgs e) { hover = true; Invalidate(); base.OnMouseEnter(e); }
        protected override void OnMouseLeave(EventArgs e) { hover = false; down = false; Invalidate(); base.OnMouseLeave(e); }
        protected override void OnMouseDown(MouseEventArgs e) { if (e.Button == MouseButtons.Left) { down = true; Invalidate(); } base.OnMouseDown(e); }
        protected override void OnMouseUp(MouseEventArgs e) { down = false; Invalidate(); base.OnMouseUp(e); }
        protected override void OnTextChanged(EventArgs e) { Invalidate(); base.OnTextChanged(e); }
        protected override void OnEnabledChanged(EventArgs e) { Invalidate(); base.OnEnabledChanged(e); }

        static Color Mix(Color a, Color b, float t) {
            return Color.FromArgb((int)(a.A + (b.A - a.A) * t), (int)(a.R + (b.R - a.R) * t),
                                  (int)(a.G + (b.G - a.G) * t), (int)(a.B + (b.B - a.B) * t));
        }

        protected override void OnPaint(PaintEventArgs e) {
            Graphics g = e.Graphics;
            g.SmoothingMode = SmoothingMode.AntiAlias;
            float sh = Shadow ? Math.Max(2f, Height / 16f) : 0f;
            RectangleF r = new RectangleF(0.5f, 0.5f, Width - 1.5f, Height - 1.5f - sh);
            if (down) r.Offset(0, sh / 2f);
            float rad = Radius < 0 ? r.Height / 2f : Radius;
            Color c1 = Color1, c2 = Color2;
            if (!Enabled) { c1 = Color.FromArgb(c1.A, 205, 205, 210); c2 = Color.FromArgb(c2.A, 180, 180, 188); }
            else if (down) { c1 = Mix(c1, Color.FromArgb(c1.A, 0, 0, 0), 0.12f); c2 = Mix(c2, Color.FromArgb(c2.A, 0, 0, 0), 0.12f); }
            else if (hover) {
                if (Hover != Color.Empty) { c1 = Hover; c2 = Hover; }
                else { c1 = Mix(c1, Color.FromArgb(c1.A, 255, 255, 255), 0.18f); c2 = Mix(c2, Color.FromArgb(c2.A, 255, 255, 255), 0.18f); }
            }
            if (sh > 0 && c2.A > 0 && !down) {
                using (GraphicsPath sp = Art.Round(new RectangleF(r.X, r.Y + sh, r.Width, r.Height), rad))
                using (SolidBrush sb = new SolidBrush(Color.FromArgb(70, Mix(c2, Color.Black, 0.45f))))
                    g.FillPath(sb, sp);
            }
            using (GraphicsPath p = Art.Round(r, rad)) {
                if (c1.A > 0 || c2.A > 0) {
                    using (LinearGradientBrush b = new LinearGradientBrush(new RectangleF(r.X, r.Y - 1, r.Width, r.Height + 2), c1, c2, 90f))
                        g.FillPath(b, p);
                    if (Gloss) {
                        RectangleF top = new RectangleF(r.X + rad * 0.35f, r.Y + 1.5f, r.Width - rad * 0.7f, r.Height * 0.46f);
                        if (top.Width > 2 && top.Height > 2)
                            using (GraphicsPath gp = Art.Round(top, Math.Min(rad, top.Height / 2f)))
                            using (LinearGradientBrush gb = new LinearGradientBrush(new RectangleF(top.X, top.Y - 1, top.Width, top.Height + 2),
                                       Color.FromArgb(95, 255, 255, 255), Color.FromArgb(8, 255, 255, 255), 90f))
                                g.FillPath(gb, gp);
                    }
                }
                if (Border != Color.Empty)
                    using (Pen pen = new Pen(Border, Math.Max(1.5f, Height / 24f))) g.DrawPath(pen, p);
            }
            Color fc = Enabled ? Fore : Color.FromArgb(120, 120, 128);
            Rectangle tr = Rectangle.Round(r);
            TextFormatFlags f = TextFormatFlags.VerticalCenter | TextFormatFlags.SingleLine | TextFormatFlags.NoPrefix | TextFormatFlags.EndEllipsis;
            if (LeftAlign) {
                int pad = (int)(r.Height * 0.4f);
                int x = tr.X + pad;
                if (!string.IsNullOrEmpty(Icon) && IconFont != null) {
                    int iw = (int)(IconFont.Height * 1.5f);
                    TextRenderer.DrawText(g, Icon, IconFont, new Rectangle(x, tr.Y, iw, tr.Height), fc, f | TextFormatFlags.HorizontalCenter);
                    x += iw + pad / 2;
                }
                tr = new Rectangle(x, tr.Y, tr.Right - x, tr.Height);
                f |= TextFormatFlags.Left;
            } else {
                f |= TextFormatFlags.HorizontalCenter;
            }
            TextRenderer.DrawText(g, Text, Font, tr, fc, f);
        }
    }

    // Painting the page backgrounds. Coordinates are in the launcher's own units; the caller scales.
    public static class Art {
        public static GraphicsPath Round(RectangleF r, float rad) {
            GraphicsPath p = new GraphicsPath();
            float d = Math.Min(rad * 2f, Math.Min(r.Width, r.Height));
            if (d < 1f) { p.AddRectangle(r); return p; }
            p.AddArc(r.X, r.Y, d, d, 180, 90);
            p.AddArc(r.Right - d, r.Y, d, d, 270, 90);
            p.AddArc(r.Right - d, r.Bottom - d, d, d, 0, 90);
            p.AddArc(r.X, r.Bottom - d, d, d, 90, 90);
            p.CloseFigure();
            return p;
        }

        // Reads a picture without keeping the file open. null when it is missing or broken.
        public static Image Load(string path) {
            try {
                if (!File.Exists(path)) return null;
                using (MemoryStream ms = new MemoryStream(File.ReadAllBytes(path)))
                using (Image i = Image.FromStream(ms))
                    return new Bitmap(i);
            } catch { return null; }
        }

        static ImageAttributes Fade(float alpha) {
            ImageAttributes ia = new ImageAttributes();
            ia.SetWrapMode(WrapMode.TileFlipXY);
            if (alpha < 1f) {
                ColorMatrix cm = new ColorMatrix();
                cm.Matrix33 = alpha;
                ia.SetColorMatrix(cm);
            }
            return ia;
        }

        // Fills d with the picture, cutting off what sticks out. fx/fy: which part stays (0 = left/top, 1 = right/bottom).
        public static void Cover(Graphics g, Image img, RectangleF d, float fx, float fy, float alpha) {
            float s = Math.Max(d.Width / img.Width, d.Height / img.Height);
            float w = d.Width / s, h = d.Height / s;
            using (ImageAttributes ia = Fade(alpha))
                g.DrawImage(img, Rectangle.Round(d), (img.Width - w) * fx, (img.Height - h) * fy, w, h, GraphicsUnit.Pixel, ia);
        }

        // The whole picture inside d, keeping its shape, centred horizontally and at the bottom.
        public static RectangleF Contain(Graphics g, Image img, RectangleF d, float alpha) {
            float s = Math.Min(d.Width / img.Width, d.Height / img.Height);
            float w = img.Width * s, h = img.Height * s;
            RectangleF t = new RectangleF(d.X + (d.Width - w) / 2f, d.Bottom - h, w, h);
            using (ImageAttributes ia = Fade(alpha))
                g.DrawImage(img, Rectangle.Round(t), 0, 0, img.Width, img.Height, GraphicsUnit.Pixel, ia);
            return t;
        }

        // The picture cover-fitted into a rounded rectangle, with smooth edges.
        public static void RoundImage(Graphics g, Image img, RectangleF d, float rad) {
            float s = Math.Max(d.Width / img.Width, d.Height / img.Height);
            using (TextureBrush tb = new TextureBrush(img, WrapMode.Clamp)) {
                tb.TranslateTransform(d.X - (img.Width * s - d.Width) / 2f, d.Y - (img.Height * s - d.Height) / 2f);
                tb.ScaleTransform(s, s);
                using (GraphicsPath p = Round(d, rad)) g.FillPath(tb, p);
            }
        }

        public static void Gradient(Graphics g, RectangleF r, Color a, Color b, float angle) {
            using (LinearGradientBrush br = new LinearGradientBrush(new RectangleF(r.X - 1, r.Y - 1, r.Width + 2, r.Height + 2), a, b, angle))
                g.FillRectangle(br, r);
        }

        // A soft drop shadow, then the card itself.
        public static void Card(Graphics g, RectangleF r, float rad, Color fill) {
            for (int i = 6; i >= 1; i--) {
                RectangleF s = new RectangleF(r.X - i, r.Y - i + 4, r.Width + 2 * i, r.Height + 2 * i);
                using (GraphicsPath p = Round(s, rad + i))
                using (SolidBrush b = new SolidBrush(Color.FromArgb(7, 120, 0, 60)))
                    g.FillPath(b, p);
            }
            using (GraphicsPath p = Round(r, rad))
            using (SolidBrush b = new SolidBrush(fill))
                g.FillPath(b, p);
        }

        public static void Bubble(Graphics g, RectangleF r, PointF tail, Color fill) {
            using (GraphicsPath p = Round(r, r.Height / 2.5f)) {
                float cx = Math.Max(r.X + r.Height / 2f, Math.Min(r.Right - r.Height / 2f, tail.X));
                p.AddPolygon(new PointF[] { new PointF(cx - 10, r.Bottom - 2), tail, new PointF(cx + 8, r.Bottom - 2) });
                p.FillMode = FillMode.Winding;
                using (SolidBrush b = new SolidBrush(Color.FromArgb(40, 120, 0, 60))) {
                    g.TranslateTransform(0, 3); g.FillPath(b, p); g.TranslateTransform(0, -3);
                }
                using (SolidBrush b = new SolidBrush(fill)) g.FillPath(b, p);
            }
        }

        public static void Star(Graphics g, PointF c, float r, float turn, Brush b) {
            PointF[] pts = new PointF[10];
            for (int i = 0; i < 10; i++) {
                double a = turn + i * Math.PI / 5 - Math.PI / 2;
                float rr = (i % 2 == 0) ? r : r * 0.45f;
                pts[i] = new PointF(c.X + (float)(Math.Cos(a) * rr), c.Y + (float)(Math.Sin(a) * rr));
            }
            g.FillPolygon(b, pts);
        }

        // Little stars like the ones on the box, scattered over an area.
        public static void Stars(Graphics g, RectangleF area, int count, int seed, Color color, float min, float max) {
            Random rnd = new Random(seed);
            using (SolidBrush b = new SolidBrush(color))
                for (int i = 0; i < count; i++) {
                    PointF c = new PointF(area.X + (float)rnd.NextDouble() * area.Width, area.Y + (float)rnd.NextDouble() * area.Height);
                    Star(g, c, min + (float)rnd.NextDouble() * (max - min), (float)rnd.NextDouble(), b);
                }
        }

        public static void Ring(Graphics g, PointF c, float r, float width, Color color) {
            using (Pen p = new Pen(color, width)) g.DrawEllipse(p, c.X - r, c.Y - r, 2 * r, 2 * r);
        }
    }
}
// END UI
'@

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

$cfg = @{ disc = ''; scale = 'Auto'; aspect = '16:9'; fullscreen = '1'; sideways = '0'
         name = $env:USERNAME; local = '0'; friends = '1'; port = '47123'; join = '' }
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
function Get-KeyState([int]$vk) { return ([WPR.Native]::GetAsyncKeyState($vk) -band 0x8000) -ne 0 }

# Windows key -> SDL key name (what the game understands)
function Get-SdlName([System.Windows.Forms.Keys]$key) {
    $n = ([System.Windows.Forms.Keys]([int]$key -band 0xFFFF)).ToString()
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

# ------------------------------------------------------------------ look
# Everything is laid out on a 980 x 620 grid and scaled to the screen ($k), so it is sharp on any display.
[void][WPR.Native]::SetProcessDPIAware()
$g0 = [System.Drawing.Graphics]::FromHwnd([IntPtr]::Zero); $k = $g0.DpiX / 96; $g0.Dispose()
$wa = [System.Windows.Forms.Screen]::PrimaryScreen.WorkingArea
$k = [Math]::Max(0.7, [Math]::Min($k, [Math]::Min(($wa.Height * 0.95 - 40 * $k) / 620, $wa.Width * 0.95 / 980)))

function S([double]$v) { return [int][Math]::Round($v * $k) }
function New-Font([double]$px, [string]$style = 'Regular', [string]$family = 'Segoe UI', [switch]$Unscaled) {
    $size = if ($Unscaled) { $px } else { $px * $k }
    return New-Object System.Drawing.Font($family, [single]$size, [System.Drawing.FontStyle]$style, [System.Drawing.GraphicsUnit]::Pixel)
}
function RF([double]$x, [double]$y, [double]$w, [double]$h) { return New-Object System.Drawing.RectangleF([single]$x, [single]$y, [single]$w, [single]$h) }
function PF([double]$x, [double]$y) { return New-Object System.Drawing.PointF([single]$x, [single]$y) }
function Rgb([int]$r, [int]$g, [int]$b, [int]$a = 255) { return [System.Drawing.Color]::FromArgb($a, $r, $g, $b) }
function Set-Bounds($c, $x, $y, $w, $h) {
    $c.Location = New-Object System.Drawing.Point((S $x), (S $y))
    $c.Size = New-Object System.Drawing.Size((S $w), (S $h))
}

$pink = Rgb 230 0 126; $pinkDeep = Rgb 160 0 92; $pinkSoft = Rgb 255 232 244; $ink = Rgb 55 52 66; $muted = Rgb 112 106 124
$white = [System.Drawing.Color]::White; $clear = [System.Drawing.Color]::Transparent
$fBody = New-Font 14; $fSmall = New-Font 12.5; $fBold = New-Font 14 'Bold'; $fButton = New-Font 14 'Bold'

$art = Join-Path $dir 'art'
$imgTitle = [WPR.Art]::Load((Join-Path $art 'title.jpg'))
$imgCover = [WPR.Art]::Load((Join-Path $art 'cover.jpg'))
$imgCase = [WPR.Art]::Load((Join-Path $art 'case.png'))
$imgHost = [WPR.Art]::Load((Join-Path $art 'host.png'))

# Draws a page background once, at screen resolution, in layout units.
function New-Art([int]$w, [int]$h, [scriptblock]$draw) {
    $bmp = New-Object System.Drawing.Bitmap((S $w), (S $h))
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.SmoothingMode = 'AntiAlias'; $g.InterpolationMode = 'HighQualityBicubic'; $g.PixelOffsetMode = 'HighQuality'
    $g.TextRenderingHint = 'AntiAliasGridFit'
    $g.ScaleTransform([single]$k, [single]$k)
    & $draw $g | Out-Null
    $g.Dispose()
    return $bmp
}
function Write-Art($g, [string]$text, [double]$x, [double]$y, [double]$px, [string]$style = 'Bold', $color = $pink) {
    $f = New-Font $px $style -Unscaled
    $b = New-Object System.Drawing.SolidBrush($color)
    $g.DrawString($text, $f, $b, [single]$x, [single]$y)
    $b.Dispose(); $f.Dispose()
}
# a pink dot with a step number, then the step's title
function Write-Step($g, [string]$num, [string]$text, [double]$x, [double]$y) {
    $b = New-Object System.Drawing.SolidBrush($pink)
    $g.FillEllipse($b, [single]$x, [single]($y + 2), [single]24, [single]24); $b.Dispose()
    $f = New-Font 14 'Bold' -Unscaled
    $sf = New-Object System.Drawing.StringFormat; $sf.Alignment = 'Center'; $sf.LineAlignment = 'Center'
    $g.DrawString($num, $f, [System.Drawing.Brushes]::White, (RF $x ($y + 2.5) 24 24), $sf)
    $f.Dispose(); $sf.Dispose()
    Write-Art $g $text ($x + 32) $y 19
}
function Draw-PageBack($g, [int]$seed) {
    [WPR.Art]::Gradient($g, (RF 0 0 770 620), (Rgb 255 247 251), (Rgb 255 226 240), 90)
    [WPR.Art]::Stars($g, (RF 0 0 770 620), 34, $seed, (Rgb 200 160 185 70), 3, 7)
}

function Add-Text($parent, [string]$text, $x, $y, $w, $h, $font = $fBody, $color = $ink) {
    $l = New-Object System.Windows.Forms.Label
    $l.BackColor = $clear; $l.Text = $text; $l.Font = $font; $l.ForeColor = $color; $l.UseMnemonic = $false
    Set-Bounds $l $x $y $w $h
    $parent.Controls.Add($l); return $l
}
function Set-PillStyle($b, [string]$style) {
    switch ($style) {
        'pink' { $b.Color1 = Rgb 255 70 166; $b.Color2 = Rgb 214 0 120; $b.Fore = $white; $b.Border = [System.Drawing.Color]::Empty; $b.Hover = [System.Drawing.Color]::Empty }
        'blue' { $b.Color1 = Rgb 70 190 255; $b.Color2 = Rgb 0 140 220; $b.Fore = $white; $b.Border = [System.Drawing.Color]::Empty; $b.Hover = [System.Drawing.Color]::Empty }
        'white' { $b.Color1 = $white; $b.Color2 = Rgb 255 240 248; $b.Fore = $pink; $b.Border = Rgb 245 160 205; $b.Hover = Rgb 255 228 242 }
    }
    $b.Invalidate()
}
function Add-Pill($parent, [string]$text, $x, $y, $w, $h, [string]$style = 'pink', $font = $fButton) {
    $b = New-Object WPR.Pill
    $b.Text = $text; $b.Font = $font
    Set-Bounds $b $x $y $w $h
    Set-PillStyle $b $style
    $parent.Controls.Add($b); return $b
}
function Add-Box($parent, $x, $y, $w, $h, $font = $fBody) {
    $t = New-Object System.Windows.Forms.TextBox
    $t.Font = $font; $t.BorderStyle = 'FixedSingle'; $t.ForeColor = $ink
    Set-Bounds $t $x $y $w $h
    $parent.Controls.Add($t); return $t
}
function Add-Choice($parent, $x, $y, $w, [string[]]$items) {
    $c = New-Object System.Windows.Forms.ComboBox
    $c.DropDownStyle = 'DropDownList'; $c.Font = $fBody; $c.ForeColor = $ink
    Set-Bounds $c $x $y $w 30
    [void]$c.Items.AddRange($items)
    $parent.Controls.Add($c); return $c
}

# ------------------------------------------------------------------ window
$form = New-Object WPR.KeyForm
$form.Text = 'Wii Party'
$form.AutoScaleMode = 'None'
$form.FormBorderStyle = 'FixedSingle'
$form.MaximizeBox = $false
$form.StartPosition = 'CenterScreen'
$form.Font = $fBody
$form.BackColor = $pinkSoft
$form.ClientSize = New-Object System.Drawing.Size((S 980), (S 620))
if ($imgCover) {
    # window icon: the pink badge from the box
    $ib = New-Object System.Drawing.Bitmap(64, 64)
    $ig = [System.Drawing.Graphics]::FromImage($ib); $ig.SmoothingMode = 'AntiAlias'; $ig.InterpolationMode = 'HighQualityBicubic'
    $ig.ScaleTransform(64 / 1060, 64 / 1060); $ig.TranslateTransform(-110, -370)
    $ig.DrawImage($imgCover, 0, 0, $imgCover.Width, $imgCover.Height); $ig.Dispose()
    $form.Icon = [System.Drawing.Icon]::FromHandle($ib.GetHicon())
}

$pages = [ordered]@{}
function New-Page([string]$name, [scriptblock]$draw) {
    $p = New-Object WPR.Page
    Set-Bounds $p 210 0 770 620
    $p.BackgroundImage = New-Art 770 620 $draw
    $p.BackgroundImageLayout = 'None'
    $p.Visible = $false
    $form.Controls.Add($p); $pages[$name] = $p
    return $p
}

# ---- sidebar
$bar = New-Object WPR.Page
Set-Bounds $bar 0 0 210 620
$bar.BackgroundImageLayout = 'None'
$bar.BackgroundImage = New-Art 210 620 {
    param($g)
    [WPR.Art]::Gradient($g, (RF 0 0 210 620), (Rgb 238 20 140), (Rgb 150 0 86), 90)
    [WPR.Art]::Ring($g, (PF 30 600), 120, 18, (Rgb 255 255 255 22))
    [WPR.Art]::Ring($g, (PF 200 40), 70, 10, (Rgb 255 255 255 18))
    # stars above and below the menu, not behind it
    [WPR.Art]::Stars($g, (RF 0 0 210 262), 12, 11, (Rgb 255 255 255 70), 2.5, 6)
    [WPR.Art]::Stars($g, (RF 0 540 210 80), 5, 12, (Rgb 255 255 255 70), 2.5, 6)
    if ($imgCover) {
        [WPR.Art]::Card($g, (RF 33 24 144 203), 12, (Rgb 255 255 255))
        [WPR.Art]::RoundImage($g, $imgCover, (RF 36 27 138 197), 10)
    } else {
        $b = New-Object System.Drawing.SolidBrush((Rgb 255 255 255))
        $g.FillEllipse($b, 25, 30, 160, 160); $b.Dispose()
        Write-Art $g 'Wii' 64 62 40 'Bold' (Rgb 150 150 160)
        Write-Art $g 'Party' 46 106 44 'Bold' $pink
    }
    Write-Art $g 'on your PC' 62 236 15 'Bold' (Rgb 255 255 255 225)
    Write-Art $g 'Close the game window' 30 572 11.5 'Regular' (Rgb 255 255 255 170)
    Write-Art $g 'to come back here.' 30 588 11.5 'Regular' (Rgb 255 255 255 170)
}
$form.Controls.Add($bar)

$mdl = $null
if ((New-Object System.Drawing.Text.InstalledFontCollection).Families | Where-Object { $_.Name -eq 'Segoe MDL2 Assets' }) {
    $mdl = New-Font 17 'Regular' 'Segoe MDL2 Assets'
}
$navs = [ordered]@{}
$navList = @(@('Play', 0xE768), @('Online', 0xE774), @('Settings', 0xE713), @('Controls', 0xE765), @('Help', 0xE897))
for ($i = 0; $i -lt $navList.Count; $i++) {
    $n = $navList[$i][0]
    $b = Add-Pill $bar $n 14 (270 + $i * 54) 182 46 'pink' (New-Font 17 'Bold')
    $b.LeftAlign = $true; $b.Shadow = $false; $b.Gloss = $false
    if ($mdl) { $b.Icon = [string][char]$navList[$i][1]; $b.IconFont = $mdl }
    $b.Tag = $n
    $b.Add_Click({ param($s, $e) Show-Page $s.Tag })
    $navs[$n] = $b
}

# ---- Play page
$pPlay = New-Page 'Play' {
    param($g)
    $g.Clear((Rgb 255 228 241))
    if ($imgTitle) {
        [WPR.Art]::Cover($g, $imgTitle, (RF 0 0 770 452), 0.5, 0, 1)
        [WPR.Art]::Gradient($g, (RF 0 372 770 81), (Rgb 255 228 241 0), (Rgb 255 228 241), 90)
    } else {
        [WPR.Art]::Gradient($g, (RF 0 0 770 452), (Rgb 255 120 190), (Rgb 255 228 241), 90)
        [WPR.Art]::Ring($g, (PF 120 90), 110, 26, (Rgb 255 255 255 60))
        [WPR.Art]::Ring($g, (PF 660 220), 90, 20, (Rgb 255 255 255 60))
        [WPR.Art]::Stars($g, (RF 0 0 770 300), 30, 5, (Rgb 255 255 255 120), 4, 10)
        Write-Art $g 'Wii' 196 90 96 'Bold' (Rgb 140 140 150)
        Write-Art $g 'Party' 360 90 96 'Bold' $white
    }
    [WPR.Art]::Card($g, (RF 24 318 722 280), 22, $white)
    $pen = New-Object System.Drawing.Pen((Rgb 250 205 228), 1.5); $g.DrawLine($pen, 520, 338, 520, 578); $pen.Dispose()
    Write-Step $g '1' 'Your game' 48 334
    Write-Step $g '2' 'Controllers' 48 458
    Write-Step $g '3' 'Play!' 540 334
}
$gameStatus = Add-Text $pPlay '' 80 364 430 42 $fBody
$findBtn = Add-Pill $pPlay 'Find my game' 80 410 136 36 'blue' (New-Font 13 'Bold')
$chooseBtn = Add-Pill $pPlay 'Choose folder...' 222 410 136 36 'white' (New-Font 13 'Bold')
$noGameBtn = Add-Pill $pPlay "I don't have it yet" 364 410 144 36 'white' (New-Font 13 'Bold')
Add-Text $pPlay ("Player 1: keyboard and mouse. Point with the mouse, left click = A, right click = B.`r`n" +
    "Players 2, 3 and 4: plug in gamepads (Xbox, PlayStation, Switch) before you press PLAY.") 80 488 430 60 $fSmall | Out-Null
Add-Text $pPlay "In the game: F11 = full screen, F1 = hold the Remote sideways or upright." 48 552 460 36 $fSmall $muted | Out-Null
$play = Add-Pill $pPlay 'PLAY' 544 376 182 100 'pink' (New-Font 38 'Bold')
$playStatus = Add-Text $pPlay '' 540 488 190 96 $fSmall $muted
$playStatus.TextAlign = 'TopCenter'

function Update-GameStatus {
    $p = Resolve-GameFolder $cfg.disc
    if ($p) {
        $cfg.disc = $p
        $gameStatus.ForeColor = Rgb 0 140 70
        $gameStatus.Text = "Found your game:`r`n$p"
        $playStatus.Text = 'Ready! Press PLAY.'
        return $true
    }
    $gameStatus.ForeColor = Rgb 200 20 60
    $gameStatus.Text = "The game hasn't been found yet. Press 'Find my game'.`r`nNo copy of your disc yet? Press 'I don't have it yet'."
    $playStatus.Text = 'First find your game (step 1).'
    return $false
}

$findBtn.Add_Click({
    $form.Cursor = [System.Windows.Forms.Cursors]::WaitCursor; $gameStatus.ForeColor = $ink
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

# ---- Online page: everyone runs the game; the host's game and the friends' games swap only controller input
$pOnline = New-Page 'Online' {
    param($g)
    Draw-PageBack $g 21
    if ($imgCover) { [WPR.Art]::Cover($g, $imgCover, (RF 0 0 770 620), 0.5, 0.45, 0.16) }
    Write-Art $g 'Play online' 30 18 32
    [WPR.Art]::Card($g, (RF 24 112 560 92), 18, $white)
    [WPR.Art]::Card($g, (RF 24 216 272 196), 18, $white)
    [WPR.Art]::Card($g, (RF 312 216 272 196), 18, $white)
    [WPR.Art]::Card($g, (RF 24 424 560 176), 18, $white)
    Write-Art $g 'Host a game' 42 226 19
    Write-Art $g "Join a friend's game" 330 226 19
    if ($imgHost) {
        [WPR.Art]::Bubble($g, (RF 598 120 160 66), (PF 646 232), $white)
        Write-Art $g "Let's party" 618 128 17 'Bold' $pink
        Write-Art $g 'together!' 624 152 17 'Bold' $pink
        [void][WPR.Art]::Contain($g, $imgHost, (RF 596 230 170 380), 1)
    } else {
        [WPR.Art]::Stars($g, (RF 600 120 160 460), 10, 3, (Rgb 230 0 126 90), 8, 18)
    }
}
Add-Text $pOnline ("Everyone needs Wii Party on their own PC: their own disc and this same version of the game. " +
    "One person hosts, the others join with the room code.") 32 62 556 44 $fSmall $muted | Out-Null
Add-Text $pOnline 'Your name' 44 124 240 22 $fBold | Out-Null
$netName = Add-Box $pOnline 44 150 240 30
$netName.Text = $cfg.name
Add-Text $pOnline 'Players on this PC' 300 124 264 22 $fBold | Out-Null
$localChoices = @(@('0', '1 player: keyboard and mouse'), @('1', '1 player: gamepad'), @('0,1', '2 players: keyboard + 1 gamepad'),
    @('1,2', '2 players: 2 gamepads'), @('0,1,2', '3 players'), @('0,1,2,3', '4 players'))
$netLocal = Add-Choice $pOnline 300 150 264 @($localChoices | ForEach-Object { $_[1] })
$netLocal.SelectedIndex = [Math]::Max(0, [Array]::IndexOf(@($localChoices | ForEach-Object { $_[0] }), $cfg.local))

Add-Text $pOnline 'Friends joining' 44 266 130 26 $fBody | Out-Null
$netFriends = Add-Choice $pOnline 180 262 70 @('1', '2', '3')
$netFriends.SelectedIndex = [Math]::Max(0, [Math]::Min(2, [int]$cfg.friends - 1))
$hostInfo = Add-Text $pOnline '' 44 298 160 38 (New-Font 24 'Bold') $pink
$copyBtn = Add-Pill $pOnline 'Copy' 206 302 70 30 'white' (New-Font 12.5 'Bold')
$copyBtn.Shadow = $false; $copyBtn.Visible = $false
$hostBtn = Add-Pill $pOnline 'HOST' 44 342 232 50 'pink' (New-Font 20 'Bold')

Add-Text $pOnline 'Room code' 332 262 232 24 $fBody | Out-Null
$netJoin = Add-Box $pOnline 332 288 232 40 (New-Font 22 'Bold')
$netJoin.TextAlign = 'Center'; $netJoin.CharacterCasing = 'Upper'; $netJoin.Text = $cfg.join
$joinBtn = Add-Pill $pOnline 'JOIN' 332 342 232 50 'blue' (New-Font 20 'Bold')

$netHelp = New-Object System.Windows.Forms.TextBox
$netHelp.Multiline = $true; $netHelp.ReadOnly = $true; $netHelp.ScrollBars = 'Vertical'; $netHelp.BorderStyle = 'None'
$netHelp.BackColor = $white; $netHelp.ForeColor = $ink; $netHelp.Font = $fSmall
Set-Bounds $netHelp 44 438 524 112
$netHelp.Text = ("HOW IT WORKS`r`n" +
    "1. The host picks how many friends join and presses HOST. The game window opens and waits for them.`r`n" +
    "2. The host sends the room code shown here to the friends (it is already copied: paste it in a chat).`r`n" +
    "3. Each friend types the room code and presses JOIN. When everyone is in, the game starts on all PCs.`r`n" +
    "The host's Miis and saves are used by everyone. Saves only change on the host's PC.`r`n`r`n" +
    "CAN'T CONNECT?`r`n" +
    "- If Windows asks about the firewall when you host, press Allow.`r`n" +
    "- Try once more: the games find each other through your routers by themselves, and that sometimes needs a second try.`r`n" +
    "- Some routers (and most phone hotspots) don't allow that. Then the host opens UDP port 47123 to their PC in the router settings (search for 'port forwarding' with your router's name), and friends type the host's internet address (search 'what is my IP' on the host's PC) instead of the room code.`r`n" +
    "- Or: everyone installs a free 'virtual LAN' program such as Radmin VPN or ZeroTier, joins the same network, and friends type the host's address from that program instead of the room code.`r`n" +
    "- 'Different version': everyone needs the same game version and a Wii Party disc from the same region.`r`n`r`n" +
    "FRIEND WITHOUT THE GAME?`r`n" +
    "They can still play on the host's PC through Parsec (free): it turns their gamepad into a controller on the host's PC. Press 'Get Parsec' below.")
$pOnline.Controls.Add($netHelp)
$parsecBtn = Add-Pill $pOnline 'Get Parsec' 44 558 140 32 'white' (New-Font 12.5 'Bold')
$parsecBtn.Shadow = $false
$parsecBtn.Add_Click({ Start-Process 'https://parsec.app/downloads' })
$copyBtn.Add_Click({ if ($script:shareAddress) { [System.Windows.Forms.Clipboard]::SetText($script:shareAddress); $copyBtn.Text = 'Copied' } })

# Asks the router to send the port to this PC (UPnP). Returns $true when it agreed.
function Open-RouterPort([int]$port) {
    try {
        $ip = (Get-NetIPConfiguration | Where-Object { $_.IPv4DefaultGateway } | Select-Object -First 1).IPv4Address.IPAddress
        if (-not $ip) { return $false }
        $maps = (New-Object -ComObject HNetCfg.NATUPnP).StaticPortMappingCollection
        if (-not $maps) { return $false }
        try { $maps.Remove($port, 'UDP') } catch { }
        [void]$maps.Add($port, 'UDP', $port, $ip, $true, 'Wii Party online')
        return $true
    } catch { return $false }
}

# ---- Settings page
$pSet = New-Page 'Settings' {
    param($g)
    Draw-PageBack $g 31
    Write-Art $g 'Settings' 30 18 32
    [WPR.Art]::Card($g, (RF 24 96 440 344), 18, $white)
    if ($imgCase) {
        [WPR.Art]::Ring($g, (PF 618 300), 160, 22, (Rgb 255 255 255 150))
        [void][WPR.Art]::Contain($g, $imgCase, (RF 480 110 276 360), 1)
    }
}
Add-Text $pSet 'Picture quality' 44 112 400 24 $fBold | Out-Null
$scale = Add-Choice $pSet 44 138 400 @('Automatic (recommended)', 'Original (fastest, blurry)', 'Sharp (2x)', 'Sharper (3x, good for 1080p)',
    'Very sharp (4x)', 'Ultra (5x, good for 4K)', 'Maximum (6x, needs a strong PC)')
$scale.SelectedIndex = if ($cfg.scale -match '^[1-6]$') { [int]$cfg.scale } else { 0 }
Add-Text $pSet 'Screen shape' 44 184 400 24 $fBold | Out-Null
$aspect = Add-Choice $pSet 44 210 400 @('Widescreen 16:9 (recommended)', 'Old TV 4:3')
$aspect.SelectedIndex = if ($cfg.aspect -eq '4:3') { 1 } else { 0 }
$full = New-Object System.Windows.Forms.CheckBox
$full.Text = 'Start in full screen (F11 switches in the game)'; $full.BackColor = $clear; $full.ForeColor = $ink
Set-Bounds $full 44 256 400 28; $full.Checked = $cfg.fullscreen -eq '1'; $pSet.Controls.Add($full)
$side = New-Object System.Windows.Forms.CheckBox
$side.Text = 'Start with the Remote held sideways (F1 switches)'; $side.BackColor = $clear; $side.ForeColor = $ink
Set-Bounds $side 44 288 400 28; $side.Checked = $cfg.sideways -eq '1'; $pSet.Controls.Add($side)
Add-Text $pSet "If the game is slow, pick a lower picture quality. 'Automatic' picks what suits your screen." 44 326 400 44 $fSmall $muted | Out-Null
$resetAll = Add-Pill $pSet 'Back to recommended settings' 44 380 280 40 'white'
$resetAll.Add_Click({ $scale.SelectedIndex = 0; $aspect.SelectedIndex = 0; $full.Checked = $true; $side.Checked = $false })

# ---- Controls page
$pKeys = New-Page 'Controls' {
    param($g)
    Draw-PageBack $g 41
    Write-Art $g 'Controls' 30 18 32
    [WPR.Art]::Card($g, (RF 24 100 722 392), 18, $white)
}
Add-Text $pKeys 'Player 1 keys: click a key, then press the key you want to use instead.' 32 64 700 26 $fBody $muted | Out-Null
$waiting = $null
$keyButtons = @{}
$fKeyLabel = New-Font 13; $fKey = New-Font 13 'Bold'
for ($i = 0; $i -lt $actions.Count; $i++) {
    $a = $actions[$i]
    $x = 44 + [Math]::Floor($i / 8) * 234; $y = 118 + ($i % 8) * 46
    Add-Text $pKeys $a[1] $x ($y + 6) 118 24 $fKeyLabel | Out-Null
    $b = Add-Pill $pKeys $cfg['key_' + $a[0]] ($x + 118) $y 104 34 'white' $fKey
    $b.Shadow = $false; $b.Gloss = $false; $b.Tag = $a[0]
    $b.Add_Click({
        param($s, $e)
        if ($script:waiting) { $script:waiting.Text = $cfg['key_' + $script:waiting.Tag]; Set-PillStyle $script:waiting 'white' }
        $script:waiting = $s; $s.Text = 'press a key'; Set-PillStyle $s 'pink'
    })
    $keyButtons[$a[0]] = $b
}
$resetKeys = Add-Pill $pKeys 'Default keys' 24 508 160 40 'white'
$resetKeys.Add_Click({ foreach ($a in $actions) { $cfg['key_' + $a[0]] = $a[2]; $keyButtons[$a[0]].Text = $a[2] } })
Add-Text $pKeys ("Mouse: point, left click = A, right click = B, middle click = shake, wheel = swing.`r`n" +
    "Gamepads: A/B/X/Y buttons, right stick points, left stick tilts.") 204 508 540 60 $fSmall $muted | Out-Null
function Stop-KeyWait {
    if (-not $script:waiting) { return }
    $script:waiting.Text = $cfg['key_' + $script:waiting.Tag]; Set-PillStyle $script:waiting 'white'
    $script:waiting = $null
}
# while a key button waits, every key press goes to it (arrows and F1 too)
$form.KeyHook = [Func[System.Windows.Forms.Keys, bool]] {
    param([System.Windows.Forms.Keys]$keyData)
    if (-not $script:waiting) { return $false }
    $name = Get-SdlName $keyData
    if ($name) {
        $cfg['key_' + $script:waiting.Tag] = $name
        Stop-KeyWait
    }
    return $true
}

# ---- Help page
$pHelp = New-Page 'Help' {
    param($g)
    Draw-PageBack $g 51
    Write-Art $g 'Help' 30 18 32
    [WPR.Art]::Card($g, (RF 24 80 722 518), 18, $white)
}
$help = New-Object System.Windows.Forms.TextBox
$help.Multiline = $true; $help.ReadOnly = $true; $help.ScrollBars = 'Vertical'; $help.BorderStyle = 'None'
$help.BackColor = $white; $help.ForeColor = $ink; $help.Font = $fBody
Set-Bounds $help 48 100 678 480
$help.Text = ("HOW TO START`r`n" +
    "1. On the Play page, press 'Find my game'.`r`n2. Plug in any gamepads.`r`n3. Press PLAY.`r`n`r`n" +
    "GETTING THE GAME FOLDER`r`n" + $extractHelp + "`r`n`r`n" +
    "IN THE GAME`r`n" +
    "Mouse = pointer. Left click = A, right click = B.`r`n" +
    "Arrow keys or W A S D = the Remote's + pad. Left Shift or middle click = shake.`r`n" +
    "Q / E = turn the Remote left / right, R / F = tilt it forward / back.`r`n" +
    "F1 = hold the Remote sideways or upright (the window title shows which). Most minigames want it sideways.`r`n" +
    "F11 = full screen on/off. Close the window to quit.`r`n`r`n" +
    "SOMETHING WRONG?`r`n" +
    "- The game is slow: Settings > Picture quality > Original.`r`n" +
    "- A gamepad does nothing: plug it in before pressing PLAY.`r`n" +
    "- The game closed by itself: send the file log.txt from this folder to the person who helps you.`r`n" +
    "- Windows said 'Windows protected your PC': press 'More info' and then 'Run anyway'.`r`n`r`n" +
    "PLAYING ONLINE`r`nSee the Online page: one person presses HOST, the others type the room code and press JOIN.")
$pHelp.Controls.Add($help)

function Show-Page([string]$name) {
    Stop-KeyWait
    foreach ($n in $pages.Keys) { if ($n -eq $name) { $pages[$n].Visible = $true } }
    foreach ($n in $pages.Keys) { if ($n -ne $name) { $pages[$n].Visible = $false } }
    foreach ($n in $navs.Keys) {
        $b = $navs[$n]
        if ($n -eq $name) { $b.Color1 = $white; $b.Color2 = $white; $b.Fore = $pink; $b.Hover = $white }
        else { $b.Color1 = $clear; $b.Color2 = $clear; $b.Fore = $white; $b.Hover = Rgb 255 255 255 45 }
        $b.Invalidate()
    }
    $form.ActiveControl = $null
}

# ------------------------------------------------------------------ play
function Read-Settings {
    $cfg.scale = if ($scale.SelectedIndex -le 0) { 'Auto' } else { [string]$scale.SelectedIndex }
    $cfg.aspect = if ($aspect.SelectedIndex -eq 1) { '4:3' } else { '16:9' }
    $cfg.fullscreen = if ($full.Checked) { '1' } else { '0' }
    $cfg.sideways = if ($side.Checked) { '1' } else { '0' }
    $cfg.name = $netName.Text.Trim()
    $cfg.local = $localChoices[[Math]::Max(0, $netLocal.SelectedIndex)][0]
    $cfg.friends = [string]($netFriends.SelectedIndex + 1)
    $cfg.join = $netJoin.Text.Trim()
}

# Starts the game. $online: the WPR_NET_* settings for an online game, or empty.
function Start-Game([hashtable]$online) {
    Read-Settings
    if (-not (Update-GameStatus)) {
        Show-Page 'Play'
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
        [void](Update-GameStatus)
    }
    if (-not (Test-Path -LiteralPath $exe)) {
        Show-Message "The game program (wiiparty-native.exe) isn't in this folder:`r`n$dir`r`n`r`nThis launcher has to be in the same folder as the game program." 'Error'
        return
    }
    # a friend's game started on the same PC (to try online play) writes its own log
    $log = if ($online -and $online.WPR_NET_JOIN) { 'log-join.txt' } else { 'log.txt' }
    if (Test-Path $log) { Move-Item -Force $log ($log -replace '\.txt$', '-previous.txt') }
    $env:WPR_ASPECT = $cfg.aspect
    if ($cfg.scale -ne 'Auto') { $env:WPR_SCALE = $cfg.scale } else { Remove-Item Env:WPR_SCALE -ErrorAction SilentlyContinue }
    $env:WPR_FULLSCREEN = $cfg.fullscreen
    $env:WPR_SIDEWAYS = $cfg.sideways
    foreach ($a in $actions) { Set-Item ('Env:WPR_KEY_' + $a[0]) $cfg['key_' + $a[0]] }
    foreach ($k2 in 'WPR_NET_HOST', 'WPR_NET_JOIN', 'WPR_NET_ROOM', 'WPR_NET_PLAYERS', 'WPR_NET_NAME', 'WPR_NET_LOCAL') { Remove-Item "Env:$k2" -ErrorAction SilentlyContinue }
    if ($online) { foreach ($k2 in $online.Keys) { Set-Item "Env:$k2" $online[$k2] } }
    $started = Get-Date
    $proc = Start-Process -FilePath $exe -ArgumentList ('"' + $cfg.disc + '"') -WorkingDirectory $dir `
        -RedirectStandardError (Join-Path $dir $log) -NoNewWindow -PassThru
    $null = $proc.Handle  # keeps the exit code readable after it ends
    $form.WindowState = 'Minimized'
    $proc.WaitForExit()
    if ($proc.ExitCode -eq 2) {
        # an online game that couldn't start: the game already said why; back to the launcher
        $form.WindowState = 'Normal'
        return
    }
    if ($proc.ExitCode -ne 0) {
        Show-Message ("The game stopped unexpectedly.`r`n`r`nPlease send the file $log from this folder to the person who helps you:`r`n$dir") 'Error'
    } elseif (((Get-Date) - $started).TotalSeconds -lt 5) {
        Show-Message ("The game closed right away.`r`n`r`nPlease send the file $log from this folder to the person who helps you:`r`n$dir") 'Warning'
    }
    $form.Close()
}

$play.Add_Click({ Start-Game $null })

# A room code: 6 letters and digits that are hard to mix up, with at least one of each
function New-RoomCode {
    $letters = 'ABCDEFGHJKLMNPQRSTUVWXYZ'; $digits = '23456789'; $all = $letters + $digits
    do { $code = -join (1..6 | ForEach-Object { $all[(Get-Random -Maximum $all.Length)] }) }
    until ($code -match '[A-Z]' -and $code -match '[0-9]')
    return $code
}

$hostBtn.Add_Click({
    Read-Settings
    $port = if ($cfg.port -match '^\d+$') { [int]$cfg.port } else { 47123 }
    $form.Cursor = [System.Windows.Forms.Cursors]::WaitCursor; $form.Refresh()
    [void](Open-RouterPort $port)  # helps with strict routers; the room code works without it too
    $form.Cursor = [System.Windows.Forms.Cursors]::Default
    $script:shareAddress = New-RoomCode
    $hostInfo.Text = $script:shareAddress
    $copyBtn.Text = 'Copy'; $copyBtn.Visible = $true
    [System.Windows.Forms.Clipboard]::SetText($script:shareAddress)
    Show-Message ("Send your friends this room code (it is already copied, paste it in a chat):`r`n`r`n$($script:shareAddress)`r`n`r`nThe game opens now and waits until they have joined. The code is also in the game window's title.")
    Start-Game @{ WPR_NET_HOST = [string]$port; WPR_NET_ROOM = $script:shareAddress; WPR_NET_PLAYERS = $cfg.friends; WPR_NET_NAME = $cfg.name; WPR_NET_LOCAL = $cfg.local }
})

$joinBtn.Add_Click({
    Read-Settings
    if (-not $cfg.join) { Show-Message "Type the room code the host sent you first." 'Warning'; return }
    Start-Game @{ WPR_NET_JOIN = $cfg.join; WPR_NET_NAME = $cfg.name; WPR_NET_LOCAL = $cfg.local }
})

$form.Add_FormClosing({ Read-Settings; Save-Settings })
Show-Page 'Play'
[void](Update-GameStatus)
[void]$form.ShowDialog()

} catch {
    $msg = "The launcher ran into a problem:`r`n$($_.Exception.Message)`r`n(line $($_.InvocationInfo.ScriptLineNumber))"
    $msg | Out-File -Encoding ASCII (Join-Path $dir 'launcher-error.txt')
    Show-Message $msg 'Error'
}
