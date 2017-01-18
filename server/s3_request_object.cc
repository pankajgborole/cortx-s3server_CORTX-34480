/*
 * COPYRIGHT 2015 SEAGATE LLC
 *
 * THIS DRAWING/DOCUMENT, ITS SPECIFICATIONS, AND THE DATA CONTAINED
 * HEREIN, ARE THE EXCLUSIVE PROPERTY OF SEAGATE TECHNOLOGY
 * LIMITED, ISSUED IN STRICT CONFIDENCE AND SHALL NOT, WITHOUT
 * THE PRIOR WRITTEN PERMISSION OF SEAGATE TECHNOLOGY LIMITED,
 * BE REPRODUCED, COPIED, OR DISCLOSED TO A THIRD PARTY, OR
 * USED FOR ANY PURPOSE WHATSOEVER, OR STORED IN A RETRIEVAL SYSTEM
 * EXCEPT AS ALLOWED BY THE TERMS OF SEAGATE LICENSES AND AGREEMENTS.
 *
 * YOU SHOULD HAVE RECEIVED A COPY OF SEAGATE'S LICENSE ALONG WITH
 * THIS RELEASE. IF NOT PLEASE CONTACT A SEAGATE REPRESENTATIVE
 * http://www.seagate.com/contact
 *
 * Original author:  Kaustubh Deorukhkar   <kaustubh.deorukhkar@seagate.com>
 * Original author:  Rajesh Nambiar   <rajesh.nambiar@seagate.com>
 * Original creation date: 1-Oct-2015
 */

#include "s3_request_object.h"
#include <evhttp.h>
#include <string>
#include "s3_error_codes.h"
#include "s3_stats.h"

// evhttp Helpers
/* evhtp_kvs_iterator */
extern "C" int consume_header(evhtp_kv_t* kvobj, void* arg) {
  S3RequestObject* request = (S3RequestObject*)arg;
  request->in_headers_copy[kvobj->key] = kvobj->val ? kvobj->val : "";
  return 0;
}

S3RequestObject::S3RequestObject(evhtp_request_t* req,
                                 EvhtpInterface* evhtp_obj_ptr)
    : ev_req(req),
      is_paused(false),
      notify_read_watermark(0),
      total_bytes_received(0),
      is_client_connected(true),
      is_chunked_upload(false),
      in_headers_copied(false),
      reply_buffer(NULL) {
  s3_log(S3_LOG_DEBUG, "Constructor\n");

  request_timer.start();
  bucket_name = object_name = user_name = user_id = account_name = account_id =
      "";
  // For auth disabled, use some dummy user.
  if (S3Option::get_instance()->is_auth_disabled()) {
    account_name = "s3_test";
    account_id = "12345";
    user_name = "tester";
    user_id = "123";
  }

  S3Uuid uuid;
  request_id = uuid.get_string_uuid();
  request_error = S3RequestError::None;
  evhtp_obj.reset(evhtp_obj_ptr);
  if (ev_req != NULL && ev_req->uri != NULL) {
    if (ev_req->uri->path != NULL) {
      char* decoded_str = evhttp_decode_uri(ev_req->uri->path->full);
      full_path_decoded_uri = decoded_str;
      free(decoded_str);
    }
    if (ev_req->uri->path->file != NULL) {
      char* decoded_str = evhttp_decode_uri(ev_req->uri->path->file);
      file_path_decoded_uri = decoded_str;
      free(decoded_str);
    }
    if (ev_req->uri->query_raw != NULL) {
      char* decoded_str =
          evhttp_decode_uri((const char*)ev_req->uri->query_raw);
      query_raw_decoded_uri = decoded_str;
      free(decoded_str);
    }
  }

  initialise();
}

void S3RequestObject::initialise() {
  s3_log(S3_LOG_DEBUG, "Initializing the request.\n");
  if (get_header_value("x-amz-content-sha256") ==
      "STREAMING-AWS4-HMAC-SHA256-PAYLOAD") {
    is_chunked_upload = true;
  }
  pending_in_flight = get_data_length();
  if (pending_in_flight == 0) {
    // We are not expecting any payload.
    buffered_input.freeze();
  }
}

S3RequestObject::~S3RequestObject() {
  s3_log(S3_LOG_DEBUG, "Destructor\n");
  request_timer.stop();
  LOG_PERF("total_request_time_ms", request_timer.elapsed_time_in_millisec());
  s3_stats_timing("total_request_time",
                  request_timer.elapsed_time_in_millisec());
  if (ev_req) {
    ev_req->cbarg = NULL;
  }
}

