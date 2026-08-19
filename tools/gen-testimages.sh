#!/bin/bash
# Regenerates testimages/ — inputs for exercising src/image.c display paths.
set -e
cd "$(dirname "$0")/.."
out=testimages
rm -rf "$out"
mkdir -p "$out"

# label <size> <gradient> <text> <pointsize> <outfile>
label() { magick -size "$1" "gradient:$2" -depth 8 -gravity center -pointsize "$4" \
    -fill white -stroke black -strokewidth 1 -annotate 0 "$3" "$out/$5"; }

# --- PNG fast path: no conversion, dimensions read straight from IHDR ---
label 800x600   red-blue     "800x600"       40 basic.png
label 1600x400  green-purple "wide 1600x400"  40 wide.png
label 400x1600  orange-navy  "tall 400x1600"  40 tall.png
label 2000x40   teal-black   "strip 2000x40"  20 strip-2000x40.png
label 40x2000   black-teal   ""               10 strip-40x2000.png
label 3000x2000 white-black  "3000x2000"      90 large-3000x2000.png
label 16x16     red-yellow   ""               6  tiny-16.png
magick -size 1x1 xc:magenta "$out/pixel-1x1.png"

# alignment reference: exact cell grid should stay square and unskewed
magick -size 8x8 pattern:checkerboard -scale 6400% -bordercolor red -border 4 \
    "$out/grid-square.png"

# --- PNG encoding variants ---
magick -size 400x400 gradient:none-blue -depth 8 -alpha set "$out/alpha.png"      # RGBA
magick -size 400x400 gradient:black-white -depth 8 -colorspace gray "$out/gray.png"
magick -size 400x400 plasma:fractal -colors 8 -type palette "$out/palette8.png"
magick -size 400x400 gradient:red-blue -depth 16 "$out/depth16.png"
magick -size 400x400 plasma:fractal -depth 8 -interlace PNG "$out/interlaced.png"

# --- conversion path: sips is forked for anything that is not a PNG ---
magick -size 800x600 plasma:fractal -quality 80 "$out/photo.jpg"
magick -size 512x512 gradient:cyan-red "$out/bmp.bmp"
magick -size 512x512 gradient:cyan-red -depth 8 "$out/tiff.tiff"
magick -size 512x512 gradient:cyan-red "$out/webp.webp"
magick -size 256x256 gradient:cyan-red "$out/icon.ico"
magick -delay 20 -size 200x200 xc:red xc:green xc:blue -loop 0 "$out/anim.gif"
magick -size 400x300 gradient:red-blue "$out/doc.pdf"                     # sips handles
cat > "$out/vector.svg" <<'SVG'
<svg xmlns="http://www.w3.org/2000/svg" width="400" height="300">
  <rect width="400" height="300" fill="#204080"/>
  <circle cx="200" cy="150" r="100" fill="#ffcc00"/>
</svg>
SVG

# --- failure paths ---
printf '\x89PNG\r\n\x1a\n' > "$out/broken.png"                # header only, no IHDR
: > "$out/empty.png"                                          # zero bytes
echo "this is not an image at all" > "$out/nonimage.png"      # wrong magic, png name
magick -size 400x400 gradient:red-blue -depth 8 "png:$out/noext"           # png bytes, no extension
head -c 4000 "$out/photo.jpg" > "$out/truncated.jpg"          # partial decode
magick -size 2600x2600 xc: +noise Random -depth 8 "$out/oversize-20mb.png"   # > IMAGE_MAX_BYTES
magick -size 400x400 gradient:lime-black -depth 8 "$out/has space.png" # quoting in paths

ls -laS "$out"
