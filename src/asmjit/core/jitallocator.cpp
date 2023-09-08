// This file is part of AsmJit project <https://asmjit.com>
//
// See asmjit.h or LICENSE.md for license and copyright information
// SPDX-License-Identifier: Zlib

#include "../core/api-build_p.h"
#ifndef ASMJIT_NO_JIT

#include "../core/archtraits.h"
#include "../core/jitallocator.h"
#include "../core/osutils_p.h"
#include "../core/support.h"
#include "../core/virtmem.h"
#include "../core/zone.h"
#include "../core/zonelist.h"
#include "../core/zonetree.h"

ASMJIT_BEGIN_NAMESPACE

// JitAllocator - Constants
// ========================

//! Number of pools to use when `JitAllocatorOptions::kUseMultiplePools` is set.
//!
//! Each pool increases granularity twice to make memory management more
//! efficient. Ideal number of pools appears to be 3 to 4 as it distributes
//! small and large functions properly.
static constexpr uint32_t kJitAllocatorMultiPoolCount = 3;

//! Minimum granularity (and the default granularity for pool #0).
static constexpr uint32_t kJitAllocatorBaseGranularity = 64;

//! Maximum block size (32MB).
static constexpr uint32_t kJitAllocatorMaxBlockSize = 1024 * 1024 * 32;

// JitAllocator - Fill Pattern
// ===========================

static inline uint32_t JitAllocator_defaultFillPattern() noexcept {
  // X86 and X86_64 - 4x 'int3' instruction.
  if (ASMJIT_ARCH_X86)
    return 0xCCCCCCCCu;

  // Unknown...
  return 0u;
}

// JitAllocator - BitVectorRangeIterator
// =====================================

template<typename T, uint32_t B>
class BitVectorRangeIterator {
public:
  const T* _ptr;
  size_t _idx;
  size_t _end;
  T _bitWord;

  enum : uint32_t { kBitWordSize = Support::bitSizeOf<T>() };
  enum : T { kXorMask = B == 0 ? Support::allOnes<T>() : T(0) };

  ASMJIT_FORCE_INLINE BitVectorRangeIterator(const T* data, size_t numBitWords) noexcept {
    init(data, numBitWords);
  }

  ASMJIT_FORCE_INLINE BitVectorRangeIterator(const T* data, size_t numBitWords, size_t start, size_t end) noexcept {
    init(data, numBitWords, start, end);
  }

  ASMJIT_FORCE_INLINE void init(const T* data, size_t numBitWords) noexcept {
    init(data, numBitWords, 0, numBitWords * kBitWordSize);
  }

  ASMJIT_FORCE_INLINE void init(const T* data, size_t numBitWords, size_t start, size_t end) noexcept {
    ASMJIT_ASSERT(numBitWords >= (end + kBitWordSize - 1) / kBitWordSize);
    DebugUtils::unused(numBitWords);

    size_t idx = Support::alignDown(start, kBitWordSize);
    const T* ptr = data + (idx / kBitWordSize);

    T bitWord = 0;
    if (idx < end)
      bitWord = (*ptr ^ kXorMask) & (Support::allOnes<T>() << (start % kBitWordSize));

    _ptr = ptr;
    _idx = idx;
    _end = end;
    _bitWord = bitWord;
  }

  ASMJIT_FORCE_INLINE bool nextRange(size_t* rangeStart, size_t* rangeEnd, size_t rangeHint = std::numeric_limits<size_t>::max()) noexcept {
    // Skip all empty BitWords.
    while (_bitWord == 0) {
      _idx += kBitWordSize;
      if (_idx >= _end)
        return false;
      _bitWord = (*++_ptr) ^ kXorMask;
    }

    size_t i = Support::ctz(_bitWord);

    *rangeStart = _idx + i;
    _bitWord = ~(_bitWord ^ ~(Support::allOnes<T>() << i));

    if (_bitWord == 0) {
      *rangeEnd = Support::min(_idx + kBitWordSize, _end);
      while (*rangeEnd - *rangeStart < rangeHint) {
        _idx += kBitWordSize;
        if (_idx >= _end)
          break;

        _bitWord = (*++_ptr) ^ kXorMask;
        if (_bitWord != Support::allOnes<T>()) {
          size_t j = Support::ctz(~_bitWord);
          *rangeEnd = Support::min(_idx + j, _end);
          _bitWord = _bitWord ^ ~(Support::allOnes<T>() << j);
          break;
        }

        *rangeEnd = Support::min(_idx + kBitWordSize, _end);
        _bitWord = 0;
        continue;
      }

      return true;
    }
    else {
      size_t j = Support::ctz(_bitWord);
      *rangeEnd = Support::min(_idx + j, _end);

      _bitWord = ~(_bitWord ^ ~(Support::allOnes<T>() << j));
      return true;
    }
  }
};

// JitAllocator - Pool
// ===================

class JitAllocatorBlock;

class JitAllocatorPool {
public:
  ASMJIT_NONCOPYABLE(JitAllocatorPool)

  //! Double linked list of blocks.
  ZoneList<JitAllocatorBlock> blocks;
  //! Where to start looking first.
  JitAllocatorBlock* cursor = nullptr;

  //! Count of blocks.
  uint32_t blockCount = 0;
  //! Allocation granularity.
  uint16_t granularity = 0;
  //! Log2(granularity).
  uint8_t granularityLog2 = 0;
  //! Count of empty blocks (either 0 or 1 as we won't keep more blocks empty).
  uint8_t emptyBlockCount = 0;

  //! Number of bits reserved across all blocks.
  size_t totalAreaSize = 0;
  //! Number of bits used across all blocks.
  size_t totalAreaUsed = 0;
  //! Overhead of all blocks (in bytes).
  size_t totalOverheadBytes = 0;

  inline JitAllocatorPool(uint32_t granularity) noexcept
    : blocks(),
      granularity(uint16_t(granularity)),
      granularityLog2(uint8_t(Support::ctz(granularity))) {}

  inline void reset() noexcept {
    blocks.reset();
    cursor = nullptr;
    blockCount = 0;
    totalAreaSize = 0;
    totalAreaUsed = 0;
    totalOverheadBytes = 0;
  }

  inline size_t byteSizeFromAreaSize(uint32_t areaSize) const noexcept { return size_t(areaSize) * granularity; }
  inline uint32_t areaSizeFromByteSize(size_t size) const noexcept { return uint32_t((size + granularity - 1) >> granularityLog2); }

  inline size_t bitWordCountFromAreaSize(uint32_t areaSize) const noexcept {
    using namespace Support;
    return alignUp<size_t>(areaSize, kBitWordSizeInBits) / kBitWordSizeInBits;
  }
};

// JitAllocator - Block
// ====================

