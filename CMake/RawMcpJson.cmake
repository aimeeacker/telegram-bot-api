# TDLib normally decodes JSON request parameters in place and discards parse errors.
# Keep the original MCP body so that HttpConnection can validate the whole request.
# Generate an overlay in the build directory without modifying the TDLib checkout.
function(telegram_bot_api_preserve_mcp_json)
  if (NOT TARGET tdnet)
    return()
  endif()
  get_target_property(tdnet_source_dir tdnet SOURCE_DIR)
  set(http_reader_path "${tdnet_source_dir}/td/net/HttpReader.cpp")
  file(READ "${http_reader_path}" http_reader)
  set(original [=[          query_->container_.emplace_back(content_->cut_head(size).move_as_buffer_slice());
          Status result;]=])
  set(replacement [=[          query_->container_.emplace_back(content_->cut_head(size).move_as_buffer_slice());
          if (query_->url_path_ == "/mcp" && content_type_lowercased_.find("application/json") != string::npos) {
            query_->content_ = query_->container_.back().as_mutable_slice();
            break;
          }
          Status result;]=])
  string(FIND "${http_reader}" "${original}" insertion_point)
  if (insertion_point EQUAL -1)
    message(FATAL_ERROR "TDLib HttpReader changed; update the raw MCP JSON overlay")
  endif()
  string(REPLACE "${original}" "${replacement}" http_reader "${http_reader}")
  set(overlay "${CMAKE_CURRENT_BINARY_DIR}/tdnet/HttpReader.cpp")
  file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/tdnet")
  set(existing_overlay "")
  if (EXISTS "${overlay}")
    file(READ "${overlay}" existing_overlay)
  endif()
  if (NOT http_reader STREQUAL existing_overlay)
    file(WRITE "${overlay}" "${http_reader}")
  endif()
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${http_reader_path}")

  get_target_property(tdnet_sources tdnet SOURCES)
  set(patched_sources "")
  foreach(source IN LISTS tdnet_sources)
    if (source STREQUAL "td/net/HttpReader.cpp" OR source STREQUAL http_reader_path)
      list(APPEND patched_sources "${overlay}")
    elseif (IS_ABSOLUTE "${source}")
      list(APPEND patched_sources "${source}")
    else()
      list(APPEND patched_sources "${tdnet_source_dir}/${source}")
    endif()
  endforeach()
  set_property(TARGET tdnet PROPERTY SOURCES "${patched_sources}")
endfunction()

telegram_bot_api_preserve_mcp_json()
