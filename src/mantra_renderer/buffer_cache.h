#pragma once
// Guest vertex and index data converted for the host (endian swap, index
// conversion), kept across draws until the CPU writes the guest pages again.
//
// Invalidation: the guest physical pages behind an entry are write-protected
// through Memory::EnablePhysicalMemoryAccessCallbacks. The first CPU write to
// such a page faults; our invalidation callback stamps the page with a new
// epoch, and the memory system unprotects it. An entry stays valid while no
// page it covers carries an epoch newer than its upload. The epoch is read
// before protecting and the data copied after, so a write racing with the
// upload always invalidates the entry.
//
// Storage: entry contents are immutable once written, bump-allocated in
// host-visible blocks. A re-upload takes new space, since in-flight
// submissions may still read the old copy. When the blocks run out the whole
// cache is dropped, and the blocks are reused once the GPU is done with them.
#include <atomic>
#include <cstdint>
#include <cstring>
#include <memory>
#include <unordered_map>
#include <vector>

#include <rex/ui/vulkan/api.h>

namespace rex::memory {
class Memory;
}
namespace rex::ui::vulkan {
class VulkanDevice;
}

namespace mantra {

class BufferCache {
 public:
  enum class Kind : uint32_t { kVertex = 1, kIndex = 2 };

  // What an entry holds. Compared and hashed bytewise: zero-initialize.
  struct Key {
    uint32_t start;   // guest physical address of the source data
    uint32_t length;  // bytes of source data (the protected range)
    Kind kind;
    uint32_t params[5];  // kind-specific conversion parameters

    bool operator==(const Key& other) const { return std::memcmp(this, &other, sizeof(*this)) == 0; }
  };

  struct Data {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceSize offset = 0;  // of the entry's first byte in `buffer`
    uint32_t size = 0;
    uint32_t meta[4] = {};  // kind-specific results (index range and so on)
    const uint8_t* contents = nullptr;  // the entry's bytes (host-visible)
  };

  struct Stats {
    uint64_t hits = 0;
    uint64_t uploads = 0;        // entries (re)written
    uint64_t reuploads = 0;      // of those, replacing an invalidated entry
    uint64_t upload_bytes = 0;
    uint64_t not_cacheable = 0;  // too large or out of space
    uint64_t volatile_skips = 0; // in regions the CPU keeps rewriting
    uint64_t resets = 0;         // whole-cache drops
  };

  bool Initialize(const rex::ui::vulkan::VulkanDevice* device, rex::memory::Memory* memory);
  void Shutdown();

  // Finds a valid entry. Bumps stats.hits.
  bool Lookup(const Key& key, Data& out);

  // Starts (re)writing the entry for `key`: protects the guest range and
  // returns `size` writable bytes, or nullptr if the data can't be cached
  // (the caller streams it instead). `submission` is the submission that will
  // read the data. Finish with Commit, passing the same key; nothing else may
  // be reserved in between.
  uint8_t* Reserve(const Key& key, uint32_t size, uint64_t submission, Data& out);
  void Commit(const Key& key, const Data& data);

  // Write watching for other caches (textures): protects the range and
  // returns the epoch to compare against; read the data only after this call.
  uint32_t WatchRange(uint32_t start, uint32_t length);
  // Whether no CPU write hit the range since WatchRange returned `epoch`.
  // `checked_epoch` (initialize it to `epoch`) skips the page scan while
  // nothing at all was written.
  bool RangeUnchanged(uint32_t start, uint32_t length, uint32_t epoch, uint32_t& checked_epoch);
  // Host pointer of guest physical memory.
  const uint8_t* Translate(uint32_t physical_address) const;
  // The current epoch (a later WatchRange result is at least this).
  uint32_t epoch() const { return epoch_.load(std::memory_order_acquire); }

  // Reclaims blocks whose readers have completed.
  void SubmissionCompleted(uint64_t completed);
  // Once per guest frame: ages the per-region write counts.
  void FrameEnded();

  const Stats& stats() const { return stats_; }
  uint64_t invalidations() const { return invalidations_.load(std::memory_order_relaxed); }
  size_t size() const { return entries_.size(); }
  uint32_t blocks_in_use() const { return uint32_t(blocks_.size() - free_blocks_.size()); }

 private:
  static constexpr uint32_t kPageShift = 12;
  static constexpr uint32_t kPhysicalBytes = 512u << 20;
  static constexpr uint32_t kPageCount = kPhysicalBytes >> kPageShift;
  static constexpr VkDeviceSize kBlockBytes = 32u << 20;
  static constexpr uint32_t kMaxBlocks = 8;
  static constexpr VkDeviceSize kAlignment = 16;
  // Regions where the CPU rewrote cached data this often (recently) are
  // streamed instead: protecting them costs a fault per page and the copies
  // are never reused. Counts halve every kDecayFrames frames.
  static constexpr uint32_t kRegionShift = 16;
  static constexpr uint32_t kRegionCount = kPhysicalBytes >> kRegionShift;
  static constexpr uint8_t kVolatileWrites = 4;
  static constexpr uint32_t kDecayFrames = 120;

  struct KeyHash {
    size_t operator()(const Key& key) const;
  };
  struct Entry {
    Data data;
    uint32_t upload_epoch = 0;   // epoch read before the range was protected
    uint32_t checked_epoch = 0;  // global epoch at the last successful check
    bool valid = false;
  };
  struct Block {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    uint8_t* mapping = nullptr;
    uint32_t memory_type = 0;
    VkDeviceSize memory_size = 0;
    uint64_t last_submission = 0;  // last submission reading from it (once retired)
  };

  static std::pair<uint32_t, uint32_t> InvalidationCallbackThunk(void* context,
                                                                 uint32_t physical_start,
                                                                 uint32_t length, bool exact);
  std::pair<uint32_t, uint32_t> InvalidationCallback(uint32_t physical_start, uint32_t length);
  bool EntryValid(Entry& entry, const Key& key);
  // Returns the block for an allocation of `size`, starting a new one (or
  // dropping the cache) when the current one is full.
  Block* AllocateSpace(VkDeviceSize size, uint64_t submission, VkDeviceSize& offset);
  bool CreateBlock(Block& block);
  void Reset(uint64_t submission);

  const rex::ui::vulkan::VulkanDevice* device_ = nullptr;
  rex::memory::Memory* memory_ = nullptr;
  void* callback_handle_ = nullptr;

  // Written by guest threads from the access-violation handler.
  std::unique_ptr<std::atomic<uint32_t>[]> page_epochs_;
  std::atomic<uint32_t> epoch_{1};
  std::atomic<uint64_t> invalidations_{0};
  std::unique_ptr<std::atomic<uint8_t>[]> region_writes_;
  uint32_t frames_since_decay_ = 0;

  std::unordered_map<Key, Entry, KeyHash> entries_;
  std::vector<Block> blocks_;
  std::vector<uint32_t> free_blocks_;      // ready to be filled
  std::vector<uint32_t> retired_blocks_;   // dropped by a reset, maybe still read
  int32_t current_block_ = -1;
  VkDeviceSize current_offset_ = 0;
  Entry* reserving_ = nullptr;
  uint32_t reserve_epoch_ = 0;
  Stats stats_;
};

}  // namespace mantra