class JitAllocatorBlock : public ZoneTreeNodeT<JitAllocatorBlock>,
                          public ZoneListNode<JitAllocatorBlock> {
public:
  ASMJIT_NONCOPYABLE(JitAllocatorBlock)

  enum Flags : uint32_t {
    //! Block has initial padding, see \ref JitAllocatorOptions::kDisableInitialPadding.
    kFlagInitialPadding = 0x00000001u,
    //! Block is empty.
    kFlagEmpty = 0x00000002u,
    //! Block is dirty (largestUnusedArea, searchStart, searchEnd).
    kFlagDirty = 0x00000004u,
    //! Block is dual-mapped.
    kFlagDualMapped = 0x00000008u
  };

  static_assert(kFlagInitialPadding == 1, "JitAllocatorBlock::kFlagInitialPadding must be equal to 1");

  static inline uint32_t initialAreaStartByFlags(uint32_t flags) noexcept { return flags & kFlagInitialPadding; }

  //! Link to the pool that owns this block.
  JitAllocatorPool* _pool {};
  //! Virtual memory mapping - either single mapping (both pointers equal) or
  //! dual mapping, where one pointer is Read+Execute and the second Read+Write.
  VirtMem::DualMapping _mapping {};
  //! Virtual memory size (block size) [bytes].
  size_t _blockSize = 0;

  //! Block flags.
  uint32_t _flags = 0;
  //! Size of the whole block area (bit-vector size).
  uint32_t _areaSize = 0;
  //! Used area (number of bits in bit-vector used).
  uint32_t _areaUsed = 0;
  //! The largest unused continuous area in the bit-vector (or `areaSize` to initiate rescan).
  uint32_t _largestUnusedArea = 0;
  //! Start of a search range (for unused bits).
  uint32_t _searchStart = 0;
  //! End of a search range (for unused bits).
  uint32_t _searchEnd = 0;

  //! Used bit-vector (0 = unused, 1 = used).
  Support::BitWord* _usedBitVector {};
  //! Stop bit-vector (0 = don't care, 1 = stop).
  Support::BitWord* _stopBitVector {};

  inline JitAllocatorBlock(
    JitAllocatorPool* pool,
    VirtMem::DualMapping mapping,
    size_t blockSize,
    uint32_t blockFlags,
    Support::BitWord* usedBitVector,
    Support::BitWord* stopBitVector,
    uint32_t areaSize) noexcept
    : ZoneTreeNodeT(),
      _pool(pool),
      _mapping(mapping),
      _blockSize(blockSize),
      _flags(blockFlags),
      _areaSize(areaSize),
      _areaUsed(0),          // Will be initialized by clearBlock().
      _largestUnusedArea(0), // Will be initialized by clearBlock().
      _searchStart(0),       // Will be initialized by clearBlock().
      _searchEnd(0),         // Will be initialized by clearBlock().
      _usedBitVector(usedBitVector),
      _stopBitVector(stopBitVector) {

    clearBlock();
  }

  inline JitAllocatorPool* pool() const noexcept { return _pool; }

  inline uint8_t* rxPtr() const noexcept { return static_cast<uint8_t*>(_mapping.rx); }
  inline uint8_t* rwPtr() const noexcept { return static_cast<uint8_t*>(_mapping.rw); }

  inline bool hasFlag(uint32_t f) const noexcept { return (_flags & f) != 0; }
  inline void addFlags(uint32_t f) noexcept { _flags |= f; }
  inline void clearFlags(uint32_t f) noexcept { _flags &= ~f; }

  inline bool hasInitialPadding() const noexcept { return hasFlag(kFlagInitialPadding); }
  inline uint32_t initialAreaStart() const noexcept { return initialAreaStartByFlags(_flags); }

  inline bool empty() const noexcept { return hasFlag(kFlagEmpty); }
  inline bool isDirty() const noexcept { return hasFlag(kFlagDirty); }
  inline void makeDirty() noexcept { addFlags(kFlagDirty); }

  inline size_t blockSize() const noexcept { return _blockSize; }

  inline uint32_t areaSize() const noexcept { return _areaSize; }
  inline uint32_t areaUsed() const noexcept { return _areaUsed; }
  inline uint32_t areaAvailable() const noexcept { return _areaSize - _areaUsed; }
  inline uint32_t largestUnusedArea() const noexcept { return _largestUnusedArea; }

  inline void decreaseUsedArea(uint32_t value) noexcept {
    _areaUsed -= value;
    _pool->totalAreaUsed -= value;
  }

  inline void clearBlock() noexcept {
    bool bit = hasInitialPadding();
    size_t numBitWords = _pool->bitWordCountFromAreaSize(_areaSize);

    memset(_usedBitVector, 0, numBitWords * sizeof(Support::BitWord));
    memset(_stopBitVector, 0, numBitWords * sizeof(Support::BitWord));

    Support::bitVectorSetBit(_usedBitVector, 0, bit);
    Support::bitVectorSetBit(_stopBitVector, 0, bit);

    uint32_t start = initialAreaStartByFlags(_flags);
    _areaUsed = start;
    _largestUnusedArea = _areaSize - start;
    _searchStart = start;
    _searchEnd = _areaSize;

    addFlags(JitAllocatorBlock::kFlagEmpty);
    clearFlags(JitAllocatorBlock::kFlagDirty);
  }

  inline void markAllocatedArea(uint32_t allocatedAreaStart, uint32_t allocatedAreaEnd) noexcept {
    uint32_t allocatedAreaSize = allocatedAreaEnd - allocatedAreaStart;

    // Mark the newly allocated space as occupied and also the sentinel.
    Support::bitVectorFill(_usedBitVector, allocatedAreaStart, allocatedAreaSize);
    Support::bitVectorSetBit(_stopBitVector, allocatedAreaEnd - 1, true);

    // Update search region and statistics.
    _pool->totalAreaUsed += allocatedAreaSize;
    _areaUsed += allocatedAreaSize;

    if (areaAvailable() == 0) {
      _searchStart = _areaSize;
      _searchEnd = 0;
      _largestUnusedArea = 0;

      clearFlags(kFlagDirty | kFlagEmpty);
    }
    else {
      if (_searchStart == allocatedAreaStart)
        _searchStart = allocatedAreaEnd;
      if (_searchEnd == allocatedAreaEnd)
        _searchEnd = allocatedAreaStart;

      addFlags(kFlagDirty);
      clearFlags(kFlagEmpty);
    }
  }

  inline void markReleasedArea(uint32_t releasedAreaStart, uint32_t releasedAreaEnd) noexcept {
    uint32_t releasedAreaSize = releasedAreaEnd - releasedAreaStart;

    // Update the search region and statistics.
    _pool->totalAreaUsed -= releasedAreaSize;
    _areaUsed -= releasedAreaSize;
    _searchStart = Support::min(_searchStart, releasedAreaStart);
    _searchEnd = Support::max(_searchEnd, releasedAreaEnd);

    // Unmark occupied bits and also the sentinel.
    Support::bitVectorClear(_usedBitVector, releasedAreaStart, releasedAreaSize);
    Support::bitVectorSetBit(_stopBitVector, releasedAreaEnd - 1, false);

    if (areaUsed() == initialAreaStart()) {
      _searchStart = initialAreaStart();
      _searchEnd = _areaSize;
      _largestUnusedArea = _areaSize - initialAreaStart();
      addFlags(kFlagEmpty);
      clearFlags(kFlagDirty);
    }
    else {
      addFlags(kFlagDirty);
    }
  }

  inline void markShrunkArea(uint32_t shrunkAreaStart, uint32_t shrunkAreaEnd) noexcept {
    uint32_t shrunkAreaSize = shrunkAreaEnd - shrunkAreaStart;

    // Shrunk area cannot start at zero as it would mean that we have shrunk the first
    // block to zero bytes, which is not allowed as such block must be released instead.
    ASMJIT_ASSERT(shrunkAreaStart != 0);
    ASMJIT_ASSERT(shrunkAreaSize != 0);

    // Update the search region and statistics.
    _pool->totalAreaUsed -= shrunkAreaSize;
    _areaUsed -= shrunkAreaSize;
    _searchStart = Support::min(_searchStart, shrunkAreaStart);
    _searchEnd = Support::max(_searchEnd, shrunkAreaEnd);

    // Unmark the released space and move the sentinel.
    Support::bitVectorClear(_usedBitVector, shrunkAreaStart, shrunkAreaSize);
    Support::bitVectorSetBit(_stopBitVector, shrunkAreaEnd - 1, false);
    Support::bitVectorSetBit(_stopBitVector, shrunkAreaStart - 1, true);

    addFlags(kFlagDirty);
  }

  // RBTree default CMP uses '<' and '>' operators.
  inline bool operator<(const JitAllocatorBlock& other) const noexcept { return rxPtr() < other.rxPtr(); }
  inline bool operator>(const JitAllocatorBlock& other) const noexcept { return rxPtr() > other.rxPtr(); }

  // Special implementation for querying blocks by `key`, which must be in `[BlockPtr, BlockPtr + BlockSize)` range.
  inline bool operator<(const uint8_t* key) const noexcept { return rxPtr() + _blockSize <= key; }
  inline bool operator>(const uint8_t* key) const noexcept { return rxPtr() > key; }
};

