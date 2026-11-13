#include "flashforge_watch/vision.h"
#include <assert.h>
#include <stdio.h>

int main() {
  Vision detector;
  Cell cells[Vision::count];
  for (auto &cell : cells) cell = {80, 5};
  for (int frame = 0; frame < 6; frame++) {
    auto s = detector.update(cells);
    assert(!s.shifted && !s.textured);
  }
  for (auto &cell : cells) cell.light += 32;
  assert(!detector.update(cells).shifted);
  for (int i = 2; i < 8; i++) cells[i].light += 50;
  assert(!detector.update(cells).shifted);
  assert(!detector.update(cells).shifted);
  assert(detector.update(cells).shifted == 6);
  detector.reset();
  for (int i = 0; i < 5; i++) detector.update(cells);
  for (int i = 24; i < 29; i++) cells[i].edges += 25;
  assert(!detector.update(cells).textured);
  assert(!detector.update(cells).textured);
  assert(detector.update(cells).textured == 5);
  for (int i = 24; i < 29; i++) cells[i].edges -= 25;
  assert(!detector.update(cells).textured);
  puts("PASS: stable scene, exposure change, persistent shift, edge growth, reset and recovery");
}