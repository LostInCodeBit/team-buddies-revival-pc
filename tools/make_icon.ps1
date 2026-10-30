# Draws the app icon (a stacking crate like the ones in the game) into runtime\app.ico.
# Multi-size ICO with PNG entries: 16, 24, 32, 48, 64, 256.
$ErrorActionPreference = "Stop"
Add-Type -AssemblyName System.Drawing
$root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$out = Join-Path $root "runtime\app.ico"

function Round-Rect([Drawing.Drawing2D.GraphicsPath]$p, [float]$x, [float]$y, [float]$w, [float]$h, [float]$r) {
    $d = 2 * $r
    $p.AddArc($x, $y, $d, $d, 180, 90); $p.AddArc($x + $w - $d, $y, $d, $d, 270, 90)
    $p.AddArc($x + $w - $d, $y + $h - $d, $d, $d, 0, 90); $p.AddArc($x, $y + $h - $d, $d, $d, 90, 90)
    $p.CloseFigure()
}

function Draw-Crate([int]$s) {
    $bmp = New-Object Drawing.Bitmap $s, $s
    $g = [Drawing.Graphics]::FromImage($bmp)
    $g.SmoothingMode = "AntiAlias"
    $k = $s / 256.0
    # red frame, darker lower-right bevel, yellow panel with a diagonal brace
    $frame = New-Object Drawing.Drawing2D.GraphicsPath; Round-Rect $frame (16*$k) (16*$k) (224*$k) (224*$k) (36*$k)
    $g.FillPath((New-Object Drawing.SolidBrush ([Drawing.Color]::FromArgb(255, 150, 22, 30))), $frame)
    $top = New-Object Drawing.Drawing2D.GraphicsPath; Round-Rect $top (16*$k) (16*$k) (216*$k) (212*$k) (34*$k)
    $g.FillPath((New-Object Drawing.SolidBrush ([Drawing.Color]::FromArgb(255, 214, 40, 44))), $top)
    $panel = New-Object Drawing.Drawing2D.GraphicsPath; Round-Rect $panel (60*$k) (58*$k) (128*$k) (124*$k) (14*$k)
    $g.FillPath((New-Object Drawing.SolidBrush ([Drawing.Color]::FromArgb(255, 245, 196, 52))), $panel)
    $g.SetClip($panel)
    $pen = New-Object Drawing.Pen ([Drawing.Color]::FromArgb(255, 214, 150, 20)), (22*$k)
    $g.DrawLine($pen, 60*$k, 182*$k, 188*$k, 58*$k)
    $g.ResetClip()
    $g.Dispose()
    $ms = New-Object IO.MemoryStream
    $bmp.Save($ms, [Drawing.Imaging.ImageFormat]::Png)
    $bmp.Dispose()
    return ,$ms.ToArray()
}

$sizes = 16, 24, 32, 48, 64, 256
$pngs = @($sizes | ForEach-Object { ,(Draw-Crate $_) })
$fs = [IO.File]::Create($out)
$bw = New-Object IO.BinaryWriter $fs
$bw.Write([uint16]0); $bw.Write([uint16]1); $bw.Write([uint16]$sizes.Count)
$offset = 6 + 16 * $sizes.Count
for ($i = 0; $i -lt $sizes.Count; $i++) {
    $d = if ($sizes[$i] -ge 256) { 0 } else { $sizes[$i] }
    $bw.Write([byte]$d); $bw.Write([byte]$d); $bw.Write([byte]0); $bw.Write([byte]0)
    $bw.Write([uint16]1); $bw.Write([uint16]32); $bw.Write([uint32]$pngs[$i].Length); $bw.Write([uint32]$offset)
    $offset += $pngs[$i].Length
}
foreach ($p in $pngs) { $bw.Write([byte[]]$p) }
$bw.Close()
"wrote $out"