// JitAllocator - PrivateImpl
// ==========================

class JitAllocatorPrivateImpl : public JitAllocator::Impl {
public:
  //! Lock for thread safety.
  mutable Lock lock;
  //! System page size (also a minimum block size).
  uint32_t pageSize;
  //! Number of active allocations.
  size_t allocationCount;

  //! Blocks from all pools in RBTree.
  ZoneTree<JitAllocatorBlock> tree;
  //! Allocator pools.
  JitAllocatorPool* pools;
  //! Number of allocator pools.
  size_t poolCount;

  inline JitAllocatorPrivateImpl(JitAllocatorPool* pools, size_t poolCount) noexcept
    : JitAllocator::Impl {},
      pageSize(0),
      allocationCount(0),
      pools(pools),
      poolCount(poolCount) {}
  inline ~JitAllocatorPrivateImpl() noexcept {}
};

static const JitAllocator::Impl JitAllocatorImpl_none {};
static const JitAllocator::CreateParams JitAllocatorParams_none {};

// JitAllocator - Utilities
// ========================

static inline JitAllocatorPrivateImpl* JitAllocatorImpl_new(const JitAllocator::CreateParams* params) noexcept {
  VirtMem::Info vmInfo = VirtMem::info();

  if (!params)
    params = &JitAllocatorParams_none;

  JitAllocatorOptions options = params->options;
  uint32_t blockSize = params->blockSize;
  uint32_t granularity = params->granularity;
  uint32_t fillPattern = params->fillPattern;

  // Setup pool count to [1..3].
  size_t poolCount = 1;
  if (Support::test(options, JitAllocatorOptions::kUseMultiplePools))
    poolCount = kJitAllocatorMultiPoolCount;;

  // Setup block size [64kB..256MB].
  if (blockSize < 64 * 1024 || blockSize > 256 * 1024 * 1024 || !Support::isPowerOf2(blockSize))
    blockSize = vmInfo.pageGranularity;

  // Setup granularity [64..256].
  if (granularity < 64 || granularity > 256 || !Support::isPowerOf2(granularity))
    granularity = kJitAllocatorBaseGranularity;

  // Setup fill-pattern.
  if (uint32_t(options & JitAllocatorOptions::kCustomFillPattern) == 0)
    fillPattern = JitAllocator_defaultFillPattern();

  size_t size = sizeof(JitAllocatorPrivateImpl) + sizeof(JitAllocatorPool) * poolCount;
  void* p = ::malloc(size);
  if (ASMJIT_UNLIKELY(!p))
    return nullptr;

  VirtMem::HardenedRuntimeInfo hardenedRtInfo = VirtMem::hardenedRuntimeInfo();
  if (Support::test(hardenedRtInfo.flags, VirtMem::HardenedRuntimeFlags::kEnabled)) {
    // If we are running within a hardened environment (mapping RWX is not allowed) then we have to use dual mapping
    // or other runtime capabilities like Apple specific MAP_JIT. There is no point in not enabling these as otherwise
    // the allocation would fail and JitAllocator would not be able to allocate memory.
    if (!Support::test(hardenedRtInfo.flags, VirtMem::HardenedRuntimeFlags::kMapJit))
      options |= JitAllocatorOptions::kUseDualMapping;
  }

  JitAllocatorPool* pools = reinterpret_cast<JitAllocatorPool*>((uint8_t*)p + sizeof(JitAllocatorPrivateImpl));
  JitAllocatorPrivateImpl* impl = new(p) JitAllocatorPrivateImpl(pools, poolCount);

  impl->options = options;
  impl->blockSize = blockSize;
  impl->granularity = granularity;
  impl->fillPattern = fillPattern;
  impl->pageSize = vmInfo.pageSize;

  for (size_t poolId = 0; poolId < poolCount; poolId++)
    new(&pools[poolId]) JitAllocatorPool(granularity << poolId);

  return impl;
}

static inline void JitAllocatorImpl_destroy(JitAllocatorPrivateImpl* impl) noexcept {
  impl->~JitAllocatorPrivateImpl();
  ::free(impl);
}

static inline size_t JitAllocatorImpl_sizeToPoolId(const JitAllocatorPrivateImpl* impl, size_t size) noexcept {
  size_t poolId = impl->poolCount - 1;
  size_t granularity = size_t(impl->granularity) << poolId;

  while (poolId) {
    if (Support::alignUp(size, granularity) == size)
      break;
    poolId--;
    granularity >>= 1;
  }

  return poolId;
}

static inline size_t JitAllocatorImpl_bitVectorSizeToByteSize(uint32_t areaSize) noexcept {
  using Support::kBitWordSizeInBits;
  return ((areaSize + kBitWordSizeInBits - 1u) / kBitWordSizeInBits) * sizeof(Support::BitWord);
}

static inline size_t JitAllocatorImpl_calculateIdealBlockSize(JitAllocatorPrivateImpl* impl, JitAllocatorPool* pool, size_t allocationSize) noexcept {
  JitAllocatorBlock* last = pool->blocks.last();
  size_t blockSize = last ? last->blockSize() : size_t(impl->blockSize);

  // We have to increase the allocationSize if we know that the block must provide padding.
  if (!Support::test(impl->options, JitAllocatorOptions::kDisableInitialPadding)) {
    if (SIZE_MAX - allocationSize < 64u)
      return 0; // Overflown
    allocationSize += 64u;
  }

  if (blockSize < kJitAllocatorMaxBlockSize)
    blockSize *= 2u;

  if (allocationSize > blockSize) {
    blockSize = Support::alignUp(allocationSize, impl->blockSize);
    if (ASMJIT_UNLIKELY(blockSize < allocationSize))
      return 0; // Overflown.
  }

  return blockSize;
}

