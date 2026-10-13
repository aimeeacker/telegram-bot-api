//
// Copyright Aliaksei Levin (levlam@telegram.org), Arseny Smirnov (arseny30@gmail.com) 2014-2026
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
#include "telegram-bot-api/HttpStatConnection.h"

#include "td/net/HttpHeaderCreator.h"

#include "td/utils/common.h"
#include "td/utils/Promise.h"

namespace telegram_bot_api {

void HttpStatConnection::handle(td::unique_ptr<td::HttpQuery> http_query,
                                td::ActorOwn<td::HttpInboundConnection> connection) {
  CHECK(connection_.empty());
  connection_ = std::move(connection);

  if (http_query->url_path_ != "/" && http_query->url_path_ != "/stats") {
    return on_result(td::Status::Error(404, "Not Found"));
  }
  if (http_query->type_ != td::HttpQuery::Type::Get && http_query->type_ != td::HttpQuery::Type::Post) {
    return on_result(td::Status::Error(405, "Method Not Allowed"));
  }
  if (http_query->type_ == td::HttpQuery::Type::Post && http_query->get_header("sec-fetch-site") == "cross-site") {
    return on_result(td::Status::Error(403, "Cross-site stats actions are not allowed"));
  }

  auto promise = td::PromiseCreator::lambda(
      [actor_id = actor_id(this)](td::Result<ClientManager::StatsResponse> result) {
        send_closure(actor_id, &HttpStatConnection::on_result, std::move(result));
      });
  send_closure(client_manager_, &ClientManager::get_stats, std::move(promise), http_query->get_args(),
               http_query->type_ == td::HttpQuery::Type::Post);
}

void HttpStatConnection::on_result(td::Result<ClientManager::StatsResponse> result) {
  ClientManager::StatsResponse response;
  if (result.is_error()) {
    auto error = result.move_as_error();
    response.http_status_code = error.code() >= 400 && error.code() <= 599 ? error.code() : 500;
    response.content = td::json_encode<td::BufferSlice>(JsonQueryError(response.http_status_code, error.message()));
  } else {
    response = result.move_as_ok();
  }

  td::HttpHeaderCreator hc;
  hc.init_status_line(response.http_status_code);
  hc.set_keep_alive();
  hc.set_content_type(response.content_type);
  if (response.http_status_code == 405) {
    hc.add_header("Allow", "GET, POST");
  }
  hc.add_header("Cache-Control", "no-store, max-age=0");
  hc.add_header("Pragma", "no-cache");
  hc.add_header("Referrer-Policy", "no-referrer");
  hc.add_header("X-Content-Type-Options", "nosniff");
  hc.set_content_size(response.content.size());

  auto r_header = hc.finish();
  if (r_header.is_error()) {
    send_closure(connection_.release(), &td::HttpInboundConnection::write_error, r_header.move_as_error());
    return;
  }
  send_closure(connection_, &td::HttpInboundConnection::write_next_noflush, td::BufferSlice(r_header.ok()));
  send_closure(connection_, &td::HttpInboundConnection::write_next_noflush, std::move(response.content));
  send_closure(connection_.release(), &td::HttpInboundConnection::write_ok);
}

}  // namespace telegram_bot_api