S3HttpVerb S3RequestObject::http_verb() { return (S3HttpVerb)ev_req->method; }

void S3RequestObject::set_full_path(const char* full_path) {
  full_path_decoded_uri = full_path;
}

const char* S3RequestObject::c_get_full_path() {
  return full_path_decoded_uri.c_str();
}

const char* S3RequestObject::c_get_full_encoded_path() {
  return ev_req->uri->path->full;
}

void S3RequestObject::set_file_name(const char* file_name) {
  file_path_decoded_uri = file_name;
}

const char* S3RequestObject::c_get_file_name() {
  return file_path_decoded_uri.c_str();
}

void S3RequestObject::set_query_params(const char* query_params) {
  query_raw_decoded_uri = query_params;
}

const char* S3RequestObject::c_get_uri_query() {
  return query_raw_decoded_uri.c_str();
}

std::map<std::string, std::string>& S3RequestObject::get_in_headers_copy() {
  if (!in_headers_copied) {
    evhtp_obj->http_kvs_for_each(ev_req->headers_in, consume_header, this);
    in_headers_copied = true;
  }
  return in_headers_copy;
}

std::string S3RequestObject::get_header_value(std::string key) {
  if (ev_req == NULL) {
    return "";
  }
  const char* ret =
      evhtp_obj->http_header_find(ev_req->headers_in, key.c_str());
  if (ret == NULL) {
    return "";
  } else {
    return ret;
  }
}

std::string S3RequestObject::get_host_header() {
  std::string host = "Host";
  return get_header_value(host);
}

void S3RequestObject::set_out_header_value(std::string key, std::string value) {
  evhtp_obj->http_headers_add_header(
      ev_req->headers_out,
      evhtp_obj->http_header_new(key.c_str(), value.c_str(), 1, 1));
}

std::string S3RequestObject::get_data_length_str() {
  std::string data_length_key = "x-amz-decoded-content-length";
  std::string data_length = get_header_value(data_length_key);
  if (data_length.empty()) {
    // Normal request
    return get_content_length_str();
  } else {
    return data_length;
  }
}

size_t S3RequestObject::get_data_length() {
  return std::stoi(get_data_length_str());
}

std::string S3RequestObject::get_content_length_str() {
  std::string content_length = "Content-Length";
  std::string len = get_header_value(content_length);
  if (len.empty()) {
    len = "0";
  }
  return len;
}

size_t S3RequestObject::get_content_length() {
  return std::stoi(get_content_length_str());
}

std::string& S3RequestObject::get_full_body_content_as_string() {
  full_request_body = "";
  if (buffered_input.is_freezed()) {
    full_request_body = buffered_input.get_content_as_string();
  }

  return full_request_body;
}

std::string S3RequestObject::get_query_string_value(std::string key) {
  const char* value = evhtp_obj->http_kv_find(ev_req->uri->query, key.c_str());
  std::string val_str = "";
  if (value) {
    char* decoded_value;
    decoded_value = evhttp_decode_uri(value);
    val_str = decoded_value;
    free(decoded_value);
  }
  return val_str;
}

bool S3RequestObject::has_query_param_key(std::string key) {
  return evhtp_obj->http_kvs_find_kv(ev_req->uri->query, key.c_str()) != NULL;
}

// Operation params.
std::string S3RequestObject::get_object_uri() {
  return bucket_name + "/" + object_name;
}

void S3RequestObject::set_bucket_name(const std::string& name) {
  bucket_name = name;
}

const std::string& S3RequestObject::get_bucket_name() { return bucket_name; }

void S3RequestObject::set_object_name(const std::string& name) {
  object_name = name;
}

const std::string& S3RequestObject::get_object_name() { return object_name; }

void S3RequestObject::set_user_name(const std::string& name) {
  user_name = name;
}

const std::string& S3RequestObject::get_user_name() { return user_name; }

void S3RequestObject::set_user_id(const std::string& id) { user_id = id; }

const std::string& S3RequestObject::get_user_id() { return user_id; }

void S3RequestObject::set_account_id(const std::string& id) { account_id = id; }

const std::string& S3RequestObject::get_account_id() { return account_id; }

void S3RequestObject::set_account_name(const std::string& name) {
  account_name = name;
}

const std::string& S3RequestObject::get_account_name() { return account_name; }

const std::string& S3RequestObject::get_request_id() { return request_id; }

