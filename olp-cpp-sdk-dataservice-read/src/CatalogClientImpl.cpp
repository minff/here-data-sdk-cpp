/*
 * Copyright (C) 2019 HERE Europe B.V.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * SPDX-License-Identifier: Apache-2.0
 * License-Filename: LICENSE
 */

#include "CatalogClientImpl.h"

#include <olp/core/client/PendingRequests.h>
#include <olp/core/logging/Log.h>
#include "PrefetchTilesProvider.h"
#include "repositories/ApiRepository.h"
#include "repositories/CatalogRepository.h"
#include "repositories/DataRepository.h"
#include "repositories/ExecuteOrSchedule.inl"
#include "repositories/PartitionsRepository.h"
#include "repositories/PrefetchTilesRepository.h"

namespace olp {
namespace dataservice {
namespace read {
using namespace repository;
using namespace olp::client;

namespace {
constexpr auto kLogTag = "CatalogClientImpl";
}

CatalogClientImpl::CatalogClientImpl(HRN catalog, OlpClientSettings settings)
    : catalog_(std::move(catalog)),
      settings_(std::make_shared<OlpClientSettings>(std::move(settings))) {
  auto cache = settings_->cache;
  // to avoid capturing task scheduler inside a task, we need a copy of settings
  // without the scheduler
  task_scheduler_ = std::move(settings_->task_scheduler);

  // create repositories, satisfying dependencies.
  auto api_repo = std::make_shared<ApiRepository>(catalog_, settings_, cache);

  catalog_repo_ =
      std::make_shared<CatalogRepository>(catalog_, api_repo, cache);

  partition_repo_ = std::make_shared<PartitionsRepository>(
      catalog_, api_repo, catalog_repo_, cache);

  data_repo_ = std::make_shared<DataRepository>(
      catalog_, api_repo, catalog_repo_, partition_repo_, cache);

  auto prefetch_repo = std::make_shared<PrefetchTilesRepository>(
      catalog_, api_repo, partition_repo_->GetPartitionsCacheRepository(),
      settings_);

  prefetch_provider_ = std::make_shared<PrefetchTilesProvider>(
      catalog_, api_repo, catalog_repo_, data_repo_, std::move(prefetch_repo),
      settings_);

  pending_requests_ = std::make_shared<client::PendingRequests>();
}

CatalogClientImpl::~CatalogClientImpl() { CancelPendingRequests(); }

bool CatalogClientImpl::CancelPendingRequests() {
  OLP_SDK_LOG_TRACE(kLogTag, "CancelPendingRequests");
  return pending_requests_->CancelPendingRequests();
}

CancellationToken CatalogClientImpl::GetCatalog(
    CatalogRequest request, CatalogResponseCallback callback) {
  auto add_task = [&](CatalogRequest request,
                      CatalogResponseCallback callback) {
    auto catalog = catalog_;
    auto settings = *settings_;
    auto pending_requests = pending_requests_;

    auto data_task = [=](client::CancellationContext context) {
      return repository::CatalogRepository::GetCatalog(catalog, context,
                                                       request, settings);
    };

    auto context =
        client::TaskContext::Create(std::move(data_task), std::move(callback));

    pending_requests->Insert(context);

    repository::ExecuteOrSchedule(task_scheduler_, [=]() {
      context.Execute();
      pending_requests->Remove(context);
    });

    return context.CancelToken();
  };

  if (request.GetFetchOption() == FetchOptions::CacheWithUpdate) {
    auto cache_token = add_task(
        request.WithFetchOption(FetchOptions::CacheOnly), std::move(callback));
    auto online_token =
        add_task(request.WithFetchOption(FetchOptions::OnlineOnly), nullptr);

    return client::CancellationToken([=]() {
      cache_token.cancel();
      online_token.cancel();
    });
  } else {
    return add_task(std::move(request), std::move(callback));
  }
}

CancellableFuture<CatalogResponse> CatalogClientImpl::GetCatalog(
    CatalogRequest request) {
  return AsFuture<CatalogRequest, CatalogResponse>(
      std::move(request),
      static_cast<client::CancellationToken (CatalogClientImpl::*)(
          CatalogRequest, CatalogResponseCallback)>(
          &CatalogClientImpl::GetCatalog));
}

CancellationToken CatalogClientImpl::GetCatalogMetadataVersion(
    const CatalogVersionRequest& request,
    const CatalogVersionCallback& callback) {
  OLP_SDK_LOG_TRACE_F(kLogTag, "GetCatalog '%s'", request.CreateKey().c_str());
  CancellationToken token;
  int64_t request_key = pending_requests_->GenerateRequestPlaceholder();
  auto pending_requests = pending_requests_;
  auto request_callback = [pending_requests, request_key,
                           callback](CatalogVersionResponse response) {
    OLP_SDK_LOG_INFO(kLogTag, "GetCatalog remove key: " << request_key);
    if (pending_requests->Remove(request_key)) {
      callback(response);
    }
  };
  if (CacheWithUpdate == request.GetFetchOption()) {
    auto req = request;
    token = catalog_repo_->getLatestCatalogVersion(
        req.WithFetchOption(CacheOnly), request_callback);
    auto onlineKey = pending_requests_->GenerateRequestPlaceholder();
    OLP_SDK_LOG_INFO(kLogTag, "GetCatalog add key: " << onlineKey);
    pending_requests_->Insert(
        catalog_repo_->getLatestCatalogVersion(
            req.WithFetchOption(OnlineIfNotFound),
            [pending_requests, onlineKey](CatalogVersionResponse) {
              pending_requests->Remove(onlineKey);
            }),
        onlineKey);
  } else {
    OLP_SDK_LOG_INFO(kLogTag, "GetCatalog existing: " << request_key);
    token = catalog_repo_->getLatestCatalogVersion(request, request_callback);
  }
  pending_requests_->Insert(token, request_key);
  return token;
}

CancellableFuture<CatalogVersionResponse>
CatalogClientImpl::GetCatalogMetadataVersion(
    const CatalogVersionRequest& request) {
  return AsFuture<CatalogVersionRequest, CatalogVersionResponse>(
      request,
      static_cast<client::CancellationToken (CatalogClientImpl::*)(
          const CatalogVersionRequest&, const CatalogVersionCallback&)>(
          &CatalogClientImpl::GetCatalogMetadataVersion));
}

olp::client::CancellationToken CatalogClientImpl::GetPartitions(
    const PartitionsRequest& request,
    const PartitionsResponseCallback& callback) {
  CancellationToken token;
  int64_t request_key = pending_requests_->GenerateRequestPlaceholder();
  auto pending_requests = pending_requests_;
  auto request_callback = [pending_requests, request_key,
                           callback](PartitionsResponse response) {
    if (pending_requests->Remove(request_key)) {
      callback(response);
    }
  };
  if (CacheWithUpdate == request.GetFetchOption()) {
    auto req = request;
    token = partition_repo_->GetPartitions(req.WithFetchOption(CacheOnly),
                                           request_callback);
    auto onlineKey = pending_requests_->GenerateRequestPlaceholder();
    pending_requests_->Insert(
        partition_repo_->GetPartitions(
            req.WithFetchOption(OnlineIfNotFound),
            [pending_requests, onlineKey](PartitionsResponse) {
              pending_requests->Remove(onlineKey);
            }),
        onlineKey);
  } else {
    token = partition_repo_->GetPartitions(request, request_callback);
  }
  pending_requests_->Insert(token, request_key);
  return token;
}

olp::client::CancellableFuture<PartitionsResponse>
CatalogClientImpl::GetPartitions(const PartitionsRequest& request) {
  return AsFuture<PartitionsRequest, PartitionsResponse>(
      request,
      static_cast<client::CancellationToken (CatalogClientImpl::*)(
          const PartitionsRequest&, const PartitionsResponseCallback&)>(
          &CatalogClientImpl::GetPartitions));
}

client::CancellationToken CatalogClientImpl::GetData(
    const DataRequest& request, const DataResponseCallback& callback) {
  CancellationToken token;
  int64_t request_key = pending_requests_->GenerateRequestPlaceholder();
  auto pending_requests = pending_requests_;
  auto request_callback = [pending_requests, request_key,
                           callback](DataResponse response) {
    if (pending_requests->Remove(request_key)) {
      callback(response);
    }
  };
  if (CacheWithUpdate == request.GetFetchOption()) {
    auto req = request;
    token =
        data_repo_->GetData(req.WithFetchOption(CacheOnly), request_callback);
    auto onlineKey = pending_requests_->GenerateRequestPlaceholder();
    pending_requests_->Insert(
        data_repo_->GetData(req.WithFetchOption(OnlineIfNotFound),
                            [pending_requests, onlineKey](DataResponse) {
                              pending_requests->Remove(onlineKey);
                            }),
        onlineKey);
  } else {
    token = data_repo_->GetData(request, request_callback);
  }
  pending_requests_->Insert(token, request_key);
  return token;
}

client::CancellableFuture<DataResponse> CatalogClientImpl::GetData(
    const DataRequest& request) {
  return AsFuture<DataRequest, DataResponse>(
      request, static_cast<client::CancellationToken (CatalogClientImpl::*)(
                   const DataRequest&, const DataResponseCallback&)>(
                   &CatalogClientImpl::GetData));
}

client::CancellationToken CatalogClientImpl::PrefetchTiles(
    const PrefetchTilesRequest& request,
    const PrefetchTilesResponseCallback& callback) {
  int64_t request_key = pending_requests_->GenerateRequestPlaceholder();
  auto request_callback = [=](const PrefetchTilesResponse& response) {
    pending_requests_->Remove(request_key);
    callback(response);
  };

  auto token = prefetch_provider_->PrefetchTiles(request, request_callback);

  pending_requests_->Insert(token, request_key);
  return token;
}

client::CancellableFuture<PrefetchTilesResponse>
CatalogClientImpl::PrefetchTiles(const PrefetchTilesRequest& request) {
  return AsFuture<PrefetchTilesRequest, PrefetchTilesResponse>(
      request,
      static_cast<client::CancellationToken (CatalogClientImpl::*)(
          const PrefetchTilesRequest&, const PrefetchTilesResponseCallback&)>(
          &CatalogClientImpl::PrefetchTiles));
}

}  // namespace read
}  // namespace dataservice
}  // namespace olp
