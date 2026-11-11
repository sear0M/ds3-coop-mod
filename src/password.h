#pragma once

#include <string>

namespace password {

// True when SHA-256(input) matches the stored hash. The password itself is not in the source.
bool Check(const std::string& input);

}  // namespace password
