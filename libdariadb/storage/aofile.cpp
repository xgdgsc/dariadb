#include "aofile.h"
#include "../flags.h"
#include "../utils/fs.h"
#include "../utils/metrics.h"
#include "callbacks.h"
#include "manifest.h"
#include "options.h"
#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <mutex>

using namespace dariadb;
using namespace dariadb::storage;

class AOFile::Private {
public:
  Private() {
    _writed = 0;
    _is_readonly = false;
    auto rnd_fname = utils::fs::random_file_name(AOF_FILE_EXT);
    _filename = utils::fs::append_path(Options::instance()->path, rnd_fname);
    Manifest::instance()->aof_append(rnd_fname);
    is_full = false;
  }

  Private(const std::string &fname, bool readonly) {
    _writed = AOFile::writed(fname);
    _is_readonly = readonly;
    _filename = fname;
    is_full = false;
  }

  ~Private() { this->flush(); }

  FILE *open_to_append() const {
    auto file = std::fopen(_filename.c_str(), "ab");
    if (file == nullptr) {
      throw MAKE_EXCEPTION("aofile: open_to_append error.");
    }
    return file;
  }

  FILE *open_to_read() const {
    auto file = std::fopen(_filename.c_str(), "rb");
    if (file == nullptr) {
      throw_open_error_exception();
    }
    return file;
  }

  append_result append(const Meas &value) {
    TIMECODE_METRICS(ctmd, "append", "AOFile::append");
    assert(!_is_readonly);

    if (_writed > Options::instance()->aof_max_size) {
      return append_result(0, 1);
    }
    auto file = open_to_append();
    std::fwrite(&value, sizeof(Meas), size_t(1), file);
    std::fclose(file);
    _writed++;
    return append_result(1, 0);
  }

  append_result append(const MeasArray::const_iterator &begin,
                       const MeasArray::const_iterator &end) {
    TIMECODE_METRICS(ctmd, "append", "AOFile::append(ma)");
    assert(!_is_readonly);

    auto sz = std::distance(begin, end);
    if (is_full) {
      return append_result(0, sz);
    }
    auto file = open_to_append();
    auto max_size = Options::instance()->aof_max_size;
    auto write_size = (sz + _writed) > max_size ? (max_size - _writed) : sz;
    std::fwrite(&(*begin), sizeof(Meas), write_size, file);
    std::fclose(file);
    _writed += write_size;
    return append_result(write_size, 0);
  }

  append_result append(const MeasList::const_iterator &begin,
                       const MeasList::const_iterator &end) {
    TIMECODE_METRICS(ctmd, "append", "AOFile::append(ml)");
    assert(!_is_readonly);

    auto list_size = std::distance(begin, end);
    if (is_full) {
      return append_result(0, list_size);
    }
    auto file = open_to_append();

    auto max_size = Options::instance()->aof_max_size;

    auto write_size = (list_size + _writed) > max_size ? (max_size - _writed) : list_size;
    MeasArray ma{begin, end};
    std::fwrite(ma.data(), sizeof(Meas), write_size, file);
    std::fclose(file);
    _writed += write_size;
    return append_result(write_size, 0);
  }

  void foreach (const QueryInterval &q, IReaderClb * clbk) {
    TIMECODE_METRICS(ctmd, "foreach", "AOFile::foreach");

    auto file = open_to_read();

    while (1) {
      Meas val = Meas::empty();
      if (fread(&val, sizeof(Meas), size_t(1), file) == 0) {
        break;
      }
      if (val.inQuery(q.ids, q.flag, q.from, q.to)) {
        clbk->call(val);
      }
    }
    std::fclose(file);
  }

  Id2Meas readTimePoint(const QueryTimePoint &q) {
    TIMECODE_METRICS(ctmd, "readTimePoint", "AOFile::readTimePoint");

    dariadb::IdSet readed_ids;
    dariadb::Id2Meas sub_res;

    auto file = open_to_read();

    while (1) {
      Meas val = Meas::empty();
      if (fread(&val, sizeof(Meas), size_t(1), file) == 0) {
        break;
      }
      if (val.inQuery(q.ids, q.flag) && (val.time <= q.time_point)) {
        replace_if_older(sub_res, val);
        readed_ids.insert(val.id);
      }
    }
    std::fclose(file);

    if (!q.ids.empty() && readed_ids.size() != q.ids.size()) {
      for (auto id : q.ids) {
        if (readed_ids.find(id) == readed_ids.end()) {
          auto e = Meas::empty(id);
          e.flag = Flags::_NO_DATA;
          e.time = q.time_point;
          sub_res[id] = e;
        }
      }
    }

    return sub_res;
  }

  void replace_if_older(dariadb::Id2Meas &s, const dariadb::Meas &m) const {
    auto fres = s.find(m.id);
    if (fres == s.end()) {
      s.insert(std::make_pair(m.id, m));
    } else {
      if (fres->second.time < m.time) {
        s.insert(std::make_pair(m.id, m));
      }
    }
  }

