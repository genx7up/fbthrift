/*
 * Copyright 2014 Facebook, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <thrift/lib/cpp/async/TAsyncSocket.h>

#include <thrift/lib/cpp/TLogging.h>
#include <thrift/lib/cpp/async/TEventBase.h>
#include <thrift/lib/cpp/transport/TSocketAddress.h>
#include <thrift/lib/cpp/transport/TTransportException.h>

#include <folly/io/IOBuf.h>

#include <poll.h>
#include <errno.h>
#include <limits.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>

using apache::thrift::transport::TSocketAddress;
using apache::thrift::transport::TTransportException;
using folly::IOBuf;
using std::string;
using std::unique_ptr;

namespace apache { namespace thrift { namespace async {

// static members initializers
const TAsyncSocket::OptionMap TAsyncSocket::emptyOptionMap;
const transport::TSocketAddress TAsyncSocket::anyAddress =
  transport::TSocketAddress("0.0.0.0", 0);

const TTransportException socketClosedLocallyEx(
    TTransportException::END_OF_FILE, "socket closed locally");
const TTransportException socketShutdownForWritesEx(
    TTransportException::END_OF_FILE, "socket shutdown for writes");

// TODO: It might help performance to provide a version of WriteRequest that
// users could derive from, so we can avoid the extra allocation for each call
// to write()/writev().  We could templatize TFramedAsyncChannel just like the
// protocols are currently templatized for transports.
//
// We would need the version for external users where they provide the iovec
// storage space, and only our internal version would allocate it at the end of
// the WriteRequest.

/**
 * A WriteRequest object tracks information about a pending write() or writev()
 * operation.
 *
 * A new WriteRequest operation is allocated on the heap for all write
 * operations that cannot be completed immediately.
 */
class TAsyncSocket::WriteRequest {
 public:
  static WriteRequest* newRequest(WriteCallback* callback,
                                  const iovec* ops,
                                  uint32_t opCount,
                                  unique_ptr<IOBuf>&& ioBuf,
                                  WriteFlags flags) {
    assert(opCount > 0);
    // Since we put a variable size iovec array at the end
    // of each WriteRequest, we have to manually allocate the memory.
    void* buf = malloc(sizeof(WriteRequest) +
                       (opCount * sizeof(struct iovec)));
    if (buf == nullptr) {
      throw std::bad_alloc();
    }

    return new(buf) WriteRequest(callback, ops, opCount, std::move(ioBuf),
                                 flags);
  }

  void destroy() {
    this->~WriteRequest();
    free(this);
  }

  bool cork() const {
    return isSet(flags_, WriteFlags::CORK);
  }

  WriteFlags flags() const {
    return flags_;
  }

  WriteRequest* getNext() const {
    return next_;
  }

  WriteCallback* getCallback() const {
    return callback_;
  }

  uint32_t getBytesWritten() const {
    return bytesWritten_;
  }

  const struct iovec* getOps() const {
    assert(opCount_ > opIndex_);
    return writeOps_ + opIndex_;
  }

  uint32_t getOpCount() const {
    assert(opCount_ > opIndex_);
    return opCount_ - opIndex_;
  }

  void consume(uint32_t wholeOps, uint32_t partialBytes,
               uint32_t totalBytesWritten) {
    // Advance opIndex_ forward by wholeOps
    opIndex_ += wholeOps;
    assert(opIndex_ < opCount_);

    // If we've finished writing any IOBufs, release them
    if (ioBuf_) {
      for (uint32_t i = wholeOps; i != 0; --i) {
        assert(ioBuf_);
        ioBuf_ = ioBuf_->pop();
      }
    }

    // Move partialBytes forward into the current iovec buffer
    struct iovec* currentOp = writeOps_ + opIndex_;
    assert((partialBytes < currentOp->iov_len) || (currentOp->iov_len == 0));
    currentOp->iov_base =
      reinterpret_cast<uint8_t*>(currentOp->iov_base) + partialBytes;
    currentOp->iov_len -= partialBytes;

    // Increment the bytesWritten_ count by totalBytesWritten
    bytesWritten_ += totalBytesWritten;
  }

  void append(WriteRequest* next) {
    assert(next_ == nullptr);
    next_ = next;
  }

 private:
  WriteRequest(WriteCallback* callback,
               const struct iovec* ops,
               uint32_t opCount,
               unique_ptr<IOBuf>&& ioBuf,
               WriteFlags flags)
    : next_(nullptr)
    , callback_(callback)
    , bytesWritten_(0)
    , opCount_(opCount)
    , opIndex_(0)
    , flags_(flags)
    , ioBuf_(std::move(ioBuf)) {
    memcpy(writeOps_, ops, sizeof(*ops) * opCount_);
  }

  // Private destructor, to ensure callers use destroy()
  ~WriteRequest() {}

  WriteRequest* next_;          ///< pointer to next WriteRequest
  WriteCallback* callback_;     ///< completion callback
  uint32_t bytesWritten_;       ///< bytes written
  uint32_t opCount_;            ///< number of entries in writeOps_
  uint32_t opIndex_;            ///< current index into writeOps_
  WriteFlags flags_;            ///< set for WriteFlags
  unique_ptr<IOBuf> ioBuf_;     ///< underlying IOBuf, or nullptr if N/A
  struct iovec writeOps_[];     ///< write operation(s) list
};

TAsyncSocket::TAsyncSocket(TEventBase* evb)
  : eventBase_(evb)
  , writeTimeout_(this, evb)
  , ioHandler_(this, evb) {
  VLOG(5) << "new TAsyncSocket(" << this << ", evb=" << evb << ")";
  init();
}

TAsyncSocket::TAsyncSocket(TEventBase* evb,
                           const transport::TSocketAddress& address,
                           uint32_t connectTimeout)
  : eventBase_(evb)
  , writeTimeout_(this, evb)
  , ioHandler_(this, evb) {
  VLOG(5) << "new TAsyncSocket(" << this << ", evb=" << evb << ")";
  init();
  connect(nullptr, address, connectTimeout);
}

TAsyncSocket::TAsyncSocket(TEventBase* evb,
                           const std::string& ip,
                           uint16_t port,
                           uint32_t connectTimeout)
  : eventBase_(evb)
  , writeTimeout_(this, evb)
  , ioHandler_(this, evb) {
  VLOG(5) << "new TAsyncSocket(" << this << ", evb=" << evb << ")";
  init();
  connect(nullptr, ip, port, connectTimeout);
}

TAsyncSocket::TAsyncSocket(TEventBase* evb, int fd)
  : eventBase_(evb)
  , writeTimeout_(this, evb)
  , ioHandler_(this, evb, fd) {
  VLOG(5) << "new TAsyncSocket(" << this << ", evb=" << evb << ", fd="
          << fd << ")";
  init();
  fd_ = fd;
  state_ = StateEnum::ESTABLISHED;
}

// init() method, since constructor forwarding isn't supported in most
// compilers yet.
void TAsyncSocket::init() {
  assert(eventBase_ == nullptr || eventBase_->isInEventBaseThread());
  shutdownFlags_ = 0;
  state_ = StateEnum::UNINIT;
  eventFlags_ = TEventHandler::NONE;
  fd_ = -1;
  sendTimeout_ = 0;
  maxReadsPerEvent_ = 0;
  connectCallback_ = nullptr;
  readCallback_ = nullptr;
  writeReqHead_ = nullptr;
  writeReqTail_ = nullptr;
  shutdownSocketSet_ = nullptr;
  appBytesWritten_ = 0;
  appBytesReceived_ = 0;
}

TAsyncSocket::~TAsyncSocket() {
  VLOG(7) << "actual destruction of TAsyncSocket(this=" << this
          << ", evb=" << eventBase_ << ", fd=" << fd_
          << ", state=" << state_ << ")";
}

void TAsyncSocket::destroy() {
  VLOG(5) << "TAsyncSocket::destroy(this=" << this << ", evb=" << eventBase_
          << ", fd=" << fd_ << ", state=" << state_;
  // When destroy is called, close the socket immediately
  closeNow();

  // Then call TDelayedDestruction::destroy() to take care of
  // whether or not we need immediate or delayed destruction
  TDelayedDestruction::destroy();
}

int TAsyncSocket::detachFd() {
  VLOG(6) << "TAsyncSocket::detachFd(this=" << this << ", fd=" << fd_
          << ", evb=" << eventBase_ << ", state=" << state_
          << ", events=" << std::hex << eventFlags_ << ")";
  // Extract the fd, and set fd_ to -1 first, so closeNow() won't
  // actually close the descriptor.
  if (shutdownSocketSet_) {
    shutdownSocketSet_->remove(fd_);
  }
  int fd = fd_;
  fd_ = -1;
  // Call closeNow() to invoke all pending callbacks with an error.
  closeNow();
  // Update the TEventHandler to stop using this fd.
  // This can only be done after closeNow() unregisters the handler.
  ioHandler_.changeHandlerFD(-1);
  return fd;
}

