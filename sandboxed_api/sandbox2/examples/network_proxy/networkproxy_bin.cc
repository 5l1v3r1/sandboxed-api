// This file is an example of a network sandboxed binary inside a network
// namespace. It can't connect with the server directly, but the executor can
// establish a connection and pass the connected socket to the sandboxee.

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <syscall.h>

#include <cstring>

#include "sandboxed_api/util/flag.h"
#include "absl/strings/str_format.h"
#include "sandboxed_api/sandbox2/client.h"
#include "sandboxed_api/sandbox2/comms.h"
#include "sandboxed_api/sandbox2/network_proxy/client.h"
#include "sandboxed_api/sandbox2/util/fileops.h"
#include "sandboxed_api/sandbox2/util/strerror.h"
#include "sandboxed_api/util/status.h"
#include "sandboxed_api/util/status_macros.h"
#include "sandboxed_api/util/statusor.h"

ABSL_FLAG(bool, connect_with_handler, true, "Connect using automatic mode.");

namespace {

sandbox2::NetworkProxyClient* proxy_client;

ssize_t ReadFromFd(int fd, uint8_t* buf, size_t size) {
  ssize_t received = 0;
  while (received < size) {
    ssize_t read_status =
        TEMP_FAILURE_RETRY(read(fd, &buf[received], size - received));
    if (read_status == 0) {
      break;
    }
    if (read_status < 0) {
      return -1;
    }
    received += read_status;
  }
  return received;
}

absl::Status CommunicationTest(int sock) {
  char received[1025] = {0};

  if (ReadFromFd(sock, reinterpret_cast<uint8_t*>(received),
                 sizeof(received) - 1) <= 0) {
    return absl::InternalError("Data receiving error");
  }
  absl::PrintF("Sandboxee received data from the server:\n\n%s\n", received);
  if (strcmp(received, "Hello World\n")) {
    return absl::InternalError("Data receiving error");
  }
  return absl::OkStatus();
}

sapi::StatusOr<struct sockaddr_in6> CreateAddres(int port) {
  static struct sockaddr_in6 saddr {};
  saddr.sin6_family = AF_INET6;
  saddr.sin6_port = htons(port);

  int err = inet_pton(AF_INET6, "::1", &saddr.sin6_addr);
  if (err <= 0) {
    return absl::InternalError(
        absl::StrCat("socket() failed: ", sandbox2::StrError(errno)));
  }
  return saddr;
}

absl::Status ConnectManually(int s, const struct sockaddr_in6& saddr) {
  return proxy_client->Connect(
      s, reinterpret_cast<const struct sockaddr*>(&saddr), sizeof(saddr));
}

absl::Status ConnectWithHandler(int s, const struct sockaddr_in6& saddr) {
  int err = connect(s, reinterpret_cast<const struct sockaddr*>(&saddr),
                    sizeof(saddr));
  if (err != 0) {
    return absl::InternalError("connect() failed");
  }

  return absl::OkStatus();
}

sapi::StatusOr<int> ConnectToServer(int port) {
  SAPI_ASSIGN_OR_RETURN(struct sockaddr_in6 saddr, CreateAddres(port));

  sandbox2::file_util::fileops::FDCloser s(socket(AF_INET6, SOCK_STREAM, 0));
  if (s.get() < 0) {
    return absl::InternalError(
        absl::StrCat("socket() failed: ", sandbox2::StrError(errno)));
  }

  if (absl::GetFlag(FLAGS_connect_with_handler)) {
    SAPI_RETURN_IF_ERROR(ConnectWithHandler(s.get(), saddr));
  } else {
    SAPI_RETURN_IF_ERROR(ConnectManually(s.get(), saddr));
  }

  LOG(INFO) << "Connected to the server";
  return s.Release();
}

}  // namespace

int main(int argc, char** argv) {
  gflags::ParseCommandLineFlags(&argc, &argv, false);

  // Set-up the sandbox2::Client object, using a file descriptor (1023).
  sandbox2::Comms comms(sandbox2::Comms::kSandbox2ClientCommsFD);
  sandbox2::Client sandbox2_client(&comms);

  // Enable sandboxing from here.
  sandbox2_client.SandboxMeHere();

  if (absl::GetFlag(FLAGS_connect_with_handler)) {
    absl::Status status = sandbox2_client.InstallNetworkProxyHandler();
    if (!status.ok()) {
      LOG(ERROR) << "InstallNetworkProxyHandler() failed: " << status.message();
      return 1;
    }
  } else {
    proxy_client = sandbox2_client.GetNetworkProxyClient();
  }

  // Receive port number of the server
  int port;
  if (!comms.RecvInt32(&port)) {
    LOG(ERROR) << "sandboxee_comms->RecvUint32(&crc4) failed";
    return 2;
  }

  sapi::StatusOr<int> sock_s = ConnectToServer(port);
  if (!sock_s.ok()) {
    LOG(ERROR) << sock_s.status().message();
    return 3;
  }
  sandbox2::file_util::fileops::FDCloser client{sock_s.ValueOrDie()};

  absl::Status status = CommunicationTest(client.get());
  if (!status.ok()) {
    LOG(ERROR) << sock_s.status().message();
    return 4;
  }

  return 0;
}
