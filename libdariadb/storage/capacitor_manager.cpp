#include "capacitor_manager.h"
#include "../flags.h"
#include "../timeutil.h"
#include "../utils/exception.h"
#include "../utils/fs.h"
#include "../utils/metrics.h"
#include "../utils/thread_manager.h"
#include "callbacks.h"
#include "manifest.h"
#include "options.h"
#include <cassert>

using namespace dariadb::storage;
using namespace dariadb;
using namespace dariadb::utils::async;

CapacitorManager *CapacitorManager::_instance = nullptr;

CapacitorManager::~CapacitorManager() {
  STRATEGY strat = Options::instance()->strategy;
  auto period = Options::instance()->cap_store_period;
  if (strat == STRATEGY::DYNAMIC && period != 0) {
    this->period_worker_stop();
  }
}

CapacitorManager::CapacitorManager()
    : utils::PeriodWorker(std::chrono::milliseconds(1 * 1000)) {
  _down = nullptr;

  /// open last not closed file.normally do nothing,
  /// because engine use bulk loading and file or not exists or full.
  auto files = cap_files();
  for (auto f : files) {
    try {
      auto hdr = Capacitor::readHeader(f);
      if (!hdr.is_full) {
        _cap = Capacitor_Ptr{new Capacitor(f)};
      }
      _file2header.insert(std::make_pair(utils::fs::extract_filename(f), hdr));
    } catch (utils::Exception &ex) {
      throw MAKE_EXCEPTION(ex.what());
    }
  }
  STRATEGY strat = Options::instance()->strategy;
  auto period = Options::instance()->cap_store_period;
  if (strat == STRATEGY::DYNAMIC && period != 0) {
    this->period_worker_start();
  }
}

void CapacitorManager::fsck(bool force_check) {
  auto files = cap_files();
  for (auto f : files) {
    try {
      auto hdr = Capacitor::readHeader(f);
      if (force_check || (!hdr.is_closed && hdr.is_open_to_write)) {
        auto c = Capacitor_Ptr{new Capacitor(f)};
        c->fsck();
      }
    } catch (utils::Exception &ex) {
      throw MAKE_EXCEPTION(ex.what());
    }
  }
}

void CapacitorManager::start() {
  if (CapacitorManager::_instance == nullptr) {
    CapacitorManager::_instance = new CapacitorManager();
  } else {
    throw MAKE_EXCEPTION("CapacitorManager::start started twice.");
  }
}

void CapacitorManager::stop() {
  delete CapacitorManager::_instance;
  CapacitorManager::_instance = nullptr;
}

CapacitorManager *CapacitorManager::instance() {
  return CapacitorManager::_instance;
}

/// perid_worker callback
void CapacitorManager::period_call() {
  auto closed = this->closed_caps();
  auto max_hdr_time =
      dariadb::timeutil::current_time() - Options::instance()->cap_store_period;
  for (auto &fname : closed) {
    try {
      auto without_path = utils::fs::extract_filename(fname);
      if (_files_send_to_drop.find(without_path) == _files_send_to_drop.end()) {
        Capacitor::Header hdr = Capacitor::readHeader(fname);
        if (hdr.maxTime < max_hdr_time) {
          this->drop_cap(fname);
        }
      }
    } catch (utils::Exception &ex) {
      throw MAKE_EXCEPTION(ex.what());
    }
  }
  clear_files_to_send();
}

Capacitor_Ptr CapacitorManager::create_new(std::string filename) {
  TIMECODE_METRICS(ctm, "create", "CapacitorManager::create_new");
  if (_cap != nullptr) {
    std::lock_guard<utils::Locker> lg(_cache_locker);
    _file2header[_cap->file_name()] = *_cap->header();
  }
  _cap = nullptr;
  if (_down != nullptr) {
    auto closed = this->closed_caps();

    switch (Options::instance()->strategy) {
    case STRATEGY::COMPRESSED: {
      size_t to_drop = closed.size();
      drop_closed_unsafe(to_drop);
      break;
    }
    case STRATEGY::DYNAMIC: {
      if (closed.size() > Options::instance()->cap_max_closed_caps &&
          Options::instance()->cap_max_closed_caps > 0 &&
          Options::instance()->cap_store_period == 0) {
        size_t to_drop = closed.size() - Options::instance()->cap_max_closed_caps;
        drop_closed_unsafe(to_drop);
      }
      break;
    }
    default:
      break;
    };
  }
  std::lock_guard<utils::Locker> lg(_cache_locker);
  auto result = Capacitor_Ptr{new Capacitor(filename)};
  _file2header[filename] = *result->header();
  return result;
}

