#pragma once

#include "../meas.h"
#include "../storage.h"
#include "../utils/period_worker.h"
#include <memory>

namespace dariadb {
namespace storage {

const std::string CAP_FILE_EXT = ".aof"; //append-only-file
const size_t CAP_DEFAULT_MAX_LEVELS = 10;

class Capacitor : public utils::NonCopy, public MeasStorage {
public:
  struct Params {
    size_t B; // measurements count in one datra block
    std::string path;
    size_t max_levels;
    Params(const size_t _B, const std::string _path) {
      B = _B;
      path = _path;
      max_levels = CAP_DEFAULT_MAX_LEVELS;
    }
  };
  virtual ~Capacitor();
  Capacitor(const BaseStorage_ptr stor, const Params &param);

  append_result append(const Meas &value) override;
  virtual Reader_ptr readInterval(Time from, Time to) override;
  virtual Reader_ptr readInTimePoint(Time time_point) override;
  virtual Reader_ptr readInterval(const IdArray &ids, Flag flag, Time from,
                                  Time to) override;
  virtual Reader_ptr readInTimePoint(const IdArray &ids, Flag flag,
                                     Time time_point) override;

  dariadb::Time minTime() override;
  dariadb::Time maxTime() override;

  bool flush(); // write all to storage;

  size_t in_queue_size() const;
  size_t levels_count() const;
size_t size()const;
protected:
  class Private;
  std::unique_ptr<Private> _Impl;

  // Inherited via MeasStorage
};
}
}
