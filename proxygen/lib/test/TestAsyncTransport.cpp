/*
 *  Copyright (c) 2014, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */
#include "proxygen/lib/test/TestAsyncTransport.h"

#include <folly/SocketAddress.h>
#include <folly/io/IOBuf.h>
#include <folly/io/async/EventBase.h>
#include <thrift/lib/cpp/concurrency/Util.h>
#include <thrift/lib/cpp/transport/TTransportException.h>

using apache::thrift::async::TAsyncTimeout;
using apache::thrift::async::WriteFlags;
using apache::thrift::concurrency::Util;
using apache::thrift::transport::TTransportException;
using folly::EventBase;
using folly::IOBuf;
using folly::SocketAddress;
using std::shared_ptr;
using std::unique_ptr;

/*
 * TestAsyncTransport::ReadEvent
 */

class TestAsyncTransport::ReadEvent {
 public:
  ReadEvent(const void* buf, size_t buflen, int64_t delay)
    : buffer_(nullptr),
      readStart_(nullptr),
      dataEnd_(nullptr),
      isError_(false),
      exception_(),
      delay_(delay) {
    if (buflen == 0) {
      // This means EOF
      return;
    }
    CHECK_NOTNULL(buf);

    buffer_ = static_cast<char*>(malloc(buflen));
    if (buffer_ == nullptr) {
      throw std::bad_alloc();
    }
    memcpy(buffer_, buf, buflen);
    readStart_ = buffer_;
    dataEnd_ = buffer_ + buflen;
  }

  ReadEvent(const TTransportException& ex, int64_t delay)
    : buffer_(nullptr),
      readStart_(nullptr),
      dataEnd_(nullptr),
      isError_(true),
      exception_(ex),
      delay_(delay) {}

  ~ReadEvent() {
    free(buffer_);
  }

  int64_t getDelay() const {
    return delay_;
  }

  bool isFinalEvent() const {
    return buffer_ == nullptr;
  }

  const char* getBuffer() const {
    return readStart_;
  }

  size_t getLength() const {
    return (dataEnd_ - readStart_);
  }

  void consumeData(size_t length) {
    CHECK_LE(readStart_ + length, dataEnd_);
    readStart_ += length;
  }

  bool isError() const {
    return isError_;
  }

  const TTransportException& getException() const {
    return exception_;
  }

 private:
  char* buffer_;
  char* readStart_;
  char* dataEnd_;

  bool isError_;
  TTransportException exception_;

  int64_t delay_; // milliseconds
};

/*
 * TestAsyncTransport::WriteEvent methods
 */

TestAsyncTransport::WriteEvent::WriteEvent(int64_t time, size_t count)
  : time_(time),
    count_(count) {
  // Initialize all of the iov_base pointers to nullptr.  This way we won't free
  // an uninitialized pointer in the destructor if we fail to allocate any of
  // the buffers.
  for (size_t n = 0; n < count_; ++n) {
    vec_[n].iov_base = nullptr;
  }
}

TestAsyncTransport::WriteEvent::~WriteEvent() {
  for (size_t n = 0; n < count_; ++n) {
    free(vec_[n].iov_base);
  }
}

shared_ptr<TestAsyncTransport::WriteEvent>
TestAsyncTransport::WriteEvent::newEvent(const struct iovec* vec,
                                         size_t count) {
  size_t len = sizeof(WriteEvent) + (count * sizeof(struct iovec));
  void* buf = malloc(len);
  if (buf == nullptr) {
    throw std::bad_alloc();
  }

  int64_t now = Util::currentTime();
  shared_ptr<WriteEvent> event(new(buf) WriteEvent(now, count),
                               destroyEvent);
  for (size_t n = 0; n < count; ++n) {
    size_t len = vec[n].iov_len;
    event->vec_[n].iov_len = len;
    if (len == 0) {
      event->vec_[n].iov_base = nullptr;
      continue;
    }

    event->vec_[n].iov_base = malloc(len);
    if (event->vec_[n].iov_base == nullptr) {
      throw std::bad_alloc();
    }
    memcpy(event->vec_[n].iov_base, vec[n].iov_base, len);
  }

  return event;
}

void
TestAsyncTransport::WriteEvent::destroyEvent(WriteEvent* event) {
  event->~WriteEvent();
  free(event);
}

/*
 * TestAsyncTransport methods
 */

