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

#ifndef THRIFT_TRANSPORT_TSSLSOCKET_H_
#define THRIFT_TRANSPORT_TSSLSOCKET_H_ 1

#include <errno.h>
#include <string>
#include <memory>
#include <openssl/ssl.h>
#include <openssl/tls1.h>
#include <thrift/lib/cpp/concurrency/Mutex.h>
#include <thrift/lib/cpp/transport/TSocket.h>

namespace folly {
class SocketAddress;
}

namespace apache { namespace thrift { namespace transport {

class PasswordCollector;
class SSLContext;

/**
 * OpenSSL implementation for SSL socket interface.
 */
class TSSLSocket: public TVirtualTransport<TSSLSocket, TSocket> {
 public:
  /**
   * Constructor.
   */
  explicit TSSLSocket(const std::shared_ptr<SSLContext>& ctx);
  /**
   * Constructor, create an instance of TSSLSocket given an existing socket.
   *
   * @param socket An existing socket
   */
  TSSLSocket(const std::shared_ptr<SSLContext>& ctx, int socket);
  /**
   * Constructor.
   *
   * @param host  Remote host name
   * @param port  Remote port number
   */
  TSSLSocket(const std::shared_ptr<SSLContext>& ctx,
             const std::string& host,
             int port);
  /**
   * Constructor.
   */
  TSSLSocket(const std::shared_ptr<SSLContext>& ctx,
             const folly::SocketAddress& address);
  /**
   * Destructor.
   */
  ~TSSLSocket();

  /**
   * TTransport interface.
   */
  bool     isOpen();
  bool     peek();
  void     open();
  void     close();
  uint32_t read(uint8_t* buf, uint32_t len);
  void     write(const uint8_t* buf, uint32_t len);
  void     flush();

  /**
   * Set whether to use client or server side SSL handshake protocol.
   *
   * @param flag  Use server side handshake protocol if true.
   */
  void server(bool flag) { server_ = flag; }
  /**
   * Determine whether the SSL socket is server or client mode.
   */
  bool server() const { return server_; }

protected:
  /**
   * Verify peer certificate after SSL handshake completes.
   */
  virtual void verifyCertificate();

  /**
   * Initiate SSL handshake if not already initiated.
   */
  void checkHandshake();

  bool server_;
  SSL* ssl_;
  std::shared_ptr<SSLContext> ctx_;
};

/**
 * SSL socket factory. SSL sockets should be created via SSL factory.
 */
class TSSLSocketFactory {
 public:
  /**
   * Constructor/Destructor
   */
  explicit TSSLSocketFactory(const std::shared_ptr<SSLContext>& context);
  virtual ~TSSLSocketFactory();

  /**
   * Create an instance of TSSLSocket with a fresh new socket.
   */
  virtual std::shared_ptr<TSSLSocket> createSocket();
  /**
   * Create an instance of TSSLSocket with the given socket.
   *
   * @param socket An existing socket.
   */
  virtual std::shared_ptr<TSSLSocket> createSocket(int socket);
   /**
   * Create an instance of TSSLSocket.
   *
   * @param host  Remote host to be connected to
   * @param port  Remote port to be connected to
   */
  virtual std::shared_ptr<TSSLSocket> createSocket(const std::string& host,
                                                     int port);
  /**
   * Set/Unset server mode.
   *
   * @param flag  Server mode if true
   */
  virtual void server(bool flag) { server_ = flag; }
  /**
   * Determine whether the socket is in server or client mode.
   *
   * @return true, if server mode, or, false, if client mode
   */
  virtual bool server() const { return server_; }

 private:
  std::shared_ptr<SSLContext> ctx_;
  bool server_;
};

/**
 * SSL exception.
 */
class TSSLException: public TTransportException {
 public:
  explicit TSSLException(const std::string& message):
    TTransportException(TTransportException::INTERNAL_ERROR, message) {}

  virtual const char* what() const throw() {
    if (message_.empty()) {
      return "TSSLException";
    } else {
      return message_.c_str();
    }
  }
};

/**
 * Wrap OpenSSL SSL_CTX into a class.
 */
class SSLContext {
 public:

  enum SSLVersion {
     SSLv2,
     SSLv3,
     TLSv1
  };

  enum SSLVerifyPeerEnum{
    USE_CTX,
    VERIFY,
    VERIFY_REQ_CLIENT_CERT,
    NO_VERIFY
  };

  struct NextProtocolsItem {
    int weight;
    std::list<std::string> protocols;
  };

