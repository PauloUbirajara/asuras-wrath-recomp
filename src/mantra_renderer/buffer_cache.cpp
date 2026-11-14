#include "buffer_cache.h"

#include <cstdlib>
#include "mantra_log.h"

#include <algorithm>

#include <xxhash.h>

#include <rex/system/xmemory.h>
#include <rex/ui/vulkan/device.h>
#include <rex/ui/vulkan/util.h>

namespace mantra {

namespace util = rex::ui::vulkan::util;

size_t BufferCache::KeyHash::operator()(const Key& key) const {
  return size_t(XXH3_64bits(&key, sizeof(key)));
}

bool BufferCache::Initialize(const rex::ui::vulkan::VulkanDevice* device,
                             rex::memory::Memory* memory) {
  device_ = device;
  memory_ = memory;
  page_epochs_ = std::make_unique<std::atomic<uint32_t>[]>(kPageCount);
  for (uint32_t i = 0; i < kPageCount; ++i) {
    page_epochs_[i].store(0, std::memory_order_relaxed);
  }
  region_writes_ = std::make_unique<std::atomic<uint8_t>[]>(kRegionCount);
  for (uint32_t i = 0; i < kRegionCount; ++i) {
    region_writes_[i].store(0, std::memory_order_relaxed);
  }
  callback_handle_ = memory_->RegisterPhysicalMemoryInvalidationCallback(
      &BufferCache::InvalidationCallbackThunk, this);
  return true;
}

void BufferCache::Shutdown() {
  if (callback_handle_) {
    // Pages stay protected; with no callback left the memory system just
    // unprotects them on the next write.
    memory_->UnregisterPhysicalMemoryInvalidationCallback(callback_handle_);
    callback_handle_ = nullptr;
  }
  entries_.clear();
  if (device_) {
    const auto& dfn = device_->functions();
    const VkDevice device = device_->device();
    for (Block& block : blocks_) {
      if (block.mapping) dfn.vkUnmapMemory(device, block.memory);
      if (block.buffer) dfn.vkDestroyBuffer(device, block.buffer, nullptr);
      if (block.memory) dfn.vkFreeMemory(device, block.memory, nullptr);
    }
  }
  blocks_.clear();
  free_blocks_.clear();
  retired_blocks_.clear();
  current_block_ = -1;
  page_epochs_.reset();
  region_writes_.reset();
  device_ = nullptr;
}

// --- Invalidation -----------------------------------------------------------

std::pair<uint32_t, uint32_t> BufferCache::InvalidationCallbackThunk(void* context,
                                                                     uint32_t physical_start,
                                                                     uint32_t length, bool) {
  return static_cast<BufferCache*>(context)->InvalidationCallback(physical_start, length);
}

// Called on a guest thread, inside the global critical region, before the
// faulting write is retried. Stamps exactly the pages it reports back, so no
// page is unprotected without being marked.
std::pair<uint32_t, uint32_t> BufferCache::InvalidationCallback(uint32_t physical_start,
                                                                uint32_t length) {
  if (!length || physical_start >= kPhysicalBytes) {
    return {0, UINT32_MAX};
  }
  length = std::min(length, kPhysicalBytes - physical_start);
  const uint32_t page_first = physical_start >> kPageShift;
  const uint32_t page_last = (physical_start + length - 1) >> kPageShift;
  const uint32_t epoch = epoch_.fetch_add(1) + 1;
  for (uint32_t page = page_first; page <= page_last; ++page) {
    page_epochs_[page].store(epoch, std::memory_order_release);
  }
  for (uint32_t region = page_first >> (kRegionShift - kPageShift);
       region <= page_last >> (kRegionShift - kPageShift); ++region) {
    uint8_t count = region_writes_[region].load(std::memory_order_relaxed);
    if (count < 255) {
      region_writes_[region].store(count + 1, std::memory_order_relaxed);
    }
  }
  invalidations_.fetch_add(1, std::memory_order_relaxed);
  return {page_first << kPageShift, (page_last - page_first + 1) << kPageShift};
}

bool BufferCache::EntryValid(Entry& entry, const Key& key) {
  if (!entry.valid) {
    return false;
  }
  if (!RangeUnchanged(key.start, key.length, entry.upload_epoch, entry.checked_epoch)) {
    entry.valid = false;
    return false;
  }
  return true;
}

bool BufferCache::RangeUnchanged(uint32_t start, uint32_t length, uint32_t epoch,
                                 uint32_t& checked_epoch) {
  const uint32_t now = epoch_.load(std::memory_order_acquire);
  if (checked_epoch == now) {
    return true;
  }
  if (start >= kPhysicalBytes) {
    return true;
  }
  length = std::min(std::max(length, 1u), kPhysicalBytes - start);
  const uint32_t page_first = start >> kPageShift;
  const uint32_t page_last = (start + length - 1) >> kPageShift;
  for (uint32_t page = page_first; page <= page_last; ++page) {
    if (page_epochs_[page].load(std::memory_order_acquire) > epoch) {
      return false;
    }
  }
  checked_epoch = now;
  return true;
}

const uint8_t* BufferCache::Translate(uint32_t physical_address) const {
  return memory_->TranslatePhysical<const uint8_t*>(physical_address);
}

uint32_t BufferCache::WatchRange(uint32_t start, uint32_t length) {
  const uint32_t epoch = epoch_.load(std::memory_order_acquire);
  if (length && start < kPhysicalBytes) {
    memory_->EnablePhysicalMemoryAccessCallbacks(
        start, std::min(length, kPhysicalBytes - start), true, false);
  }
  return epoch;
}

// --- Entries ----------------------------------------------------------------

namespace {
// MANTRA_NO_BUFFER_CACHE: copy all geometry into the ring each draw (debug).
bool CacheDisabled() {
  static const bool disabled = std::getenv("MANTRA_NO_BUFFER_CACHE") != nullptr;
  return disabled;
}
}  // namespace

bool BufferCache::Lookup(const Key& key, Data& out) {
  if (CacheDisabled()) {
    return false;
  }
  auto it = entries_.find(key);
  if (it == entries_.end() || !EntryValid(it->second, key)) {
    return false;
  }
  out = it->second.data;
  ++stats_.hits;
  return true;
}

uint8_t* BufferCache::Reserve(const Key& key, uint32_t size, uint64_t submission, Data& out) {
  reserving_ = nullptr;
  if (CacheDisabled()) {
    return nullptr;
  }
  if (!size || size > kBlockBytes || !key.length || key.start >= kPhysicalBytes ||
      key.length > kPhysicalBytes - key.start) {
    ++stats_.not_cacheable;
    return nullptr;
  }
  for (uint32_t region = key.start >> kRegionShift;
       region <= (key.start + key.length - 1) >> kRegionShift; ++region) {
    if (region_writes_[region].load(std::memory_order_relaxed) >= kVolatileWrites) {
      ++stats_.volatile_skips;
      return nullptr;
    }
  }
  // The order matters: epoch first, then protection, then (by the caller) the
  // copy. A write before the protection is in the copy; one after it faults
  // and stamps a newer epoch.
  const uint32_t epoch = epoch_.load(std::memory_order_acquire);
  VkDeviceSize offset;
  Block* block = AllocateSpace(size, submission, offset);
  if (!block) {
    ++stats_.not_cacheable;
    return nullptr;
  }
  memory_->EnablePhysicalMemoryAccessCallbacks(key.start, key.length, true, false);

  auto [it, inserted] = entries_.try_emplace(key);
  if (!inserted) {
    ++stats_.reuploads;
  }
  Entry& entry = it->second;
  entry.valid = false;
  entry.data = {};
  entry.data.buffer = block->buffer;
  entry.data.offset = offset;
  entry.data.size = size;
  entry.data.contents = block->mapping + offset;
  reserving_ = &entry;
  reserve_epoch_ = epoch;
  out = entry.data;
  return block->mapping + offset;
}

void BufferCache::Commit(const Key& key, const Data& data) {
  Entry* entry = reserving_;
  reserving_ = nullptr;
  if (!entry) {
    return;
  }
  for (Block& block : blocks_) {
    if (block.buffer == data.buffer) {
      util::FlushMappedMemoryRange(device_, block.memory, block.memory_type, data.offset,
                                   block.memory_size, data.size);
      break;
    }
  }
  entry->data = data;
  entry->upload_epoch = reserve_epoch_;
  entry->checked_epoch = reserve_epoch_;
  entry->valid = true;
  ++stats_.uploads;
  stats_.upload_bytes += data.size;
  (void)key;
}

// --- Storage ----------------------------------------------------------------

BufferCache::Block* BufferCache::AllocateSpace(VkDeviceSize size, uint64_t submission,
                                               VkDeviceSize& offset) {
  size = (size + kAlignment - 1) & ~(kAlignment - 1);
  if (current_block_ >= 0 && current_offset_ + size <= kBlockBytes) {
    offset = current_offset_;
    current_offset_ += size;
    return &blocks_[current_block_];
  }
  // The current block is full: take a free one, create one, or drop
  // everything and wait for the retired blocks.
  if (free_blocks_.empty() && blocks_.size() < kMaxBlocks) {
    Block block;
    if (CreateBlock(block)) {
      blocks_.push_back(block);
      free_blocks_.push_back(uint32_t(blocks_.size() - 1));
    }
  }
  if (free_blocks_.empty()) {
    Reset(submission);
    if (free_blocks_.empty()) {
      return nullptr;  // everything still in use by the GPU
    }
  }
  current_block_ = int32_t(free_blocks_.back());
  free_blocks_.pop_back();
  current_offset_ = size;
  offset = 0;
  return &blocks_[current_block_];
}

bool BufferCache::CreateBlock(Block& block) {
  if (!util::CreateDedicatedAllocationBuffer(
          device_, kBlockBytes, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
          util::MemoryPurpose::kUpload, block.buffer, block.memory, &block.memory_type,
          &block.memory_size)) {
    MANTRA_WARN_ONCE("buffer cache: failed to allocate a {} MB block", kBlockBytes >> 20);
    return false;
  }
  void* mapping = nullptr;
  const auto& dfn = device_->functions();
  if (dfn.vkMapMemory(device_->device(), block.memory, 0, VK_WHOLE_SIZE, 0, &mapping) !=
      VK_SUCCESS) {
    dfn.vkDestroyBuffer(device_->device(), block.buffer, nullptr);
    dfn.vkFreeMemory(device_->device(), block.memory, nullptr);
    block = {};
    return false;
  }
  block.mapping = static_cast<uint8_t*>(mapping);
  return true;
}

void BufferCache::Reset(uint64_t submission) {
  // Every reader of the current contents is at most `submission`.
  entries_.clear();
  reserving_ = nullptr;
  for (uint32_t i = 0; i < blocks_.size(); ++i) {
    if (std::find(free_blocks_.begin(), free_blocks_.end(), i) != free_blocks_.end() ||
        std::find(retired_blocks_.begin(), retired_blocks_.end(), i) != retired_blocks_.end()) {
      continue;
    }
    blocks_[i].last_submission = submission;
    retired_blocks_.push_back(i);
  }
  current_block_ = -1;
  current_offset_ = 0;
  ++stats_.resets;
  MANTRA_DEBUG("buffer cache: dropped all entries ({} blocks retired)", retired_blocks_.size());
}

void BufferCache::FrameEnded() {
  if (++frames_since_decay_ < kDecayFrames) {
    return;
  }
  frames_since_decay_ = 0;
  // Racy with the callback on purpose: a lost increment only delays the
  // detection.
  for (uint32_t i = 0; i < kRegionCount; ++i) {
    const uint8_t count = region_writes_[i].load(std::memory_order_relaxed);
    if (count) {
      region_writes_[i].store(count >> 1, std::memory_order_relaxed);
    }
  }
}

void BufferCache::SubmissionCompleted(uint64_t completed) {
  for (size_t i = 0; i < retired_blocks_.size();) {
    if (blocks_[retired_blocks_[i]].last_submission <= completed) {
      free_blocks_.push_back(retired_blocks_[i]);
      retired_blocks_[i] = retired_blocks_.back();
      retired_blocks_.pop_back();
    } else {
      ++i;
    }
  }
}

}  // namespace mantra
