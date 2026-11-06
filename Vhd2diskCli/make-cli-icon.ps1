# Builds the CLI icon: the GUI icon (v2d.ico) with a small terminal badge
# (dark window, blue title bar, ">_" prompt) in the bottom-left corner.
# Rerun after changing v2d.ico:  .\make-cli-icon.ps1
# -PreviewDir writes 192 px enlargements of each size for checking the pixels.
param(
    [string]$Source = "$PSScriptRoot\..\Vhd2disk\v2d.ico",
    [string]$Dest = "$PSScriptRoot\v2dcli.ico",
    [string]$PreviewDir
)

$Source = (Resolve-Path $Source).Path

Add-Type -AssemblyName System.Drawing

$dark   = [System.Drawing.Color]::FromArgb(255, 24, 24, 24)
$border = [System.Drawing.Color]::FromArgb(255, 235, 235, 235)
$title  = [System.Drawing.Color]::FromArgb(255, 70, 140, 215)
$prompt = [System.Drawing.Color]::FromArgb(255, 255, 255, 255)

# Per size: badge rectangle, title bar height, chevron (x, y, half-height), underscore (x1, x2, y), stroke
$specs = @{
    48 = @{ X = 0; Y = 25; W = 25; H = 22; Bar = 3; Chev = @(5, 36, 4); Under = @(14, 20, 41); Stroke = 2 }
    32 = @{ X = 0; Y = 16; W = 17; H = 15; Bar = 2; Chev = @(3, 24, 3); Under = @(9, 13, 27); Stroke = 1 }
    16 = @{ X = 0; Y = 7;  W = 10; H = 9;  Bar = 1; Chev = @(2, 11, 2); Under = @(6, 8, 13);  Stroke = 1 }
}

$images = @()
foreach ($size in 48, 32, 16) {
    $s = $specs[$size]
    $icon = New-Object System.Drawing.Icon($Source, $size, $size)
    $bmp = New-Object System.Drawing.Bitmap $size, $size, ([System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.Clear([System.Drawing.Color]::Transparent)
    $g.DrawImage($icon.ToBitmap(), 0, 0, $size, $size)
    $g.SmoothingMode = 'None'
    $g.PixelOffsetMode = 'Half'

    # Window: light outline, dark body, blue title bar
    $g.FillRectangle((New-Object System.Drawing.SolidBrush $border), $s.X, $s.Y, $s.W, $s.H)
    $g.FillRectangle((New-Object System.Drawing.SolidBrush $dark), $s.X + 1, $s.Y + 1, $s.W - 2, $s.H - 2)
    $g.FillRectangle((New-Object System.Drawing.SolidBrush $title), $s.X + 1, $s.Y + 1, $s.W - 2, $s.Bar)

    # ">" as pixel runs so it stays crisp at every size, then "_"
    $brush = New-Object System.Drawing.SolidBrush $prompt
    $cx = $s.Chev[0]; $cy = $s.Chev[1]; $half = $s.Chev[2]
    for ($i = -$half; $i -le $half; $i++) {
        $g.FillRectangle($brush, $cx + ($half - [Math]::Abs($i)), $cy + $i, $s.Stroke, 1)
    }
    $g.FillRectangle($brush, $s.Under[0], $s.Under[2], $s.Under[1] - $s.Under[0] + 1, $s.Stroke)
    $g.Dispose()

    $ms = New-Object System.IO.MemoryStream
    $bmp.Save($ms, [System.Drawing.Imaging.ImageFormat]::Png)
    $images += , @{ Size = $size; Png = $ms.ToArray() }

    if ($PreviewDir) {
        $big = New-Object System.Drawing.Bitmap 192, 192
        $bg = [System.Drawing.Graphics]::FromImage($big)
        $bg.InterpolationMode = 'NearestNeighbor'; $bg.PixelOffsetMode = 'Half'
        $bg.Clear([System.Drawing.Color]::FromArgb(255, 200, 200, 200))
        $bg.DrawImage($bmp, 0, 0, 192, 192); $bg.Dispose()
        $big.Save("$PreviewDir\cli_$size.png")
    }
}

# ICO container with PNG-compressed 32-bit images (Vista and later)
$fs = [System.IO.File]::Create($Dest)
$w = New-Object System.IO.BinaryWriter $fs
$w.Write([UInt16]0); $w.Write([UInt16]1); $w.Write([UInt16]$images.Count)
$offset = 6 + 16 * $images.Count
foreach ($img in $images) {
    $w.Write([byte]$img.Size); $w.Write([byte]$img.Size); $w.Write([byte]0); $w.Write([byte]0)
    $w.Write([UInt16]1); $w.Write([UInt16]32)
    $w.Write([UInt32]$img.Png.Length); $w.Write([UInt32]$offset)
    $offset += $img.Png.Length
}
foreach ($img in $images) { $w.Write($img.Png) }
$w.Close()