  struct AdvertisedNextProtocolsItem {
    unsigned char *protocols;
    unsigned length;
    double probability;
  };

  /**
   * Convenience function to call getErrors() with the current errno value.
   *
   * Make sure that you only call this when there was no intervening operation
   * since the last OpenSSL error that may have changed the current errno value.
   */
  static std::string getErrors() {
    return getErrors(errno);
  }

  /**
   * Constructor.
   *
   * @param version The lowest or oldest SSL version to support.
   */
  explicit SSLContext(SSLVersion version = TLSv1);
  virtual ~SSLContext();

  /**
   * Set default ciphers to be used in SSL handshake process.
   *
   * @param ciphers A list of ciphers to use for TLSv1.0
   */
  virtual void ciphers(const std::string& ciphers);

  /**
   * Low-level method that attempts to set the provided ciphers on the
   * SSL_CTX object, and throws if something goes wrong.
   */
  virtual void setCiphersOrThrow(const std::string& ciphers);

  /**
   * Method to set verification option in the context object.
   *
   * @param verifyPeer SSLVerifyPeerEnum indicating the verification
   *                       method to use.
   */
  virtual void setVerificationOption(const SSLVerifyPeerEnum& verifyPeer);

  /**
   * Method to check if peer verfication is set.
   *
   * @return true if peer verification is required.
   *
   */
  virtual bool needsPeerVerification() {
    return (verifyPeer_ == SSLVerifyPeerEnum::VERIFY ||
              verifyPeer_ == SSLVerifyPeerEnum::VERIFY_REQ_CLIENT_CERT);
  }

  /**
   * Method to fetch Verification mode for a SSLVerifyPeerEnum.
   * verifyPeer cannot be SSLVerifyPeerEnum::USE_CTX since there is no
   * context.
   *
   * @param verifyPeer SSLVerifyPeerEnum for which the flags need to
   *                  to be returned
   *
   * @return mode flags that can be used with SSL_set_verify
   */
  static int getVerificationMode(const SSLVerifyPeerEnum& verifyPeer);

  /**
   * Method to fetch Verification mode determined by the options
   * set using setVerificationOption.
   *
   * @return mode flags that can be used with SSL_set_verify
   */
  virtual int getVerificationMode();

  /**
   * Enable/Disable authentication. Peer name validation can only be done
   * if checkPeerCert is true.
   *
   * @param checkPeerCert If true, require peer to present valid certificate
   * @param checkPeerName If true, validate that the certificate common name
   *                      or alternate name(s) of peer matches the hostname
   *                      used to connect.
   * @param peerName      If non-empty, validate that the certificate common
   *                      name of peer matches the given string (altername
   *                      name(s) are not used in this case).
   */
  virtual void authenticate(bool checkPeerCert, bool checkPeerName,
                            const std::string& peerName = std::string());
  /**
   * Load server certificate.
   *
   * @param path   Path to the certificate file
   * @param format Certificate file format
   */
  virtual void loadCertificate(const char* path, const char* format = "PEM");
  /**
   * Load private key.
   *
   * @param path   Path to the private key file
   * @param format Private key file format
   */
  virtual void loadPrivateKey(const char* path, const char* format = "PEM");
  /**
   * Load trusted certificates from specified file.
   *
   * @param path Path to trusted certificate file
   */
  virtual void loadTrustedCertificates(const char* path);
  /**
   * Load trusted certificates from specified X509 certificate store.
   *
   * @param store X509 certificate store.
   */
  virtual void loadTrustedCertificates(X509_STORE* store);
  /**
   * Load a client CA list for validating clients
   */
  virtual void loadClientCAList(const char* path);
  /**
   * Default randomize method.
   */
  virtual void randomize();
  /**
   * Override default OpenSSL password collector.
   *
   * @param collector Instance of user defined password collector
   */
  virtual void passwordCollector(std::shared_ptr<PasswordCollector> collector);
  /**
   * Obtain password collector.
   *
   * @return User defined password collector
   */
  virtual std::shared_ptr<PasswordCollector> passwordCollector() {
    return collector_;
  }
#if OPENSSL_VERSION_NUMBER >= 0x1000105fL && !defined(OPENSSL_NO_TLSEXT)
  /**
   * Provide SNI support
   */
  enum ServerNameCallbackResult {
    SERVER_NAME_FOUND,
    SERVER_NAME_NOT_FOUND,
    SERVER_NAME_NOT_FOUND_ALERT_FATAL,
  };
  /**
   * Callback function from openssl to give the application a
   * chance to check the tlsext_hostname just right after parsing
   * the Client Hello or Server Hello message.
   *
   * It is for the server to switch the SSL to another SSL_CTX
   * to continue the handshake. (i.e. Server Name Indication, SNI, in RFC6066).
   *
   * If the ServerNameCallback returns:
   * SERVER_NAME_FOUND:
   *    server: Send a tlsext_hostname in the Server Hello
   *    client: No-effect
   * SERVER_NAME_NOT_FOUND:
   *    server: Does not send a tlsext_hostname in Server Hello
   *            and continue the handshake.
   *    client: No-effect
   * SERVER_NAME_NOT_FOUND_ALERT_FATAL:
   *    server and client: Send fatal TLS1_AD_UNRECOGNIZED_NAME alert to
   *                       the peer.
   *
   * Quote from RFC 6066:
   * "...
   * If the server understood the ClientHello extension but
   * does not recognize the server name, the server SHOULD take one of two
   * actions: either abort the handshake by sending a fatal-level
   * unrecognized_name(112) alert or continue the handshake.  It is NOT
   * RECOMMENDED to send a warning-level unrecognized_name(112) alert,
   * because the client's behavior in response to warning-level alerts is
   * unpredictable.
   * ..."
   */