Capacitor_Ptr CapacitorManager::create_new() {
  return create_new(Capacitor::rnd_file_name());
}

std::list<std::string> CapacitorManager::cap_files() const {
  std::list<std::string> res;
  auto files = Manifest::instance()->cola_list();
  for (auto f : files) {
    auto full_path = utils::fs::append_path(Options::instance()->path, f);
    res.push_back(full_path);
  }
  return res;
}

std::list<std::string>
CapacitorManager::caps_by_filter(std::function<bool(const Capacitor::Header &)> pred) {
  std::lock_guard<utils::Locker> lg(_cache_locker);
  std::list<std::string> result;

  auto mnfst = Manifest::instance()->cola_list();
  if (mnfst.size() != _file2header.size()) {
    THROW_EXCEPTION("mnfst.size(",mnfst.size() , ") != _file2header.size(",_file2header.size() , ")");
  }

  for (auto f2h : _file2header) {
    if (pred(f2h.second)) {
      result.push_back(utils::fs::append_path(Options::instance()->path, f2h.first));
    }
  }

  return result;
}

std::list<std::string> CapacitorManager::closed_caps() {
  auto pred = [](const Capacitor::Header &hdr) { return hdr.is_full; };

  auto files = caps_by_filter(pred);
  return files;
}

void CapacitorManager::drop_cap(const std::string &fname) {
  auto without_path = utils::fs::extract_filename(fname);
  _files_send_to_drop.insert(without_path);
  _down->drop_cap(fname);
}

void CapacitorManager::drop_closed_unsafe(size_t count) {
  TIMECODE_METRICS(ctmd, "drop", "CapacitorManager::drop_part");
  auto closed = this->closed_caps();
  using FileWithHeader = std::tuple<std::string, Capacitor::Header>;
  std::vector<FileWithHeader> file2headers{closed.size()};

  size_t pos = 0;
  for (auto f : closed) {
    try {
      auto without_path = utils::fs::extract_filename(f);
      if (_files_send_to_drop.find(without_path) == _files_send_to_drop.end()) {
        auto cheader = Capacitor::readHeader(f);
        file2headers[pos] = std::tie(f, cheader);
        ++pos;
      }
    } catch (utils::Exception &ex) {
      throw MAKE_EXCEPTION(ex.what());
    }
  }
  std::sort(file2headers.begin(), file2headers.begin() + pos,
            [](const FileWithHeader &l, const FileWithHeader &r) {
              return std::get<1>(l).minTime < std::get<1>(r).minTime;
            });
  auto drop_count = std::min(pos, count);

  for (size_t i = 0; i < drop_count; ++i) {
    std::string f = std::get<0>(file2headers[i]);
    this->drop_cap(f);
  }

  clear_files_to_send();
}

void CapacitorManager::clear_files_to_send() {
  auto caps_exists = Manifest::instance()->cola_list();
  std::unordered_set<std::string> caps_exists_set{caps_exists.begin(), caps_exists.end()};
  std::unordered_set<std::string> new_sended_files;
  for (auto &v : _files_send_to_drop) {
    if (caps_exists_set.find(v) != caps_exists_set.end()) {
      new_sended_files.insert(v);
    }
  }
  _files_send_to_drop = new_sended_files;
}

void CapacitorManager::drop_closed_files(size_t count) {
  drop_closed_unsafe(count);
}

dariadb::Time CapacitorManager::minTime() {
  auto files = cap_files();
  dariadb::Time result = dariadb::MAX_TIME;
  for (auto filename : files) {
    try {
      auto local = Capacitor::readHeader(filename).minTime;
      result = std::min(local, result);
    } catch (utils::Exception &ex) {
      throw MAKE_EXCEPTION(ex.what());
    }
  }
  return result;
}