void TAsyncSocket::setShutdownSocketSet(ShutdownSocketSet* newSS) {
  if (shutdownSocketSet_ == newSS) {
    return;
  }
  if (shutdownSocketSet_ && fd_ != -1) {
    shutdownSocketSet_->remove(fd_);
  }
  shutdownSocketSet_ = newSS;
  if (shutdownSocketSet_ && fd_ != -1) {
    shutdownSocketSet_->add(fd_);
  }
}

void TAsyncSocket::connect(ConnectCallback* callback,
                           const TSocketAddress& address,
                           int timeout,
                           const OptionMap &options,
                           const TSocketAddress& bindAddr) noexcept {
  DestructorGuard dg(this);
  assert(eventBase_->isInEventBaseThread());

  addr_ = address;

  // Make sure we're in the uninitialized state
  if (state_ != StateEnum::UNINIT) {
    return invalidState(callback);
  }

  assert(fd_ == -1);
  state_ = StateEnum::CONNECTING;
  connectCallback_ = callback;

  sockaddr_storage addrStorage;
  sockaddr* saddr = reinterpret_cast<sockaddr*>(&addrStorage);

  try {
    // Create the socket
    // Technically the first parameter should actually be a protocol family
    // constant (PF_xxx) rather than an address family (AF_xxx), but the
    // distinction is mainly just historical.  In pretty much all
    // implementations the PF_foo and AF_foo constants are identical.
    fd_ = socket(address.getFamily(), SOCK_STREAM, 0);
    if (fd_ < 0) {
      throw TTransportException(TTransportException::INTERNAL_ERROR,
                                withAddr("failed to create socket"), errno);
    }
    if (shutdownSocketSet_) {
      shutdownSocketSet_->add(fd_);
    }
    ioHandler_.changeHandlerFD(fd_);

    // Set the FD_CLOEXEC flag so that the socket will be closed if the program
    // later forks and execs.
    int rv = fcntl(fd_, F_SETFD, FD_CLOEXEC);
    if (rv != 0) {
      throw TTransportException(TTransportException::INTERNAL_ERROR,
                                withAddr("failed to set close-on-exec flag"),
                                errno);
    }

    // Put the socket in non-blocking mode
    int flags = fcntl(fd_, F_GETFL, 0);
    if (flags == -1) {
      throw TTransportException(TTransportException::INTERNAL_ERROR,
                                withAddr("failed to get socket flags"), errno);
    }
    rv = fcntl(fd_, F_SETFL, flags | O_NONBLOCK);
    if (rv == -1) {
      throw TTransportException(
          TTransportException::INTERNAL_ERROR,
          withAddr("failed to put socket in non-blocking mode"),
          errno);
    }

#if !defined(MSG_NOSIGNAL) && defined(F_SETNOSIGPIPE)
    // iOS and OS X don't support MSG_NOSIGNAL; set F_SETNOSIGPIPE instead
    rv = fcntl(fd_, F_SETNOSIGPIPE, 1);
    if (rv == -1) {
      throw TTransportException(
          TTransportException::INTERNAL_ERROR,
          "failed to enable F_SETNOSIGPIPE on socket",
          errno);
    }
#endif

    // By default, turn on TCP_NODELAY
    // If setNoDelay() fails, we continue anyway; this isn't a fatal error.
    // setNoDelay() will log an error message if it fails.
    if (address.getFamily() != AF_UNIX) {
      (void)setNoDelay(true);
    }

    VLOG(5) << "TAsyncSocket::connect(this=" << this << ", evb=" << eventBase_
            << ", fd=" << fd_ << ", host=" << address.describe().c_str();

    // bind the socket
    if (bindAddr != anyAddress) {
      int one = 1;
      if (::setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one))) {
        doClose();
        throw TTransportException(
          TTransportException::NOT_OPEN,
          "failed to setsockopt prior to bind on " + bindAddr.describe(),
          errno);
      }

      bindAddr.getAddress(&addrStorage);

      if (::bind(fd_, saddr, bindAddr.getActualSize()) != 0) {
        doClose();
        throw TTransportException(TTransportException::NOT_OPEN,
                                  "failed to bind to async socket: " +
                                  bindAddr.describe(),
                                  errno);
      }
    }

    // Apply the additional options if any.
    for (const auto& opt: options) {
      int rv = opt.first.apply(fd_, opt.second);
      if (rv != 0) {
        throw TTransportException(TTransportException::INTERNAL_ERROR,
                                  withAddr("failed to set socket option"),
                                  errno);
      }
    }

    // Perform the connect()
    address.getAddress(&addrStorage);

    rv = ::connect(fd_, saddr, address.getActualSize());
    if (rv < 0) {
      if (errno == EINPROGRESS) {
        // Connection in progress.
        if (timeout > 0) {
          // Start a timer in case the connection takes too long.
          if (!writeTimeout_.scheduleTimeout(timeout)) {
            throw TTransportException(TTransportException::INTERNAL_ERROR,
                withAddr("failed to schedule TAsyncSocket connect timeout"));
          }
        }

        // Register for write events, so we'll
        // be notified when the connection finishes/fails.
        // Note that we don't register for a persistent event here.
        assert(eventFlags_ == TEventHandler::NONE);
        eventFlags_ = TEventHandler::WRITE;
        if (!ioHandler_.registerHandler(eventFlags_)) {
          throw TTransportException(TTransportException::INTERNAL_ERROR,
              withAddr("failed to register TAsyncSocket connect handler"));
        }
        return;
      } else {
        throw TTransportException(TTransportException::NOT_OPEN,
                                  "connect failed (immediately)", errno);
      }
    }

    // If we're still here the connect() succeeded immediately.
    // Fall through to call the callback outside of this try...catch block
  } catch (const TTransportException& ex) {
    return failConnect(__func__, ex);
  } catch (const std::exception& ex) {
    // shouldn't happen, but handle it just in case
    VLOG(4) << "TAsyncSocket::connect(this=" << this << ", fd=" << fd_
               << "): unexpected " << typeid(ex).name() << " exception: "
               << ex.what();
    TTransportException tex(TTransportException::INTERNAL_ERROR,
                            withAddr(string("unexpected exception: ") +
                                     ex.what()));
    return failConnect(__func__, tex);
  }

  // The connection succeeded immediately
  // The read callback may not have been set yet, and no writes may be pending
  // yet, so we don't have to register for any events at the moment.
  VLOG(8) << "TAsyncSocket::connect succeeded immediately; this=" << this;
  assert(readCallback_ == nullptr);
  assert(writeReqHead_ == nullptr);
  state_ = StateEnum::ESTABLISHED;
  if (callback) {
    connectCallback_ = nullptr;
    callback->connectSuccess();
  }
}

void TAsyncSocket::connect(ConnectCallback* callback,
                           const string& ip, uint16_t port,
                           int timeout,
                           const OptionMap &options) noexcept {
  DestructorGuard dg(this);
  try {
    connectCallback_ = callback;
    connect(callback, TSocketAddress(ip, port), timeout, options);
  } catch (const std::exception& ex) {
    TTransportException tex(TTransportException::INTERNAL_ERROR,
                            ex.what());
    return failConnect(__func__, tex);
  }
}

void TAsyncSocket::setSendTimeout(uint32_t milliseconds) {
  sendTimeout_ = milliseconds;
  assert(eventBase_ == nullptr || eventBase_->isInEventBaseThread());

  // If we are currently pending on write requests, immediately update
  // writeTimeout_ with the new value.
  if ((eventFlags_ & TEventHandler::WRITE) &&
      (state_ != StateEnum::CONNECTING)) {
    assert(state_ == StateEnum::ESTABLISHED);
    assert((shutdownFlags_ & SHUT_WRITE) == 0);
    if (sendTimeout_ > 0) {
      if (!writeTimeout_.scheduleTimeout(sendTimeout_)) {
        TTransportException ex(TTransportException::INTERNAL_ERROR,
            withAddr("failed to reschedule send timeout in setSendTimeout"));
        return failWrite(__func__, ex);
      }
    } else {
      writeTimeout_.cancelTimeout();
    }
  }
}

