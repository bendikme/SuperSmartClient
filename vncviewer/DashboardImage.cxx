/* Copyright 2026 SuperSmartClient contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "DashboardImage.h"

#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <stdexcept>
#include <system_error>
#include <thread>

namespace dashboard {
namespace {
double lanczos(double x) {
  x = std::abs(x);
  if (x < 1e-9) return 1;
  if (x >= 3) return 0;
  constexpr double pi = 3.14159265358979323846;
  return std::sin(pi * x) * std::sin(pi * x / 3) / (pi * pi * x * x / 3);
}

// Every picture update is rescaled on the interface thread, so the two passes
// are written for speed: several independent sums are computed side by side,
// and large pictures are divided between a few threads. Each output sample is
// still summed in tap order, so the result does not depend on any of this.

// One RGB pixel plus a spare value, as four floats the compiler handles together.
#if defined(__GNUC__) || defined(__clang__)
typedef float Pixel __attribute__((vector_size(16)));
#else
struct Pixel {
  float channel[4];
  Pixel& operator+=(const Pixel& other) { for (int i = 0; i < 4; ++i) channel[i] += other.channel[i]; return *this; }
  Pixel operator*(float weight) const { Pixel result; for (int i = 0; i < 4; ++i) result.channel[i] = channel[i] * weight; return result; }
};
#endif
inline Pixel load(const float* from) { Pixel value; std::memcpy(&value, from, sizeof(value)); return value; }

// Source rows filtered side by side in the horizontal pass.
constexpr int together = 8;
constexpr unsigned maxParts = 4;

// Helper threads shared by every scaler. They are started on first use and
// live until the process exits; the calling thread always takes part too.
class Workers {
public:
  // Calls job(part) once for every part below parts and returns when all are done.
  template <typename Job> void run(unsigned parts, Job job) {
    if (parts <= 1) { job(0u); return; }
    struct Call { static void part(void* job, unsigned number) { (*static_cast<Job*>(job))(number); } };
    std::unique_lock<std::mutex> lock(mutex_);
    busy_.wait(lock, [this] { return !function_; });
    while (threads_ + 1 < parts) {
      try { std::thread([this] { work(); }).detach(); ++threads_; }
      catch (const std::system_error&) { break; }  // The caller does those parts itself.
    }
    function_ = Call::part; job_ = &job; parts_ = parts; next_ = 0; unfinished_ = parts;
    wake_.notify_all();
    take(lock);
    done_.wait(lock, [this] { return !unfinished_; });
    function_ = nullptr;
    busy_.notify_one();
  }
private:
  void take(std::unique_lock<std::mutex>& lock) {
    while (function_ && next_ < parts_) {
      unsigned part = next_++;
      lock.unlock();
      function_(job_, part);
      lock.lock();
      if (!--unfinished_) done_.notify_all();
    }
  }
  void work() {
    std::unique_lock<std::mutex> lock(mutex_);
    for (;;) {
      wake_.wait(lock, [this] { return function_ && next_ < parts_; });
      take(lock);
    }
  }
  std::mutex mutex_;
  std::condition_variable wake_, done_, busy_;
  void (*function_)(void*, unsigned) = nullptr;
  void* job_ = nullptr;
  unsigned parts_ = 0, next_ = 0, unfinished_ = 0, threads_ = 0;
};

// Small pictures are finished sooner than other threads can be woken. The
// work of a pass follows the larger of the picture it reads and the one it writes.
unsigned partsFor(int rows, size_t samples) {
  if (samples < 400000) return 1;
  unsigned parts = std::min(maxParts, std::max(1u, std::thread::hardware_concurrency()));
  return std::min(parts, static_cast<unsigned>(std::max(1, rows / together)));
}

Workers& workers() {
  // Never destroyed: its threads may still be waiting when the process exits.
  static Workers* shared = new Workers;
  return *shared;
}
}

ImageScaler::Filters ImageScaler::filters(int source, int target)
{
  Filters result;
  result.first.resize(target); result.offset.resize(target + 1);
  double ratio = source / static_cast<double>(target);
  // Expand the filter footprint when shrinking so thin strokes contribute
  // instead of disappearing between samples. Align both grids at pixel centres.
  double footprint = std::max(1.0, ratio);
  for (int position = 0; position < target; ++position) {
    result.offset[position] = static_cast<int>(result.weights.size());
    if (source == target) { result.first[position] = position; result.weights.push_back(1); continue; }
    double centre = (position + 0.5) * ratio - 0.5;
    int first = std::max(0, static_cast<int>(std::ceil(centre - 3 * footprint)));
    int last = std::min(source - 1, static_cast<int>(std::floor(centre + 3 * footprint)));
    result.first[position] = first;
    double total = 0;
    for (int sample = first; sample <= last; ++sample) {
      double weight = lanczos((sample - centre) / footprint);
      result.weights.push_back(static_cast<float>(weight)); total += weight;
    }
    // Renormalise the clipped footprint at edges, preserving flat colours.
    for (size_t index = result.offset[position]; index < result.weights.size(); ++index)
      result.weights[index] = static_cast<float>(result.weights[index] / total);
  }
  result.offset[target] = static_cast<int>(result.weights.size());
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
  const size_t stride = static_cast<size_t>(width) * 3;
  // One spare sample per source row lets its last pixel be read as four values.
  const size_t rowSamples = static_cast<size_t>(sourceWidth) * 3 + 1;
  intermediate_.resize(stride * sourceHeight);
  pixels_.resize(stride * height);
  // Working memory is taken before any thread starts, so the parts cannot fail.
  scratch_.resize(maxParts * std::max(rowSamples * together, stride));

  const uint8_t* input = source.data();
  float* intermediate = intermediate_.data();
  float* scratch = scratch_.data();
  const Filters& across = horizontal_;
  unsigned parts = partsFor(sourceHeight, std::max(rowSamples, stride) * sourceHeight);
  workers().run(parts, [=, &across](unsigned part) {
    float* rows = scratch + part * rowSamples * together;
    const int end = static_cast<int>(static_cast<long long>(sourceHeight) * (part + 1) / parts);
    for (int y = static_cast<int>(static_cast<long long>(sourceHeight) * part / parts); y < end; y += together) {
      const int count = std::min(together, end - y);
      for (int line = 0; line < count; ++line) {
        const uint8_t* bytes = input + static_cast<size_t>(y + line) * sourceWidth * 3;
        float* row = rows + line * rowSamples;
        for (int index = 0; index < sourceWidth * 3; ++index) row[index] = bytes[index];
      }
      float* output = intermediate + static_cast<size_t>(y) * stride;
      for (int x = 0; x < width; ++x) {
        const float* pixel = rows + static_cast<size_t>(across.first[x]) * 3;
        const float* weight = across.weights.data() + across.offset[x];
        const float* last = across.weights.data() + across.offset[x + 1];
        // Rows beyond count hold older samples; their sums are not stored.
        Pixel sum[together] = {};
        for (; weight != last; ++weight, pixel += 3)
          for (int line = 0; line < together; ++line) sum[line] += load(pixel + line * rowSamples) * *weight;
        for (int line = 0; line < count; ++line)
          std::memcpy(output + line * stride + static_cast<size_t>(x) * 3, &sum[line], 3 * sizeof(float));
      }
    }
  });
  // Keep signed, floating-point intermediates; rounding/clipping between the
  // two passes would reduce contrast and introduce coloured edge artefacts.
  uint8_t* result = pixels_.data();
  const Filters& down = vertical_;
  parts = partsFor(height, stride * std::max(sourceHeight, height));
  workers().run(parts, [=, &down](unsigned part) {
    float* total = scratch + part * stride;
    const int end = static_cast<int>(static_cast<long long>(height) * (part + 1) / parts);
    for (int y = static_cast<int>(static_cast<long long>(height) * part / parts); y < end; ++y) {
      std::fill(total, total + stride, 0.0f);
      const float* samples = intermediate + static_cast<size_t>(down.first[y]) * stride;
      const float* weight = down.weights.data() + down.offset[y];
      const float* last = down.weights.data() + down.offset[y + 1];
      // Four source rows per sweep, added in the same order as one at a time.
      for (; last - weight >= 4; weight += 4, samples += 4 * stride) {
        const float w0 = weight[0], w1 = weight[1], w2 = weight[2], w3 = weight[3];
        const float *s0 = samples, *s1 = s0 + stride, *s2 = s1 + stride, *s3 = s2 + stride;
        for (size_t index = 0; index < stride; ++index)
          total[index] = (((total[index] + s0[index] * w0) + s1[index] * w1) + s2[index] * w2) + s3[index] * w3;
      }
      for (; weight != last; ++weight, samples += stride) {
        const float w0 = *weight;
        for (size_t index = 0; index < stride; ++index) total[index] += samples[index] * w0;
      }
      uint8_t* output = result + static_cast<size_t>(y) * stride;
      for (size_t index = 0; index < stride; ++index)
        output[index] = static_cast<uint8_t>(std::min(std::max(total[index] + 0.5f, 0.0f), 255.0f));
    }
  });
  return pixels_;
}
}
