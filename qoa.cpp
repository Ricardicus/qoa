// SPDX-FileCopyrightText: 2023 Robin Lindén <dev@robinlinden.eu>
//
// SPDX-License-Identifier: BSD-2-Clause

#include "qoa.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <istream>
#include <utility>
#include <vector>

namespace util {
template <typename T> T net_pack(const T in) {
  T out{};
  auto *const bytes = reinterpret_cast<std::byte *>(&out);

  for (std::size_t i = 0; i < sizeof(T); ++i) {
    bytes[i] = static_cast<std::byte>(in >> ((sizeof(T) - i - 1) * CHAR_BIT));
  }

  return out;
}
} // namespace util

namespace qoa {
namespace {

static_assert((std::endian::native == std::endian::big) ||
                  (std::endian::native == std::endian::little),
              "Mixed endian is unsupported right now");

struct FrameHeader {
  std::uint8_t channel_count{};
  std::uint32_t sample_rate{}; // u24 in the spec.
  std::uint16_t sample_count{};
  std::uint16_t size{};

  static std::optional<FrameHeader> parse(std::istream &is) {
    FrameHeader h{};
    if (!is.read(reinterpret_cast<char *>(&h.channel_count),
                 sizeof(h.channel_count))) {
      return std::nullopt;
    }

    // The spec has this as a 24-bit unsigned integer.
    if (!is.read(reinterpret_cast<char *>(&h.sample_rate), 3)) {
      return std::nullopt;
    }

    if (!is.read(reinterpret_cast<char *>(&h.sample_count),
                 sizeof(h.sample_count))) {
      return std::nullopt;
    }

    if (!is.read(reinterpret_cast<char *>(&h.size), sizeof(h.size))) {
      return std::nullopt;
    }

    if constexpr (std::endian::native != std::endian::big) {
      // TODO(robinlinden): u24, byteswap weirdness?
      h.sample_rate = util::net_pack(h.sample_rate);
      h.sample_count = util::net_pack(h.sample_count);
      h.size = util::net_pack(h.size);
    }

    return h;
  }
};

struct LmsState {
  std::array<std::int16_t, 4> history{};
  std::array<std::int16_t, 4> weights{};