void TAsyncSocket::setReadCallback(ReadCallback *callback) {
  VLOG(6) << "TAsyncSocket::setReadCallback() this=" << this << ", fd=" << fd_
          << ", callback=" << callback << ", state=" << state_;

  // Short circuit if callback is the same as the existing readCallback_.
  //
  // Note that this is needed for proper functioning during some cleanup cases.
  // During cleanup we allow setReadCallback(nullptr) to be called even if the
  // read callback is already unset and we have been detached from an event
  // base.  This check prevents us from asserting
  // eventBase_->isInEventBaseThread() when eventBase_ is nullptr.
  if (callback == readCallback_) {
    return;
  }

  if (shutdownFlags_ & SHUT_READ) {
    // Reads have already been shut down on this socket.
    //
    // Allow setReadCallback(nullptr) to be called in this case, but don't
    // allow a new callback to be set.
    //
    // For example, setReadCallback(nullptr) can happen after an error if we
    // invoke some other error callback before invoking readError().  The other
    // error callback that is invoked first may go ahead and clear the read
    // callback before we get a chance to invoke readError().
    if (callback != nullptr) {
      return invalidState(callback);
    }
    assert((eventFlags_ & TEventHandler::READ) == 0);
    readCallback_ = nullptr;
    return;
  }

  DestructorGuard dg(this);
  assert(eventBase_->isInEventBaseThread());

  switch ((StateEnum)state_) {
    case StateEnum::CONNECTING:
      // For convenience, we allow the read callback to be set while we are
      // still connecting.  We just store the callback for now.  Once the
      // connection completes we'll register for read events.
      readCallback_ = callback;
      return;
    case StateEnum::ESTABLISHED:
    {
      readCallback_ = callback;
      uint16_t oldFlags = eventFlags_;
      if (readCallback_) {
        eventFlags_ |= TEventHandler::READ;
      } else {
        eventFlags_ &= ~TEventHandler::READ;
      }

      // Update our registration if our flags have changed
      if (eventFlags_ != oldFlags) {
        // We intentionally ignore the return value here.
        // updateEventRegistration() will move us into the error state if it
        // fails, and we don't need to do anything else here afterwards.
        (void)updateEventRegistration();
      }

      if (readCallback_) {
        checkForImmediateRead();
      }
      return;
    }
    case StateEnum::CLOSED:
    case StateEnum::ERROR:
      // We should never reach here.  SHUT_READ should always be set
      // if we are in STATE_CLOSED or STATE_ERROR.
      assert(false);
      return invalidState(callback);
    case StateEnum::UNINIT:
      // We do not allow setReadCallback() to be called before we start
      // connecting.
      return invalidState(callback);
  }

  // We don't put a default case in the switch statement, so that the compiler
  // will warn us to update the switch statement if a new state is added.
  return invalidState(callback);
}

TAsyncTransport::ReadCallback* TAsyncSocket::getReadCallback() const {
  return readCallback_;
}

void TAsyncSocket::write(WriteCallback* callback,
                         const void* buf, size_t bytes, WriteFlags flags) {
  iovec op;
  op.iov_base = const_cast<void*>(buf);
  op.iov_len = bytes;
  writeImpl(callback, &op, 1, std::move(unique_ptr<IOBuf>()), flags);
}

void TAsyncSocket::writev(WriteCallback* callback,
                          const iovec* vec,
                          size_t count,
                          WriteFlags flags) {
  writeImpl(callback, vec, count, std::move(unique_ptr<IOBuf>()), flags);
}

void TAsyncSocket::writeChain(WriteCallback* callback, unique_ptr<IOBuf>&& buf,
                              WriteFlags flags) {
  size_t count = buf->countChainElements();
  if (count <= 64) {
    iovec vec[count];
    writeChainImpl(callback, vec, count, std::move(buf), flags);
  } else {
    iovec* vec = new iovec[count];
    writeChainImpl(callback, vec, count, std::move(buf), flags);
    delete[] vec;
  }
}

void TAsyncSocket::writeChainImpl(WriteCallback* callback, iovec* vec,
    size_t count, unique_ptr<IOBuf>&& buf, WriteFlags flags) {
  const IOBuf* head = buf.get();
  const IOBuf* next = head;
  unsigned i = 0;
  do {
    vec[i].iov_base = const_cast<uint8_t *>(next->data());
    vec[i].iov_len = next->length();
    // IOBuf can get confused by empty iovec buffers, so increment the
    // output pointer only if the iovec buffer is non-empty.  We could
    // end the loop with i < count, but that's ok.
    if (vec[i].iov_len != 0) {
      i++;
    }
    next = next->next();
  } while (next != head);
  writeImpl(callback, vec, i, std::move(buf), flags);
}

void TAsyncSocket::writeImpl(WriteCallback* callback, const iovec* vec,
                             size_t count, unique_ptr<IOBuf>&& buf,
                             WriteFlags flags) {
  VLOG(6) << "TAsyncSocket::writev() this=" << this << ", fd=" << fd_
          << ", callback=" << callback << ", count=" << count
          << ", state=" << state_;
  DestructorGuard dg(this);
  unique_ptr<IOBuf>ioBuf(std::move(buf));
  assert(eventBase_->isInEventBaseThread());

  if (shutdownFlags_ & (SHUT_WRITE | SHUT_WRITE_PENDING)) {
    // No new writes may be performed after the write side of the socket has
    // been shutdown.
    //
    // We could just call callback->writeError() here to fail just this write.
    // However, fail hard and use invalidState() to fail all outstanding
    // callbacks and move the socket into the error state.  There's most likely
    // a bug in the caller's code, so we abort everything rather than trying to
    // proceed as best we can.
    return invalidState(callback);
  }

  uint32_t countWritten = 0;
  uint32_t partialWritten = 0;
  int bytesWritten = 0;
  bool mustRegister = false;
  if (state_ == StateEnum::ESTABLISHED && !connecting()) {
    if (writeReqHead_ == nullptr) {
      // If we are established and there are no other writes pending,
      // we can attempt to perform the write immediately.
      assert(writeReqTail_ == nullptr);
      assert((eventFlags_ & TEventHandler::WRITE) == 0);

      bytesWritten = performWrite(vec, count, flags,
                                  &countWritten, &partialWritten);
      if (bytesWritten < 0) {
        TTransportException ex(TTransportException::INTERNAL_ERROR,
                               withAddr("writev failed"), errno);
        return failWrite(__func__, callback, 0, ex);
      } else if (countWritten == count) {
        // We successfully wrote everything.
        // Invoke the callback and return.
        if (callback) {
          callback->writeSuccess();
        }
        return;
      } // else { continue writing the next writeReq }
      mustRegister = true;
    }
  } else if (!connecting()) {
    // Invalid state for writing
    return invalidState(callback);
  }

  // Create a new WriteRequest to add to the queue
  WriteRequest* req;
  try {
    req = WriteRequest::newRequest(callback, vec + countWritten,
                                   count - countWritten, std::move(ioBuf),
                                   flags);
  } catch (const std::exception& ex) {
    // we mainly expect to catch std::bad_alloc here
    TTransportException tex(TTransportException::INTERNAL_ERROR,
        withAddr(string("failed to append new WriteRequest: ") + ex.what()));
    return failWrite(__func__, callback, bytesWritten, tex);
  }
  req->consume(0, partialWritten, bytesWritten);
  if (writeReqTail_ == nullptr) {
    assert(writeReqHead_ == nullptr);
    writeReqHead_ = writeReqTail_ = req;
  } else {
    writeReqTail_->append(req);
    writeReqTail_ = req;
  }

  // Register for write events if are established and not currently
  // waiting on write events
  if (mustRegister) {
    assert(state_ == StateEnum::ESTABLISHED);
    assert((eventFlags_ & TEventHandler::WRITE) == 0);
    if (!updateEventRegistration(TEventHandler::WRITE, 0)) {
      assert(state_ == StateEnum::ERROR);
      return;
    }
    if (sendTimeout_ > 0) {
      // Schedule a timeout to fire if the write takes too long.
      if (!writeTimeout_.scheduleTimeout(sendTimeout_)) {
        TTransportException ex(TTransportException::INTERNAL_ERROR,
                               withAddr("failed to schedule send timeout"));
        return failWrite(__func__, ex);
      }
    }
  }
}

