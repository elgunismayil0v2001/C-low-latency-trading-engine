#include <cstddef>
#include <cstdint>
#include <span>

#include "tickbook/binary_file.hpp"
#include "tickbook/binary_replay.hpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  const auto* bytes = reinterpret_cast<const std::byte*>(data);
  const std::span<const std::byte> file(bytes, size);
  tickbook::BinaryFileView view{};
  if (tickbook::validate_binary_file(file, view) != tickbook::BinaryFileError::None) return 0;

  std::uint64_t checksum = view.event_count;
  for (std::size_t offset = 0; offset < view.event_bytes.size();
       offset += tickbook::kBinaryEventSize) {
    const auto event_bytes = std::span<const std::byte, tickbook::kBinaryEventSize>(
        view.event_bytes.data() + offset, tickbook::kBinaryEventSize);
    const tickbook::BinaryEventView event(event_bytes);
    checksum ^= static_cast<std::uint64_t>(event.ts_ns());
    checksum += event.first_id() + event.last_id();
    checksum ^= static_cast<std::uint64_t>(event.price()) + static_cast<std::uint64_t>(event.qty());
    checksum += static_cast<std::uint8_t>(event.type());
    checksum += static_cast<std::uint8_t>(event.side());
    checksum += event.flags() + event.padding(0) + event.padding(4);
    const tickbook::Event copy = event.copy();
    checksum ^= static_cast<std::uint64_t>(copy.ts_ns);
  }
  const volatile std::uint64_t observed_checksum = checksum;
  static_cast<void>(observed_checksum);
  return 0;
}
