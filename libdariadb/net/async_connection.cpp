#include "async_connection.h"
#include "../utils/exception.h"

#include <functional>

using namespace std::placeholders;

using namespace boost::asio;

using namespace dariadb;
using namespace dariadb::net;

AsyncConnection::AsyncConnection() {
  _stoped = true;
}

AsyncConnection::~AsyncConnection() noexcept(false) {
	full_stop();
}

void AsyncConnection::start(const socket_ptr &sock) {
  if (!_stoped) {
    return;
  }
  _current_query = nullptr;
  _sock = sock;
  _stoped = false;
  _stop_flag = false;
  _thread_handler = std::thread(&AsyncConnection::queue_thread, this);
  readNextAsync();
}

void AsyncConnection::readNextAsync() {
	if (auto spt = _sock.lock()) {
		spt->async_read_some(buffer(this->marker_read_buffer, MARKER_SIZE),
			std::bind(&AsyncConnection::onReadMarker, this, _1, _2));
	}
}

void AsyncConnection::mark_stoped() {
  _stop_flag = true;
}

void AsyncConnection::full_stop() {
	mark_stoped();
	if (!_stoped) {
		_cond.notify_all();
		while (queue_size() != 0) {
			_cond.notify_all();
		}
		_thread_handler.join();
		_stoped = true;
	}
}

void AsyncConnection::send(const NetData_ptr &d) {
  if (!_stop_flag) {
    std::unique_lock<std::mutex> lock(_ac_locker);
    _queries.push_back(d);
    _cond.notify_one();
  }
}

void AsyncConnection::queue_thread() {
  while (true) {
    std::unique_lock<std::mutex> lock(_ac_locker);
	_cond.wait(lock,[&]() { return _stop_flag || (!_queries.empty() && _current_query == nullptr); });

	if (_stop_flag && _queries.empty()) {
      break;
    }

    if (_queries.empty() || _current_query!=nullptr) {
      continue;
	}
	else {
		_current_query = _queries.front();
		_queries.pop_front();
	}
    memcpy(marker_buffer, &(_current_query->size), MARKER_SIZE);
	if (auto spt = _sock.lock()) {
		async_write(*spt.get(), buffer(marker_buffer, MARKER_SIZE),
			std::bind(&AsyncConnection::onMarkerSended, this, _1, _2));
	}
  }
}

void AsyncConnection::onMarkerSended(const boost::system::error_code &err,
                                     size_t read_bytes) {
  logger_info("AsyncConnection::onMarkerSended");
  if (err) {
    this->onNetworkError(err);
  } else {
	  if (auto spt = _sock.lock()) {
		  async_write(*spt.get(), buffer(_current_query->data, _current_query->size),
			  std::bind(&AsyncConnection::onDataSended, this, _1, _2));
	  }
  }
}

void AsyncConnection::onDataSended(const boost::system::error_code &err,
                                   size_t read_bytes) {
  logger_info("AsyncConnection::onDataSended");
  if (err) {
    this->onNetworkError(err);
  } else {
	_current_query = nullptr;
  }
}

void AsyncConnection::onReadMarker(const boost::system::error_code &err,
                                   size_t read_bytes) {
  logger_info("AsyncConnection::onReadMarker");
  if (err) {
    this->onNetworkError(err);
  } else {
    if (read_bytes != MARKER_SIZE) {
      THROW_EXCEPTION_SS("AsyncConnection::onReadMarker - wrong marker size: expected "
                         << MARKER_SIZE << " readed " << read_bytes);
    }
    uint64_t *data_size_ptr = reinterpret_cast<uint64_t *>(marker_read_buffer);

    data_read_buffer_size = *data_size_ptr;
    data_read_buffer = new uint8_t[data_read_buffer_size];
	if (auto spt = _sock.lock()) {
		spt->async_read_some(buffer(this->data_read_buffer, data_read_buffer_size),
			std::bind(&AsyncConnection::onReadData, this, _1, _2));
	}
  }
}

void AsyncConnection::onReadData(const boost::system::error_code &err,
                                 size_t read_bytes) {
  logger_info("AsyncConnection::onReadData");
  if (err) {
    this->onNetworkError(err);
  } else {
    NetData_ptr d = std::make_shared<NetData>(data_read_buffer_size, data_read_buffer);
    this->data_read_buffer = nullptr;
    this->data_read_buffer_size = 0;
    try {
      this->onDataRecv(d);
    } catch (std::exception &ex) {
      THROW_EXCEPTION_SS("exception on async readData. " << ex.what());
    }
    readNextAsync();
  }
}