#!/usr/bin/env python3
"""Patch TDLib's Td.h/Td.cpp to wrap bot queries with a 3-second asyncio.wait_for-style timeout.

When a bot common NetQuery does not complete within 3 seconds (e.g. during an upstream
MTProto disconnect), Td::on_query_timeout cancels the underlying NetQuery (setting its
cancellation token so Session/SessionConnection drop it instead of sending on reconnect)
and immediately completes the ResultHandler with a 504 Gateway Timeout error.
"""

import sys
from pathlib import Path


def patch_file(path: Path, hunks: list[tuple[str, str]], label: str) -> None:
    if not path.is_file():
        sys.exit(f"Error: {path} not found. Ensure TDLib submodule is checked out.")

    content = path.read_text(encoding="utf-8")
    if all(repl in content for _, repl in hunks):
        print(f"TDLib {label} is already patched for 3s query timeout.")
        return

    for idx, (orig, repl) in enumerate(hunks, start=1):
        if repl in content:
            continue
        count = content.count(orig)
        if count != 1:
            sys.exit(
                f"Error: Expected 1 occurrence for {label} hunk #{idx}, found {count}."
            )
        content = content.replace(orig, repl, 1)

    path.write_text(content, encoding="utf-8")
    print(f"Successfully patched {path} for 3s query timeout.")


def main() -> None:
    td_h_hunks = [
        (
            """  void add_handler(uint64 id, std::shared_ptr<ResultHandler> handler);
  std::shared_ptr<ResultHandler> extract_handler(uint64 id);""",
            """  void add_handler(uint64 id, std::shared_ptr<ResultHandler> handler);
  std::shared_ptr<ResultHandler> extract_handler(uint64 id);
  bool should_timeout_query(const NetQueryPtr &query) const;
  void schedule_query_timeout(NetQueryPtr &query);
  void on_query_timeout(uint64 query_id, NetQueryRef query_ref);""",
        ),
    ]

    td_cpp_hunks = [
        (
            """#include "td/telegram/UpdatesManager.h\"""",
            """#include "td/telegram/UniqueId.h"
#include "td/telegram/UpdatesManager.h\"""",
        ),
        (
            """#include "td/actor/actor.h\"""",
            """#include "td/actor/actor.h"
#include "td/actor/SleepActor.h\"""",
        ),
        (
            """void Td::ResultHandler::send_query(NetQueryPtr query) {
  CHECK(!is_query_sent_);
  is_query_sent_ = true;
  td_->add_handler(query->id(), shared_from_this());
  query->debug("Send to NetQueryDispatcher");
  G()->net_query_dispatcher().dispatch(std::move(query));
}""",
            """void Td::ResultHandler::send_query(NetQueryPtr query) {
  CHECK(!is_query_sent_);
  is_query_sent_ = true;
  td_->add_handler(query->id(), shared_from_this());
  td_->schedule_query_timeout(query);
  query->debug("Send to NetQueryDispatcher");
  G()->net_query_dispatcher().dispatch(std::move(query));
}""",
        ),
        (
            """std::shared_ptr<Td::ResultHandler> Td::extract_handler(uint64 id) {
  auto it = result_handlers_.find(id);
  if (it == result_handlers_.end()) {
    return nullptr;
  }
  auto result = std::move(it->second);
  result_handlers_.erase(it);
  return result;
}""",
            """std::shared_ptr<Td::ResultHandler> Td::extract_handler(uint64 id) {
  auto it = result_handlers_.find(id);
  if (it == result_handlers_.end()) {
    return nullptr;
  }
  auto result = std::move(it->second);
  result_handlers_.erase(it);
  return result;
}

bool Td::should_timeout_query(const NetQueryPtr &query) const {
  if (G()->close_flag() || close_flag_ != 0) {
    return false;
  }
  if (auth_manager_ == nullptr || !auth_manager_->is_bot()) {
    return false;
  }
  if (query->type() != NetQuery::Type::Common || query->auth_flag() != NetQuery::AuthFlag::On) {
    return false;
  }
  if (query->total_timeout_limit_ > 8 || UniqueId::extract_type(query->id()) == UniqueId::BindKey) {
    return false;
  }
  auto tl_id = query->tl_constructor();
  if (tl_id == telegram_api::updates_getState::ID || tl_id == telegram_api::updates_getDifference::ID ||
      tl_id == telegram_api::updates_getChannelDifference::ID) {
    return false;
  }
  return true;
}

void Td::schedule_query_timeout(NetQueryPtr &query) {
  if (!should_timeout_query(query)) {
    return;
  }
  static constexpr double BOT_QUERY_TIMEOUT = 3.0;
  auto query_id = query->id();
  auto query_ref = query.get_weak();
  create_actor<SleepActor>("QueryTimeoutActor", BOT_QUERY_TIMEOUT,
                           [actor_id = actor_id(this), query_id, query_ref] {
                             send_closure(actor_id, &Td::on_query_timeout, query_id, query_ref);
                           })
      .release();
}

void Td::on_query_timeout(uint64 query_id, NetQueryRef query_ref) {
  if (close_flag_ > 1) {
    return;
  }
  auto handler = extract_handler(query_id);
  if (handler == nullptr) {
    return;
  }
  cancel_query(query_ref);
  handler->on_error(Status::Error(504, "Gateway Timeout: Request processing timed out after 3 seconds"));
}""",
        ),
        (
            """    if (!query->is_ok() || query->ok_tl_constructor() != telegram_api::upload_file::ID) {
      LOG(WARNING) << query << " is ignored: no handlers found";
    }""",
            """    if ((!query->is_ok() || query->ok_tl_constructor() != telegram_api::upload_file::ID) &&
        (!query->is_error() || query->error().code() != NetQuery::Canceled)) {
      LOG(WARNING) << query << " is ignored: no handlers found";
    }""",
        ),
    ]

    patch_file(Path("td/td/telegram/Td.h"), td_h_hunks, "Td.h")
    patch_file(Path("td/td/telegram/Td.cpp"), td_cpp_hunks, "Td.cpp")


if __name__ == "__main__":
    main()
