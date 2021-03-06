/*  Copyright 2014 MaidSafe.net limited

    This MaidSafe Software is licensed to you under (1) the MaidSafe.net Commercial License,
    version 1.0 or later, or (2) The General Public License (GPL), version 3, depending on which
    licence you accepted on initial access to the Software (the "Licences").

    By contributing code to the MaidSafe Software, or to this project generally, you agree to be
    bound by the terms of the MaidSafe Contributor Agreement, version 1.0, found in the root
    directory of this project at LICENSE, COPYING and CONTRIBUTOR respectively and also
    available at: http://www.maidsafe.net/licenses

    Unless required by applicable law or agreed to in writing, the MaidSafe Software distributed
    under the GPL Licence is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS
    OF ANY KIND, either express or implied.

    See the Licences for the specific language governing permissions and limitations relating to
    use of the MaidSafe Software.                                                                 */

#include "maidsafe/detail/session_getter.h"

#include "maidsafe/common/make_unique.h"

namespace maidsafe {

namespace detail {

SessionGetter::SessionGetter(const routing::BootstrapContacts& bootstrap_contacts)
    : network_health_mutex_(),
      network_health_condition_variable_(),
      network_health_(-1),
      routing_(),
      data_getter_(),  // deferred construction until asio service is created
      public_pmid_helper_(),
      asio_service_(2) {
  data_getter_ = maidsafe::make_unique<nfs_client::DataGetter>(asio_service_, routing_);
  InitRouting(bootstrap_contacts);
}

void SessionGetter::InitRouting(const routing::BootstrapContacts& bootstrap_contacts) {
  routing::Functors functors{ InitialiseRoutingCallbacks() };
  routing_.Join(functors, bootstrap_contacts);
  // FIXME BEFORE_RELEASE discuss this
  std::unique_lock<std::mutex> lock{ network_health_mutex_ };
  network_health_condition_variable_.wait(lock, [this] { return network_health_ == 100; });
}

routing::Functors SessionGetter::InitialiseRoutingCallbacks() {
  routing::Functors functors;
  functors.typed_message_and_caching.group_to_single.message_received =
      [this](const routing::GroupToSingleMessage& message) {
        data_getter_->HandleMessage(message);
      };

  functors.network_status =
      [this](const int& network_health) { OnNetworkStatusChange(network_health); };
  functors.matrix_changed = [this](std::shared_ptr<routing::MatrixChange> /*matrix_change*/) {};
  functors.request_public_key = [this](const NodeId& node_id,
                                       const routing::GivePublicKeyFunctor& give_key) {
      auto future_key(data_getter_->Get(passport::PublicPmid::Name{ Identity{ node_id.string() } },
                                        std::chrono::seconds(10)));
      public_pmid_helper_.AddEntry(std::move(future_key), give_key);
  };

  // Required to pick cached messages
  functors.typed_message_and_caching.single_to_single.message_received =
      [this](const routing::SingleToSingleMessage& message) {
      data_getter_->HandleMessage(message);
  };

  // TODO(Prakash) fix routing asserts for clients so client need not to provide callbacks for all
  // functors
  functors.typed_message_and_caching.single_to_group.message_received =
      [this](const routing::SingleToGroupMessage& /*message*/) {};
  functors.typed_message_and_caching.group_to_group.message_received =
      [this](const routing::GroupToGroupMessage& /*message*/) {};
  functors.typed_message_and_caching.single_to_group_relay.message_received =
      [this](const routing::SingleToGroupRelayMessage& /*message*/) {};
  functors.typed_message_and_caching.single_to_group.put_cache_data =
      [this](const routing::SingleToGroupMessage& /*message*/) {};
  functors.typed_message_and_caching.group_to_single.put_cache_data =
      [this](const routing::GroupToSingleMessage& /*message*/) {};
  functors.typed_message_and_caching.group_to_group.put_cache_data =
      [this](const routing::GroupToGroupMessage& /*message*/) {};
  functors.new_bootstrap_contact =
      [this](const routing::BootstrapContact& /*bootstrap_contact*/) {};

  return functors;
}

void SessionGetter::OnNetworkStatusChange(int updated_network_health) {
  asio_service_.service().post([=] {
    routing::UpdateNetworkHealth(updated_network_health, network_health_, network_health_mutex_,
                                 network_health_condition_variable_, routing_.kNodeId());
  });
}

}  // namespace detail

}  // namespace maidsafe
