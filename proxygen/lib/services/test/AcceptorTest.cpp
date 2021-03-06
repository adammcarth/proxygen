/*
 *  Copyright (c) 2014, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */
#include <folly/io/async/EventBase.h>
#include <glog/logging.h>
#include <gtest/gtest.h>
#include <proxygen/lib/services/Acceptor.h>

using namespace apache::thrift::async;
using namespace folly;
using namespace proxygen;

class TestConnection : public folly::wangle::ManagedConnection {
 public:
  void timeoutExpired() noexcept {
  }
  void describe(std::ostream& os) const {
  }
  bool isBusy() const {
    return false;
  }
  void notifyPendingShutdown() {
  }
  void closeWhenIdle() {
  }
  void dropConnection() {
  }
  void dumpConnectionState(uint8_t loglevel) {
  }
};

class TestAcceptor : public Acceptor {
 public:
  explicit TestAcceptor(const ServerSocketConfig& accConfig)
      : Acceptor(accConfig) {}

  virtual void onNewConnection(
      apache::thrift::async::TAsyncSocket::UniquePtr sock,
      const folly::SocketAddress* address,
      const std::string& nextProtocolName,
      const TransportInfo& tinfo) {
    addConnection(new TestConnection);

    getEventBase()->terminateLoopSoon();
  }
};

TEST(AcceptorTest, Basic) {

  EventBase base;
  auto socket = TAsyncServerSocket::newSocket(&base);
  ServerSocketConfig config;

  TestAcceptor acceptor(config);
  socket->addAcceptCallback(&acceptor, &base);

  acceptor.init(socket.get(), &base);
  socket->bind(0);
  socket->listen(100);

  SocketAddress addy;
  socket->getAddress(&addy);

  socket->startAccepting();

  auto client_socket = TAsyncSocket::newSocket(
    &base, addy);

  base.loopForever();

  CHECK_EQ(acceptor.getNumConnections(), 1);

  CHECK(acceptor.getState() == Acceptor::State::kRunning);
  acceptor.forceStop();
  socket->stopAccepting();
  base.loop();
}
