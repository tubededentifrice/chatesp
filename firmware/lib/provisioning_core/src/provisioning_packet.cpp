#include "chatesp/provisioning_packet.hpp"

#include <algorithm>
#include <array>
#include <cstring>

namespace chatesp {
namespace provisioning {
namespace {

constexpr std::array<std::uint8_t, 4> kMagic{'C', 'E', 'S', 'P'};
constexpr std::array<std::uint8_t, 15> kFingerprintDomainV1{
    'C', 'E', 'S', 'P', '-', 'C', 'O', 'N', 'T', 'E', 'N', 'T', '-', 'V', '1'};
constexpr std::array<std::uint8_t, 15> kFingerprintDomainV2{
    'C', 'E', 'S', 'P', '-', 'C', 'O', 'N', 'T', 'E', 'N', 'T', '-', 'V', '2'};
constexpr std::array<std::uint8_t, 15> kDeviceContextFingerprintDomain{
    'C', 'E', 'S', 'P', '-', 'C', 'O', 'N', 'T', 'E', 'X', 'T', '-', 'V', '1'};

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
        if (data == nullptr || size == 0) {
            return;
        }
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

    std::array<std::uint8_t, kFingerprintSize> finish() {
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

        std::array<std::uint8_t, kFingerprintSize> result{};
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
            words[index] =
                (static_cast<std::uint32_t>(block[offset]) << 24U) |
                (static_cast<std::uint32_t>(block[offset + 1]) << 16U) |
                (static_cast<std::uint32_t>(block[offset + 2]) << 8U) |
                static_cast<std::uint32_t>(block[offset + 3]);
        }
        for (std::size_t index = 16; index < 64; ++index) {
            const std::uint32_t small0 = rotate_right(words[index - 15], 7) ^
                rotate_right(words[index - 15], 18) ^ (words[index - 15] >> 3U);
            const std::uint32_t small1 = rotate_right(words[index - 2], 17) ^
                rotate_right(words[index - 2], 19) ^ (words[index - 2] >> 10U);
            words[index] = words[index - 16] + small0 + words[index - 7] + small1;
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
            const std::uint32_t large1 = rotate_right(e, 6) ^ rotate_right(e, 11) ^
                rotate_right(e, 25);
            const std::uint32_t choose = (e & f) ^ (~e & g);
            const std::uint32_t temp1 = h + large1 + choose + kShaRound[index] + words[index];
            const std::uint32_t large0 = rotate_right(a, 2) ^ rotate_right(a, 13) ^
                rotate_right(a, 22);
            const std::uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
            const std::uint32_t temp2 = large0 + majority;
            h = g;
            g = f;
            f = e;
            e = d + temp1;
            d = c;
            c = b;
            b = a;
            a = temp1 + temp2;
        }
        state_[0] += a;
        state_[1] += b;
        state_[2] += c;
        state_[3] += d;
        state_[4] += e;
        state_[5] += f;
        state_[6] += g;
        state_[7] += h;
    }

    std::array<std::uint32_t, 8> state_{{
        kShaInitial[0], kShaInitial[1], kShaInitial[2], kShaInitial[3],
        kShaInitial[4], kShaInitial[5], kShaInitial[6], kShaInitial[7]}};
    std::array<std::uint8_t, 64> block_{};
    std::size_t block_size_ = 0;
    std::size_t total_size_ = 0;
};

std::uint16_t read_u16(const std::uint8_t *data) {
    return static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>(data[0]) << 8U) |
        static_cast<std::uint16_t>(data[1]));
}

std::uint32_t read_u32(const std::uint8_t *data) {
    return (static_cast<std::uint32_t>(data[0]) << 24U) |
        (static_cast<std::uint32_t>(data[1]) << 16U) |
        (static_cast<std::uint32_t>(data[2]) << 8U) |
        static_cast<std::uint32_t>(data[3]);
}

std::uint64_t read_u64(const std::uint8_t *data) {
    std::uint64_t value = 0;
    for (std::size_t index = 0; index < 8; ++index) {
        value = (value << 8U) | data[index];
    }
    return value;
}

void write_u64(std::uint8_t *data, std::uint64_t value) {
    for (std::size_t index = 0; index < 8; ++index) {
        data[7U - index] = static_cast<std::uint8_t>(value >> (index * 8U));
    }
}

