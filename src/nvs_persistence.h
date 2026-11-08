// sendspin-cpp persistence provider backed by an NVS namespace.
//
// The library owns serialization; this only stores and returns its fixed-size blobs byte for
// byte under the library's own keys (all at most 12 characters, within NVS's 15-character
// limit). It holds the device's X25519 identity, the pairing records and the output delay.
#pragma once

#include <optional>
#include <string>
#include <vector>

#include "sendspin/client.h"

class NvsPersistenceProvider : public sendspin::SendspinPersistenceProvider {
public:
    std::optional<std::vector<uint8_t>> load_blob(const std::string& key) override;
    bool save_blob(const std::string& key, const uint8_t* data, size_t len) override;
    bool commit() override;
};