ASMJIT_FAVOR_SPEED static void JitAllocatorImpl_fillPattern(void* mem, uint32_t pattern, size_t sizeInBytes) noexcept {
  size_t n = sizeInBytes / 4u;
  uint32_t* p = static_cast<uint32_t*>(mem);

  for (size_t i = 0; i < n; i++)
    p[i] = pattern;
}

// Allocate a new `JitAllocatorBlock` for the given `blockSize`.
//
// NOTE: The block doesn't have `kFlagEmpty` flag set, because the new block
// is only allocated when it's actually needed, so it would be cleared anyway.
static Error JitAllocatorImpl_newBlock(JitAllocatorPrivateImpl* impl, JitAllocatorBlock** dst, JitAllocatorPool* pool, size_t blockSize) noexcept {
  using Support::BitWord;
  using Support::kBitWordSizeInBits;

  uint32_t areaSize = uint32_t((blockSize + pool->granularity - 1) >> pool->granularityLog2);
  uint32_t numBitWords = (areaSize + kBitWordSizeInBits - 1u) / kBitWordSizeInBits;

  JitAllocatorBlock* block = static_cast<JitAllocatorBlock*>(::malloc(sizeof(JitAllocatorBlock) + size_t(numBitWords) * 2u * sizeof(BitWord)));
  if (ASMJIT_UNLIKELY(block == nullptr))
    return DebugUtils::errored(kErrorOutOfMemory);

  BitWord* bitWords = reinterpret_cast<BitWord*>(reinterpret_cast<uint8_t*>(block) + sizeof(JitAllocatorBlock));
  uint32_t blockFlags = 0;

  if (!Support::test(impl->options, JitAllocatorOptions::kDisableInitialPadding))
    blockFlags |= JitAllocatorBlock::kFlagInitialPadding;

  Error err {};
  VirtMem::DualMapping virtMem {};

  if (Support::test(impl->options, JitAllocatorOptions::kUseDualMapping)) {
    err = VirtMem::allocDualMapping(&virtMem, blockSize, VirtMem::MemoryFlags::kAccessRWX);
    blockFlags |= JitAllocatorBlock::kFlagDualMapped;
  }
  else {
    err = VirtMem::alloc(&virtMem.rx, blockSize, VirtMem::MemoryFlags::kAccessRWX);
    virtMem.rw = virtMem.rx;
  }

  // Out of memory.
  if (ASMJIT_UNLIKELY(err != kErrorOk)) {
    if (block)
      ::free(block);
    return err;
  }

  // Fill the memory if the secure mode is enabled.
  if (Support::test(impl->options, JitAllocatorOptions::kFillUnusedMemory)) {
    VirtMem::ProtectJitReadWriteScope scope(virtMem.rw, blockSize);
    JitAllocatorImpl_fillPattern(virtMem.rw, impl->fillPattern, blockSize);
  }

  *dst = new(block) JitAllocatorBlock(pool, virtMem, blockSize, blockFlags, bitWords, bitWords + numBitWords, areaSize);
  return kErrorOk;
}

static void JitAllocatorImpl_deleteBlock(JitAllocatorPrivateImpl* impl, JitAllocatorBlock* block) noexcept {
  DebugUtils::unused(impl);

  if (block->hasFlag(JitAllocatorBlock::kFlagDualMapped))
    VirtMem::releaseDualMapping(&block->_mapping, block->blockSize());
  else
    VirtMem::release(block->rxPtr(), block->blockSize());

  ::free(block);
}

static void JitAllocatorImpl_insertBlock(JitAllocatorPrivateImpl* impl, JitAllocatorBlock* block) noexcept {
  JitAllocatorPool* pool = block->pool();

  if (!pool->cursor)
    pool->cursor = block;

  // Add to RBTree and List.
  impl->tree.insert(block);
  pool->blocks.append(block);

  // Update statistics.
  pool->blockCount++;
  pool->totalAreaSize += block->areaSize();
  pool->totalAreaUsed += block->areaUsed();
  pool->totalOverheadBytes += sizeof(JitAllocatorBlock) + JitAllocatorImpl_bitVectorSizeToByteSize(block->areaSize()) * 2u;
}

static void JitAllocatorImpl_removeBlock(JitAllocatorPrivateImpl* impl, JitAllocatorBlock* block) noexcept {
  JitAllocatorPool* pool = block->pool();

  // Remove from RBTree and List.
  if (pool->cursor == block)
    pool->cursor = block->hasPrev() ? block->prev() : block->next();

  impl->tree.remove(block);
  pool->blocks.unlink(block);

  // Update statistics.
  pool->blockCount--;
  pool->totalAreaSize -= block->areaSize();
  pool->totalAreaUsed -= block->areaUsed();
  pool->totalOverheadBytes -= sizeof(JitAllocatorBlock) + JitAllocatorImpl_bitVectorSizeToByteSize(block->areaSize()) * 2u;
}

static void JitAllocatorImpl_wipeOutBlock(JitAllocatorPrivateImpl* impl, JitAllocatorBlock* block) noexcept {
  if (block->hasFlag(JitAllocatorBlock::kFlagEmpty))
    return;

  JitAllocatorPool* pool = block->pool();
  if (Support::test(impl->options, JitAllocatorOptions::kFillUnusedMemory)) {
    VirtMem::protectJitMemory(VirtMem::ProtectJitAccess::kReadWrite);

    uint32_t granularity = pool->granularity;
    uint8_t* rwPtr = block->rwPtr();
    BitVectorRangeIterator<Support::BitWord, 0> it(block->_usedBitVector, pool->bitWordCountFromAreaSize(block->areaSize()));

    size_t rangeStart;
    size_t rangeEnd;

    while (it.nextRange(&rangeStart, &rangeEnd)) {
      uint8_t* spanPtr = rwPtr + rangeStart * granularity;
      size_t spanSize = (rangeEnd - rangeStart) * granularity;

      JitAllocatorImpl_fillPattern(spanPtr, impl->fillPattern, spanSize);
      VirtMem::flushInstructionCache(spanPtr, spanSize);
    }
    VirtMem::protectJitMemory(VirtMem::ProtectJitAccess::kReadExecute);
  }

  block->clearBlock();
}

// JitAllocator - Construction & Destruction
// =========================================

JitAllocator::JitAllocator(const CreateParams* params) noexcept {
  _impl = JitAllocatorImpl_new(params);
  if (ASMJIT_UNLIKELY(!_impl))
    _impl = const_cast<JitAllocator::Impl*>(&JitAllocatorImpl_none);
}

JitAllocator::~JitAllocator() noexcept {
  if (_impl == &JitAllocatorImpl_none)
    return;

  reset(ResetPolicy::kHard);
  JitAllocatorImpl_destroy(static_cast<JitAllocatorPrivateImpl*>(_impl));
}

// JitAllocator - Reset
// ====================

