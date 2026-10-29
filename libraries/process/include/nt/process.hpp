#pragma once
#include <string>
#include <vector>
namespace nt {
// Execute an argument vector directly, without a command shell.
int run_process(const std::vector<std::string>& arguments);
}
