// SPDX-FileCopyrightText: 2023 Robin Lindén <dev@robinlinden.eu>
//
// SPDX-License-Identifier: BSD-2-Clause

#include "qoa.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cassert>
#include <cstdint>
#include <iostream>
#include <istream>
#include <utility>
#include <vector>

namespace qoa {
namespace {

static_assert((std::endian::native == std::endian::big) || (std::endian::native == std::endian::little),
        "Mixed endian is unsupported right now");

struct FrameHeader {
    std::uint8_t channel_count{};
    std::uint32_t sample_rate{}; // u24 in the spec.
    std::uint16_t sample_count{};
    std::uint16_t size{};

    static std::optional<FrameHeader> parse(std::istream &is) {
        FrameHeader h{};
        if (!is.read(reinterpret_cast<char *>(&h.channel_count), sizeof(h.channel_count))) {
            return std::nullopt;
        }

        // The spec has this as a 24-bit unsigned integer.
        if (!is.read(reinterpret_cast<char *>(&h.sample_rate), 3)) {
            return std::nullopt;
        }

        if (!is.read(reinterpret_cast<char *>(&h.sample_count), sizeof(h.sample_count))) {
            return std::nullopt;
        }

        if (!is.read(reinterpret_cast<char *>(&h.size), sizeof(h.size))) {
            return std::nullopt;
        }

        if constexpr (std::endian::native != std::endian::big) {
            // TODO(robinlinden): u24, byteswap weirdness?
            h.sample_rate = std::byteswap(h.sample_rate);
            h.sample_count = std::byteswap(h.sample_count);
            h.size = std::byteswap(h.size);
        }

        return h;
    }
};

struct LmsState {
    std::array<std::int16_t, 4> history{};
    std::array<std::int16_t, 4> weights{};

    static std::optional<LmsState> parse(std::istream &is) {
        LmsState s{};
        if (!is.read(reinterpret_cast<char *>(s.history.data()), sizeof(s.history.size() * 2))) {
            return std::nullopt;
        }

        if (!is.read(reinterpret_cast<char *>(s.weights.data()), sizeof(s.weights.size() * 2))) {
            return std::nullopt;
        }

        if constexpr (std::endian::native != std::endian::big) {
            std::ranges::for_each(s.history, [](auto &v) { v = std::byteswap(v); });
            std::ranges::for_each(s.weights, [](auto &v) { v = std::byteswap(v); });
        }

        return s;
    }
};

// [2] Each quantized residual is an index into the kDequantTable.
constexpr std::array<float, 8> kDequantTable{
        .75f,
        -.75f,
        2.5f,
        -2.5f,
        4.5f,
        -4.5f,
        7.f,
        -7.f,
};

} // namespace

// https://qoaformat.org/
std::optional<Qoa> Qoa::parse(std::istream &is) {
    std::array<char, 4> magic{};
    if (!is.read(reinterpret_cast<char *>(magic.data()), magic.size()) || magic != std::array{'q', 'o', 'a', 'f'}) {
        return std::nullopt;
    }

    std::uint32_t sample_count{};
    if (!is.read(reinterpret_cast<char *>(&sample_count), sizeof(sample_count))) {
        return std::nullopt;
    }

    if constexpr (std::endian::native != std::endian::big) {
        sample_count = std::byteswap(sample_count);
    }

    std::uint32_t frame_count = std::lround(sample_count / 256.f / 20.f + 0.5f);

    std::cout << "File contains " << sample_count << " across " << frame_count << " frames\n";
    std::vector<std::uint16_t> output;
    std::optional<std::uint8_t> channel_count{};
    for (std::size_t frame_idx = 0; frame_idx < frame_count; ++frame_idx) {
        auto frame_hdr = FrameHeader::parse(is);
        if (!frame_hdr) {
            return std::nullopt;
        }

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

        assert(frame_hdr->sample_count % 20 == 0);
        for (std::uint16_t i = 0; i < frame_hdr->sample_count / 20; ++i) {
            for (std::uint8_t ch = 0; ch < *channel_count; ++ch) {
                std::uint64_t slice{};
                if (!is.read(reinterpret_cast<char *>(&slice), sizeof(slice))) {
                    return std::nullopt;
                }

                std::uint8_t scale_factor{};
                std::array<std::uint8_t, 20> residuals{};
                scale_factor = slice & 0b0000'1111;
                slice >>= 4;

                // [1] Dequantize scale factor.
                scale_factor = static_cast<std::uint8_t>(std::lround(std::pow(scale_factor + 1, 2.25f)));

                for (auto &residual : residuals) {
                    residual = slice & 0b0000'0111;
                    slice >>= 3;
                    // [3] Multiply w/ scale factor, round to nearest, tie away from 0.
                    auto r = static_cast<std::uint16_t>(std::lround(scale_factor * kDequantTable.at(residual)));
                    // [4] The predicted sample is the sum of history[n] * weight[n] >>= 13.
                    auto &lms = lms_state.at(ch);
                    int p = [&] {
                        return (lms.history[0] * lms.weights[0] + lms.history[1] * lms.weights[1]
                                       + lms.history[2] * lms.weights[2] + lms.history[3] * lms.weights[3])
                                >> 13;
                    }();

                    // [5] The final sample is p + r, clamped to the signed 16-bit range.
                    output.push_back(static_cast<std::uint16_t>(std::clamp(r + p, -32768, 32767)));

                    // [6] The LMS weights are updated using the quantized and
                    // scaled residual r, right-shifted by 4 bits.
                    auto delta = r >> 4;
                    for (std::size_t j = 0; j < 4; ++j) {
                        lms.weights[j] += static_cast<std::int16_t>(lms.history[j] < 0 ? -delta : delta);
                    }
                    for (std::size_t j = 0; j < 3; ++j) {
                        lms.history[j] = lms.history[j + 1];
                    }
                    lms.history[3] = output.back();
                }
            }
        }
    }

    std::cerr << "Samples read: " << output.size() << '\n';
    return Qoa{.audio_frames = std::move(output)};
}

} // namespace qoa
