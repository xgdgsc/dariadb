#pragma once

#include "common.h"
#include "net_common.h"
#include "../utils/locker.h"
#include "../utils/exception.h"
#include <atomic>
#include <memory>
#include <numeric>
#include <tuple>

namespace dariadb {
namespace net {

#pragma pack(push, 1)
struct NetData {
	typedef uint16_t MessageSize;
	static const size_t MAX_MESSAGE_SIZE = std::numeric_limits<MessageSize>::max();
  MessageSize size;
  uint8_t data[MAX_MESSAGE_SIZE];

  NetData();
  NetData(const DataKinds &k);

  ~NetData();

  std::tuple<MessageSize, uint8_t*> as_buffer();
};
#pragma pack(pop)

using NetData_ptr = std::shared_ptr<NetData>;

const size_t MARKER_SIZE = sizeof(NetData::MessageSize);

class AsyncConnection {
public:
  AsyncConnection();
  virtual ~AsyncConnection() noexcept(false);
  void send(const NetData_ptr &d);
  void start(const socket_ptr &sock);
  void mark_stoped();
  void full_stop(); ///stop thread, clean queue

  ///if method set 'cancel' to true, then read loop stoping.
  virtual void onDataRecv(const NetData_ptr&d, bool&cancel) = 0;
  virtual void onNetworkError(const boost::system::error_code&err) = 0;

  void set_id(int id) { _async_con_id = id; }
  int id()const { return _async_con_id; }
  int queue_size()const{return _messages_to_send;}
private:
  void readNextAsync();

  void onDataSended(NetData_ptr &d,const boost::system::error_code &err, size_t read_bytes);
  void onReadMarker(const boost::system::error_code &err, size_t read_bytes);
  void onReadData(NetData_ptr&d, const boost::system::error_code &err, size_t read_bytes);
private:
  std::atomic_int _messages_to_send;
  int _async_con_id; // TODO just for logging. remove after release.
  socket_weak _sock;
  
  char marker_read_buffer[MARKER_SIZE];
  
  bool _is_stoped;
  std::atomic_bool _begin_stoping_flag;
};
}
}
