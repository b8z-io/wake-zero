#pragma once
int font_width(const char *s, int scale);
void font_draw(unsigned char *rgba, int w, int h, int x, int y, const char *s, int scale,
               unsigned char r, unsigned char g, unsigned char b);
