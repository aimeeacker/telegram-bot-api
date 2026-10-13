//
// Copyright Aliaksei Levin (levlam@telegram.org), Arseny Smirnov (arseny30@gmail.com) 2014-2026
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
#include "telegram-bot-api/ClientManager.h"

#include "telegram-bot-api/ClientParameters.h"
#include "telegram-bot-api/WebhookActor.h"
#include "stats_html.h"

#include "td/telegram/ClientActor.h"
#include "td/telegram/td_api.h"

#include "td/db/binlog/Binlog.h"
#include "td/db/binlog/ConcurrentBinlog.h"
#include "td/db/BinlogKeyValue.h"
#include "td/db/DbKey.h"
#include "td/db/TQueue.h"

#include "td/net/HttpFile.h"

#include "td/actor/MultiPromise.h"

#include "td/utils/common.h"
#include "td/utils/format.h"
#include "td/utils/logging.h"
#include "td/utils/misc.h"
#include "td/utils/Parser.h"
#include "td/utils/port/IPAddress.h"
#include "td/utils/port/Stat.h"
#include "td/utils/port/thread.h"
#include "td/utils/Slice.h"
#include "td/utils/SliceBuilder.h"
#include "td/utils/StackAllocator.h"
#include "td/utils/StringBuilder.h"
#include "td/utils/Time.h"

#include "memprof/memprof.h"

#include <algorithm>
#include <atomic>
#include <tuple>

