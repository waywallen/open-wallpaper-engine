module;
#include <rstd/macro.hpp>

export module wescene.io;
import rstd;

using namespace rstd::prelude;

export namespace owe::io
{

enum class ByteOrder : rstd::uint8_t
{
    BigEndian,
    LittleEndian,
};

template<rstd::num::Integer T>
constexpr auto byte_swap(T value) -> T {
    if constexpr (sizeof(T) == 1) {
        return value;
    } else {
        return value.swap_bytes();
    }
}

consteval auto system_byte_order() -> ByteOrder {
#ifdef WP_BIG_ENDIAN
    return ByteOrder::BigEndian;
#else
    return ByteOrder::LittleEndian;
#endif
}

class BinaryReader {
public:
    explicit BinaryReader(rstd::io::ReadRange range): BinaryReader(prepare(rstd::move(range))) {}

    explicit BinaryReader(Vec<u8> bytes): BinaryReader(prepare(rstd::move(bytes))) {}

    explicit BinaryReader(BinaryReader& source): BinaryReader(read_remaining(source)) {}

    BinaryReader(const BinaryReader&)                        = delete;
    auto operator=(const BinaryReader&) -> BinaryReader&     = delete;
    BinaryReader(BinaryReader&&) noexcept                    = default;
    auto operator=(BinaryReader&&) noexcept -> BinaryReader& = default;

    void set_byte_order(ByteOrder order) noexcept { m_byte_order = order; }
    void SetByteOrder(ByteOrder order) noexcept { set_byte_order(order); }

    auto read(void* buffer, usize size) -> rstd::io::Result<usize> {
        auto bytes  = rstd::mut_ref<u8[]>::from_raw_parts(static_cast<byte*>(buffer), size);
        auto result = m_handle->read(bytes);
        if (result.is_ok()) m_position += rstd::as_cast<u64>(*result);
        return result;
    }

    auto read_exact(void* buffer, usize size) -> rstd::io::Result<empty> {
        auto* bytes = static_cast<byte*>(buffer);
        while (size > usize()) {
            auto count = rstd_try(read(bytes, size));
            if (count == usize()) {
                return Err(rstd::io::error::Error::from_kind(
                    rstd::io::error::ErrorKind { rstd::io::error::ErrorKind::UnexpectedEof }));
            }
            bytes += count.to_primitive();
            size -= count;
        }
        return Ok(empty {});
    }

    auto seek(rstd::io::SeekFrom from) -> rstd::io::Result<u64> {
        auto result = m_handle->seek(from);
        if (result.is_ok()) m_position = *result;
        return result;
    }

    auto position() const noexcept -> u64 { return m_position; }
    auto len() const noexcept -> u64 { return m_len; }
    auto remaining() const noexcept -> u64 {
        return m_position < m_len ? m_len - m_position : u64();
    }

    rstd::size_t Read(void* buffer, rstd::size_t size) {
        auto*        bytes = static_cast<byte*>(buffer);
        rstd::size_t total {};
        while (total < size) {
            auto result = read(bytes + total, usize(size - total));
            if (result.is_err()) break;
            auto count = rstd::move(result).unwrap_unchecked().to_primitive();
            if (count == 0) break;
            total += count;
        }
        return total;
    }

    char* Gets(char* buffer, rstd::size_t size) {
        auto read = Read(buffer, size);
        return read == 0 ? nullptr : buffer;
    }

    rstd::ptrdiff_t Tell() const noexcept {
        return static_cast<rstd::ptrdiff_t>(m_position.to_primitive());
    }

    bool SeekSet(rstd::ptrdiff_t offset) {
        return offset >= 0 &&
               seek(rstd::io::SeekFrom::from_start(u64(static_cast<rstd::uint64_t>(offset))))
                   .is_ok();
    }

    bool SeekCur(rstd::ptrdiff_t offset) {
        return seek(rstd::io::SeekFrom::from_current(i64(static_cast<rstd::int64_t>(offset))))
            .is_ok();
    }

    bool SeekEnd(rstd::ptrdiff_t offset) {
        return seek(rstd::io::SeekFrom::from_end(i64(static_cast<rstd::int64_t>(offset)))).is_ok();
    }

    rstd::ptrdiff_t Size() const noexcept {
        return static_cast<rstd::ptrdiff_t>(m_len.to_primitive());
    }
    rstd::size_t Usize() const noexcept { return static_cast<rstd::size_t>(m_len.to_primitive()); }
    bool         Rewind() { return SeekSet(0); }

    float ReadFloat() {
        f32 value {};
        Read(&value, sizeof(value));
        return value.to_primitive();
    }

    rstd::int64_t  ReadInt64() { return read_integer<i64>().to_primitive(); }
    rstd::uint64_t ReadUint64() { return read_integer<u64>().to_primitive(); }
    rstd::int32_t  ReadInt32() { return read_integer<i32>().to_primitive(); }
    rstd::uint32_t ReadUint32() { return read_integer<u32>().to_primitive(); }
    rstd::int16_t  ReadInt16() { return read_integer<i16>().to_primitive(); }
    rstd::uint16_t ReadUint16() { return read_integer<u16>().to_primitive(); }
    rstd::int8_t   ReadInt8() { return read_integer<i8>().to_primitive(); }
    rstd::uint8_t  ReadUint8() { return read_integer<u8>().to_primitive(); }

