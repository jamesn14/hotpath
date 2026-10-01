#ifndef HOTPATH_BENCH_LATENCY_RECORDER_H
#define HOTPATH_BENCH_LATENCY_RECORDER_H
#include <cstdint>
#include <vector>

template <typename Buf>
struct latency_recorder {
    std::vector<uint64_t>* out;
    uint64_t warmup;
    uint64_t seen = 0;

    void operator()(Buf, uint64_t latency) {
        if (seen++ >= warmup) {
            out->push_back(latency);
        }
    }
};

#endif //HOTPATH_BENCH_LATENCY_RECORDER_H