  static std::optional<LmsState> parse(std::istream &is) {
    LmsState s{};
    if (!is.read(reinterpret_cast<char *>(s.history.data()),
                 sizeof(s.history.size() * 2))) {
      return std::nullopt;
    }

    if (!is.read(reinterpret_cast<char *>(s.weights.data()),
                 sizeof(s.weights.size() * 2))) {
      return std::nullopt;
    }

    if constexpr (std::endian::native != std::endian::big) {
      for (int i = 0; i < 4; ++i) {
        s.history[i] = util::net_pack(s.history[i]);
        s.weights[i] = util::net_pack(s.weights[i]);
      }
    }

    return s;
  }
};

// [2] Each quantized residual is an index into the kDequantTable.
constexpr std::array<float, 8> kDequantTable{
    .75f, -.75f, 2.5f, -2.5f, 4.5f, -4.5f, 7.f, -7.f,
};

} // namespace

// https://qoaformat.org/
std::optional<Qoa> Qoa::parse(std::istream &is) {
  std::array<char, 4> magic{};
  FrameHeader last_frame;
  if (!is.read(reinterpret_cast<char *>(magic.data()), magic.size()) ||
      magic != std::array{'q', 'o', 'a', 'f'}) {
    return std::nullopt;
  }

  std::uint32_t sample_count{};
  if (!is.read(reinterpret_cast<char *>(&sample_count), sizeof(sample_count))) {
    return std::nullopt;
  }

  if constexpr (std::endian::native != std::endian::big) {
    sample_count = util::net_pack(sample_count);
  }

  std::uint32_t frame_count = std::round(sample_count / 256.f / 20.f + 0.5f);

  std::cout << "File contains " << sample_count << " across " << frame_count
            << " frames\n";
  std::array<std::vector<std::int16_t>, 2> output;
  std::optional<std::uint8_t> channel_count{};
  for (std::size_t frame_idx = 0; frame_idx < frame_count; ++frame_idx) {
    auto frame_hdr = FrameHeader::parse(is);
    if (!frame_hdr) {
      return std::nullopt;
    }
    last_frame = frame_hdr.value();
    if (!channel_count) {
      channel_count = frame_hdr->channel_count;
    } else if (channel_count != frame_hdr->channel_count) {
      return std::nullopt;
    }

    std::vector<LmsState> lms_state{};
    for (std::uint8_t ch = 0; ch < *channel_count; ++ch) {
      auto lms = LmsState::parse(is);
      if (!lms) {
        return std::nullopt;
      }
      lms_state.push_back(*std::move(lms));
    }

    // assert(frame_hdr->sample_count % 20 == 0);
    for (std::uint16_t i = 0; i < frame_hdr->sample_count / 20; ++i) {
      for (std::uint8_t ch = 0; ch < *channel_count; ++ch) {
        std::uint64_t slice{};
        if (!is.read(reinterpret_cast<char *>(&slice), sizeof(slice))) {
          return std::nullopt;
        }

        slice = util::net_pack(slice);

        std::uint8_t sf_quant{};
        std::array<std::int16_t, 20> residuals{};
        // scale_factor = slice & 0b0000'1111;
        // slice >>= 4;
        int offset = 4;
        sf_quant = static_cast<uint8_t>(slice >> (64 - offset));

        // [1] Dequantize scale factor.
        int16_t scale_factor = static_cast<std::int16_t>(
            std::round(std::pow(sf_quant + 1, 2.75f)));

        for (auto &residual : residuals) {
          // residual = slice & 0b0000'0111;
          // slice >>= 3;
          offset += 3;
          residual = (slice >> (64 - offset)) & 0b111;
          // [3] Multiply with scale factor, round to nearest, tie away from 0.
          double r_d =
              static_cast<double>(scale_factor * kDequantTable.at(residual));
          int r = r_d < 0 ? static_cast<int>(std::ceil(r_d - 0.5))
                          : static_cast<int>(std::floor(r_d + 0.5));

          // [4] The predicted sample is the sum of history[n] * weight[n]
          // >>= 13.
          auto &lms = lms_state.at(ch);
          int16_t p = [&] {
            return (lms.history[0] * lms.weights[0] +
                    lms.history[1] * lms.weights[1] +
                    lms.history[2] * lms.weights[2] +
                    lms.history[3] * lms.weights[3]) >>
                   13;
          }();

          // [5] The final sample is p + r, clamped to the signed 16-bit range.
          output[ch].push_back(
              static_cast<std::int16_t>(std::clamp(r + p, -32768, 32767)));

          // [6] The LMS weights are updated using the quantized and
          // scaled residual r, right-shifted by 4 bits.
          int16_t delta = r >> 4;
          for (std::size_t j = 0; j < 4; ++j) {
            lms.weights[j] +=
                static_cast<std::int16_t>(lms.history[j] < 0 ? -delta : delta);
          }
          for (std::size_t j = 0; j < 3; ++j) {
            lms.history[j] = lms.history[j + 1];
          }
          lms.history[3] = output[ch].back();
        }
      }
    }
  }
  std::vector<int16_t> output_interleaved;
  for (uint32_t i = 0; i < output[0].size(); i++) {
    output_interleaved.push_back(output[0][i]);
    if (*channel_count == 2) {
      output_interleaved.push_back(output[1][i]);
    }
  }

  std::cerr << "Samples read: " << output_interleaved.size() << '\n';
  return Qoa{.audio_frames = std::move(output_interleaved),
             .sample_rate = last_frame.sample_rate,
             .nbr_channels = last_frame.channel_count};
}

} // namespace qoa