namespace telegram_bot_api {

void ClientManager::close(td::Promise<td::Unit> &&promise) {
  close_promises_.push_back(std::move(promise));
  if (close_flag_) {
    return;
  }

  close_flag_ = true;
  watchdog_id_.reset();
  dump_statistics();
  auto ids = clients_.ids();
  for (auto id : ids) {
    auto *client_info = clients_.get(id);
    CHECK(client_info);
    send_closure(client_info->client_, &Client::close);
  }
  if (ids.empty()) {
    close_db();
  }
}

void ClientManager::send(PromisedQueryPtr query) {
  if (close_flag_) {
    // automatically send 429
    return;
  }

  td::string token = query->token().str();
  if (token[0] == '0' || token.size() > 80u || token.find('/') != td::string::npos ||
      token.find(':') == td::string::npos) {
    return fail_query(401, "Unauthorized: invalid token specified", std::move(query));
  }
  auto r_user_id = td::to_integer_safe<td::int64>(query->token().substr(0, token.find(':')));
  if (r_user_id.is_error() || !token_range_(r_user_id.ok())) {
    return fail_query(421, "Misdirected Request: forbidden token specified", std::move(query));
  }
  auto user_id = r_user_id.ok();
  if (user_id <= 0 || user_id >= (static_cast<td::int64>(1) << 54)) {
    return fail_query(401, "Unauthorized: invalid token specified", std::move(query));
  }

  if (query->is_test_dc()) {
    token += "/test";
  }

  auto id_it = token_to_id_.find(token);
  if (id_it == token_to_id_.end()) {
    auto method = query->method();
    if (method == "close") {
      return fail_query(400, "Bad Request: the bot has already been closed", std::move(query));
    }

    td::string ip_address = query->get_peer_ip_address();
    if (!ip_address.empty()) {
      td::IPAddress tmp;
      tmp.init_host_port(ip_address, 0).ignore();
      tmp.clear_ipv6_interface();
      if (tmp.is_valid()) {
        ip_address = tmp.get_ip_str().str();
      }
    }
    LOG(DEBUG) << "Receive incoming query for new bot " << token << " from " << ip_address;
    if (!ip_address.empty()) {
      LOG(DEBUG) << "Check Client creation flood control for IP address " << ip_address;
      auto res = flood_controls_.emplace(std::move(ip_address), td::FloodControlFast());
      auto &flood_control = res.first->second;
      if (res.second) {
        flood_control.add_limit(60, 20);        // 20 in a minute
        flood_control.add_limit(60 * 60, 600);  // 600 in an hour
      }
      auto now = td::Time::now();
      auto wakeup_at = flood_control.get_wakeup_at();
      if (wakeup_at > now) {
        LOG(INFO) << "Failed to create Client from IP address " << ip_address << " with token";
        return query->set_retry_after_error(static_cast<int>(wakeup_at - now) + 1);
      }
      flood_control.add_event(now);
    }
    if (is_global_flood_control_enabled_) {
      auto now = td::Time::now();
      auto wakeup_at = global_flood_control_.get_wakeup_at();
      if (wakeup_at > now) {
        LOG(WARNING) << "Failed to create Client with token " << token;
        return query->set_retry_after_error(static_cast<int>(wakeup_at - now) + 1);
      }
      global_flood_control_.add_event(now);
    }
    auto tqueue_id = get_tqueue_id(user_id, query->is_test_dc());
    if (active_client_count_.count(tqueue_id) != 0) {
      // return query->set_retry_after_error(1);
    }

    auto id =
        clients_.create(ClientInfo{BotStatActor(stat_.actor_id(&stat_)), token, tqueue_id, td::ActorOwn<Client>()});
    auto *client_info = clients_.get(id);
    client_info->client_ = td::create_actor<Client>(PSLICE() << "Client/" << token, actor_shared(this, id),
                                                    query->token().str(), query->is_test_dc(), tqueue_id, parameters_,
                                                    client_info->stat_.actor_id(&client_info->stat_));

    if (method != "deletewebhook" && method != "setwebhook") {
      auto bot_token_with_dc = PSTRING() << query->token() << (query->is_test_dc() ? ":T" : "");
      auto webhook_info = parameters_->shared_data_->webhook_db_->get(bot_token_with_dc);
      if (!webhook_info.empty()) {
        send_closure(client_info->client_, &Client::send,
                     get_webhook_restore_query(bot_token_with_dc, webhook_info, parameters_->shared_data_));
      }
    }

    std::tie(id_it, std::ignore) = token_to_id_.emplace(token, id);
  }
  send_closure(clients_.get(id_it->second)->client_, &Client::send,
               std::move(query));  // will send 429 if the client is already closed
}

ClientManager::TopClients ClientManager::get_top_clients(std::size_t max_count, td::Slice token_filter) {
  auto now = td::Time::now();
  TopClients result;
  td::vector<std::pair<td::int64, td::uint64>> top_client_ids;
  for (auto id : clients_.ids()) {
    auto *client_info = clients_.get(id);
    CHECK(client_info);

    if (client_info->stat_.is_active(now)) {
      result.active_count++;
    }

    if (!td::begins_with(client_info->token_, token_filter)) {
      continue;
    }

    auto score = static_cast<td::int64>(client_info->stat_.get_score(now) * -1e9);
    if (score == 0 && top_client_ids.size() >= max_count) {
      continue;
    }
    top_client_ids.emplace_back(score, id);
  }
  if (top_client_ids.size() < max_count) {
    max_count = top_client_ids.size();
  }
  std::partial_sort(top_client_ids.begin(), top_client_ids.begin() + max_count, top_client_ids.end());
  result.top_client_ids.reserve(max_count);
  for (std::size_t i = 0; i < max_count; i++) {
    result.top_client_ids.push_back(top_client_ids[i].second);
  }
  return result;
}

static ClientManager::StatsResponse make_stats_action_response(td::Slice message) {
  return {td::json_encode<td::BufferSlice>(JsonQueryOk<td::JsonTrue>(td::JsonTrue(), message))};
}

void ClientManager::get_stats(td::Promise<StatsResponse> promise,
                              td::vector<std::pair<td::string, td::string>> args, bool allow_actions) {
  if (close_flag_) {
    promise.set_error(td::Status::Error(503, "Server is closing"));
    return;
  }

  td::Slice id_filter;
  int new_verbosity_level = -1;
  td::string tag;
  td::string format;
  td::string action;
  td::string client_id_str;
  td::string webhook_url;
  td::string secret_token;
  td::string max_connections;
  td::string allowed_updates;
  bool drop_pending_updates = false;

  for (auto &arg : args) {
    if (arg.first == "id") {
      id_filter = arg.second;
    } else if (arg.first == "v") {
      auto r_new_verbosity_level = td::to_integer_safe<int>(arg.second);
      if (r_new_verbosity_level.is_ok()) {
        new_verbosity_level = r_new_verbosity_level.ok();
      }
    } else if (arg.first == "tag") {
      tag = arg.second;
    } else if (arg.first == "format") {
      format = arg.second;
    } else if (arg.first == "text" || arg.first == "tsv") {
      format = "text";
    } else if (arg.first == "json") {
      format = "json";
    } else if (arg.first == "action") {
      action = arg.second;
    } else if (arg.first == "client") {
      client_id_str = arg.second;
    } else if (arg.first == "url") {
      webhook_url = arg.second;
    } else if (arg.first == "secret_token") {
      secret_token = arg.second;
    } else if (arg.first == "max_connections") {
      max_connections = arg.second;
    } else if (arg.first == "allowed_updates") {
      allowed_updates = arg.second;
    } else if (arg.first == "drop_pending_updates") {
      drop_pending_updates = arg.second == "1" || arg.second == "true" || arg.second == "on";
    }
  }

  if (!action.empty()) {
    if (!allow_actions) {
      promise.set_error(td::Status::Error(405, "Stats actions require HTTP POST"));
      return;
    }
    if (action != "logout_all" && action != "logout" && action != "set_webhook" && action != "delete_webhook") {
      promise.set_error(td::Status::Error(400, "Unknown stats action"));
      return;
    }
    if (action == "logout_all") {
      for (auto id : clients_.ids()) {
        auto *client_info = clients_.get(id);
        if (client_info != nullptr) {
          send_closure(client_info->client_, &Client::log_out_from_stats);
        }
      }
      promise.set_value(make_stats_action_response("Logout requested for all bot sessions"));
      return;
    }

    auto r_client_id = td::to_integer_safe<td::uint64>(client_id_str);
    auto *client_info = r_client_id.is_ok() ? clients_.get(r_client_id.ok()) : nullptr;
    if (client_info == nullptr) {
      promise.set_error(td::Status::Error(404, "Bot session not found; refresh the dashboard"));
      return;
    }

    if (action == "logout") {
      send_closure(client_info->client_, &Client::log_out_from_stats);
      promise.set_value(make_stats_action_response("Bot logout requested"));
      return;
    }

    if (action == "set_webhook" || action == "delete_webhook") {
      td::vector<std::pair<td::string, td::string>> query_args;
      if (action == "set_webhook") {
        if (webhook_url.empty()) {
          promise.set_error(td::Status::Error(400, "Webhook URL is required"));
          return;
        }
        query_args.emplace_back("url", webhook_url);
        if (!secret_token.empty()) {
          query_args.emplace_back("secret_token", secret_token);
        }
        if (!max_connections.empty()) {
          query_args.emplace_back("max_connections", max_connections);
        }
        if (!allowed_updates.empty()) {
          auto r_updates = td::json_decode(td::MutableSlice(allowed_updates));
          if (r_updates.is_error() || r_updates.ok().type() != td::JsonValue::Type::Array) {
            promise.set_error(td::Status::Error(400, "Allowed updates must be a JSON array of strings"));
            return;
          }
          for (auto &update : r_updates.ok().get_array()) {
            if (update.type() != td::JsonValue::Type::String) {
              promise.set_error(td::Status::Error(400, "Allowed updates must be a JSON array of strings"));
              return;
            }
          }
          query_args.emplace_back("allowed_updates", td::json_encode<td::string>(r_updates.ok()));
        }
      }
      if (drop_pending_updates) {
        query_args.emplace_back("drop_pending_updates", "true");
      }
      auto method = action == "set_webhook" ? td::Slice("setWebhook") : td::Slice("deleteWebhook");
      auto query_promise = td::PromiseCreator::lambda(
          [promise = std::move(promise)](td::Result<td::unique_ptr<Query>> result) mutable {
            if (result.is_error()) {
              promise.set_error(result.move_as_error());
              return;
            }
            auto query = result.move_as_ok();
            promise.set_value(StatsResponse{std::move(query->answer()), "application/json; charset=utf-8",
                                            query->http_status_code()});
          });
      td::Slice token = client_info->token_;
      bool is_test_dc = td::ends_with(token, "/test");
      if (is_test_dc) {
        token.remove_suffix(5);
      }
      send_closure(client_info->client_, &Client::send,
                   make_internal_query(token, method, query_args, parameters_->shared_data_, td::IPAddress(),
                                       std::move(query_promise), is_test_dc));
      return;
    }
  }

  if (new_verbosity_level > 0) {
    if (tag.empty()) {
      parameters_->shared_data_->next_verbosity_level_ = new_verbosity_level;
    } else {
      td::ClientActor::execute(td::td_api::make_object<td::td_api::setLogTagVerbosityLevel>(tag, new_verbosity_level));
    }
  }

  if (format.empty()) {
    promise.set_value(StatsResponse{td::BufferSlice(STATS_HTML), "text/html; charset=utf-8"});
    return;
  }

  if (format != "json" && format != "text" && format != "tsv") {
    promise.set_error(td::Status::Error(400, "Unsupported stats format"));
    return;
  }
  auto now = td::Time::now();
  auto top_clients = get_top_clients(format == "json" ? clients_.size() : 50, id_filter);

  if (format == "json") {
    auto content = td::json_encode<td::BufferSlice>(td::json_object([&](auto &object) {
      object("version", parameters_->version_);
      object("uptime", static_cast<td::int64>(now - parameters_->start_time_));
      object("bot_count", static_cast<td::int64>(clients_.size()));
      object("active_bot_count", top_clients.active_count);
      object("active_requests", static_cast<td::int64>(
                                   parameters_->shared_data_->query_count_.load(std::memory_order_relaxed)));
      object("active_webhook_connections", static_cast<td::int64>(WebhookActor::get_total_connection_count()));
      object("active_network_queries",
             static_cast<td::int64>(td::get_pending_network_query_count(*parameters_->net_query_stats_)));
      object("default_max_webhook_connections", parameters_->default_max_webhook_connections_);
      object("max_webhook_connections", parameters_->local_mode_ ? 100000 : 100);
      object("mcp", td::json_object([&](auto &mcp) {
        mcp("configured", td::JsonBool(!parameters_->shared_data_->mcp_bot_token_.empty() &&
                                     !parameters_->shared_data_->mcp_chat_id_.empty()));
        mcp("has_bearer", td::JsonBool(!parameters_->shared_data_->mcp_bearer_token_.empty()));
      }));
      auto r_mem_stat = td::mem_stat();
      if (r_mem_stat.is_ok()) {
        auto mem = r_mem_stat.move_as_ok();
        object("memory", td::json_object([&](auto &memory) {
          memory("rss", PSTRING() << td::format::as_size(mem.resident_size_));
          memory("vm", PSTRING() << td::format::as_size(mem.virtual_size_));
          memory("rss_peak", PSTRING() << td::format::as_size(mem.resident_size_peak_));
          memory("vm_peak", PSTRING() << td::format::as_size(mem.virtual_size_peak_));
        }));
      }
      object("bots", td::json_array([&](auto &bots) {
        for (auto top_client_id : top_clients.top_client_ids) {
          auto *client_info = clients_.get(top_client_id);
          CHECK(client_info);
          auto bot_info = client_info->client_.get_actor_unsafe()->get_bot_info();
          td::string update_rate = "0.00";
          td::string request_rate = "0.00";
          for (auto &stat : client_info->stat_.as_vector(now)) {
            if (stat.key_ == "update_count") {
              update_rate = stat.value_;
            } else if (stat.key_ == "request_count") {
              request_rate = stat.value_;
            }
          }
          bots(td::json_object([&](auto &bot) {
            bot("id", bot_info.id_);
            bot("client_id", td::to_string(top_client_id));
            bot("username", bot_info.username_);
            bot("uptime", static_cast<td::int64>(now - bot_info.start_time_));
            bot("webhook", bot_info.webhook_);
            bot("webhook_max_connections", bot_info.webhook_max_connections_);
            bot("pending_updates", static_cast<td::int64>(bot_info.pending_update_count_));
            bot("active_requests", client_info->stat_.get_active_request_count());
            bot("update_rate", update_rate);
            bot("request_rate", request_rate);
          }));
        }
      }));
      object("diagnostics", td::json_array([&](auto &diagnostics) {
        auto add_diag = [&](td::Slice name, td::Slice value) {
          diagnostics(td::json_object([&](auto &diagnostic) {
            diagnostic("name", name);
            diagnostic("value", value);
          }));
        };
        for (auto &stat : ServerCpuStat::instance().as_vector(now)) {
          add_diag(stat.key_, stat.value_);
        }
        add_diag("buffer_memory", PSTRING() << td::format::as_size(td::BufferAllocator::get_buffer_mem()));
        for (auto &stat : stat_.as_vector(now)) {
          add_diag(stat.key_, stat.value_);
        }
      }));
    }));
    promise.set_value(StatsResponse{std::move(content)});
    return;
  }

  size_t buf_size = 1 << 14;
  auto buf = td::StackAllocator::alloc(buf_size);
  td::StringBuilder sb(buf.as_slice());

  sb << BotStatActor::get_description() << '\n';
  if (id_filter.empty()) {
    sb << "uptime\t" << now - parameters_->start_time_ << '\n';
    sb << "bot_count\t" << clients_.size() << '\n';
    sb << "active_bot_count\t" << top_clients.active_count << '\n';
    auto r_mem_stat = td::mem_stat();
    if (r_mem_stat.is_ok()) {
      auto mem_stat = r_mem_stat.move_as_ok();
      sb << "rss\t" << td::format::as_size(mem_stat.resident_size_) << '\n';
      sb << "vm\t" << td::format::as_size(mem_stat.virtual_size_) << '\n';
      sb << "rss_peak\t" << td::format::as_size(mem_stat.resident_size_peak_) << '\n';
      sb << "vm_peak\t" << td::format::as_size(mem_stat.virtual_size_peak_) << '\n';
    } else {
      LOG(INFO) << "Failed to get memory statistics: " << r_mem_stat.error();
    }

    auto cpu_stats = ServerCpuStat::instance().as_vector(td::Time::now());
    for (auto &stat : cpu_stats) {
      sb << stat.key_ << "\t" << stat.value_ << '\n';
    }

    sb << "buffer_memory\t" << td::format::as_size(td::BufferAllocator::get_buffer_mem()) << '\n';
    sb << "active_webhook_connections\t" << WebhookActor::get_total_connection_count() << '\n';
    sb << "active_requests\t" << parameters_->shared_data_->query_count_.load(std::memory_order_relaxed) << '\n';
    sb << "active_network_queries\t" << td::get_pending_network_query_count(*parameters_->net_query_stats_) << '\n';
    auto stats = stat_.as_vector(now);
    for (auto &stat : stats) {
      sb << stat.key_ << "\t" << stat.value_ << '\n';
    }
  }

  for (auto top_client_id : top_clients.top_client_ids) {
    auto *client_info = clients_.get(top_client_id);
    CHECK(client_info);

    auto bot_info = client_info->client_.get_actor_unsafe()->get_bot_info();
    auto active_request_count = client_info->stat_.get_active_request_count();
    auto active_file_upload_bytes = client_info->stat_.get_active_file_upload_bytes();
    auto active_file_upload_count = client_info->stat_.get_active_file_upload_count();
    sb << '\n';
    sb << "id\t" << bot_info.id_ << '\n';
    sb << "uptime\t" << now - bot_info.start_time_ << '\n';
    sb << "username\t" << bot_info.username_ << '\n';
    if (active_request_count != 0) {
      sb << "active_request_count\t" << active_request_count << '\n';
    }
    if (active_file_upload_bytes != 0) {
      sb << "active_file_upload_bytes\t" << active_file_upload_bytes << '\n';
    }
    if (active_file_upload_count != 0) {
      sb << "active_file_upload_count\t" << active_file_upload_count << '\n';
    }
    if (!bot_info.webhook_.empty()) {
      sb << "webhook\t" << bot_info.webhook_ << '\n';
      if (bot_info.has_webhook_certificate_) {
        sb << "has_custom_certificate\t" << bot_info.has_webhook_certificate_ << '\n';
      }
      if (bot_info.webhook_max_connections_ != parameters_->default_max_webhook_connections_) {
        sb << "webhook_max_connections\t" << bot_info.webhook_max_connections_ << '\n';
      }
    }
    sb << "head_update_id\t" << bot_info.head_update_id_ << '\n';
    if (bot_info.pending_update_count_ != 0) {
      sb << "tail_update_id\t" << bot_info.tail_update_id_ << '\n';
      sb << "pending_update_count\t" << bot_info.pending_update_count_ << '\n';
    }

    auto stats = client_info->stat_.as_vector(now);
    for (auto &stat : stats) {
      if (stat.key_ == "update_count" || stat.key_ == "request_count") {
        sb << stat.key_ << "/sec\t" << stat.value_ << '\n';
      }
    }

    if (sb.is_error()) {
      break;
    }
  }
  // ignore sb overflow
  promise.set_value(StatsResponse{td::BufferSlice(sb.as_cslice()), "text/plain; charset=utf-8"});
}

td::int64 ClientManager::get_tqueue_id(td::int64 user_id, bool is_test_dc) {
  return user_id + (static_cast<td::int64>(is_test_dc) << 54);
}

void ClientManager::start_up() {
  // init tqueue
  {
    auto load_start_time = td::Time::now();
    auto tqueue_binlog = td::make_unique<td::TQueueBinlog<td::Binlog>>();
    auto binlog = td::make_unique<td::Binlog>();
    auto tqueue = td::TQueue::create();
    td::vector<td::uint64> failed_to_replay_log_event_ids;
    td::int64 loaded_event_count = 0;
    binlog
        ->init(parameters_->working_directory_ + "tqueue.binlog",
               [&](const td::BinlogEvent &event) {
                 if (tqueue_binlog->replay(event, *tqueue).is_error()) {
                   failed_to_replay_log_event_ids.push_back(event.id_);
                 } else {
                   loaded_event_count++;
                 }
               })
        .ensure();
    tqueue_binlog.reset();

    if (!failed_to_replay_log_event_ids.empty()) {
      LOG(ERROR) << "Failed to replay " << failed_to_replay_log_event_ids.size() << " TQueue events";
      for (auto &log_event_id : failed_to_replay_log_event_ids) {
        binlog->erase(log_event_id);
      }
    }

    auto concurrent_binlog =
        std::make_shared<td::ConcurrentBinlog>(std::move(binlog), SharedData::get_binlog_scheduler_id());
    auto concurrent_tqueue_binlog = td::make_unique<td::TQueueBinlog<td::BinlogInterface>>();
    concurrent_tqueue_binlog->set_binlog(std::move(concurrent_binlog));
    tqueue->set_callback(std::move(concurrent_tqueue_binlog));

    parameters_->shared_data_->tqueue_ = std::move(tqueue);

    LOG(WARNING) << "Loaded " << loaded_event_count << " TQueue events in " << (td::Time::now() - load_start_time)
                 << " seconds";
    next_tqueue_gc_time_ = td::Time::now() + 600;
  }

  // init webhook_db
  auto concurrent_webhook_db = td::make_unique<td::BinlogKeyValue<td::ConcurrentBinlog>>();
  auto status = concurrent_webhook_db->init(parameters_->working_directory_ + "webhooks_db.binlog", td::DbKey::empty(),
                                            SharedData::get_binlog_scheduler_id());
  LOG_IF(FATAL, status.is_error()) << "Can't open webhooks_db.binlog " << status;
  parameters_->shared_data_->webhook_db_ = std::move(concurrent_webhook_db);

  auto &webhook_db = *parameters_->shared_data_->webhook_db_;
  for (const auto &key_value : webhook_db.get_all()) {
    if (!token_range_(td::to_integer<td::uint64>(key_value.first))) {
      LOG(WARNING) << "DROP WEBHOOK: " << key_value.first << " ---> " << key_value.second;
      webhook_db.erase(key_value.first);
      continue;
    }

    auto query = get_webhook_restore_query(key_value.first, key_value.second, parameters_->shared_data_);
    send_closure_later(actor_id(this), &ClientManager::send, std::move(query));
  }

  // launch watchdog
  watchdog_id_ = td::create_actor_on_scheduler<Watchdog>("ManagerWatchdog", SharedData::get_watchdog_scheduler_id(),
                                                         td::this_thread::get_id(), WATCHDOG_TIMEOUT);
  set_timeout_in(600.0);
}

PromisedQueryPtr ClientManager::get_webhook_restore_query(td::Slice token, td::Slice webhook_info,
                                                          std::shared_ptr<SharedData> shared_data) {
  LOG(WARNING) << "WEBHOOK: " << token << " ---> " << webhook_info;

  bool is_test_dc = false;
  if (td::ends_with(token, ":T")) {
    token.remove_suffix(2);
    is_test_dc = true;
  }

  td::ConstParser parser{webhook_info};
  td::vector<std::pair<td::string, td::string>> args;
  if (parser.try_skip("cert/")) {
    args.emplace_back("certificate", "previous");
  }

  if (parser.try_skip("#maxc")) {
    args.emplace_back("max_connections", parser.read_till('/').str());
    parser.skip('/');
  }

  if (parser.try_skip("#ip")) {
    args.emplace_back("ip_address", parser.read_till('/').str());
    parser.skip('/');
  }

  if (parser.try_skip("#fix_ip")) {
    args.emplace_back("fix_ip_address", "1");
    parser.skip('/');
  }

  if (parser.try_skip("#secret")) {
    args.emplace_back("secret_token", parser.read_till('/').str());
    parser.skip('/');
  }

  if (parser.try_skip("#allow")) {
    args.emplace_back("allowed_updates", parser.read_till('/').str());
    parser.skip('/');
  }

  args.emplace_back("url", parser.read_all().str());
  return make_internal_query(token, "setWebhook", args, std::move(shared_data), td::IPAddress(), {}, is_test_dc, true);
}

void ClientManager::dump_statistics() {
  if (is_memprof_on()) {
    LOG(WARNING) << "Memory dump:";
    td::vector<AllocInfo> v;
    dump_alloc([&](const AllocInfo &info) { v.push_back(info); });
    std::sort(v.begin(), v.end(), [](const AllocInfo &a, const AllocInfo &b) { return a.size > b.size; });
    size_t total_size = 0;
    size_t other_size = 0;
    int count = 0;
    for (auto &info : v) {
      if (count++ < 50) {
        LOG(WARNING) << td::format::as_size(info.size) << td::format::as_array(info.backtrace);
      } else {
        other_size += info.size;
      }
      total_size += info.size;
    }
    LOG(WARNING) << td::tag("other", td::format::as_size(other_size));
    LOG(WARNING) << td::tag("total size", td::format::as_size(total_size));
    LOG(WARNING) << td::tag("total traces", get_ht_size());
    LOG(WARNING) << td::tag("fast_backtrace_success_rate", get_fast_backtrace_success_rate());
  }
  auto r_mem_stat = td::mem_stat();
  if (r_mem_stat.is_ok()) {
    auto mem_stat = r_mem_stat.move_as_ok();
    LOG(WARNING) << td::tag("rss", td::format::as_size(mem_stat.resident_size_));
    LOG(WARNING) << td::tag("vm", td::format::as_size(mem_stat.virtual_size_));
    LOG(WARNING) << td::tag("rss_peak", td::format::as_size(mem_stat.resident_size_peak_));
    LOG(WARNING) << td::tag("vm_peak", td::format::as_size(mem_stat.virtual_size_peak_));
  }
  LOG(WARNING) << td::tag("buffer_mem", td::format::as_size(td::BufferAllocator::get_buffer_mem()));
  LOG(WARNING) << td::tag("buffer_slice_size", td::format::as_size(td::BufferAllocator::get_buffer_slice_size()));

  const auto &shared_data = parameters_->shared_data_;
  auto query_list_size = shared_data->query_list_size_.load(std::memory_order_relaxed);
  auto query_count = shared_data->query_count_.load(std::memory_order_relaxed);
  LOG(WARNING) << td::tag("pending queries", query_count) << td::tag("pending requests", query_list_size);

  td::uint64 i = 0;
  bool was_gap = false;
  for (auto end = &shared_data->query_list_, cur = end->prev; cur != end; cur = cur->prev, i++) {
    if (i < 20 || i > query_list_size - 20 || i % (query_list_size / 50 + 1) == 0) {
      if (was_gap) {
        LOG(WARNING) << "...";
        was_gap = false;
      }
      LOG(WARNING) << static_cast<const Query &>(*cur);
    } else {
      was_gap = true;
    }
  }

  td::dump_pending_network_queries(*parameters_->net_query_stats_);

  auto now = td::Time::now();
  auto top_clients = get_top_clients(10, {});
  for (auto top_client_id : top_clients.top_client_ids) {
    auto *client_info = clients_.get(top_client_id);
    CHECK(client_info);

    auto bot_info = client_info->client_.get_actor_unsafe()->get_bot_info();
    td::string update_count;
    td::string request_count;
    auto replace_tabs = [](td::string &str) {
      for (auto &c : str) {
        if (c == '\t') {
          c = ' ';
        }
      }
    };
    auto stats = client_info->stat_.as_vector(now);
    for (auto &stat : stats) {
      if (stat.key_ == "update_count") {
        replace_tabs(stat.value_);
        update_count = std::move(stat.value_);
      }
      if (stat.key_ == "request_count") {
        replace_tabs(stat.value_);
        request_count = std::move(stat.value_);
      }
    }
    LOG(WARNING) << td::tag("id", bot_info.id_) << td::tag("update_count", update_count)
                 << td::tag("request_count", request_count);
  }
}

void ClientManager::raw_event(const td::Event::Raw &event) {
  auto id = get_link_token();
  auto *info = clients_.get(id);
  CHECK(info != nullptr);
  CHECK(info->tqueue_id_ != 0);
  auto &value = active_client_count_[info->tqueue_id_];
  if (event.ptr != nullptr) {
    value++;
  } else {
    CHECK(value > 0);
    if (--value == 0) {
      active_client_count_.erase(info->tqueue_id_);
    }
  }
}

void ClientManager::timeout_expired() {
  send_closure(watchdog_id_, &Watchdog::kick);
  set_timeout_in(WATCHDOG_TIMEOUT / 10);

  double now = td::Time::now();
  if (now > next_tqueue_gc_time_) {
    auto unix_time = parameters_->shared_data_->get_unix_time(now);
    LOG(INFO) << "Run TQueue GC at " << unix_time;
    td::int64 deleted_events;
    bool is_finished;
    std::tie(deleted_events, is_finished) = parameters_->shared_data_->tqueue_->run_gc(unix_time);
    LOG(INFO) << "TQueue GC deleted " << deleted_events << " events";
    next_tqueue_gc_time_ = td::Time::now() + (is_finished ? 60.0 : 1.0);

    tqueue_deleted_events_ += deleted_events;
    if (tqueue_deleted_events_ > last_tqueue_deleted_events_ + 10000) {
      LOG(WARNING) << "TQueue GC already deleted " << tqueue_deleted_events_ << " events since the start";
      last_tqueue_deleted_events_ = tqueue_deleted_events_;
    }
  }

  if (!is_global_flood_control_enabled_ && !parameters_->local_mode_) {
    is_global_flood_control_enabled_ = true;
    global_flood_control_.add_limit(60, 1000);        // 1000 in a minute
    global_flood_control_.add_limit(60 * 60, 10000);  // 10000 in an hour
  }
}

void ClientManager::hangup_shared() {
  auto id = get_link_token();
  auto *info = clients_.get(id);
  CHECK(info != nullptr);
  info->client_.release();
  token_to_id_.erase(info->token_);
  clients_.erase(id);

  if (close_flag_ && clients_.empty()) {
    CHECK(active_client_count_.empty());
    close_db();
  }
}

void ClientManager::close_db() {
  LOG(WARNING) << "Closing databases";
  td::MultiPromiseActorSafe mpas("close binlogs");
  mpas.add_promise(td::PromiseCreator::lambda(
      [actor_id = actor_id(this)](td::Unit) { send_closure(actor_id, &ClientManager::finish_close); }));
  mpas.set_ignore_errors(true);

  auto lock = mpas.get_promise();
  parameters_->shared_data_->tqueue_->close(mpas.get_promise());
  parameters_->shared_data_->webhook_db_->close(mpas.get_promise());
  lock.set_value(td::Unit());
}

void ClientManager::finish_close() {
  LOG(WARNING) << "Stop ClientManager";
  auto promises = std::move(close_promises_);
  for (auto &promise : promises) {
    promise.set_value(td::Unit());
  }
  stop();
}

}  // namespace telegram_bot_api
