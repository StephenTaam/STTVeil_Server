# STTVeil 服务端

STTVeil Server 是 STTVeil项目 的 C++ 后端。可以编译后快速部署STTVeil的服务器，实现端对端加密通信。


## 技术栈

- C++17
- STTNet (https://sttnet.pages.dev)
- MariaDB/MySQL
- JsonCpp
- OpenSSL

## 目录结构

- `server.cpp`：主后端实现
- `makefile`：编译脚本
- `config/database.conf`：数据库运行配置
- `config/service.conf`：HTTP/WebSocket 运行配置
- `STTChat_Resources`：服务运行资源
- `README.md`、`README.zh-CN.md`

## 依赖安装（Linux）

说明：下列命令安装编译依赖。`STTNet` 需要你按自己的路径单独准备。

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

## 编译与运行

```bash
make clean
make server
./server
```

## 数据库初始化（全量建表 SQL）

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

## 配置说明

服务会读取：

- `./config/database.conf`（本地私密，建议不入库）
- `./config/service.conf`（本地私密，建议不入库）

建议先复制示例文件：

- `config/database.example.conf` -> `config/database.conf`
- `config/service.example.conf` -> `config/service.conf`

如果配置文件不存在，则只使用非敏感默认值；数据库密码/TLS/跨域地址必须在配置文件里显式设置。

### `config/database.conf`

```ini
db.host=127.0.0.1
db.port=3306
db.database=im_db
db.user=im
db.password=CHANGE_ME
db.charset=utf8mb4
```

### `config/database.conf` 参数说明

- `db.host`：数据库主机地址（IP 或域名）。
- `db.port`：数据库端口（常见为 `3306`）。
- `db.database`：数据库名称（例如 `im_db`）。
- `db.user`：数据库用户名。
- `db.password`：数据库密码。
- `db.charset`：数据库连接字符集，建议 `utf8mb4`（支持中文和 emoji）。

### `config/service.conf`（当前项目默认值）

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

### `config/service.conf` 参数说明

#### HTTP 参数

- `http.max_fd`：HTTP 服务最大并发连接上限（FD 数）。
- `http.buffer_size_kb`：单连接接收缓冲大小上限（KB）。
- `http.finish_queue_cap`：Worker 完成队列容量，建议设置为 2 的幂。
- `http.security_open`：是否开启安全模块（限速、防刷、连接控制）。
- `http.connection_num_limit`：同一 IP 可建立的最大连接数。
- `http.connection_secs`：连接速率统计窗口（秒）。
- `http.connection_times`：在连接窗口内允许的最大连接次数。
- `http.request_secs`：请求速率统计窗口（秒）。
- `http.request_times`：在请求窗口内允许的最大请求次数。
- `http.check_frequency`：僵尸连接检查周期（秒），`-1` 表示关闭检查。
- `http.connection_timeout`：连接空闲超时时间（秒），`-1` 表示不超时。
- `http.listen_port`：HTTP 监听端口。
- `http.listen_threads`：HTTP 监听线程数。
- `http.cors_allow_origin`：允许跨域的前端地址（如 `https://your-frontend.example.com`）。
- `http.tls_cert`：TLS 证书路径（PEM）。
- `http.tls_key`：TLS 私钥路径。
- `http.tls_password`：TLS 私钥密码（无密码可留空）。
- `http.tls_ca`：CA 证书链路径（按部署需要配置）。

#### WebSocket 参数

- `ws.max_fd`：WebSocket 服务最大并发连接上限（FD 数）。
- `ws.buffer_size_kb`：单连接接收缓冲大小上限（KB）。
- `ws.finish_queue_cap`：Worker 完成队列容量，建议设置为 2 的幂。
- `ws.security_open`：是否开启安全模块（限速、防刷、连接控制）。
- `ws.connection_num_limit`：同一 IP 可建立的最大连接数。
- `ws.connection_secs`：连接速率统计窗口（秒）。
- `ws.connection_times`：在连接窗口内允许的最大连接次数。
- `ws.request_secs`：请求速率统计窗口（秒）。
- `ws.request_times`：在请求窗口内允许的最大请求次数。
- `ws.check_frequency`：僵尸连接检查周期（秒），`-1` 表示关闭检查。
- `ws.connection_timeout`：连接空闲超时时间（秒），`-1` 表示不超时。
- `ws.listen_port`：WebSocket 监听端口。
- `ws.listen_threads`：WebSocket 监听线程数。
- `ws.tls_cert`：WebSocket 服务 TLS 证书路径（PEM）。
- `ws.tls_key`：WebSocket 服务 TLS 私钥路径。
- `ws.tls_password`：WebSocket TLS 私钥密码（无密码可留空）。
- `ws.tls_ca`：WebSocket TLS CA 证书链路径（按部署需要配置）。


## 安全说明

- 私聊消息在客户端加密后再传输。
- 群聊使用加密负载与成员级密钥封装。
- 群成员变更会触发密钥版本刷新和客户端同步。


## 许可证

开源前请补充 `LICENSE` 文件。