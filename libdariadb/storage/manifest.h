#pragma once

#include <libdariadb/meas.h>
#include <libdariadb/st_exports.h>
#include <libdariadb/storage/settings.h>
#include <libdariadb/utils/async/locker.h>
#include <list>
#include <memory>
#include <string>
#include <tuple>

namespace dariadb {
namespace storage {

const std::string MANIFEST_FILE_NAME = "Manifest";
class Manifest;
using Manifest_ptr = std::shared_ptr<Manifest>;
class Manifest {
public:
  EXPORT static Manifest_ptr create(const Settings_ptr &settings);
  EXPORT Manifest() = delete;
  EXPORT ~Manifest();

  EXPORT std::list<std::string> page_list();
  EXPORT void page_append(const std::string &rec);
  EXPORT void page_rm(const std::string &rec);

  EXPORT std::list<std::string> wal_list();
  EXPORT void wal_append(const std::string &rec);
  EXPORT void wal_rm(const std::string &rec);

  EXPORT void set_format(const std::string &version);
  EXPORT std::string get_format();

protected:
  EXPORT Manifest(const Settings_ptr &settings);
  class Private;
  std::unique_ptr<Private> _impl;
};
}
}
