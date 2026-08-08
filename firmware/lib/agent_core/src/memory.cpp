#include "chatesp/memory.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>

namespace chatesp {
namespace agent {
namespace {

constexpr std::array<std::uint8_t, 4> kRecordMagic{'C', 'E', 'M', '1'};
constexpr std::uint8_t kRecordVersion = 1;
constexpr std::size_t kHeaderBytes = 48;
constexpr std::size_t kChecksumBytes = 32;
constexpr std::array<std::uint8_t, 17> kContentDomain{
    'C', 'H', 'A', 'T', 'E', 'S', 'P', '-', 'M', 'E', 'M', 'O', 'R', 'Y', '-', 'V', '1'};
constexpr std::array<std::uint8_t, 24> kRecordDomain{
    'C', 'H', 'A', 'T', 'E', 'S', 'P', '-', 'M', 'E', 'M', 'O', 'R', 'Y', '-',
    'R', 'E', 'C', 'O', 'R', 'D', '-', 'V', '1'};

constexpr std::uint32_t kShaInitial[8] = {
    0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
    0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U};

constexpr std::uint32_t kShaRound[64] = {
    0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U,
    0x3956c25bU, 0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U,
    0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U,
    0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U,
    0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU,
    0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU,
    0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U,
    0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U,
    0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U,
    0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
    0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U,
    0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U,
    0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U,
    0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
    0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U,
    0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U};

constexpr std::uint32_t rotate_right(std::uint32_t value, unsigned count) {
    return (value >> count) | (value << (32U - count));
}

class Sha256 {
public:
    void update(const std::uint8_t *data, std::size_t size) {
        if (data == nullptr || size == 0) return;
        total_size_ += size;
        while (size > 0) {
            const std::size_t amount = std::min(size, block_.size() - block_size_);
            std::memcpy(block_.data() + block_size_, data, amount);
            block_size_ += amount;
            data += amount;
            size -= amount;
            if (block_size_ == block_.size()) {
                transform(block_.data());
                block_size_ = 0;
            }
        }
    }

    MemoryFingerprint finish() {
        const std::uint64_t bit_size = static_cast<std::uint64_t>(total_size_) * 8U;
        block_[block_size_++] = 0x80;
        if (block_size_ > 56) {
            std::fill(block_.begin() + block_size_, block_.end(), 0);
            transform(block_.data());
            block_size_ = 0;
        }
        std::fill(block_.begin() + block_size_, block_.begin() + 56, 0);
        for (unsigned index = 0; index < 8; ++index) {
            block_[63U - index] = static_cast<std::uint8_t>(bit_size >> (index * 8U));
        }
        transform(block_.data());
        MemoryFingerprint result{};
        for (std::size_t word = 0; word < state_.size(); ++word) {
            for (unsigned byte = 0; byte < 4; ++byte) {
                result[word * 4U + byte] = static_cast<std::uint8_t>(
                    state_[word] >> ((3U - byte) * 8U));
            }
        }
        return result;
    }

private:
    void transform(const std::uint8_t *block) {
        std::uint32_t words[64]{};
        for (std::size_t index = 0; index < 16; ++index) {
            const std::size_t offset = index * 4U;
            words[index] = (static_cast<std::uint32_t>(block[offset]) << 24U) |
                (static_cast<std::uint32_t>(block[offset + 1]) << 16U) |
                (static_cast<std::uint32_t>(block[offset + 2]) << 8U) |
                static_cast<std::uint32_t>(block[offset + 3]);
        }
        for (std::size_t index = 16; index < 64; ++index) {
            const std::uint32_t s0 = rotate_right(words[index - 15], 7) ^
                rotate_right(words[index - 15], 18) ^ (words[index - 15] >> 3U);
            const std::uint32_t s1 = rotate_right(words[index - 2], 17) ^
                rotate_right(words[index - 2], 19) ^ (words[index - 2] >> 10U);
            words[index] = words[index - 16] + s0 + words[index - 7] + s1;
        }
        std::uint32_t a = state_[0];
        std::uint32_t b = state_[1];
        std::uint32_t c = state_[2];
        std::uint32_t d = state_[3];
        std::uint32_t e = state_[4];
        std::uint32_t f = state_[5];
        std::uint32_t g = state_[6];
        std::uint32_t h = state_[7];
        for (std::size_t index = 0; index < 64; ++index) {
            const std::uint32_t s1 = rotate_right(e, 6) ^ rotate_right(e, 11) ^ rotate_right(e, 25);
            const std::uint32_t choice = (e & f) ^ ((~e) & g);
            const std::uint32_t temp1 = h + s1 + choice + kShaRound[index] + words[index];
            const std::uint32_t s0 = rotate_right(a, 2) ^ rotate_right(a, 13) ^ rotate_right(a, 22);
            const std::uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
            const std::uint32_t temp2 = s0 + majority;
            h = g; g = f; f = e; e = d + temp1;
            d = c; c = b; b = a; a = temp1 + temp2;
        }
        state_[0] += a; state_[1] += b; state_[2] += c; state_[3] += d;
        state_[4] += e; state_[5] += f; state_[6] += g; state_[7] += h;
    }

