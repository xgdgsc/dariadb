#pragma once

#include <atomic>
#include <boost/asio.hpp>
#include <boost/system/error_code.hpp>
#include <memory>

#include "../../interfaces/icallbacks.h"
#include "../../engine.h"
#include "../../meas.h"
#include "../async_connection.h"
#include "../interfaces/iclientmanager.h"
#include "../net_common.h"

namespace dariadb {
namespace net {

struct IOClient : public AsyncConnection {

  struct Environment {
    Environment() {
      srv = nullptr;
      storage = nullptr;
      nd_pool = nullptr;
      write_meases_strand = nullptr;
    }
    IClientManager *srv;
    storage::Engine *storage;
    NetData_Pool *nd_pool;
    boost::asio::io_service::strand *write_meases_strand;
    boost::asio::io_service *service;
  };

  struct ClientDataReader:public storage::IReaderClb{
      static const size_t BUFFER_LENGTH=(NetData::MAX_MESSAGE_SIZE - sizeof(QueryWrite_header))/sizeof(Meas);
      utils::Locker _locker;
      IOClient*_parent;
      QueryNumber _query_num;
      size_t pos;
      std::array<Meas, BUFFER_LENGTH> _buffer;

      ClientDataReader(IOClient*parent,QueryNumber query_num);
      ~ClientDataReader();
      void call(const Meas &m)override;
      void is_end()override;
      void send_buffer();
  };

  socket_ptr sock;
  std::string host;

  ClientState state;
  Environment *env;
  std::atomic_int pings_missed;
  std::list<storage::IReaderClb*> readers;

  IOClient(int _id, socket_ptr &_sock, Environment *_env);
  ~IOClient();
  void end_session();
  void close();
  void ping();

  void onDataRecv(const NetData_ptr &d, bool &cancel, bool &dont_free_memory) override;
  void onNetworkError(const boost::system::error_code &err) override;

  void writeMeasurementsCall(const NetData_ptr &d);
  void readInterval(const NetData_ptr &d);
  void readTimePoint(const NetData_ptr &d);
  void sendOk(QueryNumber query_num);
  void sendError(QueryNumber query_num, const ERRORS&err);
};

typedef std::shared_ptr<IOClient> ClientIO_ptr;
}
}