bool fingerprints_equal(
    const std::array<std::uint8_t, kFingerprintSize> &left,
    const std::array<std::uint8_t, kFingerprintSize> &right) {
    std::uint8_t difference = 0;
    for (std::size_t index = 0; index < left.size(); ++index) {
        difference |= static_cast<std::uint8_t>(left[index] ^ right[index]);
    }
    return difference == 0;
}

bool valid_utf8(std::string_view value, bool reject_controls = false) {
    std::size_t index = 0;
    while (index < value.size()) {
        const auto first = static_cast<std::uint8_t>(value[index]);
        std::size_t continuation = 0;
        std::uint32_t code_point = 0;
        if (first <= 0x7f) {
            code_point = first;
        } else if ((first & 0xe0U) == 0xc0U) {
            continuation = 1;
            code_point = first & 0x1fU;
            if (code_point < 2) {
                return false;
            }
        } else if ((first & 0xf0U) == 0xe0U) {
            continuation = 2;
            code_point = first & 0x0fU;
        } else if ((first & 0xf8U) == 0xf0U) {
            continuation = 3;
            code_point = first & 0x07U;
        } else {
            return false;
        }
        if (index + continuation >= value.size()) {
            return false;
        }
        for (std::size_t offset = 1; offset <= continuation; ++offset) {
            const auto next = static_cast<std::uint8_t>(value[index + offset]);
            if ((next & 0xc0U) != 0x80U) {
                return false;
            }
            code_point = (code_point << 6U) | (next & 0x3fU);
        }
        if ((continuation == 2 && code_point < 0x800U) ||
            (continuation == 3 && code_point < 0x10000U) ||
            code_point > 0x10ffffU || (code_point >= 0xd800U && code_point <= 0xdfffU) ||
            code_point == 0) {
            return false;
        }
        if (reject_controls &&
            (code_point <= 0x1fU ||
             (code_point >= 0x7fU && code_point <= 0x9fU))) {
            return false;
        }
        index += continuation + 1;
    }
    return true;
}

bool valid_visible_ascii(std::string_view value, bool allow_empty) {
    if (value.empty()) {
        return allow_empty;
    }
    return std::all_of(value.begin(), value.end(), [](char item) {
        const auto byte = static_cast<std::uint8_t>(item);
        return byte >= 0x21U && byte <= 0x7eU;
    });
}

bool valid_endpoint(std::string_view value) {
    const std::string_view prefix = "https://";
    if (value.size() < 12 || value.size() > 192 || value.substr(0, prefix.size()) != prefix ||
        !valid_visible_ascii(value, false)) {
        return false;
    }
    const std::string_view rest = value.substr(prefix.size());
    const std::size_t host_end = rest.find('/');
    const std::string_view host = rest.substr(0, host_end);
    return !host.empty() && host.front() != '.' && host.back() != '.' &&
        host.find('.') != std::string_view::npos && host.find('@') == std::string_view::npos &&
        value.find('?') == std::string_view::npos && value.find('#') == std::string_view::npos;
}

bool valid_model(std::string_view value) {
    if (value.empty() || value.size() > 96) {
        return false;
    }
    return std::all_of(value.begin(), value.end(), [](char item) {
        return (item >= 'a' && item <= 'z') || (item >= 'A' && item <= 'Z') ||
            (item >= '0' && item <= '9') || item == '-' || item == '_' ||
            item == '.' || item == '/' || item == ':' || item == '~';
    });
}

ValidationError validate_field(std::uint8_t id, std::string_view value) {
    switch (id) {
        case 1:
            return valid_endpoint(value) ? ValidationError::none : ValidationError::invalid_endpoint;
        case 2:
            return value.size() >= 8 && value.size() <= 256 && valid_visible_ascii(value, false)
                ? ValidationError::none : ValidationError::invalid_openrouter_key;
        case 3:
            return value.size() <= 128 && valid_visible_ascii(value, true)
                ? ValidationError::none : ValidationError::invalid_brave_key;
        case 4:
            return !value.empty() && value.size() <= 32 && valid_utf8(value)
                ? ValidationError::none : ValidationError::invalid_wifi_ssid;
        case 5:
            return value.size() >= 8 && value.size() <= 63 && valid_utf8(value)
                ? ValidationError::none : ValidationError::invalid_wifi_password;
        case 6:
        case 7:
        case 8:
            return valid_model(value) ? ValidationError::none : ValidationError::invalid_model;
        case 9:
            return value.size() <= kMaximumApproximateLocationSize &&
                    valid_utf8(value, true)
                ? ValidationError::none
                : ValidationError::invalid_approximate_location;
        default:
            return ValidationError::bad_field_order;
    }
}

