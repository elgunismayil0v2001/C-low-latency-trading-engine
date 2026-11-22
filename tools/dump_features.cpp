#include "dump_features.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <system_error>

#include "tickbook/binary_replay.hpp"
#include "tickbook/book_map.hpp"
#include "tickbook/features/feature_dump.hpp"
#include "tickbook/features/feature_engine.hpp"

namespace tickbook::feature_dump_tool {
namespace {

constexpr std::size_t kMaximumPendingCapacity = 1U << 20U;
constexpr std::uint8_t kAllLabelsResolved = 0b111U;

struct PendingRecord {
  FeatureDumpRecord record{};
  std::uint64_t mid2_price{};
  std::uint8_t resolved{};
};

struct SamplingGrid {
  std::uint64_t interval_ns{};
  std::int64_t next_grid_ns{};
  bool exhausted{};
};

struct HorizonTimestamps {
  std::int64_t start_ns{};
  std::int64_t now_ns{};
  std::uint64_t horizon_ns{};
};

[[nodiscard]] bool write_header(std::ofstream& output, const FeatureDumpHeader& header) {
  const auto bytes = make_feature_dump_header(header);
  output.write(reinterpret_cast<const char*>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
  return output.good();
}

[[nodiscard]] bool write_record(std::ofstream& output, const FeatureDumpRecord& record) {
  const auto bytes = encode_feature_dump_record(record);
  output.write(reinterpret_cast<const char*>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
  return output.good();
}

[[nodiscard]] bool grid_advance(std::int64_t ts_ns, SamplingGrid& grid) noexcept {
  const auto timestamp = static_cast<std::uint64_t>(ts_ns);
  const auto base = timestamp - timestamp % grid.interval_ns;
  const auto max_ts = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
  if (base > max_ts - grid.interval_ns) {
    grid.exhausted = true;
    return false;
  }
  grid.next_grid_ns = static_cast<std::int64_t>(base + grid.interval_ns);
  return true;
}

[[nodiscard]] bool target_reached(const HorizonTimestamps& timestamps) noexcept {
  const auto start = static_cast<std::uint64_t>(timestamps.start_ns);
  const auto now = static_cast<std::uint64_t>(timestamps.now_ns);
  if (start > std::numeric_limits<std::uint64_t>::max() - timestamps.horizon_ns) return false;
  return now >= start + timestamps.horizon_ns;
}

} // namespace

Result dump_feature_file(const Options& options) {
  Result result{};
  if (options.input_path.empty() || options.output_path.empty()) {
    result.error = "input and output paths are required";
    return result;
  }
  if (options.price_tick <= 0 || options.sample_interval_ns == 0 ||
      options.sample_interval_ns >
          static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
    result.error = "price tick and sample interval must be positive and representable";
    return result;
  }
  const auto required_pending = kFeatureForwardHorizonsNs.back() / options.sample_interval_ns + 2U;
  if (required_pending > kMaximumPendingCapacity) {
    result.error = "sample interval is too small for bounded forward-label storage";
    return result;
  }
  const auto pending_capacity = static_cast<std::size_t>(required_pending);

  BinaryReplay source;
  if (!source.open(options.input_path, result.error)) return result;

  auto temporary_path = options.output_path;
  temporary_path += ".tmp";
  std::error_code filesystem_error;
  std::filesystem::remove(temporary_path, filesystem_error);
  filesystem_error.clear();
  std::ofstream output(temporary_path, std::ios::binary | std::ios::trunc);
  if (!output) {
    result.error = "cannot create feature dump: " + temporary_path.string();
    return result;
  }

  FeatureDumpHeader header{};
  header.price_scale = source.price_scale();
  header.qty_scale = source.qty_scale();
  header.symbol = source.symbol();
  header.sample_interval_ns = options.sample_interval_ns;
  if (!write_header(output, header)) {
    result.error = "cannot write feature dump header";
    output.close();
    std::filesystem::remove(temporary_path, filesystem_error);
    return result;
  }

  FeatureEngine engine({options.price_tick, kDefaultFeatureTimeCapacity});
  if (!engine.valid_config()) {
    result.error = "feature engine configuration is invalid";
    output.close();
    std::filesystem::remove(temporary_path, filesystem_error);
    return result;
  }
  BookMap book;
  std::vector<PendingRecord> pending(pending_capacity);
  std::array<std::uint64_t, kFeatureForwardHorizonsNs.size()> label_cursor{};
  std::uint64_t pending_head = 0;
  std::uint64_t pending_next = 0;
  std::uint64_t written_samples = 0;
  bool grid_initialized = false;
  SamplingGrid grid{options.sample_interval_ns};

  while (const auto view = source.next()) {
    const auto event = view->copy();
    ++result.event_count;
    if ((event.flags & kEventFlagSnapshotStart) != 0) book.clear();
    book.apply(event);
    const auto update = engine.on_event(event, book);
    if (update == FeatureUpdateResult::InvalidConfiguration ||
        update == FeatureUpdateResult::InvalidTimestamp ||
        update == FeatureUpdateResult::TimeWindowCapacityExceeded) {
      result.error = update == FeatureUpdateResult::TimeWindowCapacityExceeded
                         ? "feature time-window storage capacity exceeded"
                         : "feature engine rejected the replay event stream";
      break;
    }
    if (update != FeatureUpdateResult::Updated) continue;
    ++result.message_count;

    const auto& snapshot = engine.latest();
    if (!snapshot.has_mid || grid.exhausted) continue;
    if (!grid_initialized) {
      const auto timestamp = static_cast<std::uint64_t>(snapshot.ts_ns);
      const auto first_grid = timestamp - timestamp % grid.interval_ns;
      grid.next_grid_ns = static_cast<std::int64_t>(first_grid);
      grid_initialized = true;
    }
    if (snapshot.ts_ns < grid.next_grid_ns) continue;
    static_cast<void>(grid_advance(snapshot.ts_ns, grid));

    for (std::size_t horizon = 0; horizon < kFeatureForwardHorizonsNs.size(); ++horizon) {
      auto& cursor = label_cursor[horizon];
      while (cursor < pending_next) {
        auto& row = pending[static_cast<std::size_t>(cursor % pending_capacity)];
        if (!target_reached(
                {row.record.ts_ns, snapshot.ts_ns, kFeatureForwardHorizonsNs[horizon]})) {
          break;
        }
        row.record.forward_mid_return_q20_ticks[horizon] = feature_detail::signed_price_return_q20(
            {snapshot.mid2_price, row.mid2_price, static_cast<std::uint64_t>(options.price_tick)});
        row.resolved |= static_cast<std::uint8_t>(1U << horizon);
        ++cursor;
      }
    }

    while (pending_head < pending_next) {
      auto& row = pending[static_cast<std::size_t>(pending_head % pending_capacity)];
      if (row.resolved != kAllLabelsResolved) break;
      if (!write_record(output, row.record)) {
        result.error = "cannot write feature dump record";
        break;
      }
      ++written_samples;
      ++pending_head;
    }
    if (!result.error.empty()) break;
    if (pending_next - pending_head >= pending_capacity) {
      result.error = "pending forward-label queue capacity exceeded";
      break;
    }

    auto& row = pending[static_cast<std::size_t>(pending_next % pending_capacity)];
    row = {};
    row.record.ts_ns = snapshot.ts_ns;
    row.record.features = snapshot.values;
    row.mid2_price = snapshot.mid2_price;
    if (pending_next == pending_head) label_cursor.fill(pending_next);
    ++pending_next;
  }

  if (!result.error.empty()) {
    output.close();
    std::filesystem::remove(temporary_path, filesystem_error);
    return result;
  }

  header.sample_count = written_samples;
  output.seekp(0);
  if (!output || !write_header(output, header)) {
    result.error = "cannot finalize feature dump header";
    output.close();
    std::filesystem::remove(temporary_path, filesystem_error);
    return result;
  }
  output.flush();
  if (!output) {
    result.error = "cannot flush feature dump";
    output.close();
    std::filesystem::remove(temporary_path, filesystem_error);
    return result;
  }
  output.close();
  if (!output) {
    result.error = "cannot close feature dump";
    std::filesystem::remove(temporary_path, filesystem_error);
    return result;
  }

  std::filesystem::rename(temporary_path, options.output_path, filesystem_error);
  if (filesystem_error) {
    result.error = "cannot publish feature dump: " + filesystem_error.message();
    std::filesystem::remove(temporary_path, filesystem_error);
    return result;
  }
  result.sample_count = written_samples;
  result.success = true;
  return result;
}

} // namespace tickbook::feature_dump_tool