void JitAllocator::reset(ResetPolicy resetPolicy) noexcept {
  if (_impl == &JitAllocatorImpl_none)
    return;

  JitAllocatorPrivateImpl* impl = static_cast<JitAllocatorPrivateImpl*>(_impl);
  impl->tree.reset();
  size_t poolCount = impl->poolCount;

  for (size_t poolId = 0; poolId < poolCount; poolId++) {
    JitAllocatorPool& pool = impl->pools[poolId];
    JitAllocatorBlock* block = pool.blocks.first();

    JitAllocatorBlock* blockToKeep = nullptr;
    if (resetPolicy != ResetPolicy::kHard && uint32_t(impl->options & JitAllocatorOptions::kImmediateRelease) == 0) {
      blockToKeep = block;
      block = block->next();
    }

    while (block) {
      JitAllocatorBlock* next = block->next();
      JitAllocatorImpl_deleteBlock(impl, block);
      block = next;
    }

    pool.reset();

    if (blockToKeep) {
      blockToKeep->_listNodes[0] = nullptr;
      blockToKeep->_listNodes[1] = nullptr;
      JitAllocatorImpl_wipeOutBlock(impl, blockToKeep);
      JitAllocatorImpl_insertBlock(impl, blockToKeep);
      pool.emptyBlockCount = 1;
    }
  }
}

// JitAllocator - Statistics
// =========================

JitAllocator::Statistics JitAllocator::statistics() const noexcept {
  Statistics statistics;
  statistics.reset();

  if (ASMJIT_LIKELY(_impl != &JitAllocatorImpl_none)) {
    JitAllocatorPrivateImpl* impl = static_cast<JitAllocatorPrivateImpl*>(_impl);
    LockGuard guard(impl->lock);

    size_t poolCount = impl->poolCount;
    for (size_t poolId = 0; poolId < poolCount; poolId++) {
      const JitAllocatorPool& pool = impl->pools[poolId];
      statistics._blockCount   += size_t(pool.blockCount);
      statistics._reservedSize += size_t(pool.totalAreaSize) * pool.granularity;
      statistics._usedSize     += size_t(pool.totalAreaUsed) * pool.granularity;
      statistics._overheadSize += size_t(pool.totalOverheadBytes);
    }

    statistics._allocationCount = impl->allocationCount;
  }

  return statistics;
}

// JitAllocator - Alloc & Release
// ==============================

Error JitAllocator::alloc(void** rxPtrOut, void** rwPtrOut, size_t size) noexcept {
  if (ASMJIT_UNLIKELY(_impl == &JitAllocatorImpl_none))
    return DebugUtils::errored(kErrorNotInitialized);

  JitAllocatorPrivateImpl* impl = static_cast<JitAllocatorPrivateImpl*>(_impl);
  constexpr uint32_t kNoIndex = std::numeric_limits<uint32_t>::max();

  *rxPtrOut = nullptr;
  *rwPtrOut = nullptr;

  // Align to the minimum granularity by default.
  size = Support::alignUp<size_t>(size, impl->granularity);
  if (ASMJIT_UNLIKELY(size == 0))
    return DebugUtils::errored(kErrorInvalidArgument);

  if (ASMJIT_UNLIKELY(size > std::numeric_limits<uint32_t>::max() / 2))
    return DebugUtils::errored(kErrorTooLarge);

  LockGuard guard(impl->lock);
  JitAllocatorPool* pool = &impl->pools[JitAllocatorImpl_sizeToPoolId(impl, size)];

  uint32_t areaIndex = kNoIndex;
  uint32_t areaSize = uint32_t(pool->areaSizeFromByteSize(size));

  // Try to find the requested memory area in existing blocks.
  JitAllocatorBlock* block = pool->blocks.first();
  if (block) {
    JitAllocatorBlock* initial = block;
    do {
      JitAllocatorBlock* next = block->hasNext() ? block->next() : pool->blocks.first();
      if (block->areaAvailable() >= areaSize) {
        if (block->isDirty() || block->largestUnusedArea() >= areaSize) {
          BitVectorRangeIterator<Support::BitWord, 0> it(block->_usedBitVector, pool->bitWordCountFromAreaSize(block->areaSize()), block->_searchStart, block->_searchEnd);

          size_t rangeStart = 0;
          size_t rangeEnd = block->areaSize();

          size_t searchStart = SIZE_MAX;
          size_t largestArea = 0;

          while (it.nextRange(&rangeStart, &rangeEnd, areaSize)) {
            size_t rangeSize = rangeEnd - rangeStart;
            if (rangeSize >= areaSize) {
              areaIndex = uint32_t(rangeStart);
              break;
            }

            searchStart = Support::min(searchStart, rangeStart);
            largestArea = Support::max(largestArea, rangeSize);
          }

          if (areaIndex != kNoIndex)
            break;

          if (searchStart != SIZE_MAX) {
            // Because we have iterated over the entire block, we can now mark the
            // largest unused area that can be used to cache the next traversal.
            size_t searchEnd = rangeEnd;

            block->_searchStart = uint32_t(searchStart);
            block->_searchEnd = uint32_t(searchEnd);
            block->_largestUnusedArea = uint32_t(largestArea);
            block->clearFlags(JitAllocatorBlock::kFlagDirty);
          }
        }
      }

      block = next;
    } while (block != initial);
  }

  // Allocate a new block if there is no region of a required size.
  if (areaIndex == kNoIndex) {
    size_t blockSize = JitAllocatorImpl_calculateIdealBlockSize(impl, pool, size);
    if (ASMJIT_UNLIKELY(!blockSize))
      return DebugUtils::errored(kErrorOutOfMemory);

    ASMJIT_PROPAGATE(JitAllocatorImpl_newBlock(impl, &block, pool, blockSize));
    areaIndex = block->initialAreaStart();

    JitAllocatorImpl_insertBlock(impl, block);
    block->_searchStart += areaSize;
    block->_largestUnusedArea -= areaSize;
  }
  else if (block->hasFlag(JitAllocatorBlock::kFlagEmpty)) {
    pool->emptyBlockCount--;
    block->clearFlags(JitAllocatorBlock::kFlagEmpty);
  }

  // Update statistics.
  impl->allocationCount++;
  block->markAllocatedArea(areaIndex, areaIndex + areaSize);

  // Return a pointer to the allocated memory.
  size_t offset = pool->byteSizeFromAreaSize(areaIndex);
  ASMJIT_ASSERT(offset <= block->blockSize() - size);

  *rxPtrOut = block->rxPtr() + offset;
  *rwPtrOut = block->rwPtr() + offset;
  return kErrorOk;
}

