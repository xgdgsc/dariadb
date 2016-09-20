#pragma once

#include <libdariadb/utils/exception.h>
#include <libdariadb/utils/locker.h>
#include "net_common.h"
#include "net_data.h"
#include "socket_ptr.h"
#include <atomic>
#include <memory>
#include <functional>

namespace dariadb {
namespace net {

class AsyncConnection: public std::enable_shared_from_this<AsyncConnection>{
public:
	/// if method set 'cancel' to true, then read loop stoping.
	/// if dont_free_memory, then free NetData_ptr is in client side.
	using onDataRecvHandler = std::function<void(const NetData_ptr &d, bool &cancel, bool &dont_free_memory)>;
	using onNetworkErrorHandler = std::function<void(const boost::system::error_code &err)>;
public:
  AsyncConnection(NetData_Pool *pool, onDataRecvHandler onRecv, onNetworkErrorHandler onErr);
  virtual ~AsyncConnection() noexcept(false);
  void set_pool(NetData_Pool *pool);
  NetData_Pool *get_pool() { return _pool; }
  void send(const NetData_ptr &d);
  void start(const socket_ptr &sock);
  void mark_stoped();
  void full_stop(); /// stop thread, clean queue

  void set_id(int id) { _async_con_id = id; }
  int id() const { return _async_con_id; }
  int queue_size() const { return _messages_to_send; }

private:
  void readNextAsync();
private:
  std::atomic_int _messages_to_send;
  int _async_con_id; // TODO just for logging. remove after release.
  socket_weak _sock;

  bool _is_stoped;
  std::atomic_bool _begin_stoping_flag;
  NetData_Pool *_pool;

  onDataRecvHandler _on_recv_hadler;
  onNetworkErrorHandler _on_error_handler;
};
}
}