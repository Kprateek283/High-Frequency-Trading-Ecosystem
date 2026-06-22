#pragma once

#include <vector>
#include <algorithm>
#include <iostream>
#include <numeric>
#include <atomic>
#include <x86intrin.h>
#include <chrono>
#include <thread>

inline uint64_t get_tsc() {
    unsigned int dummy;
    return __rdtscp(&dummy);
}

class Timer {
public:
    Timer(size_t reserve_size) : count(0) {
        latencies.resize(reserve_size);
        calibrate();
    }

    void calibrate() {
        auto start_time = std::chrono::high_resolution_clock::now();
        uint64_t start_tsc = get_tsc();
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        uint64_t end_tsc = get_tsc();
        auto end_time = std::chrono::high_resolution_clock::now();
        
        auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(end_time - start_time).count();
        cycles_per_ns = static_cast<double>(end_tsc - start_tsc) / ns;
    }

    inline void add_latency(uint64_t duration) {
        size_t idx = count.fetch_add(1, std::memory_order_relaxed);
        if (idx < latencies.size()) [[likely]] {
            latencies[idx] = duration;
        }
    }

    void clear() {
        count.store(0, std::memory_order_relaxed);
    }

    void printStats(const std::string& label) const {
        size_t c = count.load(std::memory_order_relaxed);
        if (c == 0) return;
        if (c > latencies.size()) c = latencies.size();

        std::vector<uint64_t> sorted_latencies(latencies.begin(), latencies.begin() + c);
        std::sort(sorted_latencies.begin(), sorted_latencies.end());
        
        double avg = std::accumulate(sorted_latencies.begin(), sorted_latencies.end(), 0.0) / sorted_latencies.size();
        
        auto to_ns = [&](double cycles) { return cycles / cycles_per_ns; };

        std::cerr << "\n--- " << label << " ---" << "\n";
        std::cerr << "Orders in Window: " << sorted_latencies.size() << "\n";
        std::cerr << "Avg Latency:      " << to_ns(avg) << " ns (" << avg << " cycles)\n";
        std::cerr << "P50 (Median):     " << to_ns(sorted_latencies[sorted_latencies.size() / 2]) << " ns (" << sorted_latencies[sorted_latencies.size() / 2] << " cycles)\n";
        std::cerr << "P99:              " << to_ns(sorted_latencies[(sorted_latencies.size() * 99) / 100]) << " ns (" << sorted_latencies[(sorted_latencies.size() * 99) / 100] << " cycles)\n";
        std::cerr << "P99.9:            " << to_ns(sorted_latencies[(sorted_latencies.size() * 999) / 1000]) << " ns (" << sorted_latencies[(sorted_latencies.size() * 999) / 1000] << " cycles)\n";
        std::cerr << "P99.99:           " << to_ns(sorted_latencies[(sorted_latencies.size() * 9999) / 10000]) << " ns (" << sorted_latencies[(sorted_latencies.size() * 9999) / 10000] << " cycles)\n";
        std::cerr << "Max:              " << to_ns(sorted_latencies.back()) << " ns (" << sorted_latencies.back() << " cycles)" << std::endl;
    }

private:
    std::vector<uint64_t> latencies;
    std::atomic<size_t> count;
    double cycles_per_ns = 1.0;
};
