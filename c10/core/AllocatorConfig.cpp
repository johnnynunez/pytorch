#include <c10/core/AllocatorConfig.h>
#include <c10/core/DeviceType.h>
#include <c10/util/CallOnce.h>
#include <c10/util/env.h>

#include <array>
#include <cstdlib>

namespace c10::CachingAllocator {

namespace {
constexpr size_t kRoundUpPowerOfTwoIntervals = 16;
constexpr size_t kMB = 1024 * 1024ul;
constexpr size_t kRoundUpPowerOfTwoStart = 1 * kMB; // 1MB
constexpr size_t kRoundUpPowerOfTwoEnd = 64 * 1024ul * kMB; // 64GB
constexpr size_t kPinnedMaxRegisterThreads = 128;
} // anonymous namespace

AllocatorConfig& AllocatorConfig::instance() {
  static AllocatorConfig instance;
  static c10::once_flag init_once;
#define C10_ALLOCATOR_CONFIG_PARSE_ENV(env, deprecated)                       \
  auto env##_name = c10::utils::get_env(#env);                                \
  if (env##_name.has_value()) {                                               \
    if (deprecated) {                                                         \
      TORCH_WARN_ONCE(#env " is deprecated, use PYTORCH_ALLOC_CONF instead"); \
    }                                                                         \
    instance.parseArgs(env##_name);                                           \
    return;                                                                   \
  }
  c10::call_once(init_once, []() {
    C10_ALLOCATOR_CONFIG_PARSE_ENV(PYTORCH_ALLOC_CONF, false)
    // Keep this for backwards compatibility
    C10_ALLOCATOR_CONFIG_PARSE_ENV(PYTORCH_CUDA_ALLOC_CONF, /*deprecated=*/true)
    C10_ALLOCATOR_CONFIG_PARSE_ENV(PYTORCH_HIP_ALLOC_CONF, /*deprecated=*/true)
  });
  return instance;
}

AllocatorConfig::AllocatorConfig() {
  roundup_power2_divisions_.assign(kRoundUpPowerOfTwoIntervals, 0);
}

size_t AllocatorConfig::roundup_power2_divisions(size_t size) {
  size_t log_size = (63 - llvm::countLeadingZeros(size));

  // Our intervals start at 1MB and end at 64GB
  const size_t interval_start =
      63 - llvm::countLeadingZeros(kRoundUpPowerOfTwoStart);
  const size_t interval_end =
      63 - llvm::countLeadingZeros(kRoundUpPowerOfTwoEnd);
  TORCH_CHECK(
      interval_end - interval_start == kRoundUpPowerOfTwoIntervals,
      "kRoundUpPowerOfTwoIntervals mismatch");

  auto index = (log_size > interval_start) ? (log_size - interval_start) : 0ul;
  index = std::min(index, kRoundUpPowerOfTwoIntervals - 1);
  return instance().roundup_power2_divisions_[index];
}

size_t AllocatorConfig::pinned_max_register_threads() {
  // Based on the benchmark results, we see better allocation performance
  // with 8 threads. However on future systems, we may need more threads
  // and limiting this to 128 threads.
  return kPinnedMaxRegisterThreads;
}

void AllocatorConfig::lexArgs(
    const std::string& env,
    std::vector<std::string>& config) {
  std::vector<char> buf;

  for (char ch : env) {
    if (ch == ',' || ch == ':' || ch == '[' || ch == ']') {
      if (!buf.empty()) {
        config.emplace_back(buf.begin(), buf.end());
        buf.clear();
      }
      config.emplace_back(1, ch);
    } else if (ch != ' ') {
      buf.emplace_back(ch);
    }
  }
  if (!buf.empty()) {
    config.emplace_back(buf.begin(), buf.end());
  }
}

void AllocatorConfig::consumeToken(
    const std::vector<std::string>& config,
    size_t i,
    const char c) {
  TORCH_CHECK(
      i < config.size() && config[i] == std::string(1, c),
      "Error parsing CachingAllocator::AllocatorConfig settings, expected ",
      c);
}

size_t AllocatorConfig::parseMaxSplitSize(
    const std::vector<std::string>& config,
    size_t i) {
  consumeToken(config, ++i, ':');
  constexpr size_t min_allowed_split_size_mb = kLargeBuffer / kMB;
  constexpr size_t max_allowed_split_size_mb =
      std::numeric_limits<size_t>::max() / kMB;

  if (++i < config.size()) {
    size_t val_env = stoi(config[i]);
    TORCH_CHECK(
        val_env >= min_allowed_split_size_mb,
        "CachingAllocator option max_split_size_mb too small, must be >= ",
        min_allowed_split_size_mb);
    val_env = std::min(val_env, max_allowed_split_size_mb);
    max_split_size_ = val_env * kMB;
  } else {
    TORCH_CHECK(false, "Error, expecting max_split_size_mb value");
  }
  return i;
}

size_t AllocatorConfig::parseMaxNonSplitRoundingSize(
    const std::vector<std::string>& config,
    size_t i) {
  consumeToken(config, ++i, ':');
  constexpr size_t min_allowed_split_size_mb = kLargeBuffer / kMB;
  constexpr size_t max_allowed_split_size_mb =
      std::numeric_limits<size_t>::max() / kMB;

  if (++i < config.size()) {
    size_t val_env = stoi(config[i]);
    TORCH_CHECK(
        val_env >= min_allowed_split_size_mb,
        "CachingAllocator option max_non_split_rounding_mb too small, must be >= ",
        min_allowed_split_size_mb);
    val_env = std::min(val_env, max_allowed_split_size_mb);
    max_non_split_rounding_size_ = val_env * kMB;
  } else {
    TORCH_CHECK(false, "Error, expecting max_non_split_rounding_mb value");
  }
  return i;
}

size_t AllocatorConfig::parseGarbageCollectionThreshold(
    const std::vector<std::string>& config,
    size_t i) {
  consumeToken(config, ++i, ':');

  if (++i < config.size()) {
    double val_env = stod(config[i]);
    TORCH_CHECK(
        val_env > 0 && val_env < 1.0,
        "garbage_collect_threshold is invalid, set it in (0.0, 1.0)");
    garbage_collection_threshold_ = val_env;
  } else {
    TORCH_CHECK(false, "Error, expecting garbage_collection_threshold value");
  }
  return i;
}

size_t AllocatorConfig::parseRoundUpPower2Divisions(
    const std::vector<std::string>& config,
    size_t i) {
  consumeToken(config, ++i, ':');
  bool first_value = true;

  if (++i < config.size()) {
    if (std::string_view(config[i]) == "[") {
      size_t last_index = 0;
      // NOLINTNEXTLINE(bugprone-inc-dec-in-conditions)
      while (++i < config.size() && std::string_view(config[i]) != "]") {
        const std::string& val1 = config[i];
        size_t val2 = 0;

        consumeToken(config, ++i, ':');
        if (++i < config.size()) {
          val2 = stoi(config[i]);
        } else {
          TORCH_CHECK(false, "Error parsing roundup_power2_divisions value");
        }
        TORCH_CHECK(
            val2 == 0 || llvm::isPowerOf2_64(val2),
            "For roundups, the divisons has to be power of 2 or 0 to disable roundup ");

        if (std::string_view(val1) == ">") {
          std::fill(
              std::next(
                  roundup_power2_divisions_.begin(),
                  static_cast<std::vector<size_t>::difference_type>(
                      last_index + 1)),
              roundup_power2_divisions_.end(),
              val2);
        } else {
          size_t val1_long = stoul(val1);
          TORCH_CHECK(
              llvm::isPowerOf2_64(val1_long),
              "For roundups, the intervals have to be power of 2 ");

          size_t index = 63 - llvm::countLeadingZeros(val1_long);
          index = std::clamp(
              index, size_t{0}, roundup_power2_divisions_.size() - 1);

          if (first_value) {
            std::fill(
                roundup_power2_divisions_.begin(),
                std::next(
                    roundup_power2_divisions_.begin(),
                    static_cast<std::vector<size_t>::difference_type>(index)),
                val2);
            first_value = false;
          }
          roundup_power2_divisions_[index] = val2;
          last_index = index;
        }

        if (std::string_view(config[i + 1]) != "]") {
          consumeToken(config, ++i, ',');
        }
      }
    } else { // Keep this for backwards compatibility
      size_t val1 = stoi(config[i]);
      TORCH_CHECK(
          llvm::isPowerOf2_64(val1),
          "For roundups, the divisons has to be power of 2 ");
      std::fill(
          roundup_power2_divisions_.begin(),
          roundup_power2_divisions_.end(),
          val1);
    }
  } else {
    TORCH_CHECK(false, "Error, expecting roundup_power2_divisions value");
  }
  return i;
}

size_t AllocatorConfig::parseDeviceAllocatorBackend(
    const std::vector<std::string>& config,
    size_t i) {
  consumeToken(config, ++i, ':');

  if (++i < config.size()) {
    TORCH_CHECK(
        (config[i] == "native" ||
         config[i] == "async"
         // Keep this for backwards compatibility
         || config[i] == "cudaMallocAsync" || config[i] == "hipMallocAsync"),
        "Unknown allocator backend, options are native, async, cudaMallocAsync or hipMallocAsync");
    if (is_allocator_loaded_) {
      bool aync_allocator_at_runtime = (config[i] != "native");
      TORCH_CHECK(
          aync_allocator_at_runtime == use_async_allocator_,
          "Allocator async backend parsed at runtime != allocator async backend parsed at load time, ",
          aync_allocator_at_runtime,
          " != ",
          use_async_allocator_);
    }
    use_async_allocator_ = (config[i] != "native");
  } else {
    TORCH_CHECK(false, "Error parsing allocator backend value");
  }
  return i;
}

size_t AllocatorConfig::parseExpandableSegments(
    const std::vector<std::string>& config,
    size_t i) {
  consumeToken(config, ++i, ':');
  if (++i < config.size()) {
    TORCH_CHECK(
        (config[i] == "True" || config[i] == "False"),
        "Expected a single True/False argument for expandable_segments");
    use_expandable_segments_ = (config[i] == "True");
  } else {
    TORCH_CHECK(false, "Error, expecting expandable_segments value");
  }
  return i;
}

size_t AllocatorConfig::parseReleaseLockOnDeviceMalloc(
    const std::vector<std::string>& config,
    size_t i) {
  consumeToken(config, ++i, ':');
  if (++i < config.size()) {
    TORCH_CHECK(
        (config[i] == "True" || config[i] == "False"),
        "Expected a single True/False argument for release_lock_on_device_malloc, release_lock_on_cudamalloc or release_lock_on_hipmalloc");
    use_release_lock_on_device_malloc_ = (config[i] == "True");
  } else {
    TORCH_CHECK(
        false,
        "Error, expecting release_lock_on_device_malloc, release_lock_on_cudamalloc or release_lock_on_hipmalloc value");
  }
  return i;
}

size_t AllocatorConfig::parsePinnedUseDeviceHostRegister(
    const std::vector<std::string>& config,
    size_t i) {
  consumeToken(config, ++i, ':');

  if (++i < config.size()) {
    TORCH_CHECK(
        (config[i] == "True" || config[i] == "False"),
        "Expected a single True/False argument for pinned_used_device_host_register, pinned_used_cuda_host_register or pinned_used_hip_host_register");
    pinned_use_device_host_register_ = (config[i] == "True");
  } else {
    TORCH_CHECK(
        false,
        "Error, expecting pinned_used_device_host_register, pinned_used_cuda_host_register or pinned_used_hip_host_register value");
  }
  return i;
}

size_t AllocatorConfig::parsePinnedNumRegisterThreads(
    const std::vector<std::string>& config,
    size_t i) {
  consumeToken(config, ++i, ':');
  if (++i < config.size()) {
    size_t val_env = stoi(config[i]);
    TORCH_CHECK(
        llvm::isPowerOf2_64(val_env),
        "Number of register threads has to be power of 2");
    auto max_threads = pinned_max_register_threads();
    TORCH_CHECK(
        val_env <= max_threads,
        "Number of register threads should be less than or equal to ",
        max_threads);
    pinned_num_register_threads_ = val_env;
  } else {
    TORCH_CHECK(false, "Error, expecting pinned_num_register_threads value");
  }
  return i;
}

size_t AllocatorConfig::parsePinnedUseBackgroundThreads(
    const std::vector<std::string>& config,
    size_t i) {
  consumeToken(config, ++i, ':');
  if (++i < config.size()) {
    TORCH_CHECK(
        (config[i] == "True" || config[i] == "False"),
        "Expected a single True/False argument for pinned_use_background_threads");
    pinned_use_background_threads_ = (config[i] == "True");
  } else {
    TORCH_CHECK(false, "Error, expecting pinned_use_background_threads value");
  }
  return i;
}

void AllocatorConfig::parseArgs(const std::optional<std::string>& env) {
  // The following option will be reset to its default value if not explicitly
  // set each time.
  max_split_size_ = std::numeric_limits<size_t>::max();
  roundup_power2_divisions_.assign(kRoundUpPowerOfTwoIntervals, 0);
  garbage_collection_threshold_ = 0;
  use_async_allocator_ = false;

  bool used_native_specific_option = false;

  if (!env.has_value()) {
    return;
  }
  {
    std::lock_guard<std::mutex> lock(last_allocator_settings_mutex_);
    last_allocator_settings_ = env.value();
  }

  std::vector<std::string> config;
  lexArgs(env.value(), config);

  for (size_t i = 0; i < config.size(); i++) {
    std::string_view config_item_view(config[i]);
    if (config_item_view == "max_split_size_mb") {
      i = parseMaxSplitSize(config, i);
      used_native_specific_option = true;
    } else if (config_item_view == "max_non_split_rounding_mb") {
      i = parseMaxNonSplitRoundingSize(config, i);
      used_native_specific_option = true;
    } else if (config_item_view == "garbage_collection_threshold") {
      i = parseGarbageCollectionThreshold(config, i);
      used_native_specific_option = true;
    } else if (config_item_view == "roundup_power2_divisions") {
      i = parseRoundUpPower2Divisions(config, i);
      used_native_specific_option = true;
    } else if (config_item_view == "backend") {
      i = parseDeviceAllocatorBackend(config, i);
    } else if (config_item_view == "expandable_segments") {
      i = parseExpandableSegments(config, i);
      used_native_specific_option = true;
    } else if (
        config_item_view == "release_lock_on_device_malloc" ||
        // Keep this for backwards compatibility
        config_item_view == "release_lock_on_cudamalloc" ||
        config_item_view == "release_lock_on_hipmalloc") {
      i = parseReleaseLockOnDeviceMalloc(config, i);
      used_native_specific_option = true;
    } else if (
        config_item_view == "pinned_use_device_host_register" ||
        // Keep this for backwards compatibility
        config_item_view == "pinned_use_cuda_host_register" ||
        config_item_view == "pinned_use_hip_host_register") {
      i = parsePinnedUseDeviceHostRegister(config, i);
      used_native_specific_option = true;
    } else if (config_item_view == "pinned_num_register_threads") {
      i = parsePinnedNumRegisterThreads(config, i);
      used_native_specific_option = true;
    } else if (config_item_view == "pinned_use_background_threads") {
      i = parsePinnedUseBackgroundThreads(config, i);
      used_native_specific_option = true;
    } else {
      TORCH_CHECK(
          false, "Unrecognized CachingAllocator option: ", config_item_view);
    }

    if (i + 1 < config.size()) {
      consumeToken(config, ++i, ',');
    }

    if (use_async_allocator_ && used_native_specific_option) {
      TORCH_WARN(
          "backend: async ignores ",
          "max_split_size_mb, ",
          "max_non_split_rounding_mb, ",
          "garbage_collection_threshold, ",
          "roundup_power2_divisions, ",
          "expandable_segments, ",
          "release_lock_on_device_malloc, ",
          "pinned_use_host_register, ",
          "pinned_num_register_threads, ",
          "and pinned_use_background_threads.");
    }
  }
}

void setAllocatorSettings(const std::string& env) {
  AllocatorConfig::instance().parseArgs(env.c_str());
}

} // namespace c10::CachingAllocator
