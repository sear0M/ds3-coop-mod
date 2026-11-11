# Draws the launcher icon (bonfire with a coiled sword, DS3 colors) and writes launcher\icon.ico
# with 256/64/48/32/16 px images (PNG-compressed ICO).
#   powershell -ExecutionPolicy Bypass -File tools\make_icon.ps1
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
$root = Split-Path -Parent $PSScriptRoot

function Draw([int]$size) {
    $bmp = New-Object System.Drawing.Bitmap $size, $size, ([System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.SmoothingMode = 'AntiAlias'
    $g.ScaleTransform($size / 256.0, $size / 256.0)

    # Dark disc with a warm glow and a thin gold rim.
    $disc = New-Object System.Drawing.Drawing2D.GraphicsPath
    $disc.AddEllipse(8, 8, 240, 240)
    $glow = New-Object System.Drawing.Drawing2D.PathGradientBrush $disc
    $glow.CenterPoint = New-Object System.Drawing.PointF 128, 190
    $glow.CenterColor = [System.Drawing.Color]::FromArgb(255, 92, 44, 16)
    $glow.SurroundColors = @([System.Drawing.Color]::FromArgb(255, 16, 13, 10))
    $g.FillPath($glow, $disc)
    $rim = New-Object System.Drawing.Pen ([System.Drawing.Color]::FromArgb(255, 176, 138, 72)), 6
    $g.DrawEllipse($rim, 11, 11, 234, 234)

    # Flame.
    $flame = New-Object System.Drawing.Drawing2D.GraphicsPath
    $flame.AddBezier(128, 214, 70, 210, 66, 150, 100, 112)
    $flame.AddBezier(100, 112, 104, 140, 116, 146, 118, 132)
    $flame.AddBezier(118, 132, 110, 100, 130, 70, 150, 54)
    $flame.AddBezier(150, 54, 146, 90, 190, 120, 182, 168)
    $flame.AddBezier(182, 168, 178, 200, 156, 214, 128, 214)
    $flame.CloseFigure()
    $fb = New-Object System.Drawing.Drawing2D.LinearGradientBrush (New-Object System.Drawing.PointF 128, 54), (New-Object System.Drawing.PointF 128, 214), ([System.Drawing.Color]::FromArgb(255, 255, 214, 130)), ([System.Drawing.Color]::FromArgb(255, 214, 92, 28))
    $g.FillPath($fb, $flame)
    $inner = New-Object System.Drawing.Drawing2D.GraphicsPath
    $inner.AddBezier(128, 206, 100, 204, 98, 170, 118, 150)
    $inner.AddBezier(118, 150, 124, 168, 134, 160, 138, 140)
    $inner.AddBezier(138, 140, 160, 160, 162, 200, 128, 206)
    $inner.CloseFigure()
    $g.FillPath((New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(230, 255, 240, 200))), $inner)

    # Coiled sword: blade, guard and grip.
    $blade = New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(255, 44, 36, 30))
    $edge = New-Object System.Drawing.Pen ([System.Drawing.Color]::FromArgb(255, 210, 176, 110)), 3
    $g.FillPolygon($blade, @((New-Object System.Drawing.PointF 122, 214), (New-Object System.Drawing.PointF 134, 214), (New-Object System.Drawing.PointF 132, 78), (New-Object System.Drawing.PointF 128, 66), (New-Object System.Drawing.PointF 124, 78)))
    $g.DrawPolygon($edge, @((New-Object System.Drawing.PointF 122, 214), (New-Object System.Drawing.PointF 134, 214), (New-Object System.Drawing.PointF 132, 78), (New-Object System.Drawing.PointF 128, 66), (New-Object System.Drawing.PointF 124, 78)))
    $g.FillRectangle((New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(255, 210, 176, 110))), 104, 92, 48, 8)
    $coil = New-Object System.Drawing.Pen ([System.Drawing.Color]::FromArgb(255, 120, 60, 24)), 4
    foreach ($y in 120, 146, 172) { $g.DrawArc($coil, 116, $y, 24, 12, 200, 220) }

    $g.Dispose()
    return $bmp
}

$sizes = 256, 64, 48, 32, 16
$images = foreach ($s in $sizes) {
    $bmp = Draw $s
    $ms = New-Object System.IO.MemoryStream
    $bmp.Save($ms, [System.Drawing.Imaging.ImageFormat]::Png)
    $bmp.Dispose()
    , $ms.ToArray()
}

$out = New-Object System.IO.MemoryStream
$w = New-Object System.IO.BinaryWriter $out
$w.Write([uint16]0); $w.Write([uint16]1); $w.Write([uint16]$sizes.Count)
$offset = 6 + 16 * $sizes.Count
for ($i = 0; $i -lt $sizes.Count; $i++) {
    $s = $sizes[$i]; $dim = if ($s -ge 256) { 0 } else { $s }
    $w.Write([byte]$dim); $w.Write([byte]$dim); $w.Write([byte]0); $w.Write([byte]0)
    $w.Write([uint16]1); $w.Write([uint16]32)
    $w.Write([uint32]$images[$i].Length); $w.Write([uint32]$offset)
    $offset += $images[$i].Length
}
foreach ($img in $images) { $w.Write($img) }
$w.Flush()
[IO.File]::WriteAllBytes((Join-Path $root 'launcher\icon.ico'), $out.ToArray())
$preview = Draw 256
$preview.Save((Join-Path $root 'build\icon_preview.png'), [System.Drawing.Imaging.ImageFormat]::Png)
$preview.Dispose()
Write-Host 'Wrote launcher\icon.ico'
