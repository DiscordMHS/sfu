#include "utils.hpp"

#include <fstream>
#include <iostream>
#include <sstream>

std::string ReadPemFile(const std::string& filePath) {
    std::ifstream keyFile(filePath);
    if (!keyFile.is_open()) {
        std::cerr << "Error: Could not open file " << filePath << std::endl;
        return {};
    }
    std::stringstream buffer;
    buffer << keyFile.rdbuf();
    return buffer.str();
}