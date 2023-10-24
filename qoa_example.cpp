// SPDX-FileCopyrightText: 2023 Robin Lindén <dev@robinlinden.eu>
//
// SPDX-License-Identifier: BSD-2-Clause

#include "qoa.h"

#include <fstream>
#include <iostream>

int main(int argc, char **argv) {
    if (argc != 2) {
        // TODO(robinlinden)
        std::cerr << "Usage ...\n";
        return 1;
    }

    std::ifstream fs{argv[1], std::ifstream::in | std::ifstream::binary};
    if (!fs) {
        std::cerr << "Oh no ...\n";
        return 1;
    }

    auto qoa = qoa::Qoa::parse(fs);
    std::ignore = qoa;
}