void TAsyncSocket::close() {
  VLOG(5) << "TAsyncSocket::close(): this=" << this << ", fd_=" << fd_
          << ", state=" << state_ << ", shutdownFlags="
          << std::hex << (int) shutdownFlags_;

  // close() is only different from closeNow() when there are pending writes
  // that need to drain before we can close.  In all other cases, just call
  // closeNow().
  //
  // Note that writeReqHead_ can be non-nullptr even in STATE_CLOSED or
  // STATE_ERROR if close() is invoked while a previous closeNow() or failure
  // is still running.  (e.g., If there are multiple pending writes, and we
  // call writeError() on the first one, it may call close().  In this case we
  // will already be in STATE_CLOSED or STATE_ERROR, but the remaining pending
  // writes will still be in the queue.)
  //
  // We only need to drain pending writes if we are still in STATE_CONNECTING
  // or STATE_ESTABLISHED
  if ((writeReqHead_ == nullptr) ||
      !(state_ == StateEnum::CONNECTING ||
      state_ == StateEnum::ESTABLISHED)) {
    closeNow();
    return;
  }

  // Declare a DestructorGuard to ensure that the TAsyncSocket cannot be
  // destroyed until close() returns.
  DestructorGuard dg(this);
  assert(eventBase_->isInEventBaseThread());

  // Since there are write requests pending, we have to set the
  // SHUT_WRITE_PENDING flag, and wait to perform the real close until the
  // connect finishes and we finish writing these requests.
  //
  // Set SHUT_READ to indicate that reads are shut down, and set the
  // SHUT_WRITE_PENDING flag to mark that we want to shutdown once the
  // pending writes complete.
  shutdownFlags_ |= (SHUT_READ | SHUT_WRITE_PENDING);

  // If a read callback is set, invoke readEOF() immediately to inform it that
  // the socket has been closed and no more data can be read.
  if (readCallback_) {
    // Disable reads if they are enabled
    if (!updateEventRegistration(0, TEventHandler::READ)) {
      // We're now in the error state; callbacks have been cleaned up
      assert(state_ == StateEnum::ERROR);
      assert(readCallback_ == nullptr);
    } else {
      ReadCallback* callback = readCallback_;
      readCallback_ = nullptr;
      callback->readEOF();
    }
  }
}

void TAsyncSocket::closeNow() {
  VLOG(5) << "TAsyncSocket::closeNow(): this=" << this << ", fd_=" << fd_
          << ", state=" << state_ << ", shutdownFlags="
          << std::hex << (int) shutdownFlags_;
  DestructorGuard dg(this);
  assert(eventBase_ == nullptr || eventBase_->isInEventBaseThread());

  switch (state_) {
    case StateEnum::ESTABLISHED:
    case StateEnum::CONNECTING:
    {
      shutdownFlags_ |= (SHUT_READ | SHUT_WRITE);
      state_ = StateEnum::CLOSED;

      // If the write timeout was set, cancel it.
      writeTimeout_.cancelTimeout();

      // If we are registered for I/O events, unregister.
      if (eventFlags_ != TEventHandler::NONE) {
        eventFlags_ = TEventHandler::NONE;
        if (!updateEventRegistration()) {
          // We will have been moved into the error state.
          assert(state_ == StateEnum::ERROR);
          return;
        }
      }

      if (fd_ >= 0) {
        ioHandler_.changeHandlerFD(-1);
        doClose();
      }

      if (connectCallback_) {
        ConnectCallback* callback = connectCallback_;
        connectCallback_ = nullptr;
        callback->connectError(socketClosedLocallyEx);
      }

      failAllWrites(socketClosedLocallyEx);

      if (readCallback_) {
        ReadCallback* callback = readCallback_;
        readCallback_ = nullptr;
        callback->readEOF();
      }
      return;
    }
    case StateEnum::CLOSED:
      // Do nothing.  It's possible that we are being called recursively
      // from inside a callback that we invoked inside another call to close()
      // that is still running.
      return;
    case StateEnum::ERROR:
      // Do nothing.  The error handling code has performed (or is performing)
      // cleanup.
      return;
    case StateEnum::UNINIT:
      assert(eventFlags_ == TEventHandler::NONE);
      assert(connectCallback_ == nullptr);
      assert(readCallback_ == nullptr);
      assert(writeReqHead_ == nullptr);
      shutdownFlags_ |= (SHUT_READ | SHUT_WRITE);
      state_ = StateEnum::CLOSED;
      return;
  }

  LOG(DFATAL) << "TAsyncSocket::closeNow() (this=" << this << ", fd=" << fd_
              << ") called in unknown state " << state_;
}

void TAsyncSocket::closeWithReset() {
  // Enable SO_LINGER, with the linger timeout set to 0.
  // This will trigger a TCP reset when we close the socket.
  if (fd_ >= 0) {
    struct linger optLinger = {1, 0};
    if (setSockOpt(SOL_SOCKET, SO_LINGER, &optLinger) != 0) {
      VLOG(2) << "TAsyncSocket::closeWithReset(): error setting SO_LINGER "
              << "on " << fd_ << ": errno=" << errno;
    }
  }

  // Then let closeNow() take care of the rest
  closeNow();
}

void TAsyncSocket::shutdownWrite() {
  VLOG(5) << "TAsyncSocket::shutdownWrite(): this=" << this << ", fd=" << fd_
          << ", state=" << state_ << ", shutdownFlags="
          << std::hex << (int) shutdownFlags_;

  // If there are no pending writes, shutdownWrite() is identical to
  // shutdownWriteNow().
  if (writeReqHead_ == nullptr) {
    shutdownWriteNow();
    return;
  }

  assert(eventBase_->isInEventBaseThread());

  // There are pending writes.  Set SHUT_WRITE_PENDING so that the actual
  // shutdown will be performed once all writes complete.
  shutdownFlags_ |= SHUT_WRITE_PENDING;
}

void TAsyncSocket::shutdownWriteNow() {
  VLOG(5) << "TAsyncSocket::shutdownWriteNow(): this=" << this
          << ", fd=" << fd_ << ", state=" << state_
          << ", shutdownFlags=" << std::hex << (int) shutdownFlags_;

  if (shutdownFlags_ & SHUT_WRITE) {
    // Writes are already shutdown; nothing else to do.
    return;
  }

  // If SHUT_READ is already set, just call closeNow() to completely
  // close the socket.  This can happen if close() was called with writes
  // pending, and then shutdownWriteNow() is called before all pending writes
  // complete.
  if (shutdownFlags_ & SHUT_READ) {
    closeNow();
    return;
  }

  DestructorGuard dg(this);
  assert(eventBase_ == nullptr || eventBase_->isInEventBaseThread());

  switch (static_cast<StateEnum>(state_)) {
    case StateEnum::ESTABLISHED:
    {
      shutdownFlags_ |= SHUT_WRITE;

      // If the write timeout was set, cancel it.
      writeTimeout_.cancelTimeout();

      // If we are registered for write events, unregister.
      if (!updateEventRegistration(0, TEventHandler::WRITE)) {
        // We will have been moved into the error state.
        assert(state_ == StateEnum::ERROR);
        return;
      }

      // Shutdown writes on the file descriptor
      ::shutdown(fd_, SHUT_WR);

      // Immediately fail all write requests
      failAllWrites(socketShutdownForWritesEx);
      return;
    }
    case StateEnum::CONNECTING:
    {
      // Set the SHUT_WRITE_PENDING flag.
      // When the connection completes, it will check this flag,
      // shutdown the write half of the socket, and then set SHUT_WRITE.
      shutdownFlags_ |= SHUT_WRITE_PENDING;

      // Immediately fail all write requests
      failAllWrites(socketShutdownForWritesEx);
      return;
    }
    case StateEnum::UNINIT:
      // Callers normally shouldn't call shutdownWriteNow() before the socket
      // even starts connecting.  Nonetheless, go ahead and set
      // SHUT_WRITE_PENDING.  Once the socket eventually connects it will
      // immediately shut down the write side of the socket.
      shutdownFlags_ |= SHUT_WRITE_PENDING;
      return;
    case StateEnum::CLOSED:
    case StateEnum::ERROR:
      // We should never get here.  SHUT_WRITE should always be set
      // in STATE_CLOSED and STATE_ERROR.
      VLOG(4) << "TAsyncSocket::shutdownWriteNow() (this=" << this
                 << ", fd=" << fd_ << ") in unexpected state " << state_
                 << " with SHUT_WRITE not set ("
                 << std::hex << (int) shutdownFlags_ << ")";
      assert(false);
      return;
  }

  LOG(DFATAL) << "TAsyncSocket::shutdownWriteNow() (this=" << this << ", fd="
              << fd_ << ") called in unknown state " << state_;
}

bool TAsyncSocket::readable() const {
  if (fd_ == -1) {
    return false;
  }
  struct pollfd fds[1];
  fds[0].fd = fd_;
  fds[0].events = POLLIN;
  fds[0].revents = 0;
  int rc = poll(fds, 1, 0);
  return rc == 1;
}

bool TAsyncSocket::isPending() const {
  return ioHandler_.isPending();
}

