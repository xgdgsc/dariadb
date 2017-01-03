#pragma once

#include <libdariadb/st_exports.h>
#include <libdariadb/interfaces/imeasstorage.h>
#include <libdariadb/interfaces/ichunkcontainer.h>
#include <libdariadb/meas.h>
#include <libdariadb/storage/chunk.h>
#include <libdariadb/storage/engine_environment.h>
#include <libdariadb/storage/memstorage/allocators.h>
#include <libdariadb/storage/memstorage/description.h>
#include <memory>

namespace dariadb {
namespace storage {

class MemStorage : public IMeasStorage {
public:

public:
  /// id_count - for prealloc
  EXPORT MemStorage(const EngineEnvironment_ptr &env, size_t id_count);
  EXPORT ~MemStorage();
  EXPORT memstorage::Description description()const;

  // Inherited via IMeasStorage
  EXPORT virtual Time minTime() override;
  EXPORT virtual Time maxTime() override;
  EXPORT virtual bool minMaxTime(dariadb::Id id,
                                             dariadb::Time *minResult,
                                             dariadb::Time *maxResult) override;
  EXPORT virtual void foreach (const QueryInterval &q,
                                           IReaderClb * clbk) override;
  EXPORT virtual Id2Meas readTimePoint(const QueryTimePoint &q) override;
  EXPORT virtual Id2Meas currentValue(const IdArray &ids,
                                                  const Flag &flag) override;
  using IMeasStorage::append;
  EXPORT Status  append(const Meas &value) override;
  EXPORT void flush() override;
  EXPORT void setDownLevel(IChunkStorage*_down);
  EXPORT void setDiskStorage(IMeasWriter*_disk); //when strategy==CACHE;
  EXPORT void stop();
  EXPORT std::mutex* getLockers();
  EXPORT Id2MinMax loadMinMax()override;
  EXPORT Id2Time getSyncMap(); ///Id to max dropped to disk time.
private:
  struct Private;
  std::unique_ptr<Private> _impl;
};
using MemStorage_ptr = std::shared_ptr<MemStorage>;
}
}