TestAsyncTransport::TestAsyncTransport(EventBase* eventBase)
  : TAsyncTimeout(eventBase),
    eventBase_(eventBase),
    readCallback_(nullptr),
    sendTimeout_(0),
    prevReadEventTime_(0),
    nextReadEventTime_(0),
    readState_(kStateOpen),
    writeState_(kStateOpen),
    readEvents_()
{
}

void
TestAsyncTransport::setReadCallback(ReadCallback* callback) {
  if (readCallback_ == callback) {
    return;
  }

  if (callback == nullptr) {
    cancelTimeout();
    readCallback_ = nullptr;
    return;
  }

  bool wasNull = (readCallback_ == nullptr);

  if (readState_ == kStateClosed) {
    callback->readEOF();
    return;
  } else if (readState_ == kStateError) {
    TTransportException ex(TTransportException::NOT_OPEN,
                           "setReadCallback() called with socket in "
                           "invalid state");
    callback->readError(ex);
    return;
  }

  CHECK_EQ(readState_, kStateOpen);
  readCallback_ = callback;

  // If the callback was previously nullptr, read events were paused, so we need
  // to reschedule them now.
  //
  // If it was set before, read events are still scheduled, so we are done now
  // and can return.
  if (!wasNull) {
    return;
  }

  if (nextReadEventTime_ == 0) {
    // Either readEvents_ is empty, or startReadEvents() hasn't been called yet
    return;
  }
  CHECK(!readEvents_.empty());
  scheduleNextReadEvent(Util::currentTime());
}

TestAsyncTransport::ReadCallback*
TestAsyncTransport::getReadCallback() const {
  return readCallback_;
}

void
TestAsyncTransport::write(WriteCallback* callback,
                          const void* buf, size_t bytes,
                          WriteFlags flags) {
  iovec op;
  op.iov_base = const_cast<void*>(buf);
  op.iov_len = bytes;
  this->writev(callback, &op, 1, flags);
}

void
TestAsyncTransport::writev(WriteCallback* callback,
                           const iovec* vec, size_t count,
                           WriteFlags flags) {
  if (isSet(flags, WriteFlags::CORK)) {
    corkCount_++;
  } else if (isSet(flags, WriteFlags::EOR)) {
    eorCount_++;
  }
  if (!writesAllowed()) {
    TTransportException ex(TTransportException::NOT_OPEN,
                           "write() called on non-open TestAsyncTransport");
    callback->writeError(0, ex);
    return;
  }

  shared_ptr<WriteEvent> event = WriteEvent::newEvent(vec, count);
  if (writeState_ == kStatePaused || pendingWriteEvents_.size() > 0)  {
    pendingWriteEvents_.push_back(std::make_pair(event, callback));
  } else {
    CHECK(writeState_ == kStateOpen);
    writeEvents_.push_back(event);
    callback->writeSuccess();
  }
}

void
TestAsyncTransport::writeChain(WriteCallback* callback,
                               std::unique_ptr<folly::IOBuf>&& iob,
                               WriteFlags flags) {
  size_t count = iob->countChainElements();
  iovec vec[count];
  const IOBuf* head = iob.get();
  const IOBuf* next = head;
  unsigned i = 0;
  do {
    vec[i].iov_base = const_cast<uint8_t *>(next->buffer());
    vec[i++].iov_len = next->length();
    next = next->next();
  } while (next != head);
  this->writev(callback, vec, count, flags);
}

void
TestAsyncTransport::close() {
  closeNow();
}

void
TestAsyncTransport::closeNow() {
  if (readState_ == kStateOpen) {
    readState_ = kStateClosed;

    if (readCallback_ != nullptr) {
      ReadCallback* callback = readCallback_;
      readCallback_ = nullptr;
      callback->readEOF();
    }
  }
  shutdownWriteNow();
}

void
TestAsyncTransport::shutdownWrite() {
  shutdownWriteNow();
}

void
TestAsyncTransport::shutdownWriteNow() {
  DestructorGuard g(this);
  failPendingWrites();
  if (writeState_ == kStateOpen ||
      writeState_ == kStatePaused) {
    writeState_ = kStateClosed;
  }
}

void
TestAsyncTransport::getPeerAddress(SocketAddress* addr) const {
  // This isn't really accurate, but close enough for testing.
  addr->setFromIpPort("127.0.0.1", 0);
}

void
TestAsyncTransport::getLocalAddress(SocketAddress* addr) const {
  // This isn't really accurate, but close enough for testing.
  addr->setFromIpPort("127.0.0.1", 0);
}