bool TAsyncSocket::hangup() const {
  if (fd_ == -1) {
    // sanity check, no one should ask for hangup if we are not connected.
    assert(false);
    return false;
  }
  struct pollfd fds[1];
  fds[0].fd = fd_;
  fds[0].events = POLLRDHUP|POLLHUP;
  fds[0].revents = 0;
  poll(fds, 1, 0);
  return (fds[0].revents & (POLLRDHUP|POLLHUP)) != 0;
}

bool TAsyncSocket::good() const {
  return ((state_ == StateEnum::CONNECTING ||
          state_ == StateEnum::ESTABLISHED) &&
          (shutdownFlags_ == 0) && (eventBase_ != nullptr));
}

bool TAsyncSocket::error() const {
  return (state_ == StateEnum::ERROR);
}

void TAsyncSocket::attachEventBase(TEventBase* eventBase) {
  VLOG(5) << "TAsyncSocket::attachEventBase(this=" << this << ", fd=" << fd_
          << ", old evb=" << eventBase_ << ", new evb=" << eventBase
          << ", state=" << state_ << ", events="
          << std::hex << eventFlags_ << ")";
  assert(eventBase_ == nullptr);
  assert(eventBase->isInEventBaseThread());

  eventBase_ = eventBase;
  ioHandler_.attachEventBase(eventBase);
  writeTimeout_.attachEventBase(eventBase);
}

void TAsyncSocket::detachEventBase() {
  VLOG(5) << "TAsyncSocket::detachEventBase(this=" << this << ", fd=" << fd_
          << ", old evb=" << eventBase_ << ", state=" << state_
          << ", events=" << std::hex << eventFlags_ << ")";
  assert(eventBase_ != nullptr);
  assert(eventBase_->isInEventBaseThread());

  eventBase_ = nullptr;
  ioHandler_.detachEventBase();
  writeTimeout_.detachEventBase();
}

bool TAsyncSocket::isDetachable() const {
  DCHECK(eventBase_ != nullptr);
  DCHECK(eventBase_->isInEventBaseThread());

  return !ioHandler_.isHandlerRegistered() && !writeTimeout_.isScheduled();
}

void TAsyncSocket::getLocalAddress(TSocketAddress* address) const {
  address->setFromLocalAddress(fd_);
}

void TAsyncSocket::getPeerAddress(TSocketAddress* address) const {
  if (!addr_.isInitialized()) {
    addr_.setFromPeerAddress(fd_);
  }
  *address = addr_;
}

int TAsyncSocket::setNoDelay(bool noDelay) {
  if (fd_ < 0) {
    VLOG(4) << "TAsyncSocket::setNoDelay() called on non-open socket "
               << this << "(state=" << state_ << ")";
    return EINVAL;

  }

  int value = noDelay ? 1 : 0;
  if (setsockopt(fd_, IPPROTO_TCP, TCP_NODELAY, &value, sizeof(value)) != 0) {
    int errnoCopy = errno;
    VLOG(2) << "failed to update TCP_NODELAY option on TAsyncSocket "
            << this << " (fd=" << fd_ << ", state=" << state_ << "): "
            << strerror(errnoCopy);
    return errnoCopy;
  }

  return 0;
}

int TAsyncSocket::setCongestionFlavor(const std::string &cname) {

  #ifndef TCP_CONGESTION
  #define TCP_CONGESTION  13
  #endif

  if (fd_ < 0) {
    VLOG(4) << "TAsyncSocket::setCongestionFlavor() called on non-open "
               << "socket " << this << "(state=" << state_ << ")";
    return EINVAL;

  }

  if (setsockopt(fd_, IPPROTO_TCP, TCP_CONGESTION, cname.c_str(),
        cname.length() + 1) != 0) {
    int errnoCopy = errno;
    VLOG(2) << "failed to update TCP_CONGESTION option on TAsyncSocket "
            << this << "(fd=" << fd_ << ", state=" << state_ << "): "
            << strerror(errnoCopy);
    return errnoCopy;
  }

  return 0;
}

int TAsyncSocket::setQuickAck(bool quickack) {
  if (fd_ < 0) {
    VLOG(4) << "TAsyncSocket::setQuickAck() called on non-open socket "
               << this << "(state=" << state_ << ")";
    return EINVAL;

  }

  int value = quickack ? 1 : 0;
  if (setsockopt(fd_, IPPROTO_TCP, TCP_QUICKACK, &value, sizeof(value)) != 0) {
    int errnoCopy = errno;
    VLOG(2) << "failed to update TCP_QUICKACK option on TAsyncSocket"
            << this << "(fd=" << fd_ << ", state=" << state_ << "): "
            << strerror(errnoCopy);
    return errnoCopy;
  }

  return 0;
}

int TAsyncSocket::setSendBufSize(size_t bufsize) {
  if (fd_ < 0) {
    VLOG(4) << "TAsyncSocket::setSendBufSize() called on non-open socket "
               << this << "(state=" << state_ << ")";
    return EINVAL;
  }

  if (setsockopt(fd_, SOL_SOCKET, SO_SNDBUF, &bufsize, sizeof(bufsize)) !=0) {
    int errnoCopy = errno;
    VLOG(2) << "failed to update SO_SNDBUF option on TAsyncSocket"
            << this << "(fd=" << fd_ << ", state=" << state_ << "): "
            << strerror(errnoCopy);
    return errnoCopy;
  }

  return 0;
}

int TAsyncSocket::setRecvBufSize(size_t bufsize) {
  if (fd_ < 0) {
    VLOG(4) << "TAsyncSocket::setRecvBufSize() called on non-open socket "
               << this << "(state=" << state_ << ")";
    return EINVAL;
  }

  if (setsockopt(fd_, SOL_SOCKET, SO_RCVBUF, &bufsize, sizeof(bufsize)) !=0) {
    int errnoCopy = errno;
    VLOG(2) << "failed to update SO_RCVBUF option on TAsyncSocket"
            << this << "(fd=" << fd_ << ", state=" << state_ << "): "
            << strerror(errnoCopy);
    return errnoCopy;
  }

  return 0;
}

int TAsyncSocket::setTCPProfile(int profd) {
  if (fd_ < 0) {
    VLOG(4) << "TAsyncSocket::setTCPProfile() called on non-open socket "
               << this << "(state=" << state_ << ")";
    return EINVAL;
  }

  if (setsockopt(fd_, SOL_SOCKET, SO_SET_NAMESPACE, &profd, sizeof(int)) !=0) {
    int errnoCopy = errno;
    VLOG(2) << "failed to set socket namespace option on TAsyncSocket"
            << this << "(fd=" << fd_ << ", state=" << state_ << "): "
            << strerror(errnoCopy);
    return errnoCopy;
  }

  return 0;
}

void TAsyncSocket::ioReady(uint16_t events) noexcept {
  VLOG(7) << "TAsyncSocket::ioRead() this=" << this << ", fd" << fd_
          << ", events=" << std::hex << events << ", state=" << state_;
  DestructorGuard dg(this);
  assert(events & TEventHandler::READ_WRITE);
  assert(eventBase_->isInEventBaseThread());

  uint16_t relevantEvents = events & TEventHandler::READ_WRITE;
  if (relevantEvents == TEventHandler::READ) {
    handleRead();
  } else if (relevantEvents == TEventHandler::WRITE) {
    handleWrite();
  } else if (relevantEvents == TEventHandler::READ_WRITE) {
    TEventBase* originalEventBase = eventBase_;
    // If both read and write events are ready, process writes first.
    handleWrite();

    // Return now if handleWrite() detached us from our TEventBase
    if (eventBase_ != originalEventBase) {
      return;
    }

    // Only call handleRead() if a read callback is still installed.
    // (It's possible that the read callback was uninstalled during
    // handleWrite().)
    if (readCallback_) {
      handleRead();
    }
  } else {
    VLOG(4) << "TAsyncSocket::ioRead() called with unexpected events "
               << std::hex << events << "(this=" << this << ")";
    abort();
  }
}

ssize_t TAsyncSocket::performRead(void* buf, size_t buflen) {
  ssize_t bytes = recv(fd_, buf, buflen, MSG_DONTWAIT);
  if (bytes < 0) {
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      // No more data to read right now.
      return READ_BLOCKING;
    } else {
      return READ_ERROR;
    }
  } else {
    appBytesReceived_ += bytes;
    return bytes;
  }
}