  Id2Meas currentValue(const IdArray &ids, const Flag &flag) {
    dariadb::Id2Meas sub_res;
    dariadb::IdSet readed_ids;

    auto file = open_to_read();
    while (1) {
      Meas val = Meas::empty();
      if (fread(&val, sizeof(Meas), size_t(1), file) == 0) {
        break;
      }
      if (val.inFlag(flag) && val.inIds(ids)) {
        replace_if_older(sub_res, val);
        readed_ids.insert(val.id);
      }
    }
    std::fclose(file);

    if (!ids.empty() && readed_ids.size() != ids.size()) {
      for (auto id : ids) {
        if (readed_ids.find(id) == readed_ids.end()) {
          auto e = Meas::empty(id);
          e.flag = Flags::_NO_DATA;
          e.time = dariadb::Time(0);
          sub_res[id] = e;
        }
      }
    }

    return sub_res;
  }

  dariadb::Time minTime() const {
    auto file = open_to_read();

    dariadb::Time result = dariadb::MAX_TIME;

    while (1) {
      Meas val = Meas::empty();
      if (fread(&val, sizeof(Meas), size_t(1), file) == 0) {
        break;
      }
      result = std::min(val.time, result);
    }
    std::fclose(file);
    return result;
  }

  dariadb::Time maxTime() const {
    auto file = open_to_read();

    dariadb::Time result = dariadb::MIN_TIME;

    while (1) {
      Meas val = Meas::empty();
      if (fread(&val, sizeof(Meas), size_t(1), file) == 0) {
        break;
      }
      result = std::max(val.time, result);
    }
    std::fclose(file);
    return result;
  }

  bool minMaxTime(dariadb::Id id, dariadb::Time *minResult, dariadb::Time *maxResult) {
    TIMECODE_METRICS(ctmd, "minMaxTime", "AOFile::minMaxTime");
    auto file = open_to_read();

    *minResult = dariadb::MAX_TIME;
    *maxResult = dariadb::MIN_TIME;
    bool result = false;
    while (1) {
      Meas val = Meas::empty();
      if (fread(&val, sizeof(Meas), size_t(1), file) == 0) {
        break;
      }
      if (val.id == id) {
        result = true;
        *minResult = std::min(*minResult, val.time);
        *maxResult = std::max(*maxResult, val.time);
      }
    }
    std::fclose(file);
    return result;
  }

  void flush() { TIMECODE_METRICS(ctmd, "flush", "AOFile::flush"); }

  std::string filename() const { return _filename; }

  MeasArray readAll() {
    TIMECODE_METRICS(ctmd, "drop", "AOFile::drop");
    auto file = open_to_read();

    MeasArray ma(Options::instance()->aof_max_size);
    size_t pos = 0;
    while (1) {
      Meas val = Meas::empty();
      if (fread(&val, sizeof(Meas), size_t(1), file) == 0) {
        break;
      }
      ma[pos] = val;
      pos++;
    }
    std::fclose(file);
    return ma;
  }

  [[noreturn]]
  void throw_open_error_exception() const {
    std::stringstream ss;
    ss << "aof: file open error " << _filename;
    auto aofs_manifest = Manifest::instance()->aof_list();
    ss << "Manifest:";
    for (auto f : aofs_manifest) {
      ss << f << std::endl;
    }
    auto aofs_exists = utils::fs::ls(Options::instance()->path, ".aof");
    for (auto f : aofs_exists) {
      ss << f << std::endl;
    }
    throw MAKE_EXCEPTION(ss.str());
  }

protected:
  std::string _filename;
  bool _is_readonly;
  size_t _writed;
  bool is_full;
};

AOFile::~AOFile() {}

AOFile::AOFile() : _Impl(new AOFile::Private()) {}

AOFile::AOFile(const std::string &fname, bool readonly)
    : _Impl(new AOFile::Private(fname, readonly)) {}

dariadb::Time AOFile::minTime() {
  return _Impl->minTime();
}

dariadb::Time AOFile::maxTime() {
  return _Impl->maxTime();
}

bool AOFile::minMaxTime(dariadb::Id id, dariadb::Time *minResult,
                        dariadb::Time *maxResult) {
  return _Impl->minMaxTime(id, minResult, maxResult);
}
void AOFile::flush() { // write all to storage;
  _Impl->flush();
}

append_result AOFile::append(const Meas &value) {
  return _Impl->append(value);
}
append_result AOFile::append(const MeasArray::const_iterator &begin,
                             const MeasArray::const_iterator &end) {
  return _Impl->append(begin, end);
}
append_result AOFile::append(const MeasList::const_iterator &begin,
                             const MeasList::const_iterator &end) {
  return _Impl->append(begin, end);
}

void AOFile::foreach (const QueryInterval &q, IReaderClb * clbk) {
  return _Impl->foreach (q, clbk);
}

Id2Meas AOFile::readTimePoint(const QueryTimePoint &q) {
  return _Impl->readTimePoint(q);
}

Id2Meas AOFile::currentValue(const IdArray &ids, const Flag &flag) {
  return _Impl->currentValue(ids, flag);
}

std::string AOFile::filename() const {
  return _Impl->filename();
}

MeasArray AOFile::readAll() {
  return _Impl->readAll();
}

size_t AOFile::writed(std::string fname) {
  TIMECODE_METRICS(ctmd, "read", "AOFile::writed");
  std::ifstream in(fname, std::ifstream::ate | std::ifstream::binary);
  return in.tellg() / sizeof(Meas);
}
