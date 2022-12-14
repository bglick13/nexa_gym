# Image Sources

The directory contains source images for a lot of the images and icons used in the client.

It also contains iconmaker.py, a small python program that generates icon images of various sizes and formats.

## iconmaker.py Required Packages
sudo apt-get install icnsutils imagemagick
pip install Pillow

## Running iconmaker.py

```bash
./iconmaker.py <1024px square png file>  <output filename prefix>

./iconmaker.py with no args runs "./iconmaker.py icon1024.png bitcoin"
```

## Handling the output

The following recipe puts the output files in the appropriate locations for this project:

```bash
cp *.xpm ../../share/pixmaps
cp nexa*.png ../../share/pixmaps/
cp nexa.ico ../../share/pixmaps/nexa.ico 
cp nexa.ico ../../src/qt/res/icons
cp nexa.icns ../../src/qt/res/icons
cp nexa512.png ../../src/qt/res/icons/nexa.png

rm nexa*.png
```