void TAsyncSocket::handleRead() noexcept {
  VLOG(5) << "TAsyncSocket::handleRead() this=" << this << ", fd=" << fd_
          << ", state=" << state_;
  assert(state_ == StateEnum::ESTABLISHED);
  assert((shutdownFlags_ & SHUT_READ) == 0);
  assert(readCallback_ != nullptr);
  assert(eventFlags_ & TEventHandler::READ);

  // Loop until:
  // - a read attempt would block
  // - readCallback_ is uninstalled
  // - the number of loop iterations exceeds the optional maximum
  // - this TAsyncSocket is moved to another TEventBase
  //
  // When we invoke readDataAvailable() it may uninstall the readCallback_,
  // which is why need to check for it here.
  //
  // The last bullet point is slightly subtle.  readDataAvailable() may also
  // detach this socket from this TEventBase.  However, before
  // readDataAvailable() returns another thread may pick it up, attach it to
  // a different TEventBase, and install another readCallback_.  We need to
  // exit immediately after readDataAvailable() returns if the eventBase_ has
  // changed.  (The caller must perform some sort of locking to transfer the
  // TAsyncSocket between threads properly.  This will be sufficient to ensure
  // that this thread sees the updated eventBase_ variable after
  // readDataAvailable() returns.)
  uint16_t numReads = 0;
  TEventBase* originalEventBase = eventBase_;
  while (readCallback_ && eventBase_ == originalEventBase) {
    // Get the buffer to read into.
    void* buf = nullptr;
    size_t buflen = 0;
    try {
      readCallback_->getReadBuffer(&buf, &buflen);
    } catch (const TTransportException& ex) {
      return failRead(__func__, ex);
    } catch (const std::exception& ex) {
      TTransportException tex(TTransportException::BAD_ARGS,
                              string("ReadCallback::getReadBuffer() "
                                     "threw exception: ") +
                              ex.what());
      return failRead(__func__, tex);
    } catch (...) {
      TTransportException ex(TTransportException::BAD_ARGS,
                             "ReadCallback::getReadBuffer() threw "
                             "non-exception type");
      return failRead(__func__, ex);
    }
    if (buf == nullptr || buflen == 0) {
      TTransportException ex(TTransportException::BAD_ARGS,
                             "ReadCallback::getReadBuffer() returned "
                             "empty buffer");
      return failRead(__func__, ex);
    }

    // Perform the read
    ssize_t bytesRead = performRead(buf, buflen);
    if (bytesRead > 0) {
      readCallback_->readDataAvailable(bytesRead);
      // Fall through and continue around the loop if the read
      // completely filled the available buffer.
      // Note that readCallback_ may have been uninstalled or changed inside
      // readDataAvailable().
      if (bytesRead < buflen) {
        return;
      }
    } else if (bytesRead == READ_BLOCKING) {
        // No more data to read right now.
        return;
    } else if (bytesRead == READ_ERROR) {
      TTransportException ex(TTransportException::INTERNAL_ERROR,
                             withAddr("recv() failed"), errno);
      return failRead(__func__, ex);
    } else {
      assert(bytesRead == READ_EOF);
      // EOF
      shutdownFlags_ |= SHUT_READ;
      if (!updateEventRegistration(0, TEventHandler::READ)) {
        // we've already been moved into STATE_ERROR
        assert(state_ == StateEnum::ERROR);
        assert(readCallback_ == nullptr);
        return;
      }

      ReadCallback* callback = readCallback_;
      readCallback_ = nullptr;
      callback->readEOF();
      return;
    }
    if (maxReadsPerEvent_ && (++numReads >= maxReadsPerEvent_)) {
      return;
    }
  }
}

/**
 * This function attempts to write as much data as possible, until no more data
 * can be written.
 *
 * - If it sends all available data, it unregisters for write events, and stops
 *   the writeTimeout_.
 *
 * - If not all of the data can be sent immediately, it reschedules
 *   writeTimeout_ (if a non-zero timeout is set), and ensures the handler is
 *   registered for write events.
 */
void TAsyncSocket::handleWrite() noexcept {
  VLOG(5) << "TAsyncSocket::handleWrite() this=" << this << ", fd=" << fd_
          << ", state=" << state_;
  if (state_ == StateEnum::CONNECTING) {
    handleConnect();
    return;
  }

  // Normal write
  assert(state_ == StateEnum::ESTABLISHED);
  assert((shutdownFlags_ & SHUT_WRITE) == 0);
  assert(writeReqHead_ != nullptr);

  // Loop until we run out of write requests,
  // or until this socket is moved to another TEventBase.
  // (See the comment in handleRead() explaining how this can happen.)
  TEventBase* originalEventBase = eventBase_;
  while (writeReqHead_ != nullptr && eventBase_ == originalEventBase) {
    uint32_t countWritten;
    uint32_t partialWritten;
    WriteFlags writeFlags = writeReqHead_->flags();
    if (writeReqHead_->getNext() != nullptr) {
      writeFlags = writeFlags | WriteFlags::CORK;
    }
    int bytesWritten = performWrite(writeReqHead_->getOps(),
                                    writeReqHead_->getOpCount(),
                                    writeFlags, &countWritten, &partialWritten);
    if (bytesWritten < 0) {
      TTransportException ex(TTransportException::INTERNAL_ERROR,
                             withAddr("writev() failed"), errno);
      return failWrite(__func__, ex);
    } else if (countWritten == writeReqHead_->getOpCount()) {
      // We finished this request
      WriteRequest* req = writeReqHead_;
      writeReqHead_ = req->getNext();

      if (writeReqHead_ == nullptr) {
        writeReqTail_ = nullptr;
        // This is the last write request.
        // Unregister for write events and cancel the send timer
        // before we invoke the callback.  We have to update the state properly
        // before calling the callback, since it may want to detach us from
        // the TEventBase.
        if (eventFlags_ & TEventHandler::WRITE) {
          if (!updateEventRegistration(0, TEventHandler::WRITE)) {
            assert(state_ == StateEnum::ERROR);
            return;
          }
          // Stop the send timeout
          writeTimeout_.cancelTimeout();
        }
        assert(!writeTimeout_.isScheduled());

        // If SHUT_WRITE_PENDING is set, we should shutdown the socket after
        // we finish sending the last write request.
        //
        // We have to do this before invoking writeSuccess(), since
        // writeSuccess() may detach us from our TEventBase.
        if (shutdownFlags_ & SHUT_WRITE_PENDING) {
          assert(connectCallback_ == nullptr);
          shutdownFlags_ |= SHUT_WRITE;

          if (shutdownFlags_ & SHUT_READ) {
            // Reads have already been shutdown.  Fully close the socket and
            // move to STATE_CLOSED.
            //
            // Note: This code currently moves us to STATE_CLOSED even if
            // close() hasn't ever been called.  This can occur if we have
            // received EOF from the peer and shutdownWrite() has been called
            // locally.  Should we bother staying in STATE_ESTABLISHED in this
            // case, until close() is actually called?  I can't think of a
            // reason why we would need to do so.  No other operations besides
            // calling close() or destroying the socket can be performed at
            // this point.
            assert(readCallback_ == nullptr);
            state_ = StateEnum::CLOSED;
            if (fd_ >= 0) {
              ioHandler_.changeHandlerFD(-1);
              doClose();
            }
          } else {
            // Reads are still enabled, so we are only doing a half-shutdown
            ::shutdown(fd_, SHUT_WR);
          }
        }
      }

      // Invoke the callback
      WriteCallback* callback = req->getCallback();
      req->destroy();
      if (callback) {
        callback->writeSuccess();
      }
      // We'll continue around the loop, trying to write another request
    } else {
      // Partial write.
      writeReqHead_->consume(countWritten, partialWritten, bytesWritten);
      // Stop after a partial write; it's highly likely that a subsequent write
      // attempt will just return EAGAIN.
      //
      // Ensure that we are registered for write events.
      if ((eventFlags_ & TEventHandler::WRITE) == 0) {
        if (!updateEventRegistration(TEventHandler::WRITE, 0)) {
          assert(state_ == StateEnum::ERROR);
          return;
        }
      }

      // Reschedule the send timeout, since we have made some write progress.
      if (sendTimeout_ > 0) {
        if (!writeTimeout_.scheduleTimeout(sendTimeout_)) {
          TTransportException ex(TTransportException::INTERNAL_ERROR,
              withAddr("failed to reschedule write timeout"));
          return failWrite(__func__, ex);
        }
      }
      return;
    }
  }
}

void TAsyncSocket::checkForImmediateRead() noexcept {
  // We currently don't attempt to perform optimistic reads in TAsyncSocket.
  // (However, note that some subclasses do override this method.)
  //
  // Simply calling handleRead() here would be bad, as this would call
  // readCallback_->getReadBuffer(), forcing the callback to allocate a read
  // buffer even though no data may be available.  This would waste lots of
  // memory, since the buffer will sit around unused until the socket actually
  // becomes readable.
  //
  // Checking if the socket is readable now also seems like it would probably
  // be a pessimism.  In most cases it probably wouldn't be readable, and we
  // would just waste an extra system call.  Even if it is readable, waiting to
  // find out from libevent on the next event loop doesn't seem that bad.
}