bool
TestAsyncTransport::good() const {
  return (readState_ == kStateOpen && writesAllowed());
}

bool
TestAsyncTransport::readable() const {
  return false;
}

bool
TestAsyncTransport::connecting() const {
  return false;
}

bool
TestAsyncTransport::error() const {
  return (readState_ == kStateError || writeState_ == kStateError);
}

void
TestAsyncTransport::attachEventBase(EventBase* eventBase) {
  CHECK(nullptr == eventBase_);
  CHECK(nullptr == readCallback_);
  eventBase_ = eventBase;
}

void
TestAsyncTransport::detachEventBase() {
  CHECK_NOTNULL(eventBase_);
  CHECK(nullptr == readCallback_);
  eventBase_ = nullptr;
}

bool
TestAsyncTransport::isDetachable() const {
  return true;
}

EventBase*
TestAsyncTransport::getEventBase() const {
  return eventBase_;
}

void
TestAsyncTransport::setSendTimeout(uint32_t milliseconds) {
  sendTimeout_ = milliseconds;
}

uint32_t
TestAsyncTransport::getSendTimeout() const {
  return sendTimeout_;
}

void
TestAsyncTransport::pauseWrites() {
  if (writeState_ != kStateOpen) {
    LOG(FATAL) << "cannot pause writes on non-open transport; state=" <<
      writeState_;
  }
  writeState_ = kStatePaused;
}

void
TestAsyncTransport::resumeWrites() {
  if (writeState_ != kStatePaused) {
    LOG(FATAL) << "cannot resume writes on non-paused transport; state=" <<
      writeState_;
  }
  writeState_ = kStateOpen;
  for (auto event = pendingWriteEvents_.begin();
       event != pendingWriteEvents_.end() && writeState_ == kStateOpen;
       event = pendingWriteEvents_.begin()) {
    writeEvents_.push_back(event->first);
    pendingWriteEvents_.pop_front();
    event->second->writeSuccess();
  }
}

void
TestAsyncTransport::failPendingWrites() {
  // writeError() callback might try to delete this object
  DestructorGuard g(this);
  while (!pendingWriteEvents_.empty()) {
    auto event = pendingWriteEvents_.front();
    pendingWriteEvents_.pop_front();
    TTransportException ex(TTransportException::NOT_OPEN,
                           "Transport closed locally");
    event.second->writeError(0, ex);
  }
}

void
TestAsyncTransport::addReadEvent(folly::IOBufQueue& chain,
                                 int64_t delayFromPrevious) {
  while (true) {
    unique_ptr<IOBuf> cur = chain.pop_front();
    if (!cur) {
      break;
    }
    addReadEvent(cur->data(), cur->length(), delayFromPrevious);
  }
}

void
TestAsyncTransport::addReadEvent(const void* buf, size_t buflen,
                                 int64_t delayFromPrevious) {
  if (!readEvents_.empty() && readEvents_.back()->isFinalEvent()) {
    LOG(FATAL) << "cannot add more read events after an error or EOF";
  }

  auto event = std::make_shared<ReadEvent>(buf, buflen, delayFromPrevious);
  addReadEvent(event);
}

void
TestAsyncTransport::addReadEvent(const char* buf, int64_t delayFromPrevious) {
  addReadEvent(buf, strlen(buf), delayFromPrevious);
}

void
TestAsyncTransport::addReadEOF(int64_t delayFromPrevious) {
  addReadEvent(nullptr, 0, delayFromPrevious);
}

void
TestAsyncTransport::addReadError(const TTransportException& ex,
                                 int64_t delayFromPrevious) {
  if (!readEvents_.empty() && readEvents_.back()->isFinalEvent()) {
    LOG(FATAL) << "cannot add a read error after an error or EOF";
  }

  auto event = std::make_shared<ReadEvent>(ex, delayFromPrevious);
  addReadEvent(event);
}

void
TestAsyncTransport::addReadEvent(const shared_ptr<ReadEvent>& event) {
  bool firstEvent = readEvents_.empty();
  readEvents_.push_back(event);

  if (!firstEvent) {
    return;
  }
  if (prevReadEventTime_ == 0) {
    return;
  }

  nextReadEventTime_ = prevReadEventTime_ + event->getDelay();
  if (readCallback_ == nullptr) {
    return;
  }

  scheduleNextReadEvent(Util::currentTime());
}

