#include "chunk.h"
#include "../utils/crc.h"
#include "bloom_filter.h"
#include <algorithm>
#include <cassert>
#include <cstring>

using namespace dariadb;
using namespace dariadb::utils;
using namespace dariadb::storage;
using namespace dariadb::compression::v2;

// std::unique_ptr<ChunkCache> ChunkCache::_instance = nullptr;

std::ostream &dariadb::storage::operator<<(std::ostream &stream, const CHUNK_KIND &b) {
  switch (b) {
  case CHUNK_KIND::Simple:
    stream << "CHUNK_KIND::Simple";
    break;
  case CHUNK_KIND::Compressed:
    stream << "CHUNK_KIND::Compressed";
    break;
  }
  return stream;
}

Chunk::Chunk(ChunkHeader *hdr, uint8_t *buffer) {
  should_free = false;
  header = hdr;
  _buffer_t = buffer;
}

Chunk::Chunk(ChunkHeader *hdr, uint8_t *buffer, size_t _size, Meas first_m) {
  should_free = false;
  hdr->is_init = true;
  _buffer_t = buffer;
  header = hdr;
  header->size = _size;

  header->is_readonly = false;
  header->count = 0;
  header->first = first_m;
  header->last = first_m;
  header->minTime = first_m.time;
  header->maxTime = first_m.time;
  header->minId = first_m.id;
  header->maxId = first_m.id;
  header->flag_bloom = dariadb::storage::bloom_empty<dariadb::Flag>();
  header->id_bloom = dariadb::storage::bloom_empty<dariadb::Id>();

  std::fill(_buffer_t, _buffer_t + header->size, 0);
}

Chunk::~Chunk() {
  if (should_free) {
    delete header;
    delete[] _buffer_t;
  }
  this->bw = nullptr;
}

bool Chunk::check_id(const Id &id) {
  if (!dariadb::storage::bloom_check(header->id_bloom, id)) {
    return false;
  }
  return inInterval(header->minId, header->maxId, id);
}

bool Chunk::check_flag(const Flag &f) {
  if (f != 0) {
    if (!dariadb::storage::bloom_check(header->flag_bloom, f)) {
      return false;
    }
  }
  return true;
}

bool Chunk::check_checksum() {
  auto exists = get_checksum();
  auto calculated = calc_checksum();
  return exists == calculated;
}

ZippedChunk::ZippedChunk(ChunkHeader *index, uint8_t *buffer, size_t _size, Meas first_m)
    : Chunk(index, buffer, _size, first_m) {
  header->kind = CHUNK_KIND::Compressed;
  
  range = Range{_buffer_t, _buffer_t + index->size};
  bw = std::make_shared<ByteBuffer>(range);
  bw->reset_pos();

  header->bw_pos = uint32_t(bw->pos());

  c_writer = compression::v2::CopmressedWriter(bw);
  c_writer.append(header->first);

  header->id_bloom = dariadb::storage::bloom_add(header->id_bloom, first_m.id);
  header->flag_bloom = dariadb::storage::bloom_add(header->flag_bloom, first_m.flag);
}

ZippedChunk::ZippedChunk(ChunkHeader *index, uint8_t *buffer) : Chunk(index, buffer) {
  assert(index->kind == CHUNK_KIND::Compressed);
  range = Range{_buffer_t, _buffer_t + index->size};
  assert(size_t(range.end - range.begin) == index->size);
  bw = std::make_shared<ByteBuffer>(range);
  bw->set_pos(header->bw_pos);

  c_writer = compression::v2::CopmressedWriter(bw);
}

ZippedChunk::~ZippedChunk() {}

void ZippedChunk::close() {
  header->is_readonly = true;

  header->crc = this->calc_checksum();
  assert(header->crc != 0);
}

uint32_t ZippedChunk::calc_checksum() {
  return utils::crc32(this->_buffer_t, this->header->size);
}

uint32_t ZippedChunk::get_checksum() {
  return header->crc;
}

bool ZippedChunk::append(const Meas &m) {
  if (!header->is_init || header->is_readonly) {
    throw MAKE_EXCEPTION("(!is_not_free || is_readonly)");
  }

  auto t_f = this->c_writer.append(m);

  if (!t_f) {
    this->close();
    assert(c_writer.is_full());
    return false;
  } else {
    header->bw_pos = uint32_t(bw->pos());

    header->count++;
    header->minTime = std::min(header->minTime, m.time);
    header->maxTime = std::max(header->maxTime, m.time);
    header->minId = std::min(header->minId, m.id);
    header->maxId = std::max(header->maxId, m.id);
    header->flag_bloom = dariadb::storage::bloom_add(header->flag_bloom, m.flag);
    header->id_bloom = dariadb::storage::bloom_add(header->id_bloom, m.id);
    header->last = m;

    return true;
  }
}

class ZippedChunkReader : public Chunk::IChunkReader {
public:
  virtual Meas readNext() override {
    assert(!is_end());

    if (_is_first) {
      _is_first = false;
      return _chunk->header->first;
    }
    --count;
    return _reader->read();
  }

  bool is_end() const override { return count == 0 && !_is_first; }

  size_t count;
  bool _is_first = true;
  Chunk_Ptr _chunk;
  std::shared_ptr<ByteBuffer> bw;
  std::shared_ptr<CopmressedReader> _reader;
};

Chunk::ChunkReader_Ptr ZippedChunk::get_reader() {
  auto raw_res = new ZippedChunkReader;
  raw_res->count = this->header->count;
  raw_res->_chunk = this->shared_from_this();
  raw_res->_is_first = true;
  raw_res->bw = std::make_shared<compression::v2::ByteBuffer>(this->bw->get_range());
  raw_res->bw->reset_pos();
  raw_res->_reader = std::make_shared<CopmressedReader>(raw_res->bw, this->header->first);

  Chunk::ChunkReader_Ptr result{raw_res};
  return result;
}