void assign_field(SettingsView &settings, std::uint8_t id, std::string_view value) {
    switch (id) {
        case 1: settings.chat_endpoint = value; break;
        case 2: settings.openrouter_key = value; break;
        case 3: settings.brave_key = value; break;
        case 4: settings.wifi_ssid = value; break;
        case 5: settings.wifi_password = value; break;
        case 6: settings.chat_model = value; break;
        case 7: settings.transcription_model = value; break;
        case 8: settings.speech_model = value; break;
        case 9: settings.approximate_location = value; break;
        default: break;
    }
}

std::array<std::uint8_t, kFingerprintSize> device_context_fingerprint(
    std::uint64_t epoch_seconds,
    std::int16_t utc_offset_minutes,
    std::string_view approximate_location) {
    std::array<std::uint8_t, 12> metadata{};
    metadata[0] = kDeviceContextVersion;
    write_u64(metadata.data() + 1, epoch_seconds);
    const std::uint16_t offset_bits =
        static_cast<std::uint16_t>(utc_offset_minutes);
    metadata[9] = static_cast<std::uint8_t>(offset_bits >> 8U);
    metadata[10] = static_cast<std::uint8_t>(offset_bits);
    metadata[11] = static_cast<std::uint8_t>(approximate_location.size());
    Sha256 sha;
    sha.update(kDeviceContextFingerprintDomain.data(), kDeviceContextFingerprintDomain.size());
    sha.update(metadata.data(), metadata.size());
    sha.update(
        reinterpret_cast<const std::uint8_t *>(approximate_location.data()),
        approximate_location.size());
    return sha.finish();
}

}  // namespace

bool link_is_secure(const LinkSecurity &security) {
    return security.encrypted && security.authenticated && security.bonded && security.lesc;
}

std::array<std::uint8_t, kFingerprintSize> compute_content_fingerprint(
    std::uint8_t version,
    std::uint8_t packet_type,
    std::uint8_t field_count,
    const std::uint8_t *payload,
    std::size_t payload_size) {
    Sha256 sha;
    const auto &domain = version == 1 ? kFingerprintDomainV1
                                      : kFingerprintDomainV2;
    sha.update(domain.data(), domain.size());
    const std::array<std::uint8_t, 3> metadata{version, packet_type, field_count};
    sha.update(metadata.data(), metadata.size());
    sha.update(payload, payload_size);
    return sha.finish();
}

ValidationResult validate_settings_packet(
    const std::uint8_t *packet,
    std::size_t packet_size,
    const LinkSecurity &security,
    const StoredVersion &stored) {
    ValidationResult result;
    if (!link_is_secure(security)) {
        result.error = ValidationError::authentication_required;
        return result;
    }
    if (packet == nullptr || packet_size < kHeaderSize) {
        result.error = ValidationError::packet_too_short;
        return result;
    }
    if (packet_size > kMaximumPacketSize) {
        result.error = ValidationError::packet_too_large;
        return result;
    }
    if (!std::equal(kMagic.begin(), kMagic.end(), packet)) {
        result.error = ValidationError::bad_magic;
        return result;
    }
    if (!supported_protocol_version(packet[4])) {
        result.error = ValidationError::unsupported_version;
        return result;
    }
    if (packet[5] != kSettingsPacketType) {
        result.error = ValidationError::bad_type;
        return result;
    }
    const std::uint8_t required_field_count =
        packet[4] == 1 ? kVersion1FieldCount : kRequiredFieldCount;
    if (packet[6] != 0 || packet[7] != required_field_count) {
        result.error = packet[6] != 0 ? ValidationError::bad_flags : ValidationError::bad_field_count;
        return result;
    }

    result.revision = read_u32(packet + 8);
    if (result.revision == 0) {
        result.error = ValidationError::stale_revision;
        return result;
    }
    const std::size_t payload_size = read_u16(packet + 12);
    const std::size_t total_size = read_u16(packet + 14);
    if (total_size != packet_size || payload_size != packet_size - kHeaderSize) {
        result.error = ValidationError::bad_length;
        return result;
    }
    std::copy(packet + 16, packet + 48, result.fingerprint.begin());
    const auto computed = compute_content_fingerprint(
        packet[4], packet[5], packet[7], packet + kHeaderSize, payload_size);
    if (!fingerprints_equal(result.fingerprint, computed)) {
        result.error = ValidationError::bad_fingerprint;
        return result;
    }

    std::size_t offset = kHeaderSize;
    std::uint8_t expected_id = 1;
    while (offset < packet_size) {
        if (packet_size - offset < 3) {
            result.error = ValidationError::bad_length;
            return result;
        }
        const std::uint8_t id = packet[offset];
        const std::size_t length = read_u16(packet + offset + 1);
        offset += 3;
        if (id < expected_id) {
            result.error = ValidationError::duplicate_field;
            return result;
        }
        if (id != expected_id) {
            result.error = ValidationError::bad_field_order;
            return result;
        }
        if (length > packet_size - offset) {
            result.error = ValidationError::bad_length;
            return result;
        }
        const std::string_view value{
            reinterpret_cast<const char *>(packet + offset), length};
        const ValidationError field_error = validate_field(id, value);
        if (field_error != ValidationError::none) {
            result.error = field_error;
            return result;
        }
        assign_field(result.settings, id, value);
        offset += length;
        ++expected_id;
    }
    if (expected_id != required_field_count + 1) {
        result.error = ValidationError::missing_field;
        return result;
    }

    if (stored.present) {
        if (result.revision < stored.revision) {
            result.error = ValidationError::stale_revision;
            return result;
        }
        if (result.revision == stored.revision) {
            if (!fingerprints_equal(result.fingerprint, stored.fingerprint)) {
                result.error = ValidationError::revision_conflict;
                return result;
            }
            result.decision = ApplyDecision::unchanged;
            return result;
        }
    }
    result.decision = ApplyDecision::apply;
    return result;
}

