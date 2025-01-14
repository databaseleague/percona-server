/* Copyright (c) 2019 Francisco Miguel Biete Banon. All rights reserved.
   Copyright (c) 2022, Percona Inc. All Rights Reserved.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software Foundation,
   51 Franklin Street, Suite 500, Boston, MA 02110-1335 USA */
#include "plugin/auth_ldap/include/pool.h"

#include <cmath>
#include <iostream>
#include <thread>
#include "my_sys.h"

#include <boost/algorithm/string.hpp>
#include "plugin/auth_ldap/include/plugin_log.h"

namespace mysql {
namespace plugin {
namespace auth_ldap {

Pool::Pool(std::size_t pool_initial_size, std::size_t pool_max_size,
           const std::string &ldap_host, std::uint16_t ldap_port,
           const std::string &fallback_host, std::uint16_t fallback_port,
           bool use_ssl, bool use_tls, const std::string &ca_path,
           const std::string &bind_dn, const std::string &bind_pwd)
    : pool_initial_size_(pool_initial_size),
      pool_max_size_(pool_max_size),
      ldap_host_(ldap_host),
      ldap_port_(ldap_port),
      ldap_fallback_host_(fallback_host),
      ldap_fallback_port_(fallback_port),
      use_ssl_(use_ssl),
      use_tls_(use_tls),
      ca_path_(ca_path),
      bind_dn_(bind_dn),
      bind_pwd_(bind_pwd) {
  std::lock_guard<std::mutex> lock(pool_mutex_);

  Connection::initialize_global_ldap_parameters(true, ca_path);

  bs_used_.resize(pool_max_size_);
  v_connections_.resize(pool_max_size_);
  for (std::size_t i = 0; i < pool_max_size_; i++) {
    v_connections_[i] = std::make_shared<Connection>(
        i, ldap_host_, ldap_port_, ldap_fallback_host_, ldap_fallback_port_,
        use_ssl, use_tls);
    if (i < pool_initial_size_) {
      std::string auth_resp;
      v_connections_[i]->connect(bind_dn_, bind_pwd_, auth_resp);
    }
  }
}

Pool::~Pool() {
  std::lock_guard<std::mutex> lock(pool_mutex_);

  v_connections_.clear();
}

/**
 * Obtains a connection
 **/
Pool::pool_ptr_t Pool::borrow_connection(bool default_connect) {
  // We have to hold the lock for the entire function:
  // otherwise if the pool is resized to a small size during a
  // borrow_connection run could result to overindexing
  // internal structures
  std::lock_guard<std::mutex> lock(pool_mutex_);
  int idx = -1;
  // Find a free connection - scope

  idx = find_first_free();
  if (idx == -1) {
    log_srv_warn("WARNING: No available connections in the pool");
  } else {
    mark_as_busy(idx);
  }

  // No available connection - exit
  if (idx == -1) {
    std::thread t(&Pool::zombie_control, this);
    t.detach();
    return nullptr;
  }

  // Get connection object and connect [slow]
  auto conn = this->get_connection(idx, default_connect);
  // If we don't have a valid connection, free up the pool element
  if (conn == nullptr) {
    mark_as_free(idx);
  }

  return conn;
}

void Pool::debug_info() {
  std::stringstream log_stream;
  log_stream << "conn_init [" << pool_initial_size_ << "] conn_max ["
             << pool_max_size_ << "] conn_in_use [" << bs_used_.count() << "]";
  log_srv_dbg(log_stream.str());
}

/**
 * Returns a connection to the pool
 **/
void Pool::return_connection(pool_ptr_t conn) {
  // Mark the connection as free
  conn->mark_as_free();

  // if connection was snipped because the pool was resized...
  if (conn->is_snipped()) {
    conn.reset();
  } else {
    // Mark the element as free in the pool - scope
    {
      std::lock_guard<std::mutex> lock(pool_mutex_);
      const int idx = conn->get_idx_pool();
      // idx could be higher than the current pool size, but
      // mark as free protects against this
      mark_as_free(idx);
    }

    // Launch a detached thread for zombie control if used > 90%
    if (bs_used_.count() >= std::ceil(pool_max_size_ * 0.9)) {
      std::thread t(&Pool::zombie_control, this);
      t.detach();
    }
  }
}

void Pool::reset_group_role_mapping(std::string const &mapping) {
  std::vector<std::string> roles;
  boost::algorithm::split(roles, mapping, boost::is_any_of(","));
  group_role_mapping_.clear();
  for (auto const &role : roles) {
    std::vector<std::string> r;
    boost::algorithm::split(r, role, boost::is_any_of("="));
    if (r.size() == 1) {
      group_role_mapping_[role] = role;
    } else {
      group_role_mapping_[r[0]] = r[1];
    }
  }
}

void Pool::reconfigure(std::size_t newpool_initial_size_,
                       std::size_t newpool_max_size_,
                       const std::string &ldap_host, std::uint16_t ldap_port,
                       const std::string &fallback_host,
                       std::uint16_t fallback_port, bool use_ssl, bool use_tls,
                       const std::string &ca_path, const std::string &bind_dn,
                       const std::string &bind_pwd) {
  log_srv_dbg("Pool::reconfigure()");
  // Force zombie control
  zombie_control();

  std::lock_guard<std::mutex> lock(pool_mutex_);

  // Resize pool
  if (newpool_max_size_ != pool_max_size_) {
    bs_used_.resize(newpool_max_size_);
    // If new size is smaller -> mark newpool_max_size_ to pool_max_size for
    // deletion
    if (newpool_max_size_ < pool_max_size_) {
      log_srv_dbg("reducing max pool size");
      for (std::size_t i = newpool_max_size_; i < pool_max_size_; i++) {
        v_connections_[i]->mark_as_snipped();
      }
    }
    v_connections_.resize(newpool_max_size_);

    if (newpool_max_size_ > pool_max_size_) {
      log_srv_dbg("extending max pool size");
      for (std::size_t i = pool_max_size_; i < newpool_max_size_; i++) {
        v_connections_[i] =
            std::make_shared<Connection>(i, ldap_host, ldap_port, fallback_host,
                                         fallback_port, use_ssl, use_tls);
      }
    }

    pool_max_size_ = newpool_max_size_;
  }

  DEBUG_SYNC_C("auth_ldap_in_reconfigure");

  // Reconnect pool
  ldap_host_ = ldap_host;
  ldap_port_ = ldap_port;
  ldap_fallback_host_ = fallback_host;
  ldap_fallback_port_ = fallback_port;
  use_ssl_ = use_ssl;
  use_tls_ = use_tls;
  ca_path_ = ca_path;
  pool_initial_size_ = newpool_initial_size_;
  bind_dn_ = bind_dn;
  bind_pwd_ = bind_pwd;

  for (std::size_t i = 0; i < pool_max_size_; i++) {
    v_connections_[i]->configure(ldap_host_, ldap_port_, ldap_fallback_host_,
                                 ldap_fallback_port_, use_ssl_, use_tls_);
    if (i < pool_initial_size_) {
      std::string auth_resp;
      v_connections_[i]->connect(bind_dn_, bind_pwd_, auth_resp);
    }
  }

  for (std::size_t i = 0; i < newpool_initial_size_; i++) {
    std::string auth_resp;
    v_connections_[i]->connect(bind_dn_, bind_pwd_, auth_resp);
  }
}

void Pool::zombie_control() {
  std::lock_guard<std::mutex> lock(pool_mutex_);

  for (std::size_t i = 0; i < pool_max_size_; i++) {
    if (bs_used_.test(i) && v_connections_[i]->is_zombie()) {
      v_connections_[i]->mark_as_free();
      mark_as_free(i);
    }
  }
}

int Pool::find_first_free() {
  // requires holding the lock
  int idx = -1;

  // If everything is in use, fast-exit
  if (!bs_used_.all()) {
    for (std::size_t i = 0; i < pool_max_size_; i++) {
      if (!bs_used_.test(i)) {
        idx = i;
        break;  // exit for
      }
    }
  }

  return idx;
}

Pool::pool_ptr_t Pool::get_connection(int idx, bool default_connect) {
  // requires holding the lock
  auto conn = v_connections_[idx];
  std::string auth_resp;
  if (default_connect && conn->connect(bind_dn_, bind_pwd_, auth_resp) !=
                             Connection::status::SUCCESS) {
    log_srv_error("Connection to LDAP backend failed");
    conn = nullptr;
  } else {
    conn->mark_as_busy();
  }

  return conn;
}

// requires holding the lock
void Pool::mark_as_busy(size_t idx) { bs_used_.set(idx, true); }

// requires holding the lock
void Pool::mark_as_free(size_t idx) {
  if (bs_used_.size() > idx) bs_used_.set(idx, false);
}

}  // namespace auth_ldap
}  // namespace plugin
}  // namespace mysql
