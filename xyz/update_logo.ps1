# Logo update script for xyz fork
# Usage: powershell.exe -ExecutionPolicy Bypass -File xyz/update_logo.ps1
#
# Takes logo-xyz-base.png from resources/images/ and generates all the
# icon/logo files OrcaSlicer uses, replacing the stock ones.
# Requires ImageMagick (magick) in PATH.

$ErrorActionPreference = "Stop"
$WP = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$imgDir = "$WP\resources\images"
$base = "$imgDir\logo-xyz-base.png"
$square = "$imgDir\logo-xyz-square.png"

if (-not (Test-Path $base)) {
    Write-Host "ERROR: $base not found"
    exit 1
}

Write-Host "=== Generating square logo with rounded corners and drop shadow ==="
# Make square with transparent background, centered
$dims = & magick identify -format "%wx%h" $base
$w, $h = $dims -split 'x'
$size = [Math]::Max([int]$w, [int]$h)
$sizeM1 = $size - 1
$radius = [Math]::Round($size * 0.10)

# Four-step: white square -> rounded corners -> drop shadow -> add transparent margin
$sizeStr = "${size}x${size}"
$tmpWhiteSquare = "$env:TEMP\logo-xyz-tmp-white.png"
$tmpRounded = "$env:TEMP\logo-xyz-tmp-rounded.png"
$tmpShadowed = "$env:TEMP\logo-xyz-tmp-shadow.png"

# Step 1: Center logo on WHITE square background (iOS-style filled icon)
& magick $base -gravity center -background white -extent $sizeStr -alpha off $tmpWhiteSquare

# Step 2: Apply rounded rectangle mask via SrcIn
& magick -size $sizeStr xc:none `
    -fill white -draw "roundrectangle 0,0 ${sizeM1},${sizeM1} ${radius},${radius}" `
    $tmpWhiteSquare -compose SrcIn -composite `
    -depth 8 -define png:color-type=6 `
    $tmpRounded

# Step 3: Add drop shadow
& magick $tmpRounded `
    -compose Over `
    "(" +clone -background black -shadow "40x8+0+0" ")" `
    +swap -background none -mosaic +repage `
    -depth 8 -define png:color-type=6 `
    $tmpShadowed

# Final square logo (no extra margin - used for splash, about, home page, PNGs)
& magick $tmpShadowed -gravity center -background none -extent $sizeStr `
    -depth 8 -define png:color-type=6 `
    $square

Remove-Item $tmpWhiteSquare, $tmpRounded, $tmpShadowed -ErrorAction SilentlyContinue
Write-Host "Created: $square"

Write-Host "=== Generating PNG icons at all sizes ==="
$pngSizes = @{
    "OrcaSlicer.png" = 154
    "OrcaSlicer_128px.png" = 128
    "OrcaSlicer_154.png" = 154
    "OrcaSlicer_192px.png" = 192
    "OrcaSlicer_192px_transparent.png" = 192
    "OrcaSlicer_32px.png" = 32
    "OrcaSlicer_64.png" = 64
    "OrcaSlicer-mac_128px.png" = 128
}

foreach ($entry in $pngSizes.GetEnumerator()) {
    $outFile = "$imgDir\$($entry.Key)"
    $sz = $entry.Value
    & magick $square -resize "${sz}x${sz}" -depth 8 -define png:color-type=6 $outFile
    Write-Host "  $($entry.Key) (${sz}x${sz})"
}

# Grayscale variant
& magick $square -resize "192x192" -colorspace Gray -depth 8 -define png:color-type=6 "$imgDir\OrcaSlicer_192px_grayscale.png"
Write-Host "  OrcaSlicer_192px_grayscale.png (192x192, grayscale)"

Write-Host "=== Generating ICO files (with extra 15% margin for app icon) ==="
# Create a padded version with 15% margin for ICO files only
$icoMargin = [Math]::Round($size * 0.15)
$icoSize = $size + ($icoMargin * 2)
$icoSizeStr = "${icoSize}x${icoSize}"
$tmpIcoPadded = "$env:TEMP\logo-xyz-tmp-icopad.png"
& magick $square -gravity center -background none -extent $icoSizeStr `
    -depth 8 -define png:color-type=6 $tmpIcoPadded

# Main .ico with multiple sizes
& magick $tmpIcoPadded `
    "(" -clone 0 -resize 16x16 ")" `
    "(" -clone 0 -resize 32x32 ")" `
    "(" -clone 0 -resize 48x48 ")" `
    "(" -clone 0 -resize 64x64 ")" `
    "(" -clone 0 -resize 128x128 ")" `
    "(" -clone 0 -resize 256x256 ")" `
    -delete 0 "$imgDir\OrcaSlicer.ico"
Write-Host "  OrcaSlicer.ico (16-256px, with margin)"

# Mac 256px ico
& magick $tmpIcoPadded -resize 256x256 "$imgDir\OrcaSlicer-mac_256px.ico"
Write-Host "  OrcaSlicer-mac_256px.ico (256x256, with margin)"

Remove-Item $tmpIcoPadded -ErrorAction SilentlyContinue

# Title ico/png (these are wider banners in stock, just use our square for now)
& magick $square -resize 154x154 "$imgDir\OrcaSlicerTitle.png"
& magick $square -resize 32x32 "$imgDir\OrcaSlicerTitle.ico"
Write-Host "  OrcaSlicerTitle.png/ico"

# Web homepage logo
$webLogoDir = "$WP\resources\web\image"
if (Test-Path $webLogoDir) {
    & magick $square -resize 154x154 "$webLogoDir\logo.png"
    Write-Host "  resources/web/image/logo.png (154x154)"
}

Write-Host "=== Logo update complete ==="
Write-Host "Note: The splash screen and about dialog now use logo-xyz-square.png directly."
Write-Host "SVG files are left as-is (fallback only)."
Write-Host "Delete build/src/CMakeFiles/OrcaSlicer_app_gui.dir/OrcaSlicer.rc.res to force exe icon rebuild."
