#pragma once

#include "paperos/Display.h"

namespace paperos {

// Helpers for drawing the small line icons shown beside each app on the home
// screen. Display only has 1-pixel lines, so these build thick strokes from
// filled squares; `thickness` is the stroke width in pixels.

// Draws a line `thickness` pixels wide from (x0, y0) to (x1, y1).
void strokeLine(Display& display, int x0, int y0, int x1, int y1, int thickness, Color color);

// Draws the outline of a rectangle with sides `thickness` pixels wide, inside
// the rectangle.
void strokeRect(Display& display, int x, int y, int w, int h, int thickness, Color color);

// Draws part of a circle centred on (cx, cy) with radius `r`, from
// `startDegrees` to `endDegrees`. 0 degrees points right and angles turn
// clockwise on screen, so 90 points down.
void strokeArc(Display& display, int cx, int cy, int r, int startDegrees, int endDegrees,
               int thickness, Color color);

// Stroke width that suits an icon `size` pixels square.
int iconStroke(int size);

// Draws an icon into the square at (x, y) with sides `size` pixels long.
using IconFn = void (*)(Display& display, int x, int y, int size, Color color);

// The first letter of `name` in a box: the icon of apps that don't draw one.
void drawLetterIcon(Display& display, const char* name, int x, int y, int size, Color color);

// A gear, such as for a Settings menu.
void drawGearIcon(Display& display, int x, int y, int size, Color color);

// Four small squares: a group of apps. The default icon of an AppMenu.
void drawFolderIcon(Display& display, int x, int y, int size, Color color);

}  // namespace paperos