dariadb::Time CapacitorManager::maxTime() {
  auto files = cap_files();
  dariadb::Time result = dariadb::MIN_TIME;
  for (auto filename : files) {
    try {
      auto local = Capacitor::readHeader(filename).maxTime;
      result = std::max(local, result);
    } catch (utils::Exception &ex) {
      throw MAKE_EXCEPTION(ex.what());
    }
  }
  return result;
}

bool CapacitorManager::minMaxTime(dariadb::Id id, dariadb::Time *minResult,
                                  dariadb::Time *maxResult) {
  TIMECODE_METRICS(ctmd, "minMaxTime", "CapacitorManager::minMaxTime");
  auto files = cap_files();
  using MMRes = std::tuple<bool, dariadb::Time, dariadb::Time>;
  std::vector<MMRes> results{files.size()};
  std::vector<TaskResult_Ptr> task_res{files.size()};
  size_t num = 0;

  for (auto filename : files) {
    AsyncTask at = [filename, &results, num, id](const ThreadInfo &ti) {
      TKIND_CHECK(THREAD_COMMON_KINDS::FILE_READ, ti.kind);
      auto raw = new Capacitor(filename, true);
      Capacitor_Ptr cptr{raw};
      dariadb::Time lmin = dariadb::MAX_TIME, lmax = dariadb::MIN_TIME;
      if (cptr->minMaxTime(id, &lmin, &lmax)) {
        results[num] = MMRes(true, lmin, lmax);
      } else {
        results[num] = MMRes(false, lmin, lmax);
      }
    };
    task_res[num] =
        ThreadManager::instance()->post(THREAD_COMMON_KINDS::FILE_READ, AT(at));
    num++;
  }

  for (auto &tw : task_res) {
    tw->wait();
  }

  bool res = false;

  *minResult = dariadb::MAX_TIME;
  *maxResult = dariadb::MIN_TIME;
  for (auto &subRes : results) {
    if (std::get<0>(subRes)) {
      res = true;
      *minResult = std::min(std::get<1>(subRes), *minResult);
      *maxResult = std::max(std::get<2>(subRes), *maxResult);
    }
  }

  return res;
}

void CapacitorManager::foreach (const QueryInterval &q, IReaderClb * clbk) {
  TIMECODE_METRICS(ctmd, "foreach", "CapacitorManager::foreach");
  auto pred = [q](const Capacitor::Header &hdr) {

    bool flag_exists = hdr.check_flag(q.flag);
    if (!flag_exists) {
      return false;
    }

    auto interval_check((hdr.minTime >= q.from && hdr.maxTime <= q.to) ||
                        (utils::inInterval(q.from, q.to, hdr.minTime)) ||
                        (utils::inInterval(q.from, q.to, hdr.maxTime)) ||
                        (utils::inInterval(hdr.minTime, hdr.maxTime, q.from)) ||
                        (utils::inInterval(hdr.minTime, hdr.maxTime, q.to)));

    if (!interval_check) {
      return false;
    }
    if (!hdr.check_id(q.ids)) {
      return false;
    } else {
      return true;
    }
  };

  auto files = caps_by_filter(pred);

  std::vector<TaskResult_Ptr> task_res{files.size()};
  size_t num = 0;
  for (auto filename : files) {
    AsyncTask at = [filename, &q, &clbk](const ThreadInfo &ti) {
      TKIND_CHECK(THREAD_COMMON_KINDS::FILE_READ, ti.kind);
      std::unique_ptr<Capacitor> cap{new Capacitor(filename, true)};
      cap->foreach (q, clbk);
    };
    task_res[num] =
        ThreadManager::instance()->post(THREAD_COMMON_KINDS::FILE_READ, AT(at));
    num++;
  }

  for (auto &tw : task_res) {
    tw->wait();
  }
}

