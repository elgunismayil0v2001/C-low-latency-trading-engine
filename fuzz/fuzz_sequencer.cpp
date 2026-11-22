#include <cstddef>
#include <cstdint>

#include "tickbook/sequencer.hpp"

namespace {

std::uint64_t read_u64_le(const std::uint8_t* bytes) noexcept {
  std::uint64_t value = 0;
  for (std::size_t i = 0; i < sizeof(value); ++i) {
    value |= static_cast<std::uint64_t>(bytes[i]) << (8U * i);
  }
  return value;
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  if (size < 24) return 0;
  tickbook::DiffSequencer sequencer(read_u64_le(data));
  for (std::size_t offset = 8; offset <= size - 16; offset += 16) {
    const auto first_id = read_u64_le(data + offset);
    const auto last_id = read_u64_le(data + offset + 8);
    if ((data[offset] & 0x1fU) == 0) {
      sequencer.reset(last_id);
    } else {
      static_cast<void>(sequencer.on_message(first_id, last_id));
    }
  }
  return 0;
}
