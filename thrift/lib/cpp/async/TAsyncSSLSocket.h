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

#ifndef THRIFT_ASYNC_TASYNCSSLSOCKET_H_
#define THRIFT_ASYNC_TASYNCSSLSOCKET_H_ 1

#include <arpa/inet.h>
#include <iomanip>
#include <openssl/ssl.h>

#include <folly/Optional.h>
#include <folly/String.h>
#include <thrift/lib/cpp/async/TAsyncSocket.h>
#include <thrift/lib/cpp/async/TAsyncTimeout.h>
#include <thrift/lib/cpp/async/TimeoutManager.h>
#include <thrift/lib/cpp/concurrency/Mutex.h>
#include <thrift/lib/cpp/transport/TSSLSocket.h>
#include <thrift/lib/cpp/transport/TTransportException.h>

#include <folly/Bits.h>
#include <folly/io/IOBuf.h>
#include <folly/io/Cursor.h>

using folly::IOBuf;
using folly::io::Cursor;
using std::unique_ptr;

namespace apache { namespace thrift {

namespace async {

class TSSLException: public apache::thrift::transport::TTransportException {
 public:
  TSSLException(int sslError, int errno_copy);

  int getSSLError() const { return error_; }

 protected:
  int error_;
  char msg_[256];
};

/**
 * A class for performing asynchronous I/O on an SSL connection.
 *
 * TAsyncSSLSocket allows users to asynchronously wait for data on an
 * SSL connection, and to asynchronously send data.
 *
 * The APIs for reading and writing are intentionally asymmetric.
 * Waiting for data to read is a persistent API: a callback is
 * installed, and is notified whenever new data is available.  It
 * continues to be notified of new events until it is uninstalled.
 *
 * TAsyncSSLSocket does not provide read timeout functionality,
 * because it typically cannot determine when the timeout should be
 * active.  Generally, a timeout should only be enabled when
 * processing is blocked waiting on data from the remote endpoint.
 * For server connections, the timeout should not be active if the
 * server is currently processing one or more outstanding requests for
 * this connection.  For client connections, the timeout should not be
 * active if there are no requests pending on the connection.
 * Additionally, if a client has multiple pending requests, it will
 * ususally want a separate timeout for each request, rather than a
 * single read timeout.
 *
 * The write API is fairly intuitive: a user can request to send a
 * block of data, and a callback will be informed once the entire
 * block has been transferred to the kernel, or on error.
 * TAsyncSSLSocket does provide a send timeout, since most callers
 * want to give up if the remote end stops responding and no further
 * progress can be made sending the data.
 */
class TAsyncSSLSocket : public TAsyncSocket {
 public:
  typedef std::unique_ptr<TAsyncSSLSocket, Destructor> UniquePtr;

  class HandshakeCallback {
   public:
    virtual ~HandshakeCallback() {}

    /**
     * handshakeVerify() is invoked during handshaking to give the
     * application chance to validate it's peer's certificate.
     *
     * Note that OpenSSL performs only rudimentary internal
     * consistency verification checks by itself. Any other validation
     * like whether or not the certificate was issued by a trusted CA.
     * The default implementation of this callback mimics what what
     * OpenSSL does internally if SSL_VERIFY_PEER is set with no
     * verification callback.
     *
     * See the passages on verify_callback in SSL_CTX_set_verify(3)
     * for more details.
     */
    virtual bool handshakeVerify(TAsyncSSLSocket* sock,
                                 bool preverifyOk,
                                 X509_STORE_CTX* ctx) noexcept {
      return preverifyOk;
    }

    /**
     * handshakeSuccess() is called when a new SSL connection is
     * established, i.e., after SSL_accept/connect() returns successfully.
     *
     * The HandshakeCallback will be uninstalled before handshakeSuccess()
     * is called.
     *
     * @param sock  SSL socket on which the handshake was initiated
     */
    virtual void handshakeSuccess(TAsyncSSLSocket *sock) noexcept = 0;

    /**
     * handshakeError() is called if an error occurs while
     * establishing the SSL connection.
     *
     * The HandshakeCallback will be uninstalled before handshakeError()
     * is called.
     *
     * @param sock  SSL socket on which the handshake was initiated
     * @param ex  An exception representing the error.
     */
    virtual void handshakeError(
      TAsyncSSLSocket *sock,
      const apache::thrift::transport::TTransportException& ex)
      noexcept = 0;
  };

