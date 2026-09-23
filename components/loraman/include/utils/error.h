#pragma once

#include <cstdint>
#include <expected>
#include <source_location>
#include <utility>
#include <version>

namespace loraman {

// Subsystem-scoped error codes. Stored in Error::code. The detail field
// carries subsystem-specific context (FRESULT, RadioLib code, line number).
enum class ErrCode : uint32_t {
    Ok                  = 0,

    // NVS (0x01xx)
    NvsInitFailed       = 0x0100,
    NvsOpenFailed       = 0x0101,
    NvsReadFailed       = 0x0102,
    NvsWriteFailed      = 0x0103,

    // Config (0x02xx)
    CfgFileMissing      = 0x0200,
    CfgNoColon          = 0x0201,
    CfgUnknownGroup     = 0x0202,
    CfgUnknownKey       = 0x0203,
    CfgInvalidValue     = 0x0204,
    CfgNoActiveGroup    = 0x0205,

    // Crypto / Keychain (0x03xx)
    KeyNotFound         = 0x0300,
    KeyBufferFull       = 0x0301,
    KeyInvalidName      = 0x0302,
    KeyInvalidLength    = 0x0303,
    CryptoMismatch      = 0x0304,  // no key validated during decrypt
    CryptoBufferTooSmall= 0x0305,

    // Radio (0x04xx)
    RadioInitFailed     = 0x0400,
    RadioTxFailed       = 0x0401,
    RadioRxFailed       = 0x0402,
    RadioTimeout        = 0x0403,

    // Platform (0x05xx)
    PlatformInitFailed  = 0x0500,

    // Queue (0x06xx)
    QueueFull           = 0x0600,
    QueueEmpty          = 0x0601,  // not always an error
    PacketOversize      = 0x0602,

    // Nodes (0x07xx)
    NodeTableFull       = 0x0700,
    NodeNotFound        = 0x0701,

    // Generic
    InvalidArgument     = 0xFF00,
    Unsupported         = 0xFF01,
    InternalError       = 0xFF02,
};

// Structured error. Carries the code, a subsystem-specific detail, and the
// source location of the failure point. The location is captured by
// default-constructing std::source_location::current() as a default
// argument
struct Error {
    ErrCode code{ErrCode::Ok};
    int32_t detail{0};
    std::source_location loc{std::source_location::current()};

    bool ok() const { return code == ErrCode::Ok; }
    explicit operator bool() const { return ok(); }
};

// The type alias. std::expected<T, Error> carries either a value of type T
// or an Error. The void specialisation (std::expected<void, Error>) carries
// only the Error on failure and nothing on success.
template <typename T>
using Result = std::expected<T, Error>;

inline Result<void> ok() { return {}; }

template <typename T>
inline Result<T> ok(T value) { return std::move(value); }

inline Result<void> fail(
    ErrCode code,
    int32_t detail = 0,
    std::source_location loc = std::source_location::current()) {
    return std::unexpected<Error>(Error{code, detail, loc});
}

template <typename T>
inline Result<T> fail(
    ErrCode code,
    int32_t detail = 0,
    std::source_location loc = std::source_location::current()) {
    return std::unexpected<Error>(Error{code, detail, loc});
}

} // namespace loraman