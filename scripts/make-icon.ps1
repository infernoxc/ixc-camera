<#
.SYNOPSIS
    Generates IXC Camera's application icon (src/app/ixc.ico) from code: an original design, no
    external artwork. Each size is drawn natively (not downscaled) so it stays crisp at 16 px.
.DESCRIPTION
    Design: a rounded square with IXC's violet-to-rose gradient, a white camera lens ring with a
    dark centre and a small highlight. Sizes 16, 20, 24, 32, 40, 48, 64 and 256 (PNG entries).
    Re-run after changing the design; the .ico is committed.
#>
param([string]$Out = (Join-Path (Split-Path -Parent $PSScriptRoot) 'src\app\ixc.ico'))
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing

function Draw([int]$s) {
    $bmp = New-Object Drawing.Bitmap $s, $s, ([Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [Drawing.Graphics]::FromImage($bmp)
    $g.SmoothingMode = 'AntiAlias'; $g.PixelOffsetMode = 'HighQuality'; $g.Clear([Drawing.Color]::Transparent)
    $m = [math]::Max(0.5, $s * 0.02)          # outer margin
    $r = $s * 0.23                              # corner radius
    $rect = New-Object Drawing.RectangleF $m, $m, ($s - 2 * $m), ($s - 2 * $m)
    $path = New-Object Drawing.Drawing2D.GraphicsPath
    $d = 2 * $r
    $path.AddArc($rect.X, $rect.Y, $d, $d, 180, 90)
    $path.AddArc($rect.Right - $d, $rect.Y, $d, $d, 270, 90)
    $path.AddArc($rect.Right - $d, $rect.Bottom - $d, $d, $d, 0, 90)
    $path.AddArc($rect.X, $rect.Bottom - $d, $d, $d, 90, 90)
    $path.CloseFigure()
    $grad = New-Object Drawing.Drawing2D.LinearGradientBrush $rect, ([Drawing.Color]::FromArgb(255, 112, 92, 238)), ([Drawing.Color]::FromArgb(255, 255, 106, 150)), 45.0
    $g.FillPath($grad, $path)

    # Lens: white ring, dark centre, highlight.
    $cx = $s * 0.5; $cy = $s * 0.52
    $ro = $s * 0.285; $ring = [math]::Max(1.6, $s * 0.085)
    $white = New-Object Drawing.SolidBrush ([Drawing.Color]::FromArgb(255, 255, 255, 255))
    $g.FillEllipse($white, $cx - $ro, $cy - $ro, 2 * $ro, 2 * $ro)
    $ri = $ro - $ring
    $dark = New-Object Drawing.SolidBrush ([Drawing.Color]::FromArgb(255, 27, 24, 44))
    $g.FillEllipse($dark, $cx - $ri, $cy - $ri, 2 * $ri, 2 * $ri)
    if ($s -ge 24) {
        # Inner iris tint and glint.
        $ir = $ri * 0.55
        $iris = New-Object Drawing.SolidBrush ([Drawing.Color]::FromArgb(255, 124, 108, 242))
        $g.FillEllipse($iris, $cx - $ir, $cy - $ir, 2 * $ir, 2 * $ir)
        $gr = [math]::Max(1.2, $s * 0.045)
        $g.FillEllipse($white, $cx - $ri * 0.42 - $gr, $cy - $ri * 0.42 - $gr, 2 * $gr, 2 * $gr)
        # Viewfinder dot (top right).
        $vr = [math]::Max(1.2, $s * 0.045)
        $g.FillEllipse($white, $s * 0.77 - $vr, $s * 0.23 - $vr, 2 * $vr, 2 * $vr)
    } else {
        $gr = [math]::Max(0.8, $s * 0.06)
        $g.FillEllipse($white, $cx - $ri * 0.35 - $gr, $cy - $ri * 0.35 - $gr, 2 * $gr, 2 * $gr)
    }
    $g.Dispose()
    $ms = New-Object IO.MemoryStream
    if ($s -ge 256) {
        $bmp.Save($ms, [Drawing.Imaging.ImageFormat]::Png)   # 256: PNG (standard)
    } else {
        # Smaller sizes: classic 32-bit DIB (BGRA, bottom-up) + AND mask; readable by every loader.
        $data = $bmp.LockBits((New-Object Drawing.Rectangle 0, 0, $s, $s), 'ReadOnly', [Drawing.Imaging.PixelFormat]::Format32bppArgb)
        $px = New-Object byte[] ($s * $s * 4)
        for ($y = 0; $y -lt $s; $y++) { [Runtime.InteropServices.Marshal]::Copy([IntPtr]($data.Scan0.ToInt64() + $y * $data.Stride), $px, $y * $s * 4, $s * 4) }
        $bmp.UnlockBits($data)
        $maskRow = [int]([math]::Ceiling($s / 32.0) * 4)
        $bw = New-Object IO.BinaryWriter $ms
        $bw.Write([uint32]40); $bw.Write([int32]$s); $bw.Write([int32](2 * $s)); $bw.Write([uint16]1); $bw.Write([uint16]32)
        $bw.Write([uint32]0); $bw.Write([uint32]($s * $s * 4 + $maskRow * $s)); $bw.Write([int32]0); $bw.Write([int32]0); $bw.Write([uint32]0); $bw.Write([uint32]0)
        for ($y = $s - 1; $y -ge 0; $y--) { $bw.Write($px, $y * $s * 4, $s * 4) }
        for ($y = $s - 1; $y -ge 0; $y--) {
            $row = New-Object byte[] $maskRow
            for ($x = 0; $x -lt $s; $x++) { if ($px[($y * $s + $x) * 4 + 3] -eq 0) { $row[[math]::Floor($x / 8)] = $row[[math]::Floor($x / 8)] -bor (0x80 -shr ($x % 8)) } }
            $bw.Write($row)
        }
        $bw.Flush()
    }
    $bmp.Dispose()
    , $ms.ToArray()
}

$sizes = 16, 20, 24, 32, 40, 48, 64, 256
$images = foreach ($s in $sizes) { , (Draw $s) }
$ms = New-Object IO.MemoryStream
$bw = New-Object IO.BinaryWriter $ms
$bw.Write([uint16]0); $bw.Write([uint16]1); $bw.Write([uint16]$sizes.Count)   # ICONDIR
$offset = 6 + 16 * $sizes.Count
for ($i = 0; $i -lt $sizes.Count; $i++) {                                        # ICONDIRENTRY
    $s = $sizes[$i]; $len = $images[$i].Length
    $bw.Write([byte]($(if ($s -ge 256) { 0 } else { $s }))); $bw.Write([byte]($(if ($s -ge 256) { 0 } else { $s })))
    $bw.Write([byte]0); $bw.Write([byte]0); $bw.Write([uint16]1); $bw.Write([uint16]32)
    $bw.Write([uint32]$len); $bw.Write([uint32]$offset); $offset += $len
}
foreach ($img in $images) { $bw.Write($img) }
$bw.Flush()
[IO.File]::WriteAllBytes($Out, $ms.ToArray())
"wrote $Out ($($ms.Length) bytes, sizes $($sizes -join ', '))"
