/* Copyright 2026 SuperSmartClient contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <config.h>
#include "DashboardSession.h"

#include <algorithm>
#include <atomic>
#include <climits>
#include <cstring>
#include <mutex>
#include <system_error>
#include <thread>

#include <network/TcpSocket.h>
#include <rdr/FdInStream.h>
#include <rdr/FdOutStream.h>
#include <rfb/CConnection.h>
#include "UserDialog.h"
#include <rfb/CMsgWriter.h>
#include <rfb/Exception.h>
#include <rfb/PixelBuffer.h>
#include <rfb/encodings.h>

#ifndef WIN32
#include <fcntl.h>
#include <netdb.h>
#include <unistd.h>
#endif

namespace dashboard {
namespace {
using Clock = std::chrono::steady_clock;
class PasswordRequired : public std::runtime_error {
public:
  PasswordRequired() : std::runtime_error("Password required. Edit this connection to enter it.") {}
};
#ifdef WIN32
using SocketHandle = SOCKET;
const SocketHandle invalidSocket = INVALID_SOCKET;
void closeSocket(SocketHandle fd) { closesocket(fd); }
int socketError() { return WSAGetLastError(); }
#else
using SocketHandle = int;
const SocketHandle invalidSocket = -1;
void closeSocket(SocketHandle fd) { ::close(fd); }
int socketError() { return errno; }
#endif
bool acceptCertificates = false;
}

void Session::acceptUnknownCertificates(bool value) { acceptCertificates = value; }

struct Session::Connector {
  std::atomic<bool> cancelled{false};
  std::mutex mutex;
  bool done = false;
  std::unique_ptr<network::TcpSocket> socket;
  std::string error;

  static void run(const std::shared_ptr<Connector>& result, const std::string& host, int port)
  {
    std::unique_ptr<network::TcpSocket> connected;
    std::string message;
    addrinfo hints{}, *addresses = nullptr;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_family = AF_UNSPEC;
    int resolve = getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &addresses);
    if (resolve != 0) message = "Cannot resolve this host";
    auto deadline = Clock::now() + std::chrono::seconds(8);
    for (addrinfo* address = addresses; address && !connected && !result->cancelled;
         address = address->ai_next) {
      SocketHandle fd = ::socket(address->ai_family, address->ai_socktype, address->ai_protocol);
      if (fd == invalidSocket) { message = "Cannot create a network socket"; continue; }
#ifdef WIN32
      u_long nonblocking = 1;
      int modeResult = ioctlsocket(fd, FIONBIO, &nonblocking);
#else
      int modeResult = fcntl(fd, F_SETFL, O_NONBLOCK);
#endif
      if (modeResult != 0) { closeSocket(fd); message = "Cannot configure network socket"; continue; }
      bool ready = ::connect(fd, address->ai_addr, static_cast<int>(address->ai_addrlen)) == 0;
      int error = ready ? 0 : socketError();
#ifdef WIN32
      bool pending = error == WSAEWOULDBLOCK || error == WSAEINPROGRESS;
#else
      bool pending = error == EINPROGRESS || error == EWOULDBLOCK;
#endif
      while (!ready && pending && !result->cancelled && Clock::now() < deadline) {
        fd_set writes, errors;
        FD_ZERO(&writes); FD_ZERO(&errors);
        FD_SET(fd, &writes); FD_SET(fd, &errors);
        timeval timeout{0, 100000};
        int selected = select(static_cast<int>(fd) + 1, nullptr, &writes, &errors, &timeout);
        if (selected < 0) { error = socketError(); break; }
        if (!selected) continue;
        socklen_t length = sizeof(error);
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&error), &length) != 0)
          error = socketError();
        ready = error == 0;
        pending = false;
      }
      if (ready && !result->cancelled) {
        try {
          connected.reset(new network::TcpSocket(static_cast<int>(fd)));
          fd = invalidSocket;
        } catch (const std::exception& exception) { message = exception.what(); }
      } else if (!result->cancelled) {
        message = Clock::now() >= deadline ? "Connection timed out" :
          "Connection failed: " + std::system_category().message(error);
      }
      if (fd != invalidSocket) closeSocket(fd);
      if (Clock::now() >= deadline) break;
    }
    if (addresses) freeaddrinfo(addresses);
    std::lock_guard<std::mutex> lock(result->mutex);
    result->socket = std::move(connected);
    result->error = message.empty() ? "Cannot connect to this host" : message;
    result->done = true;
  }
};

class Session::Connection : public rfb::CConnection {
public:
  Connection(Session& owner, std::unique_ptr<network::TcpSocket> socket)
    : owner_(owner), socket_(std::move(socket))
  {
    std::string host;
    int port;
    network::getHostAndPort(owner.panel_.address.c_str(), &host, &port);
    setServerName(host.c_str());
    setShared(true);
    setSiemensTLS(owner.panel_.security == SecurityMode::Certificate);
    std::list<uint32_t> types;
    if (owner.panel_.security == SecurityMode::Standard)
      types = {rfb::secTypeVncAuth, rfb::secTypeNone};
    else {
#ifdef HAVE_GNUTLS
      types = {static_cast<uint32_t>(owner.panel_.security == SecurityMode::Certificate ?
                 rfb::secTypeX509Vnc : rfb::secTypeTLSVnc)};
#else
      throw std::runtime_error("TLS connections require a build with GnuTLS");
#endif
    }
    security.SetSecTypes(types);
    supportsDesktopResize = true;
    setPreferredEncoding(rfb::encodingTight);
    setCompressLevel(2);
    setStreams(&socket_->inStream(), &socket_->outStream());
    initialiseProtocol();
  }

  ~Connection() override { close(); }

  void pump()
  {
    socket_->outStream().flush();
    getOutStream()->cork(true);
    auto deadline = Clock::now() + std::chrono::milliseconds(4);
    for (int count = 0; count < 128; ++count) {
      if (!processMsg() || Clock::now() >= deadline) break;
    }
    getOutStream()->cork(false);
  }

protected:
  void getUserPasswd(bool, std::string* user, std::string* password) override
  {
    if (owner_.panel_.password.empty()) throw PasswordRequired();
    if (user) user->clear();
    *password = owner_.panel_.password;
  }

  bool showMsgBox(rfb::MsgBoxFlags flags, const char* title, const char* text) override
  {
    // Dashboard sessions only enable TLS and VNC password security, so every
    // yes/no prompt is a certificate trust decision from CSecurityTLS.
    if (acceptCertificates && flags == rfb::MsgBoxFlags::M_YESNO) return true;
    UserDialog dialog;
    std::string caption = owner_.panel_.name + " - " + title;
    bool accepted = dialog.showMsgBox(flags, caption.c_str(), text);
    owner_.handshakeDeadline_ = Clock::now() + std::chrono::seconds(20);
    return accepted;
  }

  void initDone() override
  {
    resizeFramebuffer();
    setPF(getFramebuffer()->getPF());
    owner_.status_ = State::Live;
    owner_.failures_ = 0;
    owner_.error_.clear();
    owner_.changed_();
  }

  void resizeFramebuffer() override
  {
    int width = server.width(), height = server.height();
    if (width <= 0 || height <= 0 || static_cast<size_t>(width) * height > 16777216)
      throw std::runtime_error("Panel resolution exceeds the supported size");
    rfb::PixelFormat format(32, 24, false, true, 255, 255, 255, 0, 8, 16);
    setFramebuffer(new rfb::ManagedPixelBuffer(format, width, height));
    owner_.width_ = width;
    owner_.height_ = height;
    owner_.pixels_.assign(static_cast<size_t>(width) * height * 3, 0);
    ++owner_.generation_;
  }

  void framebufferUpdateEnd() override
  {
    CConnection::framebufferUpdateEnd();
    int stride;
    const auto* buffer = getFramebuffer();
    const uint8_t* data = buffer->getBuffer(buffer->getRect(), &stride);
    for (int y = 0; y < buffer->height(); ++y)
      buffer->getPF().rgbFromBuffer(owner_.pixels_.data() +
        static_cast<size_t>(y) * buffer->width() * 3,
        data + static_cast<size_t>(y) * stride * 4, buffer->width());
    ++owner_.generation_;
    owner_.changed_();
  }

  void bell() override {}

private:
  Session& owner_;
  // The streams outlive close(), including TLS shutdown.
  std::unique_ptr<network::TcpSocket> socket_;
};

Session::Session(Panel& panel, std::function<void()> changed)
  : panel_(panel), changed_(std::move(changed))
{
  network::initSockets();
}

Session::~Session() { clearTransport(); }

void Session::clearTransport()
{
  if (connector_) connector_->cancelled = true;
  connector_.reset();
  connection_.reset();
  buttons_ = 0;
}

void Session::start()
{
  releaseInput();
  wanted_ = true;
  failures_ = 0;
  clearTransport();
  beginAttempt();
}

void Session::stop()
{
  wanted_ = false;
  releaseInput();
  clearTransport();
  status_ = State::Offline;
  error_.clear();
  changed_();
}

void Session::beginAttempt()
{
  try {
    validatePanel(panel_);
    if (panel_.password.empty() && panel_.rememberPassword && !panel_.credential.empty())
      panel_.password = unprotectPassword(panel_.id, panel_.credential);
    std::string host;
    int port;
    network::getHostAndPort(panel_.address.c_str(), &host, &port);
    status_ = State::Connecting;
    error_.clear();
    connector_ = std::make_shared<Connector>();
    std::thread(Connector::run, connector_, host, port).detach();
    changed_();
  } catch (const std::exception& exception) { failed(exception.what(), true); }
}

void Session::failed(const std::string& message, bool attention)
{
  clearTransport();
  error_ = message;
  if (wanted_ && panel_.reconnect && !attention) {
    retryAt_ = Clock::now() + std::chrono::seconds(reconnectDelay(++failures_));
    status_ = State::Retrying;
  } else {
    status_ = State::Attention;
    wanted_ = false;
  }
  changed_();
}

void Session::tick()
{
  if (pumping_) return;
  pumping_ = true;
  try {
    if (status_ == State::Retrying) {
      int seconds = std::max(0, static_cast<int>(
        std::chrono::duration_cast<std::chrono::seconds>(retryAt_ - Clock::now()).count()) + 1);
      if (seconds != lastCountdown_) { lastCountdown_ = seconds; changed_(); }
      if (Clock::now() >= retryAt_) beginAttempt();
    }
    if (connector_) {
      std::unique_ptr<network::TcpSocket> connected;
      std::string error;
      bool done;
      {
        std::lock_guard<std::mutex> lock(connector_->mutex);
        done = connector_->done;
        if (done) { connected = std::move(connector_->socket); error = connector_->error; }
      }
      if (done) {
        connector_.reset();
        if (!connected) throw std::runtime_error(error);
        connection_.reset(new Connection(*this, std::move(connected)));
        status_ = State::Authenticating;
        handshakeDeadline_ = Clock::now() + std::chrono::seconds(20);
        changed_();
      }
    }
    if (connection_) {
      connection_->pump();
      if (status_ == State::Authenticating && Clock::now() > handshakeDeadline_)
        throw std::runtime_error("Connection handshake timed out");
    }
  } catch (const PasswordRequired& exception) { failed(exception.what(), true);
  } catch (const rfb::auth_error&) { failed("Password rejected. Edit the connection to update it.", true);
  } catch (const rfb::auth_cancelled&) { failed("Certificate was not accepted. Reconnect to review it.", true);
  } catch (const std::exception& exception) { failed(exception.what(), false); }
  pumping_ = false;
}

std::string Session::statusText() const
{
  switch (status_) {
    case State::Offline: return "Disconnected";
    case State::Connecting: return "Connecting...";
    case State::Authenticating: return "Authenticating...";
    case State::Live: return panel_.viewOnly ? "Live - Monitor" : "Live - Control";
    case State::Attention: return "Needs attention";
    case State::Retrying: {
      int seconds = std::max(0, static_cast<int>(
        std::chrono::duration_cast<std::chrono::seconds>(retryAt_ - Clock::now()).count()) + 1);
      return "Reconnecting in " + std::to_string(seconds) + "s";
    }
  }
  return "";
}

void Session::setViewOnly(bool value)
{
  if (value && !panel_.viewOnly) releaseInput();
  panel_.viewOnly = value;
  changed_();
}

void Session::pointer(int x, int y, unsigned buttons)
{
  if (!live() || panel_.viewOnly) return;
  try {
    pointerX_ = std::clamp(x, 0, width_ - 1);
    pointerY_ = std::clamp(y, 0, height_ - 1);
    connection_->writer()->writePointerEvent({pointerX_, pointerY_}, buttons);
    buttons_ = buttons;
  } catch (const std::exception& exception) { failed(exception.what(), false); }
}

void Session::key(bool down, int code, unsigned keysym)
{
  if (!live() || panel_.viewOnly) return;
  try {
    if (down) connection_->sendKeyPress(code, 0, keysym);
    else connection_->sendKeyRelease(code);
  } catch (const std::exception& exception) { failed(exception.what(), false); }
}

void Session::releaseInput()
{
  if (!live()) return;
  try {
    connection_->releaseAllKeys();
    if (buttons_) connection_->writer()->writePointerEvent({pointerX_, pointerY_}, 0);
    buttons_ = 0;
  } catch (const std::exception& exception) { failed(exception.what(), false); }
}

void Session::refresh()
{
  if (!live()) return;
  try { connection_->refreshFramebuffer(); }
  catch (const std::exception& exception) { failed(exception.what(), false); }
}

}
