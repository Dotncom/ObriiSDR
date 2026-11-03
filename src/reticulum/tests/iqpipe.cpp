// Persistent offline test pipe: hex fragment -> LoRa IQ -> streaming RX -> hex.
// No sockets, radio device or RNS instance. Production hosts use LoRaBearer.
#include "reticulumlora.h"
#include <algorithm>
#include <iostream>
#include <stdexcept>
#include <string>

int nibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    throw std::invalid_argument("Invalid hex");
}

int main(int argc, char **argv) {
    try {
        obrii::reticulum::RadioConfig config;
        config.phy.spreadingFactor = 7; // Fast offline check; no real-time timing model.
        if (argc == 3 && std::string(argv[1]) == "--framing" && std::string(argv[2]) == "rnode")
            config = obrii::reticulum::RadioConfig::rnode(config.phy);
        else if (argc != 1 && !(argc == 3 && std::string(argv[1]) == "--framing" && std::string(argv[2]) == "obrn"))
            throw std::invalid_argument("Usage: ObriiReticulumIqPipe [--framing obrn|rnode]");
        obrii::reticulum::LoRaBearer bearer(config);
        std::string line;
        while (std::getline(std::cin, line)) {
            if (line.empty() || line.size() > 510 || line.size() % 2)
                throw std::invalid_argument("Expected 1..255 hex bytes");
            obrii::lora::Bytes input;
            for (std::size_t i = 0; i < line.size(); i += 2)
                input.push_back(static_cast<std::uint8_t>(nibble(line[i]) * 16 + nibble(line[i + 1])));
            bearer.reset();
            auto iq = bearer.modulate(input);
            std::vector<obrii::reticulum::RadioFrame> received;
            for (std::size_t i = 0; i < iq.size(); i += 317) {
                auto part = bearer.receive(iq.data() + i, std::min<std::size_t>(317, iq.size() - i));
                received.insert(received.end(), part.begin(), part.end());
            }
            if (received.size() != 1 || received[0].data != input)
                throw std::runtime_error("LoRa IQ roundtrip mismatch");
            constexpr char hex[] = "0123456789abcdef";
            for (auto b : received[0].data) std::cout << hex[b >> 4] << hex[b & 15];
            std::cout << std::endl;
        }
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