  class HandshakeTimeout : public TAsyncTimeout {
   public:
    HandshakeTimeout(TAsyncSSLSocket* sslSocket, TEventBase* eventBase)
      : TAsyncTimeout(eventBase)
      , sslSocket_(sslSocket) {}

    virtual void timeoutExpired() noexcept {
      sslSocket_->timeoutExpired();
    }

   private:
    TAsyncSSLSocket* sslSocket_;
  };


  /**
   * These are passed to the application via errno, packed in an SSL err which
   * are outside the valid errno range.  The values are chosen to be unique
   * against values in ssl.h
   */
  enum SSLError {
    SSL_CLIENT_RENEGOTIATION_ATTEMPT = 900,
    SSL_INVALID_RENEGOTIATION = 901,
    SSL_EARLY_WRITE = 902
  };

  /**
   * Create a client TAsyncSSLSocket
   */
  TAsyncSSLSocket(const std::shared_ptr<transport::SSLContext> &ctx,
                  TEventBase* evb);

  /**
   * Create a server/client TAsyncSSLSocket from an already connected
   * socket file descriptor.
   *
   * Note that while TAsyncSSLSocket enables TCP_NODELAY for sockets it creates
   * when connecting, it does not change the socket options when given an
   * existing file descriptor.  If callers want TCP_NODELAY enabled when using
   * this version of the constructor, they need to explicitly call
   * setNoDelay(true) after the constructor returns.
   *
   * @param ctx             SSL context for this connection.
   * @param evb EventBase that will manage this socket.
   * @param fd  File descriptor to take over (should be a connected socket).
   * @param server Is socket in server mode?
   */
  TAsyncSSLSocket(const std::shared_ptr<transport::SSLContext>& ctx,
                  TEventBase* evb, int fd, bool server = true);


  /**
   * Helper function to create a server/client shared_ptr<TAsyncSSLSocket>.
   */
  static std::shared_ptr<TAsyncSSLSocket> newSocket(
    const std::shared_ptr<transport::SSLContext>& ctx,
    TEventBase* evb, int fd, bool server=true) {
    return std::shared_ptr<TAsyncSSLSocket>(
      new TAsyncSSLSocket(ctx, evb, fd, server),
      Destructor());
  }

  /**
   * Helper function to create a client shared_ptr<TAsyncSSLSocket>.
   */
  static std::shared_ptr<TAsyncSSLSocket> newSocket(
    const std::shared_ptr<transport::SSLContext>& ctx,
    TEventBase* evb) {
    return std::shared_ptr<TAsyncSSLSocket>(
      new TAsyncSSLSocket(ctx, evb),
      Destructor());
  }


#if OPENSSL_VERSION_NUMBER >= 0x1000105fL && !defined(OPENSSL_NO_TLSEXT)
  /**
   * Create a client TAsyncSSLSocket with tlsext_servername in
   * the Client Hello message.
   */
  TAsyncSSLSocket(const std::shared_ptr<transport::SSLContext> &ctx,
                  TEventBase* evb,
                  const std::string& serverName);

  /**
   * Create a client TAsyncSSLSocket from an already connected
   * socket file descriptor.
   *
   * Note that while TAsyncSSLSocket enables TCP_NODELAY for sockets it creates
   * when connecting, it does not change the socket options when given an
   * existing file descriptor.  If callers want TCP_NODELAY enabled when using
   * this version of the constructor, they need to explicitly call
   * setNoDelay(true) after the constructor returns.
   *
   * @param ctx  SSL context for this connection.
   * @param evb  EventBase that will manage this socket.
   * @param fd   File descriptor to take over (should be a connected socket).
   * @param serverName tlsext_hostname that will be sent in ClientHello.
   */
  TAsyncSSLSocket(const std::shared_ptr<transport::SSLContext>& ctx,
                  TEventBase* evb,
                  int fd,
                  const std::string& serverName);

  static std::shared_ptr<TAsyncSSLSocket> newSocket(
    const std::shared_ptr<transport::SSLContext>& ctx,
    TEventBase* evb,
    const std::string& serverName) {
    return std::shared_ptr<TAsyncSSLSocket>(
      new TAsyncSSLSocket(ctx, evb, serverName),
      Destructor());
  }
#endif

  /**
   * TODO: implement support for SSL renegotiation.
   *
   * This involves proper handling of the SSL_ERROR_WANT_READ/WRITE
   * code as a result of SSL_write/read(), instead of returning an
   * error. In that case, the READ/WRITE event should be registered,
   * and a flag (e.g., writeBlockedOnRead) should be set to indiciate
   * the condition. In the next invocation of read/write callback, if
   * the flag is on, performWrite()/performRead() should be called in
   * addition to the normal call to performRead()/performWrite(), and
   * the flag should be reset.
   */

