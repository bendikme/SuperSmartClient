/* Copyright 2026 SuperSmartClient contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef SUPERSMART_DASHBOARD_IMAGE_H
#define SUPERSMART_DASHBOARD_IMAGE_H

#include <cstdint>
#include <vector>

namespace dashboard {

// Resample the original RGB framebuffer, never a previously scaled picture.
// Coefficients and working buffers are reused while the dimensions stay fixed.
class ImageScaler {
public:
  const std::vector<uint8_t>& scale(const std::vector<uint8_t>& source,
    int sourceWidth, int sourceHeight, int width, int height);
private:
  struct Filter { int first; std::vector<float> weights; };
  static std::vector<Filter> filters(int source, int target);
  int sourceWidth_ = 0, sourceHeight_ = 0, width_ = 0, height_ = 0;
  std::vector<Filter> horizontal_, vertical_;
  std::vector<float> intermediate_;
  std::vector<uint8_t> pixels_;
};

}
#endif
