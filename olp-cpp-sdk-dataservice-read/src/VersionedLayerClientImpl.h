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

#pragma once

#include <memory>

#include <olp/core/client/CancellationContext.h>
#include <olp/core/client/CancellationToken.h>
#include <olp/core/client/HRN.h>
#include <olp/core/client/OlpClientSettings.h>
#include <olp/dataservice/read/DataRequest.h>
#include <olp/dataservice/read/PartitionsRequest.h>
#include <olp/dataservice/read/PrefetchTileResult.h>
#include <olp/dataservice/read/PrefetchTilesRequest.h>
#include <olp/dataservice/read/Types.h>

#include "PendingRequests.h"

namespace olp {
namespace thread {
class TaskScheduler;
}
namespace dataservice {
namespace read {
namespace repository {
class PartitionsRepository;
}

class PrefetchTilesProvider;

class VersionedLayerClientImpl {
 public:
  VersionedLayerClientImpl(client::HRN catalog, std::string layer_id,
                           client::OlpClientSettings settings);

  virtual ~VersionedLayerClientImpl();

  virtual client::CancellationToken GetData(DataRequest data_request,
                                            DataResponseCallback callback);

  virtual client::CancellationToken GetPartitions(
      PartitionsRequest partitions_request,
      PartitionsResponseCallback callback);

  virtual client::CancellationToken PrefetchTiles(
      PrefetchTilesRequest request, PrefetchTilesResponseCallback callback);

  virtual client::CancellableFuture<PrefetchTilesResponse> PrefetchTiles(
      PrefetchTilesRequest request);

 protected:
  client::HRN catalog_;
  std::string layer_id_;
  std::shared_ptr<client::OlpClientSettings> settings_;
  std::shared_ptr<thread::TaskScheduler> task_scheduler_;
  std::shared_ptr<PendingRequests> pending_requests_;
  std::shared_ptr<repository::PartitionsRepository> partition_repo_;
  std::shared_ptr<PrefetchTilesProvider> prefetch_provider_;
};

}  // namespace read
}  // namespace dataservice
}  // namespace olp
