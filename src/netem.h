#pragma once
#include "profile.h"
#include <string>
#include <vector>

namespace chaos {

// Pure functions (unit-testable without root):
std::vector<std::string> netemArgs(const Profile& p);          // tokens that follow "netem"
std::string netemCommandLine(const std::string& dev, const Profile& p);
bool validIfaceName(const std::string& name);                  // whitelist, max 15 chars

// Kernel backend. Commands are run with fork/execvp (no shell), so nothing can be injected.
struct NetemOptions { std::string dev; bool dry_run = false; bool force = false; };

bool netemShow(const NetemOptions& o);
bool netemReset(const NetemOptions& o);
// Applies a profile, then holds until ttl_s elapses or Ctrl+C/SIGTERM/SIGHUP and removes it
// again (dead-man switch). ttl_s <= 0 with persist=true leaves it applied.
int netemApplyHold(const NetemOptions& o, const Profile& p, double ttl_s, bool persist);
int netemRunScenario(const NetemOptions& o, const std::vector<Step>& steps);

}  // namespace chaos