void
TestAsyncTransport::startReadEvents() {
  int64_t now = Util::currentTime();
  prevReadEventTime_ = now;

  if (readEvents_.empty()) {
    return;
  }
  nextReadEventTime_ = prevReadEventTime_ + readEvents_.front()->getDelay();

  if (readCallback_ == nullptr) {
    return;
  }

  scheduleNextReadEvent(now);
}

void
TestAsyncTransport::scheduleNextReadEvent(int64_t now) {
  if (nextReadEventTime_ <= now) {
    fireNextReadEvent();
  } else {
    scheduleTimeout(nextReadEventTime_ - now);
  }
}

void
TestAsyncTransport::fireNextReadEvent() {
  DestructorGuard dg(this);
  CHECK(!readEvents_.empty());
  CHECK(readCallback_ != nullptr);

  // maxReadAtOnce prevents us from starving other users of this EventBase
  unsigned int const maxReadAtOnce = 30;
  for (unsigned int n = 0; n < maxReadAtOnce; ++n) {
    fireOneReadEvent();

    if (readCallback_ == nullptr || eventBase_ == nullptr ||
        nextReadEventTime_ == 0) {
      return;
    }
    int64_t now = Util::currentTime();
    if (nextReadEventTime_ > now) {
      scheduleTimeout(nextReadEventTime_ - now);
      return;
    }
  }

  // Trigger fireNextReadEvent() to be called the next time around the event
  // loop.
  eventBase_->runInLoop(std::bind(&TestAsyncTransport::fireNextReadEvent,
                                       this));
}

void
TestAsyncTransport::fireOneReadEvent() {
  CHECK(!readEvents_.empty());
  CHECK(readCallback_ != nullptr);

  const shared_ptr<ReadEvent>& event = readEvents_.front();

  // Note that we call getReadBuffer() here even if we know the next event may
  // be an EOF or an error.  This matches the behavior of TAsyncSocket.
  // (Because TAsyncSocket merely gets notification that the socket is readable,
  // and has to call getReadBuffer() before it can make the actual read call to
  // get an error or EOF.)
  void* buf;
  size_t buflen;
  try {
    readCallback_->getReadBuffer(&buf, &buflen);
  } catch (...) {
    // TODO: we should convert the error to a TTransportException and call
    // readError() here.
    LOG(FATAL) << "readCallback_->getReadBuffer() threw an error";
  }
  if (buf == nullptr || buflen == 0) {
    // TODO: we should just call readError() here.
    LOG(FATAL) << "readCallback_->getReadBuffer() returned a nullptr or "
      "empty buffer";
  }

  // Handle errors
  if (event->isError()) {
    // Errors immediately move both read and write to an error state
    readState_ = kStateError;
    writeState_ = kStateError;

    // event is just a reference to the shared_ptr, so make a real copy of the
    // pointer before popping it off the readEvents_ list.
    shared_ptr<ReadEvent> eventPointerCopy = readEvents_.front();
    readEvents_.pop_front();
    CHECK(readEvents_.empty());
    nextReadEventTime_ = 0;

    ReadCallback* callback = readCallback_;
    readCallback_ = nullptr;
    callback->readError(eventPointerCopy->getException());
    return;
  }

  // Handle EOF
  size_t available = event->getLength();
  if (available == 0) {
    readState_ = kStateClosed;

    readEvents_.pop_front();
    CHECK(readEvents_.empty());
    nextReadEventTime_ = 0;

    ReadCallback* callback = readCallback_;
    readCallback_ = nullptr;
    callback->readEOF();
    return;
  }

  // Handle a normal read event
  size_t readlen;
  bool more;
  if (available <= buflen) {
    readlen = available;
    more = false;
  } else {
    readlen = buflen;
    more = true;
  }
  memcpy(buf, event->getBuffer(), readlen);
  if (more) {
    event->consumeData(readlen);
  } else {
    prevReadEventTime_ = nextReadEventTime_;
    // Note: since event is just a reference to the shared_ptr in readEvents_,
    // we shouldn't access the event any more after popping it off here.
    readEvents_.pop_front();

    if (readEvents_.empty()) {
      nextReadEventTime_ = 0;
    } else {
      nextReadEventTime_ = prevReadEventTime_ + readEvents_.front()->getDelay();
    }
  }
  readCallback_->readDataAvailable(readlen);
}

void
TestAsyncTransport::timeoutExpired() noexcept {
  CHECK_NOTNULL(readCallback_);
  CHECK(!readEvents_.empty());
  fireNextReadEvent();
}
