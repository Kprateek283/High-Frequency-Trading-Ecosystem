#include <iostream>
#include <vector>
#include <fstream>
#include <cstring>
#include "protocol/messages.h"

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <output_file.bin>\n";
        return 1;
    }

    std::ofstream out(argv[1], std::ios::binary);
    if (!out) {
        std::cerr << "Failed to open " << argv[1] << " for writing.\n";
        return 1;
    }

    const int TOTAL_ORDERS = 5000000; // 5 Million orders
    // Four instruments in the canonical STK##### encoding. These used to be
    // "AAPL    ", "MSFT    ", "GOOG    ", "AMZN    ", which the gateway decoded
    // to no instrument at all — every order in the generated file was rejected
    // (review A1), which is what made this file useless as a replay input.
    const uint16_t stocks[4] = {0, 1, 2, 3};

    std::cout << "Generating " << TOTAL_ORDERS << " binary OUCH orders...\n";

    for (int i = 0; i < TOTAL_ORDERS; ++i) {
        OuchEnterOrder req;
        std::memset(&req, 0, sizeof(req));
        req.msg_type = 'O';
        std::string token = std::to_string(i + 1);
        token.insert(token.begin(), 14 - token.length(), '0');
        std::memcpy(req.order_token, token.c_str(), 14);
        
        // Randomize side and stock properly so they match
        req.side = (rand() % 2 == 0) ? 'B' : 'S';
        req.shares = 100;
        encode_symbol(req.stock, stocks[rand() % 4]);
        
        // Tight spread around 50000
        req.price = 50000 + (rand() % 10);
        req.time_in_force = 99998;
        std::memcpy(req.firm, "HFT1", 4);
        req.display = 'Y';
        req.capacity = 'P';
        req.iso_eligibility = 'N';
        req.min_quantity = 0;
        req.cross_type = 'N';
        req.customer_type = 'R';

        out.write(reinterpret_cast<const char*>(&req), sizeof(req));
    }

    out.close();
    std::cout << "Successfully generated " << TOTAL_ORDERS << " orders into " << argv[1] << "\n";
    return 0;
}
