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
 * Original creation date: 1-Oct-2015
 */

#include <memory>
#include <string>
#include <regex>

#include "s3_router.h"
#include "s3_uri.h"
#include "s3_api_handler.h"
#include "s3_server_config.h"

bool S3Router::is_default_endpoint(std::string& endpoint) {
  return S3Config::get_instance()->get_default_endpoint() == endpoint;
}

bool S3Router::is_exact_valid_endpoint(std::string& endpoint) {
  if (endpoint == S3Config::get_instance()->get_default_endpoint()) {
    return true;
  }
  return S3Config::get_instance()->get_region_endpoints().find(endpoint) != S3Config::get_instance()->get_region_endpoints().end();
}

bool S3Router::is_subdomain_match(std::string& endpoint) {
  // todo check if given endpoint is subdomain or default or region.
  if (endpoint.find(S3Config::get_instance()->get_default_endpoint()) != std::string::npos) {
    return true;
  }
  for (std::set<std::string>::iterator it = S3Config::get_instance()->get_region_endpoints().begin(); it != S3Config::get_instance()->get_region_endpoints().end(); ++it) {
    if (endpoint.find(*it) != std::string::npos) {
      return true;
    }
  }
  return false;
}

void S3Router::dispatch(evhtp_request_t * req) {
  std::shared_ptr<S3RequestObject> request = std::make_shared<S3RequestObject> (req);

  std::string bucket_name, object_name;

  std::string host_header = request->get_host_header();
  printf("host_header = %s\n", host_header.c_str());
  printf("uri = %s\n", request->c_get_full_path());
  std::unique_ptr<S3URI> uri;
  if ( host_header.empty() || is_exact_valid_endpoint(host_header) || !is_subdomain_match(host_header)) {
    // Path style API
    // Bucket for the request will be the first slash-delimited component of the Request-URI
    printf("S3PathStyleURI\n");
    uri = std::unique_ptr<S3URI>(new S3PathStyleURI(request));
  } else {
    // Virtual host style endpoint
    printf("S3VirtualHostStyleURI\n");
    uri = std::unique_ptr<S3URI>(new S3VirtualHostStyleURI(request));
  }
  request->set_bucket_name(uri->get_bucket_name());
  request->set_object_name(uri->get_object_name());
  printf("bucket name = %s\n", uri->get_bucket_name().c_str());
  printf("object name = %s\n", uri->get_object_name().c_str());
  std::shared_ptr<S3APIHandler> handler;
  if (uri->is_service_api()) {
    handler = std::make_shared<S3ServiceAPIHandler> (request, uri->get_operation_code());
    printf("is_service_api\n");
  } else if (uri->is_bucket_api()) {
    printf("is_bucket_api\n");
    handler = std::make_shared<S3BucketAPIHandler> (request, uri->get_operation_code());
  } else if (uri->is_object_api()) {
    printf("is_object_api\n");
    handler = std::make_shared<S3ObjectAPIHandler>(request, uri->get_operation_code());
  } else {
    // Unsupported
    request->respond_unsupported_api();
    return;
  }
  if (handler) {
    handler->manage_self(handler);
    handler->dispatch();  // Start processing the request
  } else {
    request->respond_unsupported_api();
  }
  return;
}

// S3 URL Patterns for various APIs.

// List Buckets  -> http://s3.seagate.com/ GET

// Bucket APIs   -> http://s3.seagate.com/bucketname
// Host Header = s3.seagate.com or empty
//               -> http://bucketname.s3.seagate.com
// Host Header = *.s3.seagate.com
// Host Header = anything else = bucketname
// location      -> http://s3.seagate.com/bucketname?location
// ACL           -> http://s3.seagate.com/bucketname?acl
//               -> http://bucketname.s3.seagate.com/?acl
// Policy        -> http://s3.seagate.com/bucketname?policy
// Logging       -> http://s3.seagate.com/bucketname?logging
// Lifecycle     -> http://s3.seagate.com/bucketname?lifecycle
// CORs          -> http://s3.seagate.com/bucketname?cors
// notification  -> http://s3.seagate.com/bucketname?notification
// replicaton    -> http://s3.seagate.com/bucketname?replicaton
// tagging       -> http://s3.seagate.com/bucketname?tagging
// requestPayment-> http://s3.seagate.com/bucketname?requestPayment
// versioning    -> http://s3.seagate.com/bucketname?versioning
// website       -> http://s3.seagate.com/bucketname?website
// list multipart uploads in progress
//               -> http://s3.seagate.com/bucketname?uploads


// Object APIs   -> http://s3.seagate.com/bucketname/ObjectKey
// Host Header = s3.seagate.com or empty
//               -> http://bucketname.s3.seagate.com/ObjectKey
// Host Header = *.s3.seagate.com
// Host Header = anything else = bucketname

// ACL           -> http://s3.seagate.com/bucketname/ObjectKey?acl
//               -> http://bucketname.s3.seagate.com/ObjectKey?acl

// Multiple dels -> http://s3.seagate.com/bucketname?delete

// Initiate multipart upload
//               -> http://s3.seagate.com/bucketname/ObjectKey?uploads
// Upload part   -> http://s3.seagate.com/bucketname/ObjectKey?partNumber=PartNumber&uploadId=UploadId
// Complete      -> http://s3.seagate.com/bucketname/ObjectKey?uploadId=UploadId
// Abort         -> http://s3.seagate.com/bucketname/ObjectKey?uploadId=UploadId DEL
