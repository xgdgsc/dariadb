#pragma once

#include <libdariadb/utils/async/locker.h>
#include <libdariadb/st_exports.h>
#include <list>
#include <memory>
#include <string>

namespace dariadb {
namespace storage {

const std::string MANIFEST_FILE_NAME = "Manifest";

class Manifest {
public:
  EXPORT Manifest() = delete;
  EXPORT Manifest(const std::string &fname);
  EXPORT ~Manifest();

  EXPORT std::list<std::string> page_list();
  EXPORT void page_append(const std::string &rec);
  EXPORT void page_rm(const std::string &rec);

  EXPORT std::list<std::string> aof_list();
  EXPORT void aof_append(const std::string &rec);
  EXPORT void aof_rm(const std::string &rec);

  EXPORT void set_version(const std::string &version);
  EXPORT std::string get_version();

protected:
	class Private;
	std::unique_ptr<Private> _impl;
};

using Manifest_ptr = std::shared_ptr<Manifest>;
}
}
