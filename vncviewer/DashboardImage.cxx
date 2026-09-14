/* Copyright 2026 SuperSmartClient contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "DashboardImage.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace dashboard {
namespace {
double lanczos(double x) {
  x = std::abs(x);
  if (x < 1e-9) return 1;
  if (x >= 3) return 0;
  constexpr double pi = 3.14159265358979323846;
  return std::sin(pi * x) * std::sin(pi * x / 3) / (pi * pi * x * x / 3);
}
}

std::vector<ImageScaler::Filter> ImageScaler::filters(int source, int target)
{
  std::vector<Filter> result(target);
  double ratio = source / static_cast<double>(target);
  // Expand the filter footprint when shrinking so thin strokes contribute
  // instead of disappearing between samples. Align both grids at pixel centres.
  double footprint = std::max(1.0, ratio);
  for (int position = 0; position < target; ++position) {
    auto& filter = result[position];
    if (source == target) { filter.first = position; filter.weights = {1}; continue; }
    double centre = (position + 0.5) * ratio - 0.5;
    filter.first = std::max(0, static_cast<int>(std::ceil(centre - 3 * footprint)));
    int last = std::min(source - 1, static_cast<int>(std::floor(centre + 3 * footprint)));
    double total = 0;
    for (int sample = filter.first; sample <= last; ++sample) {
      double weight = lanczos((sample - centre) / footprint);
      filter.weights.push_back(static_cast<float>(weight)); total += weight;
    }
    // Renormalise the clipped footprint at edges, preserving flat colours.
    for (auto& weight : filter.weights) weight = static_cast<float>(weight / total);
  }
  return result;
}

const std::vector<uint8_t>& ImageScaler::scale(const std::vector<uint8_t>& source,
  int sourceWidth, int sourceHeight, int width, int height)
{
  if (sourceWidth <= 0 || sourceHeight <= 0 || width <= 0 || height <= 0 ||
      source.size() != static_cast<size_t>(sourceWidth) * sourceHeight * 3)
    throw std::invalid_argument("Invalid RGB image dimensions");
  // Native resolution is pixel-exact and needs neither filtering nor a copy.
  if (sourceWidth == width && sourceHeight == height) return source;
  if (sourceWidth_ != sourceWidth || width_ != width) horizontal_ = filters(sourceWidth, width);
  if (sourceHeight_ != sourceHeight || height_ != height) vertical_ = filters(sourceHeight, height);
  sourceWidth_ = sourceWidth; sourceHeight_ = sourceHeight; width_ = width; height_ = height;
  intermediate_.resize(static_cast<size_t>(width) * sourceHeight * 3);
  pixels_.resize(static_cast<size_t>(width) * height * 3);

  for (int y = 0; y < sourceHeight; ++y) {
    const auto* row = source.data() + static_cast<size_t>(y) * sourceWidth * 3;
    auto* output = intermediate_.data() + static_cast<size_t>(y) * width * 3;
    for (const auto& filter : horizontal_) {
      const auto* pixel = row + filter.first * 3;
      float red = 0, green = 0, blue = 0;
      for (float weight : filter.weights) {
        red += pixel[0] * weight; green += pixel[1] * weight; blue += pixel[2] * weight; pixel += 3;
      }
      *output++ = red; *output++ = green; *output++ = blue;
    }
  }
  // Keep signed, floating-point intermediates; rounding/clipping between the
  // two passes would reduce contrast and introduce coloured edge artefacts.
  const size_t stride = static_cast<size_t>(width) * 3;
  for (int y = 0; y < height; ++y) {
    const auto& filter = vertical_[y];
    const auto* first = intermediate_.data() + static_cast<size_t>(filter.first) * stride;
    auto* output = pixels_.data() + static_cast<size_t>(y) * stride;
    for (int x = 0; x < width; ++x) {
      size_t sample = 0;
      float red = 0, green = 0, blue = 0;
      for (float weight : filter.weights) {
        const auto* pixel = first + sample++ * stride + x * 3;
        red += pixel[0] * weight; green += pixel[1] * weight; blue += pixel[2] * weight;
      }
      *output++ = static_cast<uint8_t>(std::clamp(red + 0.5f, 0.0f, 255.0f));
      *output++ = static_cast<uint8_t>(std::clamp(green + 0.5f, 0.0f, 255.0f));
      *output++ = static_cast<uint8_t>(std::clamp(blue + 0.5f, 0.0f, 255.0f));
    }
  }
  return pixels_;
}
}