  // Inherit TAsyncTransport methods from TAsyncSocket except the
  // following.
  // See the documentation in TAsyncTransport.h
  // TODO: implement graceful shutdown in close()
  // TODO: implement detachSSL() that returns the SSL connection
  virtual void closeNow();
  virtual void shutdownWrite();
  virtual void shutdownWriteNow();
  virtual bool good() const;
  virtual bool connecting() const;

  bool isEorTrackingEnabled() const override;
  virtual void setEorTracking(bool track);
  virtual size_t getRawBytesWritten() const;
  virtual size_t getRawBytesReceived() const;
  void enableClientHelloParsing();

  /**
   * Accept an SSL connection on the socket.
   *
   * The callback will be invoked and uninstalled when an SSL
   * connection has been established on the underlying socket.
   * The value of verifyPeer determines the client verification method.
   * By default, its set to use the value in the underlying context
   *
   * @param callback callback object to invoke on success/failure
   * @param timeout timeout for this function in milliseconds, or 0 for no
   *                timeout
   * @param verifyPeer  SSLVerifyPeerEnum uses the options specified in the
   *                context by default, can be set explcitly to override the
   *                method in the context
   */
  virtual void sslAccept(HandshakeCallback* callback, uint32_t timeout = 0,
      const transport::SSLContext::SSLVerifyPeerEnum& verifyPeer =
            transport::SSLContext::SSLVerifyPeerEnum::USE_CTX);

  /**
   * Invoke SSL accept following an asynchronous session cache lookup
   */
  void restartSSLAccept();

  /**
   * Connect to the given address, invoking callback when complete or on error
   *
   * Note timeout applies to TCP + SSL connection time
   */
  void connect(ConnectCallback* callback,
               const transport::TSocketAddress& address,
               int timeout = 0,
               const OptionMap &options = emptyOptionMap,
               const transport::TSocketAddress& bindAddr = anyAddress)
               noexcept;

  using TAsyncSocket::connect;

  /**
   * Initiate an SSL connection on the socket
   * THe callback will be invoked and uninstalled when an SSL connection
   * has been establshed on the underlying socket.
   * The verification option verifyPeer is applied if its passed explicitly.
   * If its not, the options in SSLContext set on the underying SSLContext
   * are applied.
   *
   * @param callback callback object to invoke on success/failure
   * @param timeout timeout for this function in milliseconds, or 0 for no
   *                timeout
   * @param verifyPeer  SSLVerifyPeerEnum uses the options specified in the
   *                context by default, can be set explcitly to override the
   *                method in the context. If verification is turned on sets
   *                SSL_VERIFY_PEER and invokes
   *                HandshakeCallback::handshakeVerify().
   */
  virtual void sslConnect(HandshakeCallback *callback, uint64_t timeout = 0,
            const transport::SSLContext::SSLVerifyPeerEnum& verifyPeer =
                  transport::SSLContext::SSLVerifyPeerEnum::USE_CTX);

  enum SSLStateEnum {
    STATE_UNINIT,
    STATE_ACCEPTING,
    STATE_CACHE_LOOKUP,
    STATE_RSA_ASYNC_PENDING,
    STATE_CONNECTING,
    STATE_ESTABLISHED,
    STATE_REMOTE_CLOSED, /// remote end closed; we can still write
    STATE_CLOSING,       ///< close() called, but waiting on writes to complete
    /// close() called with pending writes, before connect() has completed
    STATE_CONNECTING_CLOSING,
    STATE_CLOSED,
    STATE_ERROR
  };

  SSLStateEnum getSSLState() const { return sslState_;}

  /**
   * Get a handle to the negotiated SSL session.  This increments the session
   * refcount and must be deallocated by the caller.
   */
  SSL_SESSION *getSSLSession();

  /**
   * Set the SSL session to be used during sslConnect.  TAsyncSSLSocket will
   * hold a reference to the session until it is destroyed or released by the
   * underlying SSL structure.
   *
   * @param takeOwnership if true, TAsyncSSLSocket will assume the caller's
   *                      reference count to session.
   */
  void setSSLSession(SSL_SESSION *session, bool takeOwnership = false);