Error JitAllocator::release(void* rxPtr) noexcept {
  if (ASMJIT_UNLIKELY(_impl == &JitAllocatorImpl_none))
    return DebugUtils::errored(kErrorNotInitialized);

  if (ASMJIT_UNLIKELY(!rxPtr))
    return DebugUtils::errored(kErrorInvalidArgument);

  JitAllocatorPrivateImpl* impl = static_cast<JitAllocatorPrivateImpl*>(_impl);
  LockGuard guard(impl->lock);

  JitAllocatorBlock* block = impl->tree.get(static_cast<uint8_t*>(rxPtr));
  if (ASMJIT_UNLIKELY(!block))
    return DebugUtils::errored(kErrorInvalidState);

  // Offset relative to the start of the block.
  JitAllocatorPool* pool = block->pool();
  size_t offset = (size_t)((uint8_t*)rxPtr - block->rxPtr());

  // The first bit representing the allocated area and its size.
  uint32_t areaIndex = uint32_t(offset >> pool->granularityLog2);
  uint32_t areaEnd = uint32_t(Support::bitVectorIndexOf(block->_stopBitVector, areaIndex, true)) + 1;
  uint32_t areaSize = areaEnd - areaIndex;

  impl->allocationCount--;
  block->markReleasedArea(areaIndex, areaEnd);

  // Fill the released memory if the secure mode is enabled.
  if (Support::test(impl->options, JitAllocatorOptions::kFillUnusedMemory)) {
    uint8_t* spanPtr = block->rwPtr() + areaIndex * pool->granularity;
    size_t spanSize = areaSize * pool->granularity;

    VirtMem::ProtectJitReadWriteScope scope(spanPtr, spanSize);
    JitAllocatorImpl_fillPattern(spanPtr, impl->fillPattern, spanSize);
  }

  // Release the whole block if it became empty.
  if (block->empty()) {
    if (pool->emptyBlockCount || Support::test(impl->options, JitAllocatorOptions::kImmediateRelease)) {
      JitAllocatorImpl_removeBlock(impl, block);
      JitAllocatorImpl_deleteBlock(impl, block);
    }
    else {
      pool->emptyBlockCount++;
    }
  }

  return kErrorOk;
}

Error JitAllocator::shrink(void* rxPtr, size_t newSize) noexcept {
  if (ASMJIT_UNLIKELY(_impl == &JitAllocatorImpl_none))
    return DebugUtils::errored(kErrorNotInitialized);

  if (ASMJIT_UNLIKELY(!rxPtr))
    return DebugUtils::errored(kErrorInvalidArgument);

  if (ASMJIT_UNLIKELY(newSize == 0))
    return release(rxPtr);

  JitAllocatorPrivateImpl* impl = static_cast<JitAllocatorPrivateImpl*>(_impl);
  LockGuard guard(impl->lock);
  JitAllocatorBlock* block = impl->tree.get(static_cast<uint8_t*>(rxPtr));

  if (ASMJIT_UNLIKELY(!block))
    return DebugUtils::errored(kErrorInvalidArgument);

  // Offset relative to the start of the block.
  JitAllocatorPool* pool = block->pool();
  size_t offset = (size_t)((uint8_t*)rxPtr - block->rxPtr());

  // The first bit representing the allocated area and its size.
  uint32_t areaStart = uint32_t(offset >> pool->granularityLog2);

  bool isUsed = Support::bitVectorGetBit(block->_usedBitVector, areaStart);
  if (ASMJIT_UNLIKELY(!isUsed))
    return DebugUtils::errored(kErrorInvalidArgument);

  uint32_t areaEnd = uint32_t(Support::bitVectorIndexOf(block->_stopBitVector, areaStart, true)) + 1;
  uint32_t areaPrevSize = areaEnd - areaStart;
  uint32_t areaShrunkSize = pool->areaSizeFromByteSize(newSize);

  if (ASMJIT_UNLIKELY(areaShrunkSize > areaPrevSize))
    return DebugUtils::errored(kErrorInvalidState);

  uint32_t areaDiff = areaPrevSize - areaShrunkSize;
  if (areaDiff) {
    block->markShrunkArea(areaStart + areaShrunkSize, areaEnd);

    // Fill released memory if the secure mode is enabled.
    if (Support::test(impl->options, JitAllocatorOptions::kFillUnusedMemory)) {
      uint8_t* spanPtr = block->rwPtr() + (areaStart + areaShrunkSize) * pool->granularity;
      size_t spanSize = areaDiff * pool->granularity;

      VirtMem::ProtectJitReadWriteScope scope(spanPtr, spanSize);
      JitAllocatorImpl_fillPattern(spanPtr, fillPattern(), spanSize);
    }
  }

  return kErrorOk;
}

Error JitAllocator::query(void* rxPtr, void** rxPtrOut, void** rwPtrOut, size_t* sizeOut) const noexcept {
  *rxPtrOut = nullptr;
  *rwPtrOut = nullptr;
  *sizeOut = 0u;

  if (ASMJIT_UNLIKELY(_impl == &JitAllocatorImpl_none))
    return DebugUtils::errored(kErrorNotInitialized);

  JitAllocatorPrivateImpl* impl = static_cast<JitAllocatorPrivateImpl*>(_impl);
  LockGuard guard(impl->lock);
  JitAllocatorBlock* block = impl->tree.get(static_cast<uint8_t*>(rxPtr));

  if (ASMJIT_UNLIKELY(!block))
    return DebugUtils::errored(kErrorInvalidArgument);

  // Offset relative to the start of the block.
  JitAllocatorPool* pool = block->pool();
  size_t offset = (size_t)((uint8_t*)rxPtr - block->rxPtr());

  // The first bit representing the allocated area and its size.
  uint32_t areaStart = uint32_t(offset >> pool->granularityLog2);

  bool isUsed = Support::bitVectorGetBit(block->_usedBitVector, areaStart);
  if (ASMJIT_UNLIKELY(!isUsed))
    return DebugUtils::errored(kErrorInvalidArgument);

  uint32_t areaEnd = uint32_t(Support::bitVectorIndexOf(block->_stopBitVector, areaStart, true)) + 1;
  size_t byteOffset = pool->byteSizeFromAreaSize(areaStart);
  size_t byteSize = pool->byteSizeFromAreaSize(areaEnd - areaStart);

  *rxPtrOut = static_cast<uint8_t*>(block->_mapping.rx) + byteOffset;
  *rwPtrOut = static_cast<uint8_t*>(block->_mapping.rw) + byteOffset;
  *sizeOut = byteSize;

  return kErrorOk;
}

// JitAllocator - Tests
// ====================

#if defined(ASMJIT_TEST)
// A pseudo random number generator based on a paper by Sebastiano Vigna:
//   http://vigna.di.unimi.it/ftp/papers/xorshiftplus.pdf
class Random {
public:
  // Constants suggested as `23/18/5`.
  enum Steps : uint32_t {
    kStep1_SHL = 23,
    kStep2_SHR = 18,
    kStep3_SHR = 5
  };

  inline explicit Random(uint64_t seed = 0) noexcept { reset(seed); }
  inline Random(const Random& other) noexcept = default;

  inline void reset(uint64_t seed = 0) noexcept {
    // The number is arbitrary, it means nothing.
    constexpr uint64_t kZeroSeed = 0x1F0A2BE71D163FA0u;

    // Generate the state data by using splitmix64.
    for (uint32_t i = 0; i < 2; i++) {
      seed += 0x9E3779B97F4A7C15u;
      uint64_t x = seed;
      x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9u;
      x = (x ^ (x >> 27)) * 0x94D049BB133111EBu;
      x = (x ^ (x >> 31));
      _state[i] = x != 0 ? x : kZeroSeed;
    }
  }

  inline uint32_t nextUInt32() noexcept {
    return uint32_t(nextUInt64() >> 32);
  }

  inline uint64_t nextUInt64() noexcept {
    uint64_t x = _state[0];
    uint64_t y = _state[1];

    x ^= x << kStep1_SHL;
    y ^= y >> kStep3_SHR;
    x ^= x >> kStep2_SHR;
    x ^= y;

    _state[0] = y;
    _state[1] = x;
    return x + y;
  }

  uint64_t _state[2];
};