    std::array<std::uint32_t, 8> state_{{
        kShaInitial[0], kShaInitial[1], kShaInitial[2], kShaInitial[3],
        kShaInitial[4], kShaInitial[5], kShaInitial[6], kShaInitial[7]}};
    std::array<std::uint8_t, 64> block_{};
    std::size_t block_size_ = 0;
    std::size_t total_size_ = 0;
};

void write_u32(std::uint8_t *output, std::uint32_t value) {
    output[0] = static_cast<std::uint8_t>(value >> 24U);
    output[1] = static_cast<std::uint8_t>(value >> 16U);
    output[2] = static_cast<std::uint8_t>(value >> 8U);
    output[3] = static_cast<std::uint8_t>(value);
}

void write_u16(std::uint8_t *output, std::uint16_t value) {
    output[0] = static_cast<std::uint8_t>(value >> 8U);
    output[1] = static_cast<std::uint8_t>(value);
}

std::uint32_t read_u32(const std::uint8_t *input) {
    return (static_cast<std::uint32_t>(input[0]) << 24U) |
        (static_cast<std::uint32_t>(input[1]) << 16U) |
        (static_cast<std::uint32_t>(input[2]) << 8U) |
        static_cast<std::uint32_t>(input[3]);
}

std::uint16_t read_u16(const std::uint8_t *input) {
    return static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>(input[0]) << 8U) | input[1]);
}

bool valid_utf8_without_controls(const char *text, std::size_t size) {
    if (text == nullptr || size == 0) return false;
    std::size_t index = 0;
    while (index < size) {
        const auto first = static_cast<std::uint8_t>(text[index]);
        std::size_t continuation = 0;
        std::uint32_t code_point = 0;
        if (first <= 0x7fU) {
            code_point = first;
        } else if ((first & 0xe0U) == 0xc0U) {
            continuation = 1; code_point = first & 0x1fU;
            if (code_point < 2) return false;
        } else if ((first & 0xf0U) == 0xe0U) {
            continuation = 2; code_point = first & 0x0fU;
        } else if ((first & 0xf8U) == 0xf0U) {
            continuation = 3; code_point = first & 0x07U;
        } else {
            return false;
        }
        if (index + continuation >= size) return false;
        for (std::size_t offset = 1; offset <= continuation; ++offset) {
            const auto next = static_cast<std::uint8_t>(text[index + offset]);
            if ((next & 0xc0U) != 0x80U) return false;
            code_point = (code_point << 6U) | (next & 0x3fU);
        }
        if ((continuation == 2 && code_point < 0x800U) ||
            (continuation == 3 && code_point < 0x10000U) ||
            code_point == 0 || code_point > 0x10ffffU ||
            (code_point >= 0xd800U && code_point <= 0xdfffU) ||
            code_point <= 0x1fU ||
            (code_point >= 0x7fU && code_point <= 0x9fU)) {
            return false;
        }
        index += continuation + 1;
    }
    return true;
}

bool append_entry_hash(Sha256 &sha, const MemoryEntry &entry) {
    if (entry.id == 0 || !valid_memory_fact(entry.fact.data(), entry.fact.size())) {
        return false;
    }
    std::array<std::uint8_t, 6> metadata{};
    write_u32(metadata.data(), entry.id);
    write_u16(metadata.data() + 4, static_cast<std::uint16_t>(entry.fact.size()));
    sha.update(metadata.data(), metadata.size());
    sha.update(reinterpret_cast<const std::uint8_t *>(entry.fact.data()), entry.fact.size());
    return true;
}

}  // namespace