  /**
   * Get the name of the protocol selected by the client during
   * Next Protocol Negotiation (NPN)
   *
   * Throw an exception if openssl does not support NPN
   *
   * @param protoName      Name of the protocol (not guaranteed to be
   *                       null terminated); will be set to nullptr if
   *                       the client did not negotiate a protocol.
   *                       Note: the TAsyncSSLSocket retains ownership
   *                       of this string.
   * @param protoNameLen   Length of the name.
   */
  virtual void getSelectedNextProtocol(const unsigned char** protoName,
      unsigned* protoLen) const;

  /**
   * Get the name of the protocol selected by the client during
   * Next Protocol Negotiation (NPN)
   *
   * @param protoName      Name of the protocol (not guaranteed to be
   *                       null terminated); will be set to nullptr if
   *                       the client did not negotiate a protocol.
   *                       Note: the TAsyncSSLSocket retains ownership
   *                       of this string.
   * @param protoNameLen   Length of the name.
   * @return false if openssl does not support NPN
   */
  virtual bool getSelectedNextProtocolNoThrow(const unsigned char** protoName,
      unsigned* protoLen) const;

  /**
   * Determine if the session specified during setSSLSession was reused
   * or if the server rejected it and issued a new session.
   */
  bool getSSLSessionReused() const;

  /**
   * true if the session was resumed using session ID
   */
  bool sessionIDResumed() const { return sessionIDResumed_; }

  void setSessionIDResumed(bool resumed) {
    sessionIDResumed_ = resumed;
  }

  /**
   * Get the negociated cipher name for this SSL connection.
   * Returns the cipher used or the constant value "NONE" when no SSL session
   * has been established.
   */
  const char *getNegotiatedCipherName() const;

  /**
   * Get the server name for this SSL connection.
   * Returns the server name used or the constant value "NONE" when no SSL
   * session has been established.
   * If openssl has no SNI support, throw TTransportException.
   */
  const char *getSSLServerName() const;

  /**
   * Get the server name for this SSL connection.
   * Returns the server name used or the constant value "NONE" when no SSL
   * session has been established.
   * If openssl has no SNI support, return "NONE"
   */
  const char *getSSLServerNameNoThrow() const;

  /**
   * Get the SSL version for this connection.
   * Possible return values are SSL2_VERSION, SSL3_VERSION, TLS1_VERSION,
   * with hexa representations 0x200, 0x300, 0x301,
   * or 0 if no SSL session has been established.
   */
  int getSSLVersion() const;

  /**
   * Get the certificate size used for this SSL connection.
   */
  int getSSLCertSize() const;

  /* Get the number of bytes read from the wire (including protocol
   * overhead). Returns 0 once the connection has been closed.
   */
  unsigned long getBytesRead() const {
    if (ssl_ != nullptr) {
      return BIO_number_read(SSL_get_rbio(ssl_));
    }
    return 0;
  }

  /* Get the number of bytes written to the wire (including protocol
   * overhead).  Returns 0 once the connection has been closed.
   */
  unsigned long getBytesWritten() const {
    if (ssl_ != nullptr) {
      return BIO_number_written(SSL_get_wbio(ssl_));
    }
    return 0;
  }

  virtual void attachEventBase(TEventBase* eventBase) {
    TAsyncSocket::attachEventBase(eventBase);
    handshakeTimeout_.attachEventBase(eventBase);
  }

  virtual void detachEventBase() {
    TAsyncSocket::detachEventBase();
    handshakeTimeout_.detachEventBase();
  }

  virtual void attachTimeoutManager(TimeoutManager* manager) {
    handshakeTimeout_.attachTimeoutManager(manager);
  }

  virtual void detachTimeoutManager() {
    handshakeTimeout_.detachTimeoutManager();
  }

#if OPENSSL_VERSION_NUMBER >= 0x009080bfL
  /**
   * This function will set the SSL context for this socket to the
   * argument. This should only be used on client SSL Sockets that have
   * already called detachSSLContext();
   */
  void attachSSLContext(const std::shared_ptr<transport::SSLContext>& ctx);

  /**
   * Detaches the SSL context for this socket.
   */
  void detachSSLContext();
#endif

#if OPENSSL_VERSION_NUMBER >= 0x1000105fL && !defined(OPENSSL_NO_TLSEXT)
  /**
   * Switch the SSLContext to continue the SSL handshake.
   * It can only be used in server mode.
   */
  void switchServerSSLContext(
    const std::shared_ptr<transport::SSLContext>& handshakeCtx);

