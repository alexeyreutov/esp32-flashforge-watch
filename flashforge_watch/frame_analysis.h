#pragma once
#include "vision.h"

inline int luminance(const uint8_t *p) {
  return (77*p[0]+150*p[1]+29*p[2]) >> 8;
}

enum class View { clear, covered, tooSmall };

inline View readRegion(const uint8_t *rgb, int width, int height, const int roi[4],
                       Cell (&cells)[Vision::count]) {
  int x0=width*roi[0]/100, y0=height*roi[1]/100;
  int w=width*roi[2]/100-x0, h=height*roi[3]/100-y0;
  if (w<Vision::cols || h<Vision::rows) return View::tooSmall;

  // The pale face of this toolhead enters from above. Require a wide,
  // vertically continuous bright patch extending into the top of the ROI.
  // A thin white line of filament on the bed does not meet that condition.
  int run=0;
  int band=height/16+1;
  int bottom=y0+h/10;
  int top=bottom>band ? bottom-band : 0;
  for (int x=x0; x<x0+w; ++x) {
    int bright=0;
    for (int y=top; y<=bottom; ++y)
      bright += luminance(rgb+3*(y*width+x)) >= 176;
    if (bright*10 >= (bottom-top+1)*9 &&
        luminance(rgb+3*(bottom*width+x)) >= 176) {
      if (++run >= width/40+1) return View::covered;
    } else run=0;
  }
  for (int r=0;r<Vision::rows;++r) for (int c=0;c<Vision::cols;++c) {
    int sum=0,edges=0,pixels=0;
    for (int y=y0+h*r/Vision::rows;y<y0+h*(r+1)/Vision::rows;++y)
      for (int x=x0+w*c/Vision::cols;x<x0+w*(c+1)/Vision::cols;++x) {
        const uint8_t *p=rgb+3*(y*width+x);
        int value=luminance(p);
        sum+=value;
        edges+=(x>x0 && abs(value-luminance(p-3))>24) ||
               (y>y0 && abs(value-luminance(p-3*width))>24);
        ++pixels;
      }
    cells[r*Vision::cols+c]={sum/pixels,100*edges/pixels};
  }
  return View::clear;
}