    auto ReadStr() -> String {
        Vec<u8> bytes;
        u8      current {};
        while (Read(&current, 1) == 1 && current != u8()) bytes.push(rstd::move(current));
        return String::from_utf8(rstd::move(bytes)).unwrap();
    }

    auto read_all_bytes() -> rstd::io::Result<Vec<u8>> {
        if (remaining() > rstd::as_cast<u64>(usize::MAX)) {
            return rstd::Err(rstd::io::error::Error::from_kind(
                rstd::io::error::ErrorKind { rstd::io::error::ErrorKind::InvalidData }));
        }
        auto value = Vec<u8>::with_capacity(rstd::as_cast<usize>(remaining()));
        value.resize(rstd::as_cast<usize>(remaining()), u8());
        rstd_try(read_exact(value.data(), value.len()));
        return Ok(rstd::move(value));
    }

    auto read_all_string() -> rstd::io::Result<String> {
        auto bytes = rstd_try(read_all_bytes());
        auto value = String::from_utf8(rstd::move(bytes));
        if (value.is_err())
            return Err(rstd::io::error::Error::from_kind(
                rstd::io::error::ErrorKind { rstd::io::error::ErrorKind::InvalidData }));
        return Ok(rstd::move(value).unwrap_unchecked());
    }

    auto ReadAllStr() -> String {
        auto value = read_all_string();
        return value.is_ok() ? rstd::move(value).unwrap_unchecked() : String {};
    }

private:
    struct Prepared {
        rstd::io::ReadSeekHandle handle;
        u64                      len;
    };

    BinaryReader(rstd::io::ReadSeekHandle handle, u64 len)
        : m_handle(rstd::move(handle)), m_len(len) {}

    explicit BinaryReader(Prepared prepared)
        : BinaryReader(rstd::move(prepared.handle), prepared.len) {}

    static auto prepare(Vec<u8> bytes) -> Prepared {
        auto len    = rstd::as_cast<u64>(bytes.len());
        auto cursor = rstd::io::Cursor<Vec<u8>>(rstd::move(bytes));
        return Prepared { .handle = rstd::io::ReadSeekHandle::make(rstd::move(cursor)),
                          .len    = len };
    }

    static auto read_remaining(BinaryReader& source) -> Vec<u8> {
        auto bytes = Vec<u8>::with_capacity(rstd::as_cast<usize>(source.remaining()));
        bytes.resize(rstd::as_cast<usize>(source.remaining()), u8());
        auto count = source.Read(bytes.data(), bytes.len().to_primitive());
        bytes.truncate(usize(count));
        return bytes;
    }

    template<typename T>
    auto read_integer() -> T {
        T value {};
        if (Read(&value, sizeof(value)) != sizeof(value)) return T {};
        if (m_byte_order != owe::io::system_byte_order()) value = byte_swap(value);
        return value;
    }

    static auto prepare(rstd::io::ReadRange range) -> Prepared {
        auto len    = range.len();
        auto reader = rstd::io::BufReader(rstd::move(range).into_reader());
        return Prepared { .handle = rstd::io::ReadSeekHandle::make(rstd::move(reader)),
                          .len    = len };
    }

    rstd::io::ReadSeekHandle m_handle;
    u64                      m_len {};
    u64                      m_position {};
    ByteOrder                m_byte_order { ByteOrder::LittleEndian };
};

class BinaryWriter {
public:
    explicit BinaryWriter(rstd::io::WriteSeekHandle handle): m_handle(rstd::move(handle)) {}

    BinaryWriter(const BinaryWriter&)                        = delete;
    auto operator=(const BinaryWriter&) -> BinaryWriter&     = delete;
    BinaryWriter(BinaryWriter&&) noexcept                    = default;
    auto operator=(BinaryWriter&&) noexcept -> BinaryWriter& = default;

    void set_byte_order(ByteOrder order) noexcept { m_byte_order = order; }
    void SetByteOrder(ByteOrder order) noexcept { set_byte_order(order); }

    auto write(const void* buffer, usize size) -> rstd::io::Result<empty> {
        auto* bytes = static_cast<const byte*>(buffer);
        while (size > usize()) {
            auto source = rstd::slice<u8>::from_raw_parts(bytes, size);
            auto count  = rstd_try(m_handle->write(source));
            if (count == usize()) {
                return Err(rstd::io::error::Error::from_kind(
                    rstd::io::error::ErrorKind { rstd::io::error::ErrorKind::WriteZero }));
            }
            bytes += count.to_primitive();
            size -= count;
        }
        return Ok(empty {});
    }

    rstd::size_t Write(const void* buffer, rstd::size_t size) {
        return write(buffer, usize(size)).is_ok() ? size : 0;
    }
    rstd::int32_t WriteInt32(rstd::int32_t value) {
        return write_integer(i32(value)) ? static_cast<rstd::int32_t>(sizeof(value)) : 0;
    }
    rstd::int32_t WriteUint32(rstd::uint32_t value) {
        return write_integer(u32(value)) ? static_cast<rstd::int32_t>(sizeof(value)) : 0;
    }

    auto flush() -> rstd::io::Result<empty> { return m_handle->flush(); }

private:
    template<typename T>
    bool write_integer(T value) {
        if (m_byte_order != system_byte_order()) value = byte_swap(value);
        return write(&value, usize(sizeof(value))).is_ok();
    }

    rstd::io::WriteSeekHandle m_handle;
    ByteOrder                 m_byte_order { ByteOrder::LittleEndian };
};

} // namespace owe::io
