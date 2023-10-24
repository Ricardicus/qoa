// SPDX-FileCopyrightText: 2023 Robin Lindén <dev@robinlinden.eu>
//
// SPDX-License-Identifier: BSD-2-Clause

#ifndef AUDIO_QOA_H_
#define AUDIO_QOA_H_

#include <cstdint>
#include <iosfwd>
#include <optional>
#include <vector>

namespace qoa {

class Qoa {
public:
    static std::optional<Qoa> parse(std::istream &);
    static std::optional<Qoa> parse(std::istream &&is) { return parse(is); }

    std::vector<std::uint16_t> audio_frames{};
};

} // namespace qoa

#endif
