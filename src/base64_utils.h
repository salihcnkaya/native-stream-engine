#pragma once

#include <cstdint>
#include <string>
#include <vector>

std::string encodeBase64(
    const std::vector<uint8_t>& data
);
