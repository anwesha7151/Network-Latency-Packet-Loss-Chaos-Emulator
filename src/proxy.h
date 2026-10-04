#pragma once
#include "profile.h"
#include <cstdint>
#include <string>
#include <vector>

namespace chaos {

struct ProxyOptions {
    std::string bind = "127.0.0.1";
    uint16_t listen_port = 0;
    std::string target_host;
    uint16_t target_port = 0;
    Profile up, down;                 // client->target and target->client
    std::vector<Step> scenario;       // optional timeline (applies to both directions)
    uint64_t seed = 1;
    double duration_s = 0;            // 0 = until Ctrl+C
    double stats_every_s = 0;
    double idle_s = 60;               // close a client's upstream socket after this many idle seconds (0 = never)
};

// Userspace UDP impairment proxy. Needs no root and cannot lock you out of a machine.
int runProxy(const ProxyOptions& o);

}  // namespace chaos
