//
// Copyright Aliaksei Levin (levlam@telegram.org), Arseny Smirnov (arseny30@gmail.com) 2014-2026
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
#include "telegram-bot-api/HttpConnection.h"

#include "telegram-bot-api/ClientParameters.h"
#include "telegram-bot-api/Query.h"

#include "td/net/HttpHeaderCreator.h"

#include "td/utils/common.h"
#include "td/utils/base64.h"
#include "td/utils/HttpUrl.h"
#include "td/utils/JsonBuilder.h"
#include "td/utils/logging.h"
#include "td/utils/misc.h"
#include "td/utils/Parser.h"
#include "td/utils/port/IPAddress.h"
#include "td/utils/Promise.h"
#include "td/utils/SliceBuilder.h"

namespace telegram_bot_api {
namespace {

constexpr td::Slice MCP_PROTOCOL_VERSION = "2026-07-28";
constexpr td::Slice MCP_LEGACY_PROTOCOL_VERSION = "2025-11-25";
constexpr td::Slice MCP_SERVER_NAME = "telegram-bot-api";
constexpr td::Slice MCP_SERVER_VERSION = "0.1";

td::string json_quote(td::Slice value) {
  return td::json_encode<td::string>(td::JsonString(value));
}

td::Result<td::string> get_mcp_string(td::JsonObject &object, td::Slice name) {
  auto value = object.extract_field(name);
  if (value.type() != td::JsonValue::Type::String) {
    return td::Status::Error(PSLICE() << name << " must be a string");
  }
  return value.get_string().str();
}

td::string make_mcp_result(td::Slice id_json, td::Slice result_json) {
  return PSTRING() << "{\"jsonrpc\":\"2.0\",\"id\":" << id_json << ",\"result\":" << result_json << "}";
}

td::string make_mcp_error(td::Slice id_json, int error_code, td::Slice message, td::Slice data_json) {
  return PSTRING() << "{\"jsonrpc\":\"2.0\",\"id\":" << id_json << ",\"error\":{\"code\":" << error_code
                   << ",\"message\":" << json_quote(message)
                   << (data_json.empty() ? td::string() : PSTRING() << ",\"data\":" << data_json) << "}}";
}

bool is_json_content_type(td::Slice content_type) {
  auto semicolon_pos = content_type.find(';');
  if (semicolon_pos != td::Slice::npos) {
    content_type = content_type.substr(0, semicolon_pos);
  }
  return td::to_lower(td::trim(content_type)) == "application/json";
}

bool mcp_name_matches(td::Slice header, td::Slice name) {
  if (header.size() >= 11 && td::begins_with(header, "=?base64?") && td::ends_with(header, "?=")) {
    auto decoded = td::base64_decode(header.substr(9, header.size() - 11));
    return decoded.is_ok() && decoded.ok() == name;
  }
  return header == name;
}

}  // namespace

void HttpConnection::handle(td::unique_ptr<td::HttpQuery> http_query,
                            td::ActorOwn<td::HttpInboundConnection> connection) {
  CHECK(connection_.empty());
  connection_ = std::move(connection);
  is_mcp_request_ = http_query->url_path_ == "/mcp";
  mcp_request_id_json_ = "null";
  mcp_protocol_version_ = MCP_LEGACY_PROTOCOL_VERSION.str();

  LOG(DEBUG) << "Handle " << *http_query;
  if (is_mcp_request_) {
    return handle_mcp(std::move(http_query));
  }

  if (http_query->url_path_ == "/stats" || http_query->url_path_ == "/stats/") {
    return handle_stats(std::move(http_query));
  }

  if (http_query->url_path_ == "/" || http_query->url_path_.empty()) {
    return send_redirect(302, "/stats");
  }

  td::Parser url_path_parser(http_query->url_path_);
  if (url_path_parser.peek_char() != '/') {
    return send_http_error(404, "Not Found: absolute URI is specified in the Request-Line");
  }

  if (!url_path_parser.try_skip("/bot")) {
    return send_http_error(404, "Not Found");
  }

  td::Slice token = url_path_parser.read_till('/');
  bool is_test_dc = false;
  if (url_path_parser.try_skip("/test")) {
    is_test_dc = true;
  }
  url_path_parser.skip('/');
  if (url_path_parser.status().is_error()) {
    return send_http_error(404, "Not Found");
  }

  if (token.find(':') == td::Slice::npos) {
    auto preset = shared_data_->find_preset_bot(token);
    if (preset == nullptr) {
      return send_http_error(401, "Unauthorized");
    }
    token = *preset;
  }

  auto method = url_path_parser.data();
  auto query = td::make_unique<Query>(std::move(http_query->container_), token, is_test_dc, method,
                                      std::move(http_query->args_), std::move(http_query->headers_),
                                      std::move(http_query->files_), shared_data_, http_query->peer_address_, false);

  auto promised_query = PromisedQueryPtr(query.release(), PromiseDeleter(make_query_promise()));
  send_closure(client_manager_, &ClientManager::send, std::move(promised_query));
}

void HttpConnection::handle_mcp(td::unique_ptr<td::HttpQuery> http_query) {
  if (http_query->type_ != td::HttpQuery::Type::Post) {
    return send_mcp_error(405, -32600, "MCP endpoint requires HTTP POST");
  }
  if (!is_json_content_type(http_query->get_header("content-type"))) {
    return send_mcp_error(415, -32600, "Content-Type must be application/json");
  }

  if (!shared_data_->mcp_bearer_token_.empty()) {
    auto expected = PSTRING() << "Bearer " << shared_data_->mcp_bearer_token_;
    if (http_query->get_header("authorization") != expected) {
      return send_mcp_error(401, -32000, "Unauthorized");
    }
  }

  td::string method;
  td::JsonValue params = td::JsonValue::make_object(td::JsonObject());

  if (http_query->content_.empty()) {
    return send_mcp_error(400, -32700, "Parse error: empty request body");
  }

  // The HTTP reader preserves /mcp JSON bodies; decode once and reject any trailing data.
  auto r_json = td::json_decode(http_query->content_);
  if (r_json.is_error()) {
    return send_mcp_error(400, -32700, PSLICE() << "Parse error: " << r_json.error().message());
  }
  auto json = r_json.move_as_ok();
  if (json.type() != td::JsonValue::Type::Object) {
    return send_mcp_error(400, -32600, "JSON-RPC request must be an object");
  }
  auto &object = json.get_object();

  auto r_jsonrpc = get_mcp_string(object, "jsonrpc");
  if (r_jsonrpc.is_error() || r_jsonrpc.ok() != "2.0") {
    return send_mcp_error(400, -32600, "jsonrpc must be \"2.0\"");
  }

  if (object.has_field("id")) {
    auto id = object.extract_field("id");
    if (id.type() != td::JsonValue::Type::String && id.type() != td::JsonValue::Type::Number) {
      return send_mcp_error(400, -32600, "JSON-RPC id must be a string or number");
    }
    mcp_request_id_json_ = td::json_encode<td::string>(id);
  }

  auto r_method = get_mcp_string(object, "method");
  if (r_method.is_error()) {
    return send_mcp_error(400, -32600, "Missing JSON-RPC method", mcp_request_id_json_);
  }
  method = r_method.move_as_ok();

  if (object.has_field("params")) {
    params = object.extract_field("params");
    if (params.type() != td::JsonValue::Type::Object) {
      return send_mcp_error(400, -32602, "params must be an object", mcp_request_id_json_);
    }
  }

  auto &params_object = params.get_object();
  auto protocol_version = http_query->get_header("mcp-protocol-version");
  if (!protocol_version.empty()) {
    if (protocol_version != MCP_PROTOCOL_VERSION && protocol_version != MCP_LEGACY_PROTOCOL_VERSION) {
      auto data = PSTRING() << "{\"supported\":[\"" << MCP_PROTOCOL_VERSION << "\",\""
                            << MCP_LEGACY_PROTOCOL_VERSION
                            << "\"],\"requested\":" << json_quote(protocol_version) << "}";
      return send_mcp_error(400, -32022, "Unsupported protocol version", mcp_request_id_json_, data);
    }
    mcp_protocol_version_ = protocol_version.str();
  }

  auto mcp_method = http_query->get_header("mcp-method");
  if (!mcp_method.empty() && mcp_method != method) {
    return send_mcp_error(400, -32020, "MCP headers must match the request metadata and method",
                          mcp_request_id_json_);
  }

  // Older clients may omit metadata and mirrored headers; validate them when supplied.
  if (params_object.has_field("_meta")) {
    auto meta = params_object.extract_field("_meta");
    if (meta.type() != td::JsonValue::Type::Object) {
      return send_mcp_error(400, -32602, "params._meta must be an object", mcp_request_id_json_);
    }
    auto &meta_object = meta.get_object();
    bool has_meta_version = meta_object.has_field("io.modelcontextprotocol/protocolVersion");
    bool has_meta_capabilities = meta_object.has_field("io.modelcontextprotocol/clientCapabilities");
    if (has_meta_version || has_meta_capabilities) {
      if (has_meta_capabilities) {
        auto capabilities = meta_object.extract_field("io.modelcontextprotocol/clientCapabilities");
        if (capabilities.type() != td::JsonValue::Type::Object) {
          return send_mcp_error(400, -32602, "Modern MCP requests require clientCapabilities", mcp_request_id_json_);
        }
      }
      if (has_meta_version) {
        auto r_version = get_mcp_string(meta_object, "io.modelcontextprotocol/protocolVersion");
        if (r_version.is_error() || r_version.ok() != MCP_PROTOCOL_VERSION ||
            (!protocol_version.empty() && protocol_version != r_version.ok())) {
          return send_mcp_error(400, -32020, "MCP headers must match the request metadata and method",
                                mcp_request_id_json_);
        }
      }
    }
  }
  if (mcp_request_id_json_ == "null") {
    // Notifications do not invoke tools or produce a JSON-RPC response.
    return send_response(202, td::BufferSlice(), 0);
  }

  if (method == "server/discover") {
    auto result = PSTRING()
                  << "{\"resultType\":\"complete\",\"supportedVersions\":[\"" << MCP_PROTOCOL_VERSION << "\",\"" << MCP_LEGACY_PROTOCOL_VERSION
                  << "\"],\"capabilities\":{\"tools\":{\"listChanged\":false}},\"_meta\":{\"io.modelcontextprotocol/serverInfo\":{\"name\":\""
                  << MCP_SERVER_NAME << "\",\"version\":\"" << MCP_SERVER_VERSION
                  << "\"}},\"instructions\":\"Manage Telegram messages, updates, and webhooks through a server-configured Telegram bot. Bot credentials are never exposed to MCP clients.\"}";
    return send_mcp_result(result);
  }

  // Compatibility with pre-2026 MCP clients. Current clients don't need initialize.
  if (method == "initialize") {
    auto r_version = get_mcp_string(params_object, "protocolVersion");
    if (r_version.is_error()) {
      return send_mcp_error(400, -32602, "initialize requires protocolVersion", mcp_request_id_json_);
    }
    mcp_protocol_version_ = MCP_LEGACY_PROTOCOL_VERSION.str();
    auto result = PSTRING() << "{\"protocolVersion\":\"" << MCP_LEGACY_PROTOCOL_VERSION
                            << "\",\"capabilities\":{\"tools\":{\"listChanged\":false}},\"serverInfo\":{\"name\":\""
                            << MCP_SERVER_NAME << "\",\"version\":\"" << MCP_SERVER_VERSION << "\"}}";
    return send_mcp_result(result);
  }
  if (method == "ping") {
    return send_mcp_result("{\"resultType\":\"complete\"}");
  }

  if (method == "tools/list") {
    static constexpr td::Slice tools_json =
        "{\"resultType\":\"complete\",\"tools\":["
        "{\"name\":\"sendMessage\",\"title\":\"Send Telegram message\",\"description\":\"Send a text message to the server-configured Telegram chat.\","
        "\"inputSchema\":{\"type\":\"object\",\"properties\":{\"text\":{\"type\":\"string\",\"description\":\"Message text\"},"
        "\"parse_mode\":{\"type\":\"string\",\"description\":\"Optional Telegram parse mode, for example HTML or MarkdownV2\"},"
        "\"disable_notification\":{\"type\":\"boolean\",\"description\":\"Send silently\"}},\"required\":[\"text\"],\"additionalProperties\":false}},"
        "{\"name\":\"editMessageText\",\"title\":\"Edit Telegram message\",\"description\":\"Edit a text message previously sent in the server-configured Telegram chat.\","
        "\"inputSchema\":{\"type\":\"object\",\"properties\":{\"message_id\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":2147483647,\"description\":\"Telegram message ID to edit\"},"
        "\"text\":{\"type\":\"string\",\"description\":\"Replacement message text\"},"
        "\"parse_mode\":{\"type\":\"string\",\"description\":\"Optional Telegram parse mode, for example HTML or MarkdownV2\"}},\"required\":[\"message_id\",\"text\"],\"additionalProperties\":false}},"
        "{\"name\":\"getUpdates\",\"title\":\"Get Telegram updates\",\"description\":\"Receive incoming updates using long polling from the server-configured Telegram bot.\","
        "\"inputSchema\":{\"type\":\"object\",\"properties\":{\"offset\":{\"type\":\"integer\",\"description\":\"Identifier of the first update to be returned\"},"
        "\"limit\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":100,\"description\":\"Limits the number of updates to be retrieved (1-100)\"},"
        "\"timeout\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":50,\"description\":\"Timeout in seconds for long polling (0-50)\"},"
        "\"allowed_updates\":{\"type\":\"array\",\"items\":{\"type\":\"string\"},\"description\":\"List of update types to receive\"}},\"additionalProperties\":false}},"
        "{\"name\":\"setWebhook\",\"title\":\"Set Telegram webhook\",\"description\":\"Specify a private/reserved IP URL to receive incoming updates, or empty string to delete webhook.\","
        "\"inputSchema\":{\"type\":\"object\",\"properties\":{\"url\":{\"type\":\"string\",\"description\":\"HTTPS or HTTP URL (private IP only, no domain) to send updates to, or empty string to remove webhook\"},"
        "\"max_connections\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":100,\"description\":\"Maximum allowed number of simultaneous connections (1-100)\"},"
        "\"allowed_updates\":{\"type\":\"array\",\"items\":{\"type\":\"string\"},\"description\":\"List of update types to receive\"},"
        "\"drop_pending_updates\":{\"type\":\"boolean\",\"description\":\"Pass True to drop all pending updates\"},"
        "\"secret_token\":{\"type\":\"string\",\"description\":\"Optional secret token for X-Telegram-Bot-Api-Secret-Token header\"}},\"required\":[\"url\"],\"additionalProperties\":false}}"
        "],\"ttlMs\":300000,\"cacheScope\":\"public\"}";
    return send_mcp_result(tools_json);
  }

  if (method != "tools/call") {
    return send_mcp_error(404, -32601, "Method not found", mcp_request_id_json_);
  }

  auto r_tool_name = get_mcp_string(params_object, "name");
  if (r_tool_name.is_error()) {
    return send_mcp_error(400, -32602, "tools/call requires a tool name", mcp_request_id_json_);
  }
  auto tool_name = r_tool_name.move_as_ok();
  auto mcp_name = http_query->get_header("mcp-name");
  if (!mcp_name.empty() && !mcp_name_matches(mcp_name, tool_name)) {
    return send_mcp_error(400, -32020, "Mcp-Name must match the tool name", mcp_request_id_json_);
  }

  bool is_send_message = (tool_name == "sendMessage" || tool_name == "send_message");
  bool is_edit_message = (tool_name == "editMessageText" || tool_name == "edit_message");
  bool is_get_updates = (tool_name == "getUpdates" || tool_name == "poll_updates");
  bool is_set_webhook = (tool_name == "setWebhook" || tool_name == "set_webhook");

  if (!is_send_message && !is_edit_message && !is_get_updates && !is_set_webhook) {
    return send_mcp_error(400, -32602, "Unknown tool", mcp_request_id_json_);
  }

  td::JsonValue arguments;
  if (params_object.has_field("arguments")) {
    arguments = params_object.extract_field("arguments");
    if (arguments.type() != td::JsonValue::Type::Object) {
      return send_mcp_error(400, -32602, "Tool arguments must be an object", mcp_request_id_json_);
    }
  } else {
    arguments = td::JsonValue::make_object(td::JsonObject());
  }
  auto &args_object = arguments.get_object();
  bool unknown_argument = false;
  args_object.foreach([&](td::Slice key, const td::JsonValue &) {
    if (is_send_message) {
      if (key != "text" && key != "parse_mode" && key != "disable_notification") {
        unknown_argument = true;
      }
    } else if (is_edit_message) {
      if (key != "text" && key != "parse_mode" && key != "message_id") {
        unknown_argument = true;
      }
    } else if (is_get_updates) {
      if (key != "offset" && key != "limit" && key != "timeout" && key != "allowed_updates") {
        unknown_argument = true;
      }
    } else if (is_set_webhook) {
      if (key != "url" && key != "max_connections" && key != "allowed_updates" &&
          key != "drop_pending_updates" && key != "secret_token") {
        unknown_argument = true;
      }
    }
  });
  if (unknown_argument) {
    return send_mcp_error(400, -32602, "Unexpected tool argument", mcp_request_id_json_);
  }

  td::vector<std::pair<td::string, td::string>> bot_args;
  td::string telegram_method;

  if (is_send_message || is_edit_message) {
    auto r_text = get_mcp_string(args_object, "text");
    if (r_text.is_error() || r_text.ok().empty()) {
      return send_mcp_error(400, -32602, "text is required", mcp_request_id_json_);
    }

    bot_args.emplace_back("chat_id", shared_data_->mcp_chat_id_);
    bot_args.emplace_back("text", r_text.move_as_ok());

    if (args_object.has_field("parse_mode")) {
      auto r_parse_mode = get_mcp_string(args_object, "parse_mode");
      if (r_parse_mode.is_error()) {
        return send_mcp_error(400, -32602, "parse_mode must be a string", mcp_request_id_json_);
      }
      if (!r_parse_mode.ok().empty()) {
        bot_args.emplace_back("parse_mode", r_parse_mode.move_as_ok());
      }
    }

    if (is_send_message) {
      telegram_method = "sendMessage";
      if (args_object.has_field("disable_notification")) {
        auto r_disable_notification = args_object.get_required_bool_field("disable_notification");
        if (r_disable_notification.is_error()) {
          return send_mcp_error(400, -32602, "disable_notification must be a boolean", mcp_request_id_json_);
        }
        bot_args.emplace_back("disable_notification", r_disable_notification.ok() ? "true" : "false");
      }
    } else {
      telegram_method = "editMessageText";
      auto message_id = args_object.extract_field("message_id");
      if (message_id.type() != td::JsonValue::Type::Number) {
        return send_mcp_error(400, -32602, "message_id must be an integer", mcp_request_id_json_);
      }
      auto r_message_id = td::to_integer_safe<td::int64>(message_id.get_number());
      if (r_message_id.is_error() || r_message_id.ok() <= 0 || r_message_id.ok() > 2147483647) {
        return send_mcp_error(400, -32602, "message_id must be an integer between 1 and 2147483647",
                              mcp_request_id_json_);
      }
      bot_args.emplace_back("message_id", td::to_string(r_message_id.ok()));
    }
  } else if (is_get_updates) {
    telegram_method = "getUpdates";
    if (args_object.has_field("offset")) {
      auto offset = args_object.extract_field("offset");
      if (offset.type() != td::JsonValue::Type::Number) {
        return send_mcp_error(400, -32602, "offset must be an integer", mcp_request_id_json_);
      }
      auto r_offset = td::to_integer_safe<td::int64>(offset.get_number());
      if (r_offset.is_error()) {
        return send_mcp_error(400, -32602, "offset must be a valid integer", mcp_request_id_json_);
      }
      bot_args.emplace_back("offset", td::to_string(r_offset.ok()));
    }
    if (args_object.has_field("limit")) {
      auto limit = args_object.extract_field("limit");
      if (limit.type() != td::JsonValue::Type::Number) {
        return send_mcp_error(400, -32602, "limit must be an integer", mcp_request_id_json_);
      }
      auto r_limit = td::to_integer_safe<int>(limit.get_number());
      if (r_limit.is_error() || r_limit.ok() < 1 || r_limit.ok() > 100) {
        return send_mcp_error(400, -32602, "limit must be between 1 and 100", mcp_request_id_json_);
      }
      bot_args.emplace_back("limit", td::to_string(r_limit.ok()));
    }
    if (args_object.has_field("timeout")) {
      auto timeout = args_object.extract_field("timeout");
      if (timeout.type() != td::JsonValue::Type::Number) {
        return send_mcp_error(400, -32602, "timeout must be an integer", mcp_request_id_json_);
      }
      auto r_timeout = td::to_integer_safe<int>(timeout.get_number());
      if (r_timeout.is_error() || r_timeout.ok() < 0 || r_timeout.ok() > 50) {
        return send_mcp_error(400, -32602, "timeout must be between 0 and 50", mcp_request_id_json_);
      }
      bot_args.emplace_back("timeout", td::to_string(r_timeout.ok()));
    }
    if (args_object.has_field("allowed_updates")) {
      auto au = args_object.extract_field("allowed_updates");
      if (au.type() != td::JsonValue::Type::Array && au.type() != td::JsonValue::Type::String) {
        return send_mcp_error(400, -32602, "allowed_updates must be an array of strings", mcp_request_id_json_);
      }
      if (au.type() == td::JsonValue::Type::Array) {
        bot_args.emplace_back("allowed_updates", td::json_encode<td::string>(au));
      } else {
        bot_args.emplace_back("allowed_updates", au.get_string().str());
      }
    }
  } else if (is_set_webhook) {
    auto r_url = get_mcp_string(args_object, "url");
    if (r_url.is_error()) {
      return send_mcp_error(400, -32602, "url is required", mcp_request_id_json_);
    }
    auto url = r_url.move_as_ok();
    if (url.empty()) {
      telegram_method = "deleteWebhook";
      if (args_object.has_field("drop_pending_updates")) {
        auto r_drop = args_object.get_required_bool_field("drop_pending_updates");
        if (r_drop.is_error()) {
          return send_mcp_error(400, -32602, "drop_pending_updates must be a boolean", mcp_request_id_json_);
        }
        bot_args.emplace_back("drop_pending_updates", r_drop.ok() ? "true" : "false");
      }
    } else {
      auto r_http_url = td::parse_url(url, td::HttpUrl::Protocol::Http);
      if (r_http_url.is_error()) {
        return send_mcp_error(400, -32602, PSLICE() << "Invalid webhook URL: " << r_http_url.error().message(),
                              mcp_request_id_json_);
      }
      auto &http_url = r_http_url.ok();
      auto r_ip = td::IPAddress::get_ip_address(http_url.host_);
      if (r_ip.is_error()) {
        return send_mcp_error(400, -32602, "Webhook host must be a valid IP address, domain names are not allowed",
                              mcp_request_id_json_);
      }
      auto &ip = r_ip.ok();
      if (!ip.is_reserved()) {
        return send_mcp_error(400, -32602, "Webhook IP address must be a private/reserved IP address",
                              mcp_request_id_json_);
      }

      telegram_method = "setWebhook";
      bot_args.emplace_back("url", url);

      if (args_object.has_field("max_connections")) {
        auto mc = args_object.extract_field("max_connections");
        if (mc.type() != td::JsonValue::Type::Number) {
          return send_mcp_error(400, -32602, "max_connections must be an integer", mcp_request_id_json_);
        }
        auto r_mc = td::to_integer_safe<int>(mc.get_number());
        if (r_mc.is_error() || r_mc.ok() < 1 || r_mc.ok() > 100) {
          return send_mcp_error(400, -32602, "max_connections must be between 1 and 100", mcp_request_id_json_);
        }
        bot_args.emplace_back("max_connections", td::to_string(r_mc.ok()));
      }
      if (args_object.has_field("allowed_updates")) {
        auto au = args_object.extract_field("allowed_updates");
        if (au.type() != td::JsonValue::Type::Array && au.type() != td::JsonValue::Type::String) {
          return send_mcp_error(400, -32602, "allowed_updates must be an array of strings", mcp_request_id_json_);
        }
        if (au.type() == td::JsonValue::Type::Array) {
          bot_args.emplace_back("allowed_updates", td::json_encode<td::string>(au));
        } else {
          bot_args.emplace_back("allowed_updates", au.get_string().str());
        }
      }
      if (args_object.has_field("drop_pending_updates")) {
        auto r_drop = args_object.get_required_bool_field("drop_pending_updates");
        if (r_drop.is_error()) {
          return send_mcp_error(400, -32602, "drop_pending_updates must be a boolean", mcp_request_id_json_);
        }
        bot_args.emplace_back("drop_pending_updates", r_drop.ok() ? "true" : "false");
      }
      if (args_object.has_field("secret_token")) {
        auto r_secret = get_mcp_string(args_object, "secret_token");
        if (r_secret.is_error()) {
          return send_mcp_error(400, -32602, "secret_token must be a string", mcp_request_id_json_);
        }
        bot_args.emplace_back("secret_token", r_secret.move_as_ok());
      }
    }
  }

  if (shared_data_->mcp_bot_token_.empty()) {
    td::Slice result = "{\"resultType\":\"complete\",\"content\":[{\"type\":\"text\",\"text\":\"MCP Telegram bridge is not configured: set TELEGRAM_MCP_BOT_TOKEN\"}],\"isError\":true}";
    return send_mcp_result(result);
  }
  if ((is_send_message || is_edit_message) && shared_data_->mcp_chat_id_.empty()) {
    td::Slice result = "{\"resultType\":\"complete\",\"content\":[{\"type\":\"text\",\"text\":\"MCP Telegram bridge is not configured: set TELEGRAM_MCP_CHAT_ID\"}],\"isError\":true}";
    return send_mcp_result(result);
  }

  auto peer_ip_address = http_query->peer_address_;
  auto query = make_internal_query(shared_data_->mcp_bot_token_, telegram_method, bot_args, shared_data_,
                                   peer_ip_address, make_query_promise());
  send_closure(client_manager_, &ClientManager::send, std::move(query));
}

td::Promise<td::unique_ptr<Query>> HttpConnection::make_query_promise() {
  return td::PromiseCreator::lambda([actor_id = actor_id(this)](td::Result<td::unique_ptr<Query>> r_query) {
    send_closure(actor_id, &HttpConnection::on_query_finished, std::move(r_query));
  });
}

void HttpConnection::on_query_finished(td::Result<td::unique_ptr<Query>> r_query) {
  LOG_CHECK(r_query.is_ok()) << r_query.error();

  auto query = r_query.move_as_ok();
  if (!is_mcp_request_) {
    return send_response(query->http_status_code(), std::move(query->answer()), query->retry_after());
  }

  auto answer = query->answer().as_slice();
  bool is_error = query->http_status_code() >= 400;
  auto result = PSTRING() << "{\"resultType\":\"complete\",\"content\":[{\"type\":\"text\",\"text\":"
                          << json_quote(answer) << "}],\"structuredContent\":" << answer
                          << ",\"isError\":" << (is_error ? "true" : "false") << "}";
  send_mcp_result(result);
}

void HttpConnection::send_response(int http_status_code, td::BufferSlice &&content, int retry_after) {
  td::HttpHeaderCreator hc;
  hc.init_status_line(http_status_code);
  hc.set_keep_alive();
  hc.set_content_type("application/json");
  if (is_mcp_request_) {
    hc.add_header("MCP-Protocol-Version", mcp_protocol_version_);
    hc.add_header("Cache-Control", "no-store");
    if (http_status_code == 405) {
      hc.add_header("Allow", "POST");
    }
  }
  if (retry_after > 0) {
    hc.add_header("Retry-After", PSLICE() << retry_after);
  }
  hc.set_content_size(content.size());

  auto r_header = hc.finish();
  LOG(DEBUG) << "Response headers: " << r_header.ok();
  if (r_header.is_error()) {
    LOG(ERROR) << "Bad response headers";
    send_closure(std::move(connection_), &td::HttpInboundConnection::write_error, r_header.move_as_error());
    return;
  }
  LOG(DEBUG) << "Send result: " << content;

  send_closure(connection_, &td::HttpInboundConnection::write_next_noflush, td::BufferSlice(r_header.ok()));
  send_closure(connection_, &td::HttpInboundConnection::write_next_noflush, std::move(content));
  send_closure(std::move(connection_), &td::HttpInboundConnection::write_ok);
}

void HttpConnection::send_http_error(int http_status_code, td::CSlice description) {
  send_response(http_status_code, td::json_encode<td::BufferSlice>(JsonQueryError(http_status_code, description)), 0);
}

void HttpConnection::send_mcp_result(td::Slice result) {
  send_response(200, td::BufferSlice(make_mcp_result(mcp_request_id_json_, result)), 0);
}

void HttpConnection::send_mcp_error(int http_status_code, int error_code, td::Slice message, td::Slice id_json,
                                   td::Slice data_json) {
  send_response(http_status_code, td::BufferSlice(make_mcp_error(id_json, error_code, message, data_json)), 0);
}

void HttpConnection::handle_stats(td::unique_ptr<td::HttpQuery> http_query) {
  if (http_query->type_ != td::HttpQuery::Type::Get && http_query->type_ != td::HttpQuery::Type::Post) {
    return on_stats_result(td::Status::Error(405, "Method Not Allowed"));
  }
  if (http_query->type_ == td::HttpQuery::Type::Post && http_query->get_header("sec-fetch-site") == "cross-site") {
    return on_stats_result(td::Status::Error(403, "Cross-site stats actions are not allowed"));
  }

  auto promise = td::PromiseCreator::lambda(
      [actor_id = actor_id(this)](td::Result<ClientManager::StatsResponse> result) {
        send_closure(actor_id, &HttpConnection::on_stats_result, std::move(result));
      });
  send_closure(client_manager_, &ClientManager::get_stats, std::move(promise), http_query->get_args(),
               http_query->type_ == td::HttpQuery::Type::Post);
}

void HttpConnection::on_stats_result(td::Result<ClientManager::StatsResponse> result) {
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

void HttpConnection::send_redirect(int http_status_code, td::CSlice location) {
  td::HttpHeaderCreator hc;
  hc.init_status_line(http_status_code);
  hc.set_keep_alive();
  hc.add_header("Location", location);
  hc.set_content_size(0);
  auto r_header = hc.finish();
  if (r_header.is_error()) {
    send_closure(connection_.release(), &td::HttpInboundConnection::write_error, r_header.move_as_error());
    return;
  }
  send_closure(connection_, &td::HttpInboundConnection::write_next_noflush, td::BufferSlice(r_header.ok()));
  send_closure(connection_.release(), &td::HttpInboundConnection::write_ok);
}

}  // namespace telegram_bot_api
