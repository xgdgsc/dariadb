#include <extern/json/src/json.hpp>
#include <fstream>
#include <libdariadb/meas.h>
#include <libdariadb/storage/settings.h>
#include <libdariadb/utils/exception.h>
#include <libdariadb/utils/fs.h>
#include <libdariadb/utils/logger.h>

using namespace dariadb::storage;
using json = nlohmann::json;

const size_t AOF_BUFFER_SIZE = 2000;
const size_t AOF_FILE_SIZE = sizeof(dariadb::Meas) * AOF_BUFFER_SIZE * 4;
const uint32_t OPENNED_PAGE_CACHE_SIZE = 10;
const uint32_t CHUNK_SIZE = 1024;

std::string settings_file_path(const std::string &path) {
  return dariadb::utils::fs::append_path(path, SETTINGS_FILE_NAME);
}

Settings::Settings(const std::string storage_path) {
  path = storage_path;
  auto f = settings_file_path(path);
  if (utils::fs::path_exists(f)) {
    load(f);
  } else {
	dariadb::utils::fs::mkdir(path);
    set_default();
    save();
  }
}

void Settings::set_default() {
  logger("engine: Settings set default Settings");
  aof_buffer_size = AOF_BUFFER_SIZE;
  aof_max_size = AOF_FILE_SIZE;
  page_chunk_size = CHUNK_SIZE;
  page_openned_page_cache_size = OPENNED_PAGE_CACHE_SIZE;

  strategy = STRATEGY::COMPRESSED;
}

std::vector<dariadb::utils::async::ThreadPool::Params> Settings::thread_pools_params() {
  using namespace dariadb::utils::async;
  std::vector<ThreadPool::Params> result{
      ThreadPool::Params{size_t(4), (ThreadKind)THREAD_COMMON_KINDS::COMMON},
      ThreadPool::Params{size_t(1), (ThreadKind)THREAD_COMMON_KINDS::DISK_IO},
      ThreadPool::Params{size_t(1), (ThreadKind)THREAD_COMMON_KINDS::DROP}};
  return result;
}

void Settings::save() {
  save(settings_file_path(this->path));
}

void Settings::save(const std::string &file) {
  logger("engine: Settings save to ", file);
  json js;

  js["aof_max_size"] = aof_max_size;
  js["aof_buffer_size"] = aof_buffer_size;

  js["page_chunk_size"] = page_chunk_size;
  js["page_openned_page_cache_size"] = page_openned_page_cache_size;

  std::stringstream ss;
  ss << strategy;
  js["stragety"] = ss.str();

  std::fstream fs;
  fs.open(file, std::ios::out);
  if (!fs.is_open()) {
    throw MAKE_EXCEPTION("!fs.is_open()");
  }
  fs << js.dump();
  fs.flush();
  fs.close();
}

void Settings::load(const std::string &file) {
  logger("engine: Settings loading ", file);
  std::string content = dariadb::utils::fs::read_file(file);
  json js = json::parse(content);
  aof_max_size = js["aof_max_size"];
  aof_buffer_size = js["aof_buffer_size"];

  page_chunk_size = js["page_chunk_size"];
  page_openned_page_cache_size = js["page_openned_page_cache_size"];

  std::istringstream iss;
  std::string strat_str = js["stragety"];
  iss.str(strat_str);
  iss >> strategy;
}