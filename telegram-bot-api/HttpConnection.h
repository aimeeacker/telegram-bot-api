//
// Copyright Aliaksei Levin (levlam@telegram.org), Arseny Smirnov (arseny30@gmail.com) 2014-2026
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
#pragma once

#include "telegram-bot-api/ClientManager.h"
#include "telegram-bot-api/Query.h"

#include "td/net/HttpInboundConnection.h"
#include "td/net/HttpQuery.h"

#include "td/actor/actor.h"

#include "td/utils/buffer.h"
#include "td/utils/common.h"
#include "td/utils/Slice.h"
#include "td/utils/Status.h"

#include <memory>

namespace telegram_bot_api {

struct SharedData;

class HttpConnection final : public td::HttpInboundConnection::Callback {
 public:
  HttpConnection(td::ActorId<ClientManager> client_manager, std::shared_ptr<SharedData> shared_data)
      : client_manager_(client_manager), shared_data_(std::move(shared_data)) {
  }

  void handle(td::unique_ptr<td::HttpQuery> http_query, td::ActorOwn<td::HttpInboundConnection> connection) final;

 private:
  td::ActorId<ClientManager> client_manager_;
  td::ActorOwn<td::HttpInboundConnection> connection_;
  std::shared_ptr<SharedData> shared_data_;

  bool is_mcp_request_ = false;
  td::string mcp_request_id_json_ = "null";
  td::string mcp_protocol_version_ = "2025-11-25";

  void hangup() final {
    connection_.release();
    stop();
  }

  void handle_mcp(td::unique_ptr<td::HttpQuery> http_query);
  td::Promise<td::unique_ptr<Query>> make_query_promise();
  void on_query_finished(td::Result<td::unique_ptr<Query>> r_query);

  void handle_stats(td::unique_ptr<td::HttpQuery> http_query);
  void on_stats_result(td::Result<ClientManager::StatsResponse> result);
  void send_redirect(int http_status_code, td::CSlice location);

  void send_response(int http_status_code, td::BufferSlice &&content, int retry_after);
  void send_http_error(int http_status_code, td::CSlice description);
  void send_mcp_result(td::Slice result);
  void send_mcp_error(int http_status_code, int error_code, td::Slice message, td::Slice id_json = "null",
                      td::Slice data_json = {});
};

}  // namespace telegram_bot_api
