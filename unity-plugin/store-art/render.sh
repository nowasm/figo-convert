#!/bin/bash
# Renders the Asset Store art from src/*.html via headless Chrome.
# Pages are captured at 2x device scale and downsampled to the exact
# store dimensions for crisp text.  Usage: ./render.sh
set -e
cd "$(dirname "$0")"
CHROME="/Applications/Google Chrome.app/Contents/MacOS/Google Chrome"

render() { # file.html width height out.png
  "$CHROME" --headless=new --disable-gpu --hide-scrollbars \
    --force-device-scale-factor=2 --window-size="$2,$3" \
    --screenshot="png/$4" "file://$PWD/src/$1" 2>/dev/null
  sips -z "$3" "$2" "png/$4" >/dev/null   # sips takes height width
}

render icon-160.html        160  160  icon-160.png
render card-420.html        420  280  card-420x280.png
render cover-1950.html      1950 1300 cover-1950x1300.png
render social-1200.html     1200 630  social-1200x630.png
render shot1-workflow.html  2400 1600 screenshot-1-workflow.png
render shot2-window.html    2400 1600 screenshot-2-editor.png
render shot3-sprites.html   2400 1600 screenshot-3-sprites.png
render shot4-features.html  2400 1600 screenshot-4-features.png

for f in png/*.png; do sips -g pixelWidth -g pixelHeight "$f" | tr '\n' ' '; echo; done