  /**
   * Set the ServerNameCallback
   */
  typedef std::function<ServerNameCallbackResult(SSL* ssl)> ServerNameCallback;
  virtual void setServerNameCallback(const ServerNameCallback& cb);

  /**
   * Generic callbacks that are run after we get the Client Hello (right
   * before we run the ServerNameCallback)
   */
  typedef std::function<void(SSL* ssl)> ClientHelloCallback;
  virtual void addClientHelloCallback(const ClientHelloCallback& cb);
#endif

  /**
   * Create an SSL object from this context.
   */
  SSL* createSSL() const;

  /**
   * Possibly validate the peer's certificate name, depending on how this
   * SSLContext was configured by authenticate().
   *
   * @return True if the peer's name is acceptable, false otherwise
   */
  bool validatePeerName(TSSLSocket* sock, SSL* ssl) const;

  /**
   * Set the options on the SSL_CTX object.
   */
  void setOptions(long options);

#ifdef OPENSSL_NPN_NEGOTIATED
  /**
   * Set the list of protocols that this SSL context supports. In server
   * mode, this is the list of protocols that will be advertised for Next
   * Protocol Negotiation (NPN). In client mode, the first protocol
   * advertised by the server that is also on this list is
   * chosen. Invoking this function with a list of length zero causes NPN
   * to be disabled.
   *
   * @param protocols   List of protocol names. This method makes a copy,
   *                    so the caller needn't keep the list in scope after
   *                    the call completes. The list must have at least
   *                    one element to enable NPN. Each element must have
   *                    a string length < 256.
   * @return true if NPN has been activated. False if NPN is disabled.
   */
  bool setAdvertisedNextProtocols(const std::list<std::string>& protocols);
  /**
   * Set weighted list of lists of protocols that this SSL context supports.
   * In server mode, each element of the list contains a list of protocols that
   * could be advertised for Next Protocol Negotiation (NPN). The list of
   * protocols that will be advertised to a client is selected randomly, based
   * on weights of elements. Client mode doesn't support randomized NPN, so
   * this list should contain only 1 element. The first protocol advertised
   * by the server that is also on the list of protocols of this element is
   * chosen. Invoking this function with a list of length zero causes NPN
   * to be disabled.
   *
   * @param items  List of NextProtocolsItems, Each item contains a list of
   *               protocol names and weight. After the call of this fucntion
   *               each non-empty list of protocols will be advertised with
   *               probability weight/sum_of_weights. This method makes a copy,
   *               so the caller needn't keep the list in scope after the call
   *               completes. The list must have at least one element with
   *               non-zero weight and non-empty protocols list to enable NPN.
   *               Each name of the protocol must have a string length < 256.
   * @return true if NPN has been activated. False if NPN is disabled.
   */
  bool setRandomizedAdvertisedNextProtocols(
      const std::list<NextProtocolsItem>& items);

  /**
   * Disables NPN on this SSL context.
   */
  void unsetNextProtocols();
  void deleteNextProtocolsStrings();
#endif // OPENSSL_NPN_NEGOTIATED

  /**
   * Gets the underlying SSL_CTX for advanced usage
   */
  SSL_CTX *getSSLCtx() const {
    return ctx_;
  }