void MemorySnapshot::clear() {
    revision = 0;
    next_id = 1;
    fingerprint.fill(0);
    for (auto &entry : entries) entry.clear();
    size = 0;
}

const MemoryEntry *MemorySnapshot::find(std::uint32_t id) const {
    for (std::size_t index = 0; index < size; ++index) {
        if (entries[index].id == id) return &entries[index];
    }
    return nullptr;
}

std::size_t MemorySnapshot::total_fact_bytes() const {
    std::size_t total = 0;
    for (std::size_t index = 0; index < size; ++index) total += entries[index].fact.size();
    return total;
}

void MemoryCompactionEntry::clear() {
    source_ids.fill(0);
    source_count = 0;
    fact.clear();
}

void MemoryCompactionPlan::clear() {
    for (auto &entry : entries) entry.clear();
    size = 0;
    include_pending = false;
}

bool valid_memory_fact(const char *text, std::size_t size) {
    return size <= Limits::max_memory_fact_bytes &&
        valid_utf8_without_controls(text, size);
}

MemoryFingerprint compute_memory_fingerprint(const MemorySnapshot &snapshot) {
    if (snapshot.size > Limits::max_memory_facts) return {};
    Sha256 sha;
    sha.update(kContentDomain.data(), kContentDomain.size());
    const std::array<std::uint8_t, 2> metadata{
        kRecordVersion, static_cast<std::uint8_t>(snapshot.size)};
    sha.update(metadata.data(), metadata.size());
    for (std::size_t index = 0; index < snapshot.size; ++index) {
        if (!append_entry_hash(sha, snapshot.entries[index])) return {};
    }
    return sha.finish();
}

bool memory_fingerprints_equal(
    const MemoryFingerprint &left, const MemoryFingerprint &right) {
    std::uint8_t difference = 0;
    for (std::size_t index = 0; index < left.size(); ++index) {
        difference |= static_cast<std::uint8_t>(left[index] ^ right[index]);
    }
    return difference == 0;
}

bool encode_memory_record(
    const MemorySnapshot &snapshot, EncodedMemoryRecord &encoded,
    std::size_t &encoded_size) {
    encoded.fill(0);
    encoded_size = 0;
    if (snapshot.size > Limits::max_memory_facts || snapshot.next_id == 0 ||
        (snapshot.size != 0 && snapshot.revision == 0)) return false;
    std::uint32_t previous_id = 0;
    std::size_t cursor = kHeaderBytes;
    for (std::size_t index = 0; index < snapshot.size; ++index) {
        const MemoryEntry &entry = snapshot.entries[index];
        if (entry.id <= previous_id || entry.id >= snapshot.next_id ||
            !valid_memory_fact(entry.fact.data(), entry.fact.size()) ||
            cursor + 6 + entry.fact.size() + kChecksumBytes > encoded.size()) return false;
        for (std::size_t prior = 0; prior < index; ++prior) {
            if (snapshot.entries[prior].fact.size() == entry.fact.size() &&
                std::memcmp(
                    snapshot.entries[prior].fact.data(), entry.fact.data(),
                    entry.fact.size()) == 0) return false;
        }
        write_u32(encoded.data() + cursor, entry.id);
        write_u16(encoded.data() + cursor + 4, static_cast<std::uint16_t>(entry.fact.size()));
        std::memcpy(encoded.data() + cursor + 6, entry.fact.data(), entry.fact.size());
        cursor += 6 + entry.fact.size();
        previous_id = entry.id;
    }
    std::copy(kRecordMagic.begin(), kRecordMagic.end(), encoded.begin());
    encoded[4] = kRecordVersion;
    encoded[5] = static_cast<std::uint8_t>(snapshot.size);
    write_u32(encoded.data() + 8, snapshot.revision);
    write_u32(encoded.data() + 12, snapshot.next_id);
    const MemoryFingerprint fingerprint = compute_memory_fingerprint(snapshot);
    std::copy(fingerprint.begin(), fingerprint.end(), encoded.begin() + 16);
    Sha256 checksum;
    checksum.update(kRecordDomain.data(), kRecordDomain.size());
    checksum.update(encoded.data(), cursor);
    const MemoryFingerprint checksum_value = checksum.finish();
    std::copy(checksum_value.begin(), checksum_value.end(), encoded.begin() + cursor);
    encoded_size = cursor + checksum_value.size();
    return true;
}

