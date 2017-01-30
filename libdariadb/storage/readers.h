#pragma once

#include <libdariadb/interfaces/ireader.h>

namespace dariadb {
namespace storage {
class FullReader : public IReader {
public:
  EXPORT FullReader(MeasArray &ml);
  EXPORT virtual Meas readNext() override;

  EXPORT bool is_end() const override;

  EXPORT Meas top() override;

  MeasArray _ma;
  size_t _index;
};

class MergeSortReader : public IReader {
public:
  EXPORT MergeSortReader(const std::list<Reader_Ptr> &readers);
  EXPORT virtual Meas readNext() override;
  EXPORT bool is_end() const override;
  EXPORT Meas top() override;
  EXPORT static Id2Reader colapseReaders(const Id2ReadersList &i2r);
  const std::list<Reader_Ptr> _readers;
};
}
}