namespace JitAllocatorUtils {
  static void fillPattern64(void* p_, uint64_t pattern, size_t sizeInBytes) noexcept {
    uint64_t* p = static_cast<uint64_t*>(p_);
    size_t n = sizeInBytes / 8u;

    for (size_t i = 0; i < n; i++)
      p[i] = pattern;
  }

  static bool verifyPattern64(const void* p_, uint64_t pattern, size_t sizeInBytes) noexcept {
    const uint64_t* p = static_cast<const uint64_t*>(p_);
    size_t n = sizeInBytes / 8u;

    for (size_t i = 0; i < n; i++) {
      if (p[i] != pattern) {
        INFO("Pattern verification failed at 0x%p [%zu * 8]: value(0x%016llX) != expected(0x%016llX)",
          p,
          i,
          (unsigned long long)p[i],
          (unsigned long long)pattern);
        return false;
      }
    }

    return true;
  }
}

// Helper class to verify that JitAllocator doesn't return addresses that overlap.
class JitAllocatorWrapper {
public:
  // Address to a memory region of a given size.
  class Range {
  public:
    inline Range(uint8_t* addr, size_t size) noexcept
      : addr(addr),
        size(size) {}
    uint8_t* addr;
    size_t size;
  };

  // Based on JitAllocator::Block, serves our purpose well...
  class Record : public ZoneTreeNodeT<Record>,
                 public Range {
  public:
    //! Read/write address, in case this is a dual mapping.
    void* _rw;
    //! Describes a pattern used to fill the allocated memory.
    uint64_t pattern;

    inline Record(void* rx, void* rw, size_t size, uint64_t pattern)
      : ZoneTreeNodeT<Record>(),
        Range(static_cast<uint8_t*>(rx), size),
        _rw(rw),
        pattern(pattern) {}

    inline void* rx() const noexcept { return addr; }
    inline void* rw() const noexcept { return _rw; }

    inline bool operator<(const Record& other) const noexcept { return addr < other.addr; }
    inline bool operator>(const Record& other) const noexcept { return addr > other.addr; }

    inline bool operator<(const uint8_t* key) const noexcept { return addr + size <= key; }
    inline bool operator>(const uint8_t* key) const noexcept { return addr > key; }
  };

  Zone _zone;
  ZoneAllocator _heap;
  ZoneTree<Record> _records;
  JitAllocator _allocator;
  Random _rng;

  explicit JitAllocatorWrapper(const JitAllocator::CreateParams* params) noexcept
    : _zone(1024 * 1024),
      _heap(&_zone),
      _allocator(params),
      _rng(0x123456789u) {}

  void _insert(void* pRX, void* pRW, size_t size) noexcept {
    uint8_t* p = static_cast<uint8_t*>(pRX);
    uint8_t* pEnd = p + size - 1;

    Record* record;

    record = _records.get(p);
    EXPECT_NULL(record)
      .message("Address [%p:%p] collides with a newly allocated [%p:%p]\n", record->addr, record->addr + record->size, p, p + size);

    record = _records.get(pEnd);
    EXPECT_NULL(record)
      .message("Address [%p:%p] collides with a newly allocated [%p:%p]\n", record->addr, record->addr + record->size, p, p + size);

    uint64_t pattern = _rng.nextUInt64();
    record = _heap.newT<Record>(pRX, pRW, size, pattern);
    EXPECT_NOT_NULL(record);

    {
      VirtMem::ProtectJitReadWriteScope scope(pRW, size);
      JitAllocatorUtils::fillPattern64(pRW, pattern, size);
    }

    VirtMem::flushInstructionCache(pRX, size);
    EXPECT_TRUE(JitAllocatorUtils::verifyPattern64(pRX, pattern, size));

    _records.insert(record);
  }

  void _remove(void* p) noexcept {
    Record* record = _records.get(static_cast<uint8_t*>(p));
    EXPECT_NOT_NULL(record);

    EXPECT_TRUE(JitAllocatorUtils::verifyPattern64(record->rx(), record->pattern, record->size));
    EXPECT_TRUE(JitAllocatorUtils::verifyPattern64(record->rw(), record->pattern, record->size));

    _records.remove(record);
    _heap.release(record, sizeof(Record));
  }

  void* alloc(size_t size) noexcept {
    void* rxPtr;
    void* rwPtr;

    Error err = _allocator.alloc(&rxPtr, &rwPtr, size);
    EXPECT_EQ(err, kErrorOk)
      .message("JitAllocator failed to allocate %zu bytes\n", size);

    _insert(rxPtr, rwPtr, size);
    return rxPtr;
  }

  void release(void* p) noexcept {
    _remove(p);
    EXPECT_EQ(_allocator.release(p), kErrorOk)
      .message("JitAllocator failed to release '%p'\n", p);
  }

  void shrink(void* p, size_t newSize) noexcept {
    Record* record = _records.get(static_cast<uint8_t*>(p));
    EXPECT_NOT_NULL(record);

    if (!newSize)
      return release(p);

    Error err = _allocator.shrink(p, newSize);
    EXPECT_EQ(err, kErrorOk)
      .message("JitAllocator failed to shrink %p to %zu bytes\n", p, newSize);

    record->size = newSize;
  }
};

static void JitAllocatorTest_shuffle(void** ptrArray, size_t count, Random& prng) noexcept {
  for (size_t i = 0; i < count; ++i)
    std::swap(ptrArray[i], ptrArray[size_t(prng.nextUInt32() % count)]);
}

static void JitAllocatorTest_usage(JitAllocator& allocator) noexcept {
  JitAllocator::Statistics stats = allocator.statistics();
  INFO("    Block Count       : %9llu [Blocks]"        , (unsigned long long)(stats.blockCount()));
  INFO("    Reserved (VirtMem): %9llu [Bytes]"         , (unsigned long long)(stats.reservedSize()));
  INFO("    Used     (VirtMem): %9llu [Bytes] (%.1f%%)", (unsigned long long)(stats.usedSize()), stats.usedSizeAsPercent());
  INFO("    Overhead (HeapMem): %9llu [Bytes] (%.1f%%)", (unsigned long long)(stats.overheadSize()), stats.overheadSizeAsPercent());
}

template<typename T, size_t kPatternSize, bool Bit>
static void BitVectorRangeIterator_testRandom(Random& rnd, size_t count) noexcept {
  for (size_t i = 0; i < count; i++) {
    T in[kPatternSize];
    T out[kPatternSize];

    for (size_t j = 0; j < kPatternSize; j++) {
      in[j] = T(uint64_t(rnd.nextUInt32() & 0xFFu) * 0x0101010101010101);
      out[j] = Bit == 0 ? Support::allOnes<T>() : T(0);
    }

    {
      BitVectorRangeIterator<T, Bit> it(in, kPatternSize);
      size_t rangeStart, rangeEnd;
      while (it.nextRange(&rangeStart, &rangeEnd)) {
        if (Bit)
          Support::bitVectorFill(out, rangeStart, rangeEnd - rangeStart);
        else
          Support::bitVectorClear(out, rangeStart, rangeEnd - rangeStart);
      }
    }

    for (size_t j = 0; j < kPatternSize; j++) {
      EXPECT_EQ(in[j], out[j])
        .message("Invalid pattern detected at [%zu] (%llX != %llX)", j, (unsigned long long)in[j], (unsigned long long)out[j]);
    }
  }
}