bool decode_memory_record(
    const std::uint8_t *encoded, std::size_t encoded_size,
    MemorySnapshot &snapshot) {
    snapshot.clear();
    if (encoded == nullptr || encoded_size < kHeaderBytes + kChecksumBytes ||
        encoded_size > max_encoded_memory_record_bytes ||
        !std::equal(kRecordMagic.begin(), kRecordMagic.end(), encoded) ||
        encoded[4] != kRecordVersion || encoded[6] != 0 || encoded[7] != 0 ||
        encoded[5] > Limits::max_memory_facts) return false;
    Sha256 checksum;
    checksum.update(kRecordDomain.data(), kRecordDomain.size());
    checksum.update(encoded, encoded_size - kChecksumBytes);
    MemoryFingerprint supplied_checksum{};
    std::copy(encoded + encoded_size - kChecksumBytes, encoded + encoded_size,
              supplied_checksum.begin());
    if (!memory_fingerprints_equal(checksum.finish(), supplied_checksum)) return false;
    MemorySnapshot decoded;
    decoded.revision = read_u32(encoded + 8);
    decoded.next_id = read_u32(encoded + 12);
    decoded.size = encoded[5];
    std::copy(encoded + 16, encoded + 48, decoded.fingerprint.begin());
    if (decoded.next_id == 0 || (decoded.size != 0 && decoded.revision == 0)) return false;
    std::size_t cursor = kHeaderBytes;
    std::uint32_t previous_id = 0;
    for (std::size_t index = 0; index < decoded.size; ++index) {
        if (cursor + 6 > encoded_size - kChecksumBytes) return false;
        const std::uint32_t id = read_u32(encoded + cursor);
        const std::size_t fact_size = read_u16(encoded + cursor + 4);
        cursor += 6;
        if (id <= previous_id || id >= decoded.next_id ||
            cursor + fact_size > encoded_size - kChecksumBytes ||
            !valid_memory_fact(reinterpret_cast<const char *>(encoded + cursor), fact_size) ||
            !decoded.entries[index].fact.assign(
                reinterpret_cast<const char *>(encoded + cursor), fact_size)) return false;
        decoded.entries[index].id = id;
        for (std::size_t prior = 0; prior < index; ++prior) {
            if (decoded.entries[prior].fact.size() == fact_size &&
                std::memcmp(
                    decoded.entries[prior].fact.data(), encoded + cursor,
                    fact_size) == 0) return false;
        }
        previous_id = id;
        cursor += fact_size;
    }
    if (cursor != encoded_size - kChecksumBytes ||
        !memory_fingerprints_equal(decoded.fingerprint,
                                   compute_memory_fingerprint(decoded))) return false;
    snapshot = decoded;
    return true;
}