void TAsyncSocket::handleInitialReadWrite() noexcept {
  // Our callers should already be holding a DestructorGuard, but grab
  // one here just to make sure, in case one of our calling code paths ever
  // changes.
  DestructorGuard dg(this);

  // If we have a readCallback_, make sure we enable read events.  We
  // may already be registered for reads if connectSuccess() set
  // the read calback.
  if (readCallback_ && !(eventFlags_ & TEventHandler::READ)) {
    assert(state_ == StateEnum::ESTABLISHED);
    assert((shutdownFlags_ & SHUT_READ) == 0);
    if (!updateEventRegistration(TEventHandler::READ, 0)) {
      assert(state_ == StateEnum::ERROR);
      return;
    }
    checkForImmediateRead();
  } else if (readCallback_ == nullptr) {
    // Unregister for read events.
    updateEventRegistration(0, TEventHandler::READ);
  }

  // If we have write requests pending, try to send them immediately.
  // Since we just finished accepting, there is a very good chance that we can
  // write without blocking.
  //
  // However, we only process them if TEventHandler::WRITE is not already set,
  // which means that we're already blocked on a write attempt.  (This can
  // happen if connectSuccess() called write() before returning.)
  if (writeReqHead_ && !(eventFlags_ & TEventHandler::WRITE)) {
    // Call handleWrite() to perform write processing.
    handleWrite();
  } else if (writeReqHead_ == nullptr) {
    // Unregister for write event.
    updateEventRegistration(0, TEventHandler::WRITE);
  }
}

void TAsyncSocket::handleConnect() noexcept {
  VLOG(5) << "TAsyncSocket::handleConnect() this=" << this << ", fd=" << fd_
          << ", state=" << state_;
  assert(state_ == StateEnum::CONNECTING);
  // SHUT_WRITE can never be set while we are still connecting;
  // SHUT_WRITE_PENDING may be set, be we only set SHUT_WRITE once the connect
  // finishes
  assert((shutdownFlags_ & SHUT_WRITE) == 0);

  // In case we had a connect timeout, cancel the timeout
  writeTimeout_.cancelTimeout();
  // We don't use a persistent registration when waiting on a connect event,
  // so we have been automatically unregistered now.  Update eventFlags_ to
  // reflect reality.
  assert(eventFlags_ == TEventHandler::WRITE);
  eventFlags_ = TEventHandler::NONE;

  // Call getsockopt() to check if the connect succeeded
  int error;
  socklen_t len = sizeof(error);
  int rv = getsockopt(fd_, SOL_SOCKET, SO_ERROR, &error, &len);
  if (rv != 0) {
    TTransportException ex(TTransportException::INTERNAL_ERROR,
                           withAddr("error calling getsockopt() after connect"),
                           errno);
    VLOG(4) << "TAsyncSocket::handleConnect(this=" << this << ", fd="
               << fd_ << " host=" << addr_.describe()
               << ") exception:" << ex.what();
    return failConnect(__func__, ex);
  }

  if (error != 0) {
    TTransportException ex(TTransportException::NOT_OPEN,
                           "connect failed", error);
    VLOG(1) << "TAsyncSocket::handleConnect(this=" << this << ", fd="
            << fd_ << " host=" << addr_.describe()
            << ") exception: " << ex.what();
    return failConnect(__func__, ex);
  }

  // Move into STATE_ESTABLISHED
  state_ = StateEnum::ESTABLISHED;

  // If SHUT_WRITE_PENDING is set and we don't have any write requests to
  // perform, immediately shutdown the write half of the socket.
  if ((shutdownFlags_ & SHUT_WRITE_PENDING) && writeReqHead_ == nullptr) {
    // SHUT_READ shouldn't be set.  If close() is called on the socket while we
    // are still connecting we just abort the connect rather than waiting for
    // it to complete.
    assert((shutdownFlags_ & SHUT_READ) == 0);
    ::shutdown(fd_, SHUT_WR);
    shutdownFlags_ |= SHUT_WRITE;
  }

  VLOG(7) << "TAsyncSocket " << this << ": fd " << fd_
          << "successfully connected; state=" << state_;

  // Remember the TEventBase we are attached to, before we start invoking any
  // callbacks (since the callbacks may call detachEventBase()).
  TEventBase* originalEventBase = eventBase_;

  // Call the connect callback.
  if (connectCallback_) {
    ConnectCallback* callback = connectCallback_;
    connectCallback_ = nullptr;
    callback->connectSuccess();
  }

  // Note that the connect callback may have changed our state.
  // (set or unset the read callback, called write(), closed the socket, etc.)
  // The following code needs to handle these situations correctly.
  //
  // If the socket has been closed, readCallback_ and writeReqHead_ will
  // always be nullptr, so that will prevent us from trying to read or write.
  //
  // The main thing to check for is if eventBase_ is still originalEventBase.
  // If not, we have been detached from this event base, so we shouldn't
  // perform any more operations.
  if (eventBase_ != originalEventBase) {
    return;
  }

  handleInitialReadWrite();
}

void TAsyncSocket::timeoutExpired() noexcept {
  VLOG(7) << "TAsyncSocket " << this << ", fd " << fd_ << ": timeout expired: "
          << "state=" << state_ << ", events=" << std::hex << eventFlags_;
  DestructorGuard dg(this);
  assert(eventBase_->isInEventBaseThread());

  if (state_ == StateEnum::CONNECTING) {
    // connect() timed out
    // Unregister for I/O events.
    TTransportException ex(TTransportException::TIMED_OUT,
                           "connect timed out");
    failConnect(__func__, ex);
  } else {
    // a normal write operation timed out
    assert(state_ == StateEnum::ESTABLISHED);
    TTransportException ex(TTransportException::TIMED_OUT, "write timed out");
    failWrite(__func__, ex);
  }
}

ssize_t TAsyncSocket::performWrite(const iovec* vec,
                                   uint32_t count,
                                   WriteFlags flags,
                                   uint32_t* countWritten,
                                   uint32_t* partialWritten) {
  // We use sendmsg() instead of writev() so that we can pass in MSG_NOSIGNAL
  // We correctly handle EPIPE errors, so we never want to receive SIGPIPE
  // (since it may terminate the program if the main program doesn't explicitly
  // ignore it).
  struct msghdr msg;
  msg.msg_name = nullptr;
  msg.msg_namelen = 0;
  msg.msg_iov = const_cast<iovec *>(vec);
  msg.msg_iovlen = std::min(count, (uint32_t)IOV_MAX);
  msg.msg_control = nullptr;
  msg.msg_controllen = 0;
  msg.msg_flags = 0;

  int msg_flags = MSG_DONTWAIT | MSG_NOSIGNAL;
  if (isSet(flags, WriteFlags::CORK)) {
    // MSG_MORE tells the kernel we have more data to send, so wait for us to
    // give it the rest of the data rather than immediately sending a partial
    // frame, even when TCP_NODELAY is enabled.
    msg_flags |= MSG_MORE;
  }
  if (isSet(flags, WriteFlags::EOR)) {
    // marks that this is the last byte of a record (response)
    msg_flags |= MSG_EOR;
  }
  ssize_t totalWritten = ::sendmsg(fd_, &msg, msg_flags);
  if (totalWritten < 0) {
    if (errno == EAGAIN) {
      // TCP buffer is full; we can't write any more data right now.
      *countWritten = 0;
      *partialWritten = 0;
      return 0;
    }
    // error
    *countWritten = 0;
    *partialWritten = 0;
    return -1;
  }

  appBytesWritten_ += totalWritten;

  uint32_t bytesWritten;
  uint32_t n;
  for (bytesWritten = totalWritten, n = 0; n < count; ++n) {
    const iovec* v = vec + n;
    if (v->iov_len > bytesWritten) {
      // Partial write finished in the middle of this iovec
      *countWritten = n;
      *partialWritten = bytesWritten;
      return totalWritten;
    }

    bytesWritten -= v->iov_len;
  }

  assert(bytesWritten == 0);
  *countWritten = n;
  *partialWritten = 0;
  return totalWritten;
}

/**
 * Re-register the TEventHandler after eventFlags_ has changed.
 *
 * If an error occurs, fail() is called to move the socket into the error state
 * and call all currently installed callbacks.  After an error, the
 * TAsyncSocket is completely unregistered.
 *
 * @return Returns true on succcess, or false on error.
 */
