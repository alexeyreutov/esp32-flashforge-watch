#include "flashforge_watch/frame_analysis.h"
#include <fstream>
#include <vector>
#include <assert.h>
#include <stdio.h>

// Replay the three Creator 5 Pro frames as half-resolution binary PPM images.
static void replay(char **paths) {
  const int roi[] = {38,21,66,29};
  Vision detector;
  Cell clearSamples[2][Vision::count]{};
  for (int i=0;i<3;++i) {
    std::ifstream in(paths[i],std::ios::binary);
    std::string format;
    int w,h,maxValue;
    in >> format >> w >> h >> maxValue;
    assert(in && format=="P6" && w==320 && h==240 && maxValue==255);
    in.get();
    std::vector<uint8_t> rgb(w*h*3);
    assert(in.read(reinterpret_cast<char*>(rgb.data()),rgb.size()));
    Cell cells[Vision::count];
    auto view=readRegion(rgb.data(),w,h,roi,cells);
    printf("Frame %d: %s\n",i+1,view==View::covered?"covered":"clear");
    assert(view==(i==0 ? View::covered : View::clear));
    if (view==View::clear) {
      memcpy(clearSamples[i-1],cells,sizeof cells);
      auto score=detector.update(cells);
      assert(!score.shifted && !score.textured && !score.ready);
    }
  }
  // Separate stress test: alternating the two real clear views after warm-up.
  for (int i=0;i<16;++i) {
    auto score=detector.update(clearSamples[i%2]);
    assert(score.shifted<5 && score.textured<4);
  }
  puts("PASS: alternating clear views after warm-up");
}

int main(int argc,char **argv) {
  assert(argc==1 || argc==4);
  if (argc==4) replay(argv+1);
  Vision detector;
  Cell cells[Vision::count];
  for (auto &cell : cells) cell = {80, 5};
  for (int frame = 0; frame < 6; frame++) {
    auto s = detector.update(cells);
    assert(!s.shifted && !s.textured);
  }
  for (auto &cell : cells) cell.light += 32;
  assert(!detector.update(cells).shifted);
  for (int i=0;i<8;++i) {
    for (int j=2;j<8;++j) cells[j].light = i%2 ? 50 : 170;
    assert(!detector.update(cells).shifted);
  }
  for (int j=2;j<8;++j) cells[j].light=112;
  detector.update(cells);
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