void S3RequestObject::notify_incoming_data(evbuf_t* buf) {
  s3_log(S3_LOG_DEBUG, "Entering\n");
  s3_log(S3_LOG_DEBUG, "pending_in_flight (before): %zu\n", pending_in_flight);
  // Keep buffering till someone starts listening.
  size_t data_bytes_received = 0;
  S3Timer buffering_timer;
  buffering_timer.start();

  if (is_chunked_upload) {
    std::vector<evbuf_t*> bufs = chunk_parser.run(buf);
    if (chunk_parser.get_state() == ChunkParserState::c_error) {
      set_request_error(S3RequestError::InvalidChunkFormat);
    } else if (!bufs.empty()) {
      for (auto b : bufs) {
        data_bytes_received += evhtp_obj->evbuffer_get_length(b);
        buffered_input.add_content(b);
      }
    }
  } else {
    data_bytes_received = evhtp_obj->evbuffer_get_length(buf);
    buffered_input.add_content(buf);
  }
  s3_log(S3_LOG_DEBUG, "Buffering data to be consumed: %zu\n",
         data_bytes_received);

  if (data_bytes_received > pending_in_flight) {
    s3_log(S3_LOG_ERROR, "Received too much unexpected data\n");
  }
  pending_in_flight -= data_bytes_received;
  s3_log(S3_LOG_DEBUG, "pending_in_flight (after): %zu\n", pending_in_flight);
  if (pending_in_flight == 0) {
    s3_log(S3_LOG_DEBUG, "Buffering complete for data to be consumed.\n");
    buffered_input.freeze();
  }
  buffering_timer.stop();
  LOG_PERF(("total_buffering_time_" + std::to_string(data_bytes_received) +
            "_bytes_ns")
               .c_str(),
           buffering_timer.elapsed_time_in_nanosec());
  s3_stats_timing("total_buffering_time",
                  buffering_timer.elapsed_time_in_millisec());

  if (incoming_data_callback &&
      ((buffered_input.length() >= notify_read_watermark) ||
       (pending_in_flight == 0))) {
    s3_log(S3_LOG_DEBUG, "Sending data to be consumed...\n");
    incoming_data_callback();
  }
  // Pause if we have read enough in buffers for this request,
  // and let the handlers resume when required.
  if (!incoming_data_callback && !buffered_input.is_freezed() &&
      buffered_input.length() >=
          (notify_read_watermark *
           S3Option::get_instance()->get_read_ahead_multiple())) {
    pause();
  }
  s3_log(S3_LOG_DEBUG, "Exiting\n");
}

void S3RequestObject::send_response(int code, std::string body) {
  // If body not empty, write to response body. TODO
  s3_log(S3_LOG_DEBUG, "Sending response as: [%s]\n", body.c_str());
  if (!body.empty()) {
    evbuffer_add_printf(ev_req->buffer_out, body.c_str());
  }
  set_out_header_value("x-amzn-RequestId", request_id);
  evhtp_obj->http_send_reply(ev_req, code);
  if (code == S3HttpFailed500) {
    s3_stats_inc("internal_error_count");
  }
  resume();  // attempt resume just in case some one forgot
}

void S3RequestObject::send_reply_start(int code) {
  set_out_header_value("x-amzn-RequestId", request_id);
  evhtp_obj->http_send_reply_start(ev_req, code);
  reply_buffer = evbuffer_new();
}

void S3RequestObject::send_reply_body(char* data, int length) {
  evbuffer_add(reply_buffer, data, length);
  evhtp_obj->http_send_reply_body(ev_req, reply_buffer);
}

void S3RequestObject::send_reply_end() {
  evhtp_obj->http_send_reply_end(ev_req);
  evbuffer_free(reply_buffer);
  reply_buffer = NULL;
}

void S3RequestObject::set_api_type(S3ApiType api_type) {
  s3_api_type = api_type;
}

S3ApiType S3RequestObject::get_api_type() { return s3_api_type; }

void S3RequestObject::respond_unsupported_api() {
  s3_log(S3_LOG_DEBUG, "Entering\n");

  S3Error error("NotImplemented", get_request_id(), "");
  std::string& response_xml = error.to_xml();
  set_out_header_value("Content-Type", "application/xml");
  set_out_header_value("Content-Length", std::to_string(response_xml.length()));
  set_out_header_value("x-amzn-RequestId", request_id);

  send_response(error.get_http_status_code(), response_xml);
  s3_stats_inc("unsupported_api_count");
  s3_log(S3_LOG_DEBUG, "Exiting\n");
}