  enum SSLLockType {
    LOCK_MUTEX,
    LOCK_SPINLOCK,
    LOCK_NONE
  };

  /**
   * Set preferences for how to treat locks in OpenSSL.  This must be
   * called before the instantiation of any SSLContext objects, otherwise
   * the defaults will be used.
   *
   * OpenSSL has a lock for each module rather than for each object or
   * data that needs locking.  Some locks protect only refcounts, and
   * might be better as spinlocks rather than mutexes.  Other locks
   * may be totally unnecessary if the objects being protected are not
   * shared between threads in the application.
   *
   * By default, all locks are initialized as mutexes.  OpenSSL's lock usage
   * may change from version to version and you should know what you are doing
   * before disabling any locks entirely.
   *
   * Example: if you don't share SSL sessions between threads in your
   * application, you may be able to do this
   *
   * setSSLLockTypes({{CRYPTO_LOCK_SSL_SESSION, SSLContext::LOCK_NONE}})
   */
  static void setSSLLockTypes(std::map<int, SSLLockType> lockTypes);

  /**
   * Examine OpenSSL's error stack, and return a string description of the
   * errors.
   *
   * This operation removes the errors from OpenSSL's error stack.
   */
  static std::string getErrors(int errnoCopy);

  /**
   * We want to vary which cipher we'll use based on the client's TLS version.
   */
  void switchCiphersIfTLS11(
    SSL* ssl,
    const std::string& tls11CipherString
  );

 protected:
  SSL_CTX* ctx_;

 private:
  SSLVerifyPeerEnum verifyPeer_{SSLVerifyPeerEnum::NO_VERIFY};

  bool checkPeerName_;
  std::string peerFixedName_;
  std::shared_ptr<PasswordCollector> collector_;
#if OPENSSL_VERSION_NUMBER >= 0x1000105fL && !defined(OPENSSL_NO_TLSEXT)
  ServerNameCallback serverNameCb_;
  std::vector<ClientHelloCallback> clientHelloCbs_;
#endif

  static concurrency::Mutex mutex_;
  static uint64_t count_;

#ifdef OPENSSL_NPN_NEGOTIATED
  /**
   * Wire-format list of advertised protocols for use in NPN.
   */
  std::vector<AdvertisedNextProtocolsItem> advertisedNextProtocols_;
  static int sNextProtocolsExDataIndex_;

  static int advertisedNextProtocolCallback(SSL* ssl,
      const unsigned char** out, unsigned int* outlen, void* data);
  static int selectNextProtocolCallback(
    SSL* ssl, unsigned char **out, unsigned char *outlen,
    const unsigned char *server, unsigned int server_len, void *args);
#endif // OPENSSL_NPN_NEGOTIATED

  static int passwordCallback(char* password, int size, int, void* data);

  static void initializeOpenSSL();
  static void cleanupOpenSSL();

  /**
   * Helper to match a hostname versus a pattern.
   */
  static bool matchName(const char* host, const char* pattern, int size);

#if OPENSSL_VERSION_NUMBER >= 0x1000105fL && !defined(OPENSSL_NO_TLSEXT)
  /**
   * The function that will be called directly from openssl
   * in order for the application to get the tlsext_hostname just after
   * parsing the Client Hello or Server Hello message. It will then call
   * the serverNameCb_ function object. Hence, it is sort of a
   * wrapper/proxy between serverNameCb_ and openssl.
   *
   * The openssl's primary intention is for SNI support, but we also use it
   * generically for performing logic after the Client Hello comes in.
   */
  static int baseServerNameOpenSSLCallback(
    SSL* ssl,
    int* al /* alert (return value) */,
    void* data
  );
#endif

  std::string providedCiphersString_;
};

typedef std::shared_ptr<SSLContext> SSLContextPtr;

/**
 * Override the default password collector.
 */
class PasswordCollector {
 public:
  virtual ~PasswordCollector() {}
  /**
   * Interface for customizing how to collect private key password.
   *
   * By default, OpenSSL prints a prompt on screen and request for password
   * while loading private key. To implement a custom password collector,
   * implement this interface and register it with TSSLSocketFactory.
   *
   * @param password Pass collected password back to OpenSSL
   * @param size     Maximum length of password including nullptr character
   */
  virtual void getPassword(std::string& password, int size) = 0;

  /**
   * Return a description of this collector for logging purposes
   */
  virtual std::string describe() const = 0;
};

std::ostream& operator<<(std::ostream& os, const PasswordCollector& collector);

}}}

#endif
