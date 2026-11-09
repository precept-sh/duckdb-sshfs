#pragma once

#include "duckdb.hpp"
#include <atomic>
#include <condition_variable>
#include <libssh2.h>
#include <libssh2_sftp.h>
#include <memory>
#include <mutex>
#include <queue>
#include <string>

namespace duckdb {

struct SSHConnectionParams {
  std::string hostname;
  int port = 22;
  std::string username;
  std::string password;
  std::string key_path;
  std::string remote_path; // Path on the remote server

  // Authentication
  bool use_agent = false; // Explicitly use SSH agent for authentication

  // Debug logging
  bool debug_logging = false;

  // Crypto policy
  bool strict_crypto = false; // Restrict to non-NIST algorithms only

  // Connection tuning
  int timeout_seconds = 300; // 5 minutes for long uploads
  int max_retries = 3;       // Maximum connection retry attempts
  int initial_retry_delay_ms =
      1000; // Initial delay between retries (exponential backoff)
  int keepalive_interval = 60; // Send keepalive every 60 seconds (0 = disabled)

  // Upload performance tuning
  size_t chunk_size = 50 * 1024 * 1024; // 50MB default chunk size
  size_t max_concurrent_uploads = 2;    // Conservative for SFTP
};

class SSHClient {
public:
  explicit SSHClient(const SSHConnectionParams &params);
  ~SSHClient();

  // Connection management
  void Connect();
  void Disconnect();
  bool IsConnected() const { return connected; }
  // Keepalive check. Returns true without probing if another thread is
  // currently using the session (it is demonstrably in use).
  bool ValidateConnection();

  // Capability detection
  bool SupportsCommands() const { return supports_commands; }
  void DetectCapabilities();

  // Command execution
  std::string ExecuteCommand(const std::string &command);

  // File operations
  void UploadChunk(const std::string &remote_path, const char *data,
                   size_t size, bool append = false);
  void RemoveFile(const std::string &remote_path);
  void RenameFile(const std::string &source_path,
                  const std::string &target_path);
  LIBSSH2_SFTP_ATTRIBUTES GetFileStats(const std::string &remote_path);

  // Read operations using dd
  size_t ReadBytes(const std::string &remote_path, char *buffer, size_t offset,
                   size_t length);

  // RAII lease on the session lock plus the pooled SFTP channel. Holds the
  // session lock (connecting first if needed) until destroyed, so every libssh2
  // call made through Get() is serialized with all other calls on the session.
  class SFTPLease {
  public:
    explicit SFTPLease(SSHClient &client)
        : client(client), lock(client.LockConnected()),
          sftp(client.BorrowSFTPSession()) {}
    ~SFTPLease() { client.ReturnSFTPSession(sftp); }
    SFTPLease(const SFTPLease &) = delete;
    SFTPLease &operator=(const SFTPLease &) = delete;
    LIBSSH2_SFTP *Get() const { return sftp; }

  private:
    SSHClient &client;
    std::unique_lock<std::recursive_mutex> lock;
    LIBSSH2_SFTP *sftp;
  };

  // SFTP-only operations (no SSH command execution)
  void CreateDirectorySFTP(const std::string &remote_path);
  void RemoveDirectorySFTP(const std::string &remote_path);
  void TruncateFileSFTP(const std::string &remote_path, int64_t new_size);
  size_t ReadBytesSFTP(const std::string &remote_path, char *buffer,
                       size_t offset, size_t length);

private:
  SSHConnectionParams params;
  int sock = -1;
  LIBSSH2_SESSION *session = nullptr;
  std::atomic<bool> connected{false};
  bool supports_commands = false; // Auto-detected: can execute SSH commands
  bool dd_disabled =
      false;               // Disabled after channel failures (use SFTP instead)

  // libssh2 sessions are not thread-safe: every libssh2 call on `session` (or
  // on anything derived from it: SFTP channels, handles, exec channels) must
  // happen while holding this lock. Recursive so public operations can nest.
  // Lock order: session_mutex before pool_mutex.
  std::recursive_mutex session_mutex;

  // SFTP session pool (guarded by pool_mutex; used only under session_mutex)
  std::queue<LIBSSH2_SFTP *> sftp_pool;
  std::mutex pool_mutex;
  std::condition_variable pool_cv;
  size_t pool_size = 1; // Single SFTP session - reused across reads
  bool pool_initialized = false;

  std::unique_lock<std::recursive_mutex> LockSession() {
    return std::unique_lock<std::recursive_mutex>(session_mutex);
  }
  // Take the session lock and (re)connect if the session is down.
  std::unique_lock<std::recursive_mutex> LockConnected();
  LIBSSH2_SFTP *BorrowSFTPSession();
  void ReturnSFTPSession(LIBSSH2_SFTP *sftp);

  void InitializeSession();
  void Authenticate();
  void CleanupSession();
  // Requires pool_mutex held
  void InitializeSFTPPool();
  // Takes pool_mutex
  void CleanupSFTPPool();
  // Requires pool_mutex held
  void CleanupSFTPPoolLocked();
};

} // namespace duckdb