  /**
   * Did server recognize/support the tlsext_hostname in Client Hello?
   * It can only be used in client mode.
   *
   * @return true - tlsext_hostname is matched by the server
   *         false - tlsext_hostname is not matched or
   *                 is not supported by server
   */
  bool isServerNameMatch() const;

  /**
   * Set the SNI hostname that we'll advertise to the server in the
   * ClientHello message.
   */
  void setServerName(std::string serverName) noexcept;
#endif

  void timeoutExpired() noexcept;

  /**
   * Get the list of supported ciphers sent by the client in the client's
   * preference order.
   */
  void getSSLClientCiphers(std::string& clientCiphers) {
    std::stringstream ciphersStream;
    std::string cipherName;

    if (parseClientHello_ == false
        || clientHelloInfo_->clientHelloCipherSuites_.empty()) {
      clientCiphers = "";
      return;
    }

    for (auto originalCipherCode : clientHelloInfo_->clientHelloCipherSuites_)
    {
      // OpenSSL expects code as a big endian char array
      auto cipherCode = htons(originalCipherCode);

#if defined(SSL_OP_NO_TLSv1_2)
      const SSL_CIPHER* cipher =
          TLSv1_2_method()->get_cipher_by_char((unsigned char*)&cipherCode);
#elif defined(SSL_OP_NO_TLSv1_1)
      const SSL_CIPHER* cipher =
          TLSv1_1_method()->get_cipher_by_char((unsigned char*)&cipherCode);
#elif defined(SSL_OP_NO_TLSv1)
      const SSL_CIPHER* cipher =
          TLSv1_method()->get_cipher_by_char((unsigned char*)&cipherCode);
#else
      const SSL_CIPHER* cipher =
          SSLv3_method()->get_cipher_by_char((unsigned char*)&cipherCode);
#endif

      if (cipher == nullptr) {
        ciphersStream << std::setfill('0') << std::setw(4) << std::hex
                      << originalCipherCode << ":";
      } else {
        ciphersStream << SSL_CIPHER_get_name(cipher) << ":";
      }
    }

    clientCiphers = ciphersStream.str();
    clientCiphers.erase(clientCiphers.end() - 1);
  }

  /**
   * Get the list of compression methods sent by the client in TLS Hello.
   */
  std::string getSSLClientComprMethods() {
    if (!parseClientHello_) {
      return "";
    }
    return folly::join(":", clientHelloInfo_->clientHelloCompressionMethods_);
  }

  /**
   * Get the list of TLS extensions sent by the client in the TLS Hello.
   */
  std::string getSSLClientExts() {
    if (!parseClientHello_) {
      return "";
    }
    return folly::join(":", clientHelloInfo_->clientHelloExtensions_);
  }

  /**
   * Get the list of shared ciphers between the server and the client.
   * Works well for only SSLv2, not so good for SSLv3 or TLSv1.
   */
  void getSSLSharedCiphers(std::string& sharedCiphers) {
    char ciphersBuffer[1024];
    ciphersBuffer[0] = '\0';
    SSL_get_shared_ciphers(ssl_, ciphersBuffer, sizeof(ciphersBuffer) - 1);
    sharedCiphers = ciphersBuffer;
  }

  /**
   * Get the list of ciphers supported by the server in the server's
   * preference order.
   */
  void getSSLServerCiphers(std::string& serverCiphers) {
    serverCiphers = SSL_get_cipher_list(ssl_, 0);
    int i = 1;
    const char *cipher;
    while ((cipher = SSL_get_cipher_list(ssl_, i)) != nullptr) {
      serverCiphers.append(":");
      serverCiphers.append(cipher);
      i++;
    }
  }

  static int getSSLExDataIndex();
  static TAsyncSSLSocket* getFromSSL(const SSL *ssl);
  static int eorAwareBioWrite(BIO *b, const char *in, int inl);
  void resetClientHelloParsing(SSL *ssl);
  static void clientHelloParsingCallback(int write_p, int version,
      int content_type, const void *buf, size_t len, SSL *ssl, void *arg);

  struct ClientHelloInfo {
    folly::IOBufQueue clientHelloBuf_;
    uint8_t clientHelloMajorVersion_;
    uint8_t clientHelloMinorVersion_;
    std::vector<uint16_t> clientHelloCipherSuites_;
    std::vector<uint8_t> clientHelloCompressionMethods_;
    std::vector<uint16_t> clientHelloExtensions_;
  };

