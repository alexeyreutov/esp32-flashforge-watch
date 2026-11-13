#pragma once
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

// Each cell describes the mean brightness and the density of visible edges.
struct Cell { int light, edges; };
class Vision {
public:
  static constexpr int cols = 12, rows = 8, count = cols * rows;
  struct Score { int shifted, textured; bool ready; };
  void reset() { samples = 0; memset(age, 0, sizeof(age)); }

  Score update(const Cell (&now)[count]) {
    Score result{};
    if (!samples) memcpy(base, now, sizeof(base));
    int offsets[count];
    for (int i = 0; i < count; ++i) offsets[i] = now[i].light - base[i].light;
    // Subtract the median shift so a global exposure change has less effect.
    qsort(offsets, count, sizeof(int), [](const void *a, const void *b) {
      return *(const int*)a - *(const int*)b;
    });
    bool moved[count]{}, tangled[count]{};
    for (int i = 0; i < count; ++i) {
      int dl = abs(now[i].light - base[i].light - offsets[count/2]);
      int de = now[i].edges - base[i].edges;
      bool change = dl >= 24 || de >= 16;
      if (samples >= 4 && change) {
        if (age[i] < 3) ++age[i];
        moved[i] = age[i] >= 3 && dl >= 24;
        tangled[i] = age[i] >= 3 && de >= 16;
      } else {
        age[i] = 0;
        int weight = samples < 4 ? 1 : 7;
        base[i].light = (weight * base[i].light + now[i].light + (weight+1)/2) / (weight+1);
        base[i].edges = (weight * base[i].edges + now[i].edges + (weight+1)/2) / (weight+1);
      }
    }
    result.shifted = largestGroup(moved);
    result.textured = largestGroup(tangled);
    if (samples < 5) ++samples;
    result.ready = samples >= 5;
    return result;
  }
private:
  Cell base[count]{};
  uint8_t age[count]{}, samples = 0;
  static int largestGroup(bool (&mask)[count]) {
    int largest = 0;
    for (int i = 0; i < count; ++i) {
      if (!mask[i]) continue;
      int queue[count], end = 1;
      queue[0] = i; mask[i] = false;
      for (int j = 0; j < end; ++j) {
        int p = queue[j];
        int neighbors[] = {p % cols ? p-1 : -1, p % cols < cols-1 ? p+1 : -1, p-cols, p+cols};
        for (int n : neighbors) if (n >= 0 && n < count && mask[n]) {
          mask[n] = false; queue[end++] = n;
        }
      }
      if (end > largest) largest = end;
    }
    return largest;
  }
};
