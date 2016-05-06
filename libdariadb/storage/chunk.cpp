#include "chunk.h"
#include "bloom_filter.h"
#include <algorithm>
#include <cassert>
#include <cstring>

using namespace dariadb;
using namespace dariadb::utils;
using namespace dariadb::storage;
using namespace dariadb::compression;

std::unique_ptr<ChunkPool> ChunkPool::_instance = nullptr;

ChunkPool::ChunkPool() : _chunks(ChunkPool_default_max_size) {}

ChunkPool::~ChunkPool() {}

void ChunkPool::start() {
  ChunkPool::instance();
}

void ChunkPool::stop() {
  ChunkPool::_instance = nullptr;
}

ChunkPool *ChunkPool::instance() {
  if (_instance == nullptr) {
    _instance = std::unique_ptr<ChunkPool>{new ChunkPool};
  }
  return _instance.get();
}

void *ChunkPool::alloc_chunk(std::size_t sz) {
  return _chunks.alloc(sz);
}

void ChunkPool::free_chunk(void *ptr, std::size_t sz) {
  return _chunks.free(ptr, sz);
}

size_t ChunkPool::polled_chunks() {
  return _chunks.polled();
}

void *Chunk::operator new(std::size_t sz) {
  return ChunkPool::instance()->alloc_chunk(sz);
}

void Chunk::operator delete(void *ptr, std::size_t sz) {
  ChunkPool::instance()->free_chunk(ptr, sz);
}

Chunk::Chunk(const ChunkIndexInfo &index, const uint8_t *buffer,
             const size_t buffer_length)
    : _buffer_t(new uint8_t[buffer_length]), _size(buffer_length), _locker{} {
  count = index.count;
  first = index.first;
  flag_bloom = index.flag_bloom;
  last = index.last;
  maxTime = index.maxTime;
  minTime = index.minTime;
  bw_pos = index.bw_pos;
  bw_bit_num = index.bw_bit_num;
  is_readonly = index.is_readonly;
  is_dropped = index.is_dropped;

  for (size_t i = 0; i < buffer_length; i++) {
    _buffer_t[i] = buffer[i];
  }
}

Chunk::Chunk(size_t size, Meas first_m)
    : _buffer_t(new uint8_t[size]), _size(size), _locker() {
  is_readonly = false;
  is_dropped = false;
  count = 0;
  first = first_m;
  last = first_m;
  minTime = std::numeric_limits<Time>::max();
  maxTime = std::numeric_limits<Time>::min();

  std::fill(_buffer_t, _buffer_t + size, 0);

  minTime = first_m.time;
  maxTime = first_m.time;
  flag_bloom = dariadb::storage::bloom_empty<dariadb::Flag>();
}

Chunk::~Chunk() {
  this->bw = nullptr;
  delete[] this->_buffer_t;
}

bool Chunk::check_flag(const Flag &f) {
  if (f != 0) {
    if (!dariadb::storage::bloom_check(flag_bloom, f)) {
      return false;
    }
  }
  return true;
}

ZippedChunk::ZippedChunk(size_t size, Meas first_m) : Chunk(size, first_m) {
  is_zipped = true;
  using compression::BinaryBuffer;
  range = Range{_buffer_t, _buffer_t + size};
  bw = std::make_shared<BinaryBuffer>(range);

  c_writer = compression::CopmressedWriter(bw);
  c_writer.append(first);
}

ZippedChunk::ZippedChunk(const ChunkIndexInfo &index, const uint8_t *buffer,
                         const size_t buffer_length)
    : Chunk(index, buffer, buffer_length) {
  assert(index.is_zipped);
  range = Range{_buffer_t, _buffer_t + buffer_length};
  assert(size_t(range.end - range.begin) == buffer_length);
  bw = std::make_shared<BinaryBuffer>(range);
  bw->set_bitnum(bw_bit_num);
  bw->set_pos(bw_pos);

  c_writer = compression::CopmressedWriter(bw);
  c_writer.restore_position(index.writer_position);
}

ZippedChunk::~ZippedChunk() {}

bool ZippedChunk::append(const Meas &m) {
  if (is_dropped || is_readonly) {
    throw MAKE_EXCEPTION("(is_dropped || is_readonly)");
  }

  std::lock_guard<utils::Locker> lg(_locker);
  auto t_f = this->c_writer.append(m);
  writer_position = c_writer.get_position();

  if (!t_f) {
    is_readonly = true;
    assert(c_writer.is_full());
    return false;
  } else {
    bw_pos = uint32_t(bw->pos());
    bw_bit_num = bw->bitnum();

    count++;

    minTime = std::min(minTime, m.time);
    maxTime = std::max(maxTime, m.time);
    flag_bloom = dariadb::storage::bloom_add(flag_bloom, m.flag);
    last = m;
    return true;
  }
}