  // For unit-tests
  ClientHelloInfo* getClientHelloInfo() {
    return clientHelloInfo_.get();
  }

 protected:

  /**
   * Protected destructor.
   *
   * Users of TAsyncSSLSocket must never delete it directly.  Instead, invoke
   * destroy() instead.  (See the documentation in TDelayedDestruction.h for
   * more details.)
   */
  ~TAsyncSSLSocket();

  // Inherit event notification methods from TAsyncSocket except
  // the following.

  void handleRead() noexcept;
  void handleWrite() noexcept;
  void handleAccept() noexcept;
  void handleConnect() noexcept;

  void invalidState(HandshakeCallback* callback);
  bool willBlock(int ret, int *errorOut) noexcept;

  virtual void checkForImmediateRead() noexcept;
  // TAsyncSocket calls this at the wrong time for SSL
  void handleInitialReadWrite() noexcept {}

  ssize_t performRead(void* buf, size_t buflen);
  ssize_t performWrite(const iovec* vec, uint32_t count, WriteFlags flags,
                       uint32_t* countWritten, uint32_t* partialWritten);

  // This virtual wrapper around SSL_write exists solely for testing/mockability
  virtual int sslWriteImpl(SSL *ssl, const void *buf, int n) {
    return SSL_write(ssl, buf, n);
  }

  /**
   * Apply verification options passed to sslConnect/sslAccept or those set
   * in the underlying SSLContext object.
   *
   * @param ssl pointer to the SSL object on which verification options will be
   * applied. If verifyPeer_ was explicitly set either via sslConnect/sslAccept,
   * those options override the settings in the underlying SSLContext.
   */
  void applyVerificationOptions(SSL * ssl);

  /**
   * A SSL_write wrapper that understand EOR
   *
   * @param ssl: SSL* object
   * @param buf: Buffer to be written
   * @param n:   Number of bytes to be written
   * @param eor: Does the last byte (buf[n-1]) have the app-last-byte?
   * @return:    The number of app bytes successfully written to the socket
   */
  int eorAwareSSLWrite(SSL *ssl, const void *buf, int n, bool eor);

  // Inherit error handling methods from TAsyncSocket, plus the following.
  void failHandshake(const char* fn, const transport::TTransportException& ex);

  void invokeHandshakeCallback();

  static void sslInfoCallback(const SSL *ssl, int type, int val);

  static concurrency::Mutex mutex_;
  static int sslExDataIndex_;
  // Whether we've applied the TCP_CORK option to the socket
  bool corked_{false};
  // SSL related members.
  bool server_{false};
  // Used to prevent client-initiated renegotiation.  Note that TAsyncSSLSocket
  // doesn't fully support renegotiation, so we could just fail all attempts
  // to enforce this.  Once it is supported, we should make it an option
  // to disable client-initiated renegotiation.
  bool handshakeComplete_{false};
  bool renegotiateAttempted_{false};
  SSLStateEnum sslState_{STATE_UNINIT};
  std::shared_ptr<transport::SSLContext> ctx_;
  // Callback for SSL_accept() or SSL_connect()
  HandshakeCallback* handshakeCallback_{nullptr};
  SSL* ssl_{nullptr};
  SSL_SESSION *sslSession_{nullptr};
  HandshakeTimeout handshakeTimeout_;
  // whether the SSL session was resumed using session ID or not
  bool sessionIDResumed_{false};

  // The app byte num that we are tracking for the MSG_EOR
  // Only one app EOR byte can be tracked.
  size_t appEorByteNo_{0};

  // When openssl is about to sendmsg() across the minEorRawBytesNo_,
  // it will pass MSG_EOR to sendmsg().
  size_t minEorRawByteNo_{0};
#if OPENSSL_VERSION_NUMBER >= 0x1000105fL && !defined(OPENSSL_NO_TLSEXT)
  std::shared_ptr<transport::SSLContext> handshakeCtx_;
  std::string tlsextHostname_;
#endif
  transport::SSLContext::SSLVerifyPeerEnum
    verifyPeer_{transport::SSLContext::SSLVerifyPeerEnum::USE_CTX};

  // Callback for SSL_CTX_set_verify()
  static int sslVerifyCallback(int preverifyOk, X509_STORE_CTX* ctx);

  bool parseClientHello_{false};
  unique_ptr<ClientHelloInfo> clientHelloInfo_;
};

}}} // apache::thrift::async

#endif // #ifndef THRIFT_ASYNC_TASYNCSSLSOCKET_H_