bool TAsyncSocket::updateEventRegistration() {
  VLOG(5) << "TAsyncSocket::updateEventRegistration(this=" << this
          << ", fd=" << fd_ << ", evb=" << eventBase_ << ", state=" << state_
          << ", events=" << std::hex << eventFlags_;
  assert(eventBase_->isInEventBaseThread());
  if (eventFlags_ == TEventHandler::NONE) {
    ioHandler_.unregisterHandler();
    return true;
  }

  // Always register for persistent events, so we don't have to re-register
  // after being called back.
  if (!ioHandler_.registerHandler(eventFlags_ | TEventHandler::PERSIST)) {
    eventFlags_ = TEventHandler::NONE; // we're not registered after error
    TTransportException ex(TTransportException::INTERNAL_ERROR,
        withAddr("failed to update TAsyncSocket event registration"));
    fail("updateEventRegistration", ex);
    return false;
  }

  return true;
}

bool TAsyncSocket::updateEventRegistration(uint16_t enable,
                                           uint16_t disable) {
  uint16_t oldFlags = eventFlags_;
  eventFlags_ |= enable;
  eventFlags_ &= ~disable;
  if (eventFlags_ == oldFlags) {
    return true;
  } else {
    return updateEventRegistration();
  }
}

void TAsyncSocket::startFail() {
  // startFail() should only be called once
  assert(state_ != StateEnum::ERROR);
  assert(getDestructorGuardCount() > 0);
  state_ = StateEnum::ERROR;
  // Ensure that SHUT_READ and SHUT_WRITE are set,
  // so all future attempts to read or write will be rejected
  shutdownFlags_ |= (SHUT_READ | SHUT_WRITE);

  if (eventFlags_ != TEventHandler::NONE) {
    eventFlags_ = TEventHandler::NONE;
    ioHandler_.unregisterHandler();
  }
  writeTimeout_.cancelTimeout();

  if (fd_ >= 0) {
    ioHandler_.changeHandlerFD(-1);
    doClose();
  }
}

void TAsyncSocket::finishFail() {
  assert(state_ == StateEnum::ERROR);
  assert(getDestructorGuardCount() > 0);

  TTransportException ex(TTransportException::INTERNAL_ERROR,
                         withAddr("socket closing after error"));
  if (connectCallback_) {
    ConnectCallback* callback = connectCallback_;
    connectCallback_ = nullptr;
    callback->connectError(ex);
  }

  failAllWrites(ex);

  if (readCallback_) {
    ReadCallback* callback = readCallback_;
    readCallback_ = nullptr;
    callback->readError(ex);
  }
}

void TAsyncSocket::fail(const char* fn, const TTransportException& ex) {
  VLOG(4) << "TAsyncSocket(this=" << this << ", fd=" << fd_ << ", state="
             << state_ << " host=" << addr_.describe()
             << "): failed in " << fn << "(): "
             << ex.what();
  startFail();
  finishFail();
}

void TAsyncSocket::failConnect(const char* fn, const TTransportException& ex) {
  VLOG(5) << "TAsyncSocket(this=" << this << ", fd=" << fd_ << ", state="
               << state_ << " host=" << addr_.describe()
               << "): failed while connecting in " << fn << "(): "
               << ex.what();
  startFail();

  if (connectCallback_ != nullptr) {
    ConnectCallback* callback = connectCallback_;
    connectCallback_ = nullptr;
    callback->connectError(ex);
  }

  finishFail();
}

void TAsyncSocket::failRead(const char* fn, const TTransportException& ex) {
  VLOG(5) << "TAsyncSocket(this=" << this << ", fd=" << fd_ << ", state="
               << state_ << " host=" << addr_.describe()
               << "): failed while reading in " << fn << "(): "
               << ex.what();
  startFail();

  if (readCallback_ != nullptr) {
    ReadCallback* callback = readCallback_;
    readCallback_ = nullptr;
    callback->readError(ex);
  }

  finishFail();
}

void TAsyncSocket::failWrite(const char* fn, const TTransportException& ex) {
  VLOG(5) << "TAsyncSocket(this=" << this << ", fd=" << fd_ << ", state="
               << state_ << " host=" << addr_.describe()
               << "): failed while writing in " << fn << "(): "
               << ex.what();
  startFail();

  // Only invoke the first write callback, since the error occurred while
  // writing this request.  Let any other pending write callbacks be invoked in
  // finishFail().
  if (writeReqHead_ != nullptr) {
    WriteRequest* req = writeReqHead_;
    writeReqHead_ = req->getNext();
    WriteCallback* callback = req->getCallback();
    uint32_t bytesWritten = req->getBytesWritten();
    req->destroy();
    if (callback) {
      callback->writeError(bytesWritten, ex);
    }
  }

  finishFail();
}

void TAsyncSocket::failWrite(const char* fn, WriteCallback* callback,
                             size_t bytesWritten,
                             const transport::TTransportException& ex) {
  // This version of failWrite() is used when the failure occurs before
  // we've added the callback to writeReqHead_.
  VLOG(4) << "TAsyncSocket(this=" << this << ", fd=" << fd_ << ", state="
             << state_ << " host=" << addr_.describe()
             <<"): failed while writing in " << fn << "(): "
             << ex.what();
  startFail();

  if (callback != nullptr) {
    callback->writeError(bytesWritten, ex);
  }

  finishFail();
}

void TAsyncSocket::failAllWrites(const TTransportException& ex) {
  // Invoke writeError() on all write callbacks.
  // This is used when writes are forcibly shutdown with write requests
  // pending, or when an error occurs with writes pending.
  while (writeReqHead_ != nullptr) {
    WriteRequest* req = writeReqHead_;
    writeReqHead_ = req->getNext();
    WriteCallback* callback = req->getCallback();
    if (callback) {
      callback->writeError(req->getBytesWritten(), ex);
    }
    req->destroy();
  }
}

void TAsyncSocket::invalidState(ConnectCallback* callback) {
  VLOG(5) << "TAsyncSocket(this=" << this << ", fd=" << fd_
             << "): connect() called in invalid state " << state_;

  /*
   * The invalidState() methods don't use the normal failure mechanisms,
   * since we don't know what state we are in.  We don't want to call
   * startFail()/finishFail() recursively if we are already in the middle of
   * cleaning up.
   */

  TTransportException ex(TTransportException::ALREADY_OPEN,
                         "connect() called with socket in invalid state");
  if (state_ == StateEnum::CLOSED || state_ == StateEnum::ERROR) {
    if (callback) {
      callback->connectError(ex);
    }
  } else {
    // We can't use failConnect() here since connectCallback_
    // may already be set to another callback.  Invoke this ConnectCallback
    // here; any other connectCallback_ will be invoked in finishFail()
    startFail();
    if (callback) {
      callback->connectError(ex);
    }
    finishFail();
  }
}

void TAsyncSocket::invalidState(ReadCallback* callback) {
  VLOG(4) << "TAsyncSocket(this=" << this << ", fd=" << fd_
             << "): setReadCallback(" << callback
             << ") called in invalid state " << state_;

  TTransportException ex(TTransportException::NOT_OPEN,
                         "setReadCallback() called with socket in "
                         "invalid state");
  if (state_ == StateEnum::CLOSED || state_ == StateEnum::ERROR) {
    if (callback) {
      callback->readError(ex);
    }
  } else {
    startFail();
    if (callback) {
      callback->readError(ex);
    }
    finishFail();
  }
}

void TAsyncSocket::invalidState(WriteCallback* callback) {
  VLOG(4) << "TAsyncSocket(this=" << this << ", fd=" << fd_
             << "): write() called in invalid state " << state_;

  TTransportException ex(TTransportException::NOT_OPEN,
                         withAddr("write() called with socket in invalid state"));
  if (state_ == StateEnum::CLOSED || state_ == StateEnum::ERROR) {
    if (callback) {
      callback->writeError(0, ex);
    }
  } else {
    startFail();
    if (callback) {
      callback->writeError(0, ex);
    }
    finishFail();
  }
}

void TAsyncSocket::doClose() {
  if (fd_ == -1) return;
  if (shutdownSocketSet_) {
    shutdownSocketSet_->close(fd_);
  } else {
    ::close(fd_);
  }
  fd_ = -1;
}

std::ostream& operator << (std::ostream& os,
                           const TAsyncSocket::StateEnum& state) {
  os << static_cast<int>(state);
  return os;
}

std::string TAsyncSocket::withAddr(const std::string& s) {
  // Don't use addr_ directly because it may not be initialized
  // e.g. if constructed from fd
  TSocketAddress peer, local;
  try {
    getPeerAddress(&peer);
    getLocalAddress(&local);
  } catch (const std::exception&) {
    // ignore
  }
  return s + " (peer=" + peer.describe() + ", local=" + local.describe() + ")";
}

}}} // apache::thrift::async