DeviceContextResult validate_device_context_packet(
    const std::uint8_t *packet,
    std::size_t packet_size,
    const LinkSecurity &security) {
    DeviceContextResult result;
    if (!link_is_secure(security)) {
        result.status = DeviceContextStatus::authentication_required;
        return result;
    }
    if (packet == nullptr || packet_size < kDeviceContextHeaderSize ||
        packet_size > kMaximumDeviceContextSize ||
        std::memcmp(packet, "CESC", 4) != 0) {
        return result;
    }
    if (packet[4] != kDeviceContextVersion) {
        result.status = DeviceContextStatus::unsupported_version;
        return result;
    }
    const std::size_t location_size = packet[48];
    if (packet[5] != 0 ||
        packet_size != kDeviceContextHeaderSize + location_size) {
        return result;
    }
    const std::string_view location{
        reinterpret_cast<const char *>(packet + kDeviceContextHeaderSize),
        location_size};
    if (location.size() > kMaximumApproximateLocationSize ||
        !valid_utf8(location, true)) {
        result.status = DeviceContextStatus::invalid_location;
        return result;
    }
    result.epoch_seconds = read_u64(packet + 6);
    result.utc_offset_minutes = static_cast<std::int16_t>(read_u16(packet + 14));
    // Accept 2020-01-01 through 9999-12-31.
    if (result.epoch_seconds < 1'577'836'800ULL ||
        result.epoch_seconds > 253'402'300'799ULL ||
        result.utc_offset_minutes < -840 || result.utc_offset_minutes > 840) {
        return result;
    }
    std::copy(packet + 16, packet + 48, result.fingerprint.begin());
    if (!fingerprints_equal(
            result.fingerprint,
            device_context_fingerprint(
                result.epoch_seconds, result.utc_offset_minutes, location))) {
        return result;
    }
    result.approximate_location = location;
    result.status = DeviceContextStatus::applied;
    return result;
}

std::array<std::uint8_t, kDeviceContextAcknowledgementSize>
make_device_context_acknowledgement(
    DeviceContextStatus status,
    std::uint64_t epoch_seconds,
    const std::array<std::uint8_t, kFingerprintSize> &fingerprint) {
    std::array<std::uint8_t, kDeviceContextAcknowledgementSize> result{};
    result[0] = 'C';
    result[1] = 'E';
    result[2] = 'S';
    result[3] = 'R';
    result[4] = kDeviceContextVersion;
    result[5] = static_cast<std::uint8_t>(status);
    write_u64(result.data() + 8, epoch_seconds);
    std::copy(fingerprint.begin(), fingerprint.end(), result.begin() + 16);
    return result;
}

}  // namespace provisioning
}  // namespace chatesp