Id2Meas CapacitorManager::readTimePoint(const QueryTimePoint &query) {
  TIMECODE_METRICS(ctmd, "readTimePoint", "CapacitorManager::readTimePoint");
  auto pred = [query](const Capacitor::Header &hdr) {
    if (!hdr.check_flag(query.flag)) {
      return false;
    }

    auto interval_check = hdr.maxTime < query.time_point;
    if (!interval_check) {
      return false;
    }

    if (!hdr.check_id(query.ids)) {
      return false;
    }
    return true;
  };

  auto files = caps_by_filter(pred);

  dariadb::Id2Meas sub_result;

  for (auto id : query.ids) {
    sub_result[id].flag = Flags::_NO_DATA;
    sub_result[id].time = query.time_point;
  }

  std::vector<Id2Meas> results{files.size()};
  std::vector<TaskResult_Ptr> task_res{files.size()};

  size_t num = 0;
  for (auto filename : files) {
    AsyncTask at = [filename, &query, num, &results](const ThreadInfo &ti) {
      TKIND_CHECK(THREAD_COMMON_KINDS::FILE_READ, ti.kind);
      std::unique_ptr<Capacitor> cap{new Capacitor(filename, true)};
      results[num] = cap->readTimePoint(query);
    };
    task_res[num] =
        ThreadManager::instance()->post(THREAD_COMMON_KINDS::FILE_READ, AT(at));
    num++;
  }

  for (auto &tw : task_res) {
    tw->wait();
  }

  for (auto &out : results) {
    for (auto &kv : out) {
      auto it = sub_result.find(kv.first);
      if (it == sub_result.end()) {
        sub_result.insert(std::make_pair(kv.first, kv.second));
      } else {
        if (it->second.flag == Flags::_NO_DATA) {
          sub_result[kv.first] = kv.second;
        }
      }
    }
  }
  return sub_result;
}

Id2Meas CapacitorManager::currentValue(const IdArray &ids, const Flag &flag) {
  TIMECODE_METRICS(ctmd, "currentValue", "CapacitorManager::currentValue");
  auto files = cap_files();

  dariadb::Id2Meas meases;
  for (const auto &f : files) {
    auto c = Capacitor_Ptr{new Capacitor(f, true)};
    auto out = c->currentValue(ids, flag);

    for (auto &kv : out) {
      auto it = meases.find(kv.first);
      if (it == meases.end()) {
        meases.insert(std::make_pair(kv.first, kv.second));
      } else {
        if (it->second.flag == Flags::_NO_DATA) {
          meases[kv.first] = kv.second;
        }
      }
    }
  }
  return meases;
}

void CapacitorManager::append(std::string filename, const MeasArray &ma) {
  TIMECODE_METRICS(ctmd, "append", "CapacitorManager::append(std::string filename)");
  auto target = create_new(filename);
  target->append(ma.begin(), ma.end());
  target->close();

  auto hdr = *(target->header());
  _file2header[target->file_name()] = hdr;
  target = nullptr;
}

dariadb::append_result CapacitorManager::append(const Meas &value) {
  TIMECODE_METRICS(ctmd, "append", "CapacitorManager::append");
  if (_cap == nullptr) {
    _cap = create_new();
  }
  auto res = _cap->append(value);
  if (res.writed != 1) {
    _cap = create_new();
    res = _cap->append(value);
  }
#ifdef DEBUG
  auto mnfst = Manifest::instance()->cola_list();
#endif
  _file2header[_cap->file_name()] = *(_cap->header());
#ifdef DEBUG
  assert(mnfst.size() == _file2header.size());
#endif
  return res;
}

void CapacitorManager::flush() {
  TIMECODE_METRICS(ctmd, "flush", "CapacitorManager::flush");
}

size_t CapacitorManager::files_count() const {
  return cap_files().size();
}

void CapacitorManager::erase(const std::string &fname) {
  if (CapacitorManager::instance() != nullptr) {
    std::lock_guard<utils::Locker> lg(CapacitorManager::instance()->_cache_locker);
    CapacitorManager::instance()->_file2header.erase(fname);

    auto capf = utils::fs::append_path(Options::instance()->path, fname);
    dariadb::utils::fs::rm(capf);
    Manifest::instance()->cola_rm(fname);
  } else {
    auto capf = utils::fs::append_path(Options::instance()->path, fname);
    dariadb::utils::fs::rm(capf);
    Manifest::instance()->cola_rm(fname);
  }
}
