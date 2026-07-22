#include "base64_utils.h"

#include <array>
#include <cstddef>
#include <iostream>

std::string encodeBase64(
    const std::vector<uint8_t>& data
)
{
    if (data.empty()) {
        std::cerr
            << "[Base64 Utils] "
            << "encode rejected"
            << " reason=empty_input"
            << "\n";

        return {};
    }

    static constexpr std::array<char, 64>
        alphabet{
            'A', 'B', 'C', 'D',
            'E', 'F', 'G', 'H',
            'I', 'J', 'K', 'L',
            'M', 'N', 'O', 'P',
            'Q', 'R', 'S', 'T',
            'U', 'V', 'W', 'X',
            'Y', 'Z',
            'a', 'b', 'c', 'd',
            'e', 'f', 'g', 'h',
            'i', 'j', 'k', 'l',
            'm', 'n', 'o', 'p',
            'q', 'r', 's', 't',
            'u', 'v', 'w', 'x',
            'y', 'z',
            '0', '1', '2', '3',
            '4', '5', '6', '7',
            '8', '9',
            '+', '/'
        };

    const size_t outputSize =
        4 *
        (
            (data.size() + 2) /
            3
        );

    std::string output;
    output.reserve(outputSize);

    size_t index = 0;

    while (index + 3 <= data.size()) {
        const uint32_t value =
            (
                static_cast<uint32_t>(
                    data[index]
                ) << 16
            ) |
            (
                static_cast<uint32_t>(
                    data[index + 1]
                ) << 8
            ) |
            static_cast<uint32_t>(
                data[index + 2]
            );

        output.push_back(
            alphabet[
                (value >> 18) & 0x3F
            ]
        );

        output.push_back(
            alphabet[
                (value >> 12) & 0x3F
            ]
        );

        output.push_back(
            alphabet[
                (value >> 6) & 0x3F
            ]
        );

        output.push_back(
            alphabet[
                value & 0x3F
            ]
        );

        index += 3;
    }

    const size_t remaining =
        data.size() - index;

    if (remaining == 1) {
        const uint32_t value =
            static_cast<uint32_t>(
                data[index]
            ) << 16;

        output.push_back(
            alphabet[
                (value >> 18) & 0x3F
            ]
        );

        output.push_back(
            alphabet[
                (value >> 12) & 0x3F
            ]
        );

        output.push_back('=');
        output.push_back('=');
    } else if (remaining == 2) {
        const uint32_t value =
            (
                static_cast<uint32_t>(
                    data[index]
                ) << 16
            ) |
            (
                static_cast<uint32_t>(
                    data[index + 1]
                ) << 8
            );

        output.push_back(
            alphabet[
                (value >> 18) & 0x3F
            ]
        );

        output.push_back(
            alphabet[
                (value >> 12) & 0x3F
            ]
        );

        output.push_back(
            alphabet[
                (value >> 6) & 0x3F
            ]
        );

        output.push_back('=');
    }

    if (output.size() != outputSize) {
        std::cerr
            << "[Base64 Utils] "
            << "encode failed"
            << " reason=unexpected_output_size"
            << " expected="
            << outputSize
            << " actual="
            << output.size()
            << "\n";

        return {};
    }

    std::cerr
        << "[Base64 Utils] "
        << "encoded inputBytes="
        << data.size()
        << " outputChars="
        << output.size()
        << "\n";

    return output;
}
