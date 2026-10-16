#!/usr/bin/env python3
"""Patch TDLib's HttpReader.cpp to preserve raw JSON bodies for /mcp requests.

TDLib normally decodes JSON request parameters into query_->args_ and clears
query_->content_ (MutableSlice). For JSON-RPC 2.0 endpoints like /mcp, HttpConnection
requires the raw JSON payload to validate schemas and adhere to JSON-RPC 2.0 specs.
"""

import sys
from pathlib import Path


def main():
    path = Path("td/tdnet/td/net/HttpReader.cpp")
    if not path.is_file():
        sys.exit(f"Error: {path} not found. Ensure TDLib submodule is checked out.")

    content = path.read_text(encoding="utf-8")

    orig = """          query_->container_.emplace_back(content_->cut_head(size).move_as_buffer_slice());
          Status result;"""

    repl = """          query_->container_.emplace_back(content_->cut_head(size).move_as_buffer_slice());
          if (query_->url_path_ == "/mcp" && content_type_lowercased_.find("application/json") != string::npos) {
            query_->content_ = query_->container_.back().as_mutable_slice();
            break;
          }
          Status result;"""

    if repl in content:
        print("TDLib HttpReader.cpp is already patched for MCP.")
        return

    if orig not in content:
        sys.exit("Error: Could not locate expected insertion point in td/tdnet/td/net/HttpReader.cpp")

    new_content = content.replace(orig, repl, 1)
    path.write_text(new_content, encoding="utf-8")
    print("Successfully patched td/tdnet/td/net/HttpReader.cpp for MCP.")


if __name__ == "__main__":
    main()