MemoryBleStatus parse_memory_ble_command(
    const std::uint8_t *data, std::size_t size, bool link_secure,
    MemoryBleCommand &command) {
    command = {};
    if (!link_secure) {
        return MemoryBleStatus::authentication_required;
    }
    if (data == nullptr || size < memory_ble_command_header_bytes ||
        size > max_memory_ble_command_bytes ||
        std::memcmp(data, "CEMC", 4) != 0) {
        return MemoryBleStatus::invalid_field;
    }
    if (data[4] != memory_ble_version) {
        return MemoryBleStatus::unsupported_version;
    }
    if (data[6] != 0 || data[7] != 0) {
        return MemoryBleStatus::invalid_field;
    }
    const std::uint8_t operation = data[5];
    if (operation < static_cast<std::uint8_t>(MemoryBleOperation::list_page) ||
        operation > static_cast<std::uint8_t>(MemoryBleOperation::clear)) {
        return MemoryBleStatus::invalid_field;
    }
    command.operation = static_cast<MemoryBleOperation>(operation);
    command.request_id = read_u32(data + 8);
    command.expected_revision = read_u32(data + 12);
    std::copy(data + 16, data + 48, command.expected_fingerprint.begin());
    command.memory_id = read_u32(data + 48);
    const std::size_t fact_size = read_u16(data + 52);
    if (command.request_id == 0 ||
        fact_size > Limits::max_memory_fact_bytes ||
        size != memory_ble_command_header_bytes + fact_size) {
        return MemoryBleStatus::invalid_field;
    }
    if (fact_size != 0 &&
        !command.fact.assign(
            reinterpret_cast<const char *>(data + memory_ble_command_header_bytes),
            fact_size)) {
        return MemoryBleStatus::invalid_field;
    }
    switch (command.operation) {
        case MemoryBleOperation::list_page:
            return fact_size == 0 ? MemoryBleStatus::applied
                                  : MemoryBleStatus::invalid_field;
        case MemoryBleOperation::add:
            return command.memory_id == 0 &&
                    valid_memory_fact(command.fact.data(), command.fact.size())
                ? MemoryBleStatus::applied
                : MemoryBleStatus::invalid_field;
        case MemoryBleOperation::forget:
            return command.memory_id != 0 && fact_size == 0
                ? MemoryBleStatus::applied
                : MemoryBleStatus::invalid_field;
        case MemoryBleOperation::clear:
            return command.memory_id == 0 && fact_size == 0
                ? MemoryBleStatus::applied
                : MemoryBleStatus::invalid_field;
        case MemoryBleOperation::changed:
            break;
    }
    return MemoryBleStatus::invalid_field;
}

bool encode_memory_ble_response(
    const MemoryBleResponse &response,
    std::array<std::uint8_t, max_memory_ble_response_bytes> &encoded,
    std::size_t &encoded_size) {
    encoded.fill(0);
    encoded_size = 0;
    if (response.total_count > Limits::max_memory_facts ||
        response.fact.size() > Limits::max_memory_fact_bytes ||
        (response.has_item &&
         (response.memory_id == 0 ||
          !valid_memory_fact(response.fact.data(), response.fact.size()))) ||
        (!response.has_item && !response.fact.empty()) ||
        (response.has_more && !response.has_item) ||
        (response.has_item &&
         response.operation != MemoryBleOperation::list_page) ||
        (response.changed_event &&
            (response.operation != MemoryBleOperation::changed ||
          response.status != MemoryBleStatus::applied ||
          response.request_id != 0 || response.has_item || response.has_more))) {
        return false;
    }
    std::memcpy(encoded.data(), "CEMR", 4);
    encoded[4] = memory_ble_version;
    encoded[5] = static_cast<std::uint8_t>(response.status);
    encoded[6] = static_cast<std::uint8_t>(response.operation);
    encoded[7] = static_cast<std::uint8_t>(
        (response.has_item ? 0x01U : 0U) |
        (response.has_more ? 0x02U : 0U) |
        (response.changed_event ? 0x04U : 0U));
    write_u32(encoded.data() + 8, response.request_id);
    write_u32(encoded.data() + 12, response.revision);
    std::copy(response.fingerprint.begin(), response.fingerprint.end(),
              encoded.begin() + 16);
    write_u32(encoded.data() + 48, response.memory_id);
    write_u16(
        encoded.data() + 52,
        static_cast<std::uint16_t>(response.fact.size()));
    encoded[54] = response.total_count;
    if (!response.fact.empty()) {
        std::memcpy(
            encoded.data() + memory_ble_response_header_bytes,
            response.fact.data(), response.fact.size());
    }
    encoded_size = memory_ble_response_header_bytes + response.fact.size();
    return true;
}

}  // namespace agent
}  // namespace chatesp
