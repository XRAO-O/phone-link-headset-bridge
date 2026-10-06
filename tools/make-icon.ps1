# Renders bridge/app.ico (headphones on a blue rounded square) at the standard Windows icon sizes.
# Only needed when changing the icon; the generated file is committed.
param([string]$Out = (Join-Path $PSScriptRoot "..\bridge\app.ico"))

Add-Type -AssemblyName System.Drawing

function New-RoundedRect([single]$x, [single]$y, [single]$w, [single]$h, [single]$r) {
    $d = 2 * $r
    $path = New-Object System.Drawing.Drawing2D.GraphicsPath
    $path.AddArc($x, $y, $d, $d, 180, 90)
    $path.AddArc($x + $w - $d, $y, $d, $d, 270, 90)
    $path.AddArc($x + $w - $d, $y + $h - $d, $d, $d, 0, 90)
    $path.AddArc($x, $y + $h - $d, $d, $d, 90, 90)
    $path.CloseFigure()
    return $path
}

function New-IconPng([int]$size) {
    $bmp = New-Object System.Drawing.Bitmap $size, $size, ([System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
    $g.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
    $g.Clear([System.Drawing.Color]::Transparent)

    $background = New-RoundedRect 0 0 $size $size ($size * 0.22)
    $rect = New-Object System.Drawing.RectangleF 0, 0, $size, $size
    $brush = New-Object System.Drawing.Drawing2D.LinearGradientBrush $rect, ([System.Drawing.Color]::FromArgb(59, 130, 246)), ([System.Drawing.Color]::FromArgb(29, 78, 216)), 90.0
    $g.FillPath($brush, $background)

    # same proportions as the tray icon drawn in gui.c, inside an inset box
    $o = $size * 0.17
    $k = $size * 0.66
    $white = [System.Drawing.Brushes]::White
    $pen = New-Object System.Drawing.Pen ([System.Drawing.Color]::White), ([single]($k * 0.12))
    $g.DrawArc($pen, [single]($o + 0.17 * $k), [single]($o + 0.10 * $k), [single](0.66 * $k), [single](0.70 * $k), 180, 180)
    $cupTop = $o + 0.44 * $k
    $cupW = 0.28 * $k
    $cupH = 0.50 * $k
    $g.FillPath($white, (New-RoundedRect ($o + 0.06 * $k) $cupTop $cupW $cupH ($k * 0.11)))
    $g.FillPath($white, (New-RoundedRect ($o + 0.66 * $k) $cupTop $cupW $cupH ($k * 0.11)))

    $g.Dispose()
    $stream = New-Object System.IO.MemoryStream
    $bmp.Save($stream, [System.Drawing.Imaging.ImageFormat]::Png)
    $bmp.Dispose()
    return , $stream.ToArray()
}

$sizes = 16, 20, 24, 32, 40, 48, 64, 256
$images = @($sizes | ForEach-Object { , (New-IconPng $_) })

$file = New-Object System.IO.MemoryStream
$writer = New-Object System.IO.BinaryWriter $file
$writer.Write([uint16]0)
$writer.Write([uint16]1)
$writer.Write([uint16]$sizes.Count)
$offset = 6 + 16 * $sizes.Count
for ($i = 0; $i -lt $sizes.Count; $i++) {
    $dim = if ($sizes[$i] -ge 256) { 0 } else { $sizes[$i] }
    $writer.Write([byte]$dim)
    $writer.Write([byte]$dim)
    $writer.Write([byte]0)
    $writer.Write([byte]0)
    $writer.Write([uint16]1)
    $writer.Write([uint16]32)
    $writer.Write([uint32]$images[$i].Length)
    $writer.Write([uint32]$offset)
    $offset += $images[$i].Length
}
foreach ($image in $images) { $writer.Write($image) }
$writer.Flush()
[System.IO.File]::WriteAllBytes((Resolve-Path -LiteralPath (Split-Path $Out) | Join-Path -ChildPath (Split-Path $Out -Leaf)), $file.ToArray())
Write-Host "Wrote $Out"
