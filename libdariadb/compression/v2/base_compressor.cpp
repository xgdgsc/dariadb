#include "base_compressor.h"

using namespace dariadb::compression::v2;

BaseCompressor::BaseCompressor(const ByteBuffer_Ptr &bw) : _bw(bw) {}