static void test_jit_allocator_alloc_release() noexcept {
  size_t kCount = BrokenAPI::hasArg("--quick") ? 20000 : 100000;

  struct TestParams {
    const char* name;
    JitAllocatorOptions options;
    uint32_t blockSize;
    uint32_t granularity;
  };

  TestParams testParams[] = {
    { "Default", JitAllocatorOptions::kNone, 0, 0 },
    { "16MB blocks", JitAllocatorOptions::kNone, 16 * 1024 * 1024, 0 },
    { "256B granularity", JitAllocatorOptions::kNone, 0, 256 },
    { "kUseDualMapping", JitAllocatorOptions::kUseDualMapping, 0, 0 },
    { "kUseMultiplePools", JitAllocatorOptions::kUseMultiplePools, 0, 0 },
    { "kFillUnusedMemory", JitAllocatorOptions::kFillUnusedMemory, 0, 0 },
    { "kImmediateRelease", JitAllocatorOptions::kImmediateRelease, 0, 0 },
    { "kDisableInitialPadding", JitAllocatorOptions::kDisableInitialPadding, 0, 0 },
    { "kUseDualMapping | kFillUnusedMemory", JitAllocatorOptions::kUseDualMapping | JitAllocatorOptions::kFillUnusedMemory, 0, 0 }
  };

  INFO("BitVectorRangeIterator<uint32_t>");
  {
    Random rnd;
    BitVectorRangeIterator_testRandom<uint32_t, 64, 0>(rnd, kCount);
  }

  INFO("BitVectorRangeIterator<uint64_t>");
  {
    Random rnd;
    BitVectorRangeIterator_testRandom<uint64_t, 64, 0>(rnd, kCount);
  }

  for (uint32_t testId = 0; testId < ASMJIT_ARRAY_SIZE(testParams); testId++) {
    INFO("JitAllocator(%s)", testParams[testId].name);

    JitAllocator::CreateParams params {};
    params.options = testParams[testId].options;
    params.blockSize = testParams[testId].blockSize;
    params.granularity = testParams[testId].granularity;

    size_t fixedBlockSize = 256;

    JitAllocatorWrapper wrapper(&params);
    Random prng(100);

    size_t i;

    INFO("  Memory alloc/release test - %d allocations", kCount);

    void** ptrArray = (void**)::malloc(sizeof(void*) * size_t(kCount));
    EXPECT_NOT_NULL(ptrArray);

    // Random blocks tests...
    INFO("  Allocating random blocks...");
    for (i = 0; i < kCount; i++)
      ptrArray[i] = wrapper.alloc((prng.nextUInt32() % 1024) + 8);
    JitAllocatorTest_usage(wrapper._allocator);

    INFO("  Releasing all allocated blocks from the beginning...");
    for (i = 0; i < kCount; i++)
      wrapper.release(ptrArray[i]);
    JitAllocatorTest_usage(wrapper._allocator);

    INFO("  Allocating random blocks again...", kCount);
    for (i = 0; i < kCount; i++)
      ptrArray[i] = wrapper.alloc((prng.nextUInt32() % 1024) + 8);
    JitAllocatorTest_usage(wrapper._allocator);

    INFO("  Shuffling allocated blocks...");
    JitAllocatorTest_shuffle(ptrArray, unsigned(kCount), prng);

    INFO("  Releasing 50%% of allocated blocks...");
    for (i = 0; i < kCount / 2; i++)
      wrapper.release(ptrArray[i]);
    JitAllocatorTest_usage(wrapper._allocator);

    INFO("  Allocating 50%% more blocks again...");
    for (i = 0; i < kCount / 2; i++)
      ptrArray[i] = wrapper.alloc((prng.nextUInt32() % 1024) + 8);
    JitAllocatorTest_usage(wrapper._allocator);

    INFO("  Releasing all allocated blocks from the end...");
    for (i = 0; i < kCount; i++)
      wrapper.release(ptrArray[kCount - i - 1]);
    JitAllocatorTest_usage(wrapper._allocator);

    // Fixed blocks tests...
    INFO("  Allocating %zuB blocks...", fixedBlockSize);
    for (i = 0; i < kCount / 2; i++)
      ptrArray[i] = wrapper.alloc(fixedBlockSize);
    JitAllocatorTest_usage(wrapper._allocator);

    INFO("  Shrinking each %zuB block to 1 byte", fixedBlockSize);
    for (i = 0; i < kCount / 2; i++)
      wrapper.shrink(ptrArray[i], 1);
    JitAllocatorTest_usage(wrapper._allocator);

    INFO("  Allocating more 64B blocks...", 64);
    for (i = kCount / 2; i < kCount; i++)
      ptrArray[i] = wrapper.alloc(64);
    JitAllocatorTest_usage(wrapper._allocator);

    INFO("  Releasing all blocks from the beginning...");
    for (i = 0; i < kCount; i++)
      wrapper.release(ptrArray[i]);
    JitAllocatorTest_usage(wrapper._allocator);

    INFO("  Allocating %zuB blocks...", fixedBlockSize);
    for (i = 0; i < kCount; i++)
      ptrArray[i] = wrapper.alloc(fixedBlockSize);
    JitAllocatorTest_usage(wrapper._allocator);

    INFO("  Shuffling allocated blocks...");
    JitAllocatorTest_shuffle(ptrArray, unsigned(kCount), prng);

    INFO("  Releasing 50%% of allocated blocks...");
    for (i = 0; i < kCount / 2; i++)
      wrapper.release(ptrArray[i]);
    JitAllocatorTest_usage(wrapper._allocator);

    INFO("  Allocating 50%% more %zuB blocks again...", fixedBlockSize);
    for (i = 0; i < kCount / 2; i++)
      ptrArray[i] = wrapper.alloc(fixedBlockSize);
    JitAllocatorTest_usage(wrapper._allocator);

    INFO("  Releasing all allocated blocks from the end...");
    for (i = 0; i < kCount; i++)
      wrapper.release(ptrArray[kCount - i - 1]);
    JitAllocatorTest_usage(wrapper._allocator);

    ::free(ptrArray);
  }
}

static void test_jit_allocator_query() noexcept {
  JitAllocator allocator;

  void* rxPtr = nullptr;
  void* rwPtr = nullptr;
  size_t size = 100;

  EXPECT_EQ(allocator.alloc(&rxPtr, &rwPtr, size), kErrorOk);
  EXPECT_NOT_NULL(rxPtr);
  EXPECT_NOT_NULL(rwPtr);

  void* rxPtrQueried = nullptr;
  void* rwPtrQueried = nullptr;
  size_t sizeQueried;

  EXPECT_EQ(allocator.query(rxPtr, &rxPtrQueried, &rwPtrQueried, &sizeQueried), kErrorOk);
  EXPECT_EQ(rxPtrQueried, rxPtr);
  EXPECT_EQ(rwPtrQueried, rwPtr);
  EXPECT_EQ(sizeQueried, Support::alignUp(size, allocator.granularity()));
}

UNIT(jit_allocator) {
  test_jit_allocator_alloc_release();
  test_jit_allocator_query();
}
#endif // ASMJIT_TEST

ASMJIT_END_NAMESPACE

#endif // !ASMJIT_NO_JIT
