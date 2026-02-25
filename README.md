# STTVeil Server

STTVeil Server is the C++ backend of the STTVeil project. After compilation, you can quickly deploy an STTVeil server to enable end-to-end encrypted communication.

## Tech Stack

- C++17
- STTNet (https://sttnet.pages.dev)
- MariaDB/MySQL
- JsonCpp
- OpenSSL

## Directory Structure

- `server.cpp`: Main backend implementation
- `makefile`: Build script
- `config/database.conf`: Database runtime configuration
- `config/service.conf`: HTTP/WebSocket runtime configuration
- `STTChat_Resources`: Runtime resources
- `README.md`, `README.zh-CN.md`

## Dependency Installation (Linux)

Note: The following commands install build dependencies. You need to prepare `STTNet` separately based on your own local path.

### Ubuntu / Debian

```bash
sudo apt update
sudo apt install -y g++ make pkg-config libssl-dev libjsoncpp-dev libmariadb-dev
```

### CentOS 7 / RHEL 7

```bash
sudo yum install -y gcc-c++ make openssl-devel jsoncpp-devel mariadb-devel
```

### Fedora

```bash
sudo dnf install -y gcc-c++ make openssl-devel jsoncpp-devel mariadb-connector-c-devel
```

### Arch Linux

```bash
sudo pacman -S --needed base-devel openssl jsoncpp mariadb-libs
```

### openSUSE

```bash
sudo zypper install -y gcc-c++ make libopenssl-devel jsoncpp-devel libmariadb-devel
```

## Build and Run

```bash
make clean
make server
./server
```

## Database Initialization (Full Table Creation SQL)

```sql
CREATE DATABASE IF NOT EXISTS im_db CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci;
USE im_db;

CREATE TABLE IF NOT EXISTS t_user (
  id BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,
  username VARCHAR(64) NOT NULL,
  password_kdf VARCHAR(32) NOT NULL,
  password_iterations INT NOT NULL,
  password_salt VARCHAR(64) NOT NULL,
  password_hash VARCHAR(64) NOT NULL,
  pubkey_alg VARCHAR(16) NOT NULL,
  pubkey_curve VARCHAR(16) NOT NULL,
  pubkey_spki TEXT NOT NULL,
  prikey_kdf VARCHAR(32) NOT NULL,
  prikey_iterations INT NOT NULL,
  prikey_salt VARCHAR(64) NOT NULL,
  prikey_cipher VARCHAR(32) NOT NULL,
  prikey_iv VARCHAR(32) NOT NULL,
  prikey_ciphertext MEDIUMTEXT NOT NULL,
  avatar_url VARCHAR(255) DEFAULT '/resources/avatars/default.png',
  PRIMARY KEY (id),
  UNIQUE KEY uk_t_user_username (username)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

CREATE TABLE IF NOT EXISTS t_friend (
  id BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,
  user_id BIGINT UNSIGNED NOT NULL,
  peer_id BIGINT UNSIGNED NOT NULL,
  status ENUM('pending_out','pending_in','accepted','blocked','none') NOT NULL DEFAULT 'none',
  blocked_by BIGINT UNSIGNED DEFAULT NULL,
  created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
  PRIMARY KEY (id),
  KEY idx_t_friend_user_id (user_id),
  KEY idx_t_friend_peer_id (peer_id),
  UNIQUE KEY uk_t_friend_pair (user_id, peer_id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

CREATE TABLE IF NOT EXISTS t_message (
  id BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,
  from_id BIGINT UNSIGNED NOT NULL,
  to_id BIGINT UNSIGNED NOT NULL,
  kind ENUM('text','image','file') DEFAULT 'text',
  iv VARCHAR(64) DEFAULT NULL,
  ciphertext MEDIUMTEXT DEFAULT NULL,
  mime VARCHAR(255) DEFAULT NULL,
  name VARCHAR(255) DEFAULT NULL,
  size BIGINT UNSIGNED DEFAULT NULL,
  file_url VARCHAR(1024) DEFAULT NULL,
  payload_json LONGTEXT DEFAULT NULL,
  created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
  ts BIGINT UNSIGNED NOT NULL,
  PRIMARY KEY (id),
  KEY idx_t_message_from_id (from_id),
  KEY idx_t_message_to_ts (to_id, ts),
  KEY idx_t_message_from_to_ts (from_id, to_id, ts)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

CREATE TABLE IF NOT EXISTS t_group (
  id BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,
  name VARCHAR(128) NOT NULL,
  owner_id BIGINT UNSIGNED NOT NULL,
  key_epoch BIGINT UNSIGNED NOT NULL DEFAULT 1,
  created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
  PRIMARY KEY (id),
  KEY idx_t_group_owner_id (owner_id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

CREATE TABLE IF NOT EXISTS t_group_member (
  id BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,
  group_id BIGINT UNSIGNED NOT NULL,
  user_id BIGINT UNSIGNED NOT NULL,
  role ENUM('owner','member') NOT NULL DEFAULT 'member',
  created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
  PRIMARY KEY (id),
  KEY idx_t_group_member_group_id (group_id),
  KEY idx_t_group_member_user_id (user_id),
  UNIQUE KEY uk_t_group_member_group_user (group_id, user_id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

CREATE TABLE IF NOT EXISTS t_group_message (
  id BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,
  group_id BIGINT UNSIGNED NOT NULL,
  from_id BIGINT UNSIGNED NOT NULL,
  payload_json LONGTEXT NOT NULL,
  ts BIGINT UNSIGNED NOT NULL,
  created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
  PRIMARY KEY (id),
  KEY idx_t_group_message_group_ts (group_id, ts),
  KEY idx_t_group_message_from_id (from_id),
  CONSTRAINT chk_group_payload_json_valid CHECK (JSON_VALID(payload_json))
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;
```

## Configuration

The service reads:

- `./config/database.conf` (local/private, recommended not to commit)
- `./config/service.conf` (local/private, recommended not to commit)

It is recommended to copy from example files first:

- `config/database.example.conf` -> `config/database.conf`
- `config/service.example.conf` -> `config/service.conf`

If config files do not exist, only non-sensitive defaults are used. Database password/TLS/CORS origin must be explicitly set in config files.

### `config/database.conf`

```ini
db.host=127.0.0.1
db.port=3306
db.database=im_db
db.user=im
db.password=CHANGE_ME
db.charset=utf8mb4
```

### `config/database.conf` Parameter Description

- `db.host`: Database host address (IP or domain).
- `db.port`: Database port (commonly `3306`).
- `db.database`: Database name (for example `im_db`).
- `db.user`: Database username.
- `db.password`: Database password.
- `db.charset`: Character set for the database connection. `utf8mb4` is recommended (supports Chinese and emoji).

### `config/service.conf` (Current Project Defaults)

```ini
# HTTP
http.max_fd=100000
http.buffer_size_kb=5120
http.finish_queue_cap=65536
http.security_open=true
http.connection_num_limit=10
http.connection_secs=1
http.connection_times=3
http.request_secs=1
http.request_times=20
http.check_frequency=30
http.connection_timeout=30
http.listen_port=8080
http.listen_threads=2
http.cors_allow_origin=https://your-frontend.example.com
http.tls_cert=/path/to/http-cert.pem
http.tls_key=/path/to/http-key.pem
http.tls_password=
http.tls_ca=/path/to/ca-bundle.pem

# WebSocket
ws.max_fd=50000
ws.buffer_size_kb=256
ws.finish_queue_cap=65536
ws.security_open=true
ws.connection_num_limit=20
ws.connection_secs=5
ws.connection_times=30
ws.request_secs=1
ws.request_times=10
ws.check_frequency=60
ws.connection_timeout=600
ws.listen_port=5050
ws.listen_threads=2
ws.tls_cert=/path/to/ws-cert.pem
ws.tls_key=/path/to/ws-key.pem
ws.tls_password=
ws.tls_ca=/path/to/ca-bundle.pem
```

### `config/service.conf` Parameter Description

#### HTTP Parameters

- `http.max_fd`: Maximum concurrent HTTP connections (FD count).
- `http.buffer_size_kb`: Maximum receive buffer size per connection (KB).
- `http.finish_queue_cap`: Worker completion queue capacity. Power-of-two values are recommended.
- `http.security_open`: Whether to enable security modules (rate limiting, anti-abuse, connection control).
- `http.connection_num_limit`: Maximum number of connections per IP.
- `http.connection_secs`: Time window for connection rate statistics (seconds).
- `http.connection_times`: Maximum allowed connection attempts within the connection window.
- `http.request_secs`: Time window for request rate statistics (seconds).
- `http.request_times`: Maximum allowed requests within the request window.
- `http.check_frequency`: Zombie connection check interval (seconds). `-1` disables checks.
- `http.connection_timeout`: Idle connection timeout (seconds). `-1` means no timeout.
- `http.listen_port`: HTTP listening port.
- `http.listen_threads`: Number of HTTP listener threads.
- `http.cors_allow_origin`: Allowed frontend origin for CORS (for example `https://your-frontend.example.com`).
- `http.tls_cert`: TLS certificate path (PEM).
- `http.tls_key`: TLS private key path.
- `http.tls_password`: TLS private key password (leave empty if not encrypted).
- `http.tls_ca`: CA certificate chain path (configure based on deployment requirements).

#### WebSocket Parameters

- `ws.max_fd`: Maximum concurrent WebSocket connections (FD count).
- `ws.buffer_size_kb`: Maximum receive buffer size per connection (KB).
- `ws.finish_queue_cap`: Worker completion queue capacity. Power-of-two values are recommended.
- `ws.security_open`: Whether to enable security modules (rate limiting, anti-abuse, connection control).
- `ws.connection_num_limit`: Maximum number of connections per IP.
- `ws.connection_secs`: Time window for connection rate statistics (seconds).
- `ws.connection_times`: Maximum allowed connection attempts within the connection window.
- `ws.request_secs`: Time window for request rate statistics (seconds).
- `ws.request_times`: Maximum allowed requests within the request window.
- `ws.check_frequency`: Zombie connection check interval (seconds). `-1` disables checks.
- `ws.connection_timeout`: Idle connection timeout (seconds). `-1` means no timeout.
- `ws.listen_port`: WebSocket listening port.
- `ws.listen_threads`: Number of WebSocket listener threads.
- `ws.tls_cert`: TLS certificate path (PEM) for the WebSocket service.
- `ws.tls_key`: TLS private key path for the WebSocket service.
- `ws.tls_password`: TLS private key password for WebSocket (leave empty if not encrypted).
- `ws.tls_ca`: CA certificate chain path for WebSocket TLS (configure based on deployment requirements).

## Security Notes

- Private chat messages are encrypted on the client before transmission.
- Group chat uses encrypted payloads and member-level key wrapping.
- Group membership changes trigger key-epoch refresh and client synchronization.

## License

Please add a `LICENSE` file before open-sourcing.
