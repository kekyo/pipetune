#include "wireplumber_visibility.h"

#include <string_view>

namespace pipetune {

constexpr auto kWirePlumberNodeVisibilityPolicy =
    std::string_view{R"wpvis(-- Managed by PipeTune.
-- Permission management is derived from WirePlumber policy-dsp.lua.
-- Copyright © 2022-2023 The WirePlumber project contributors
-- SPDX-License-Identifier: MIT
--
-- Internal processing nodes must remain available to PipeTune and
-- WirePlumber while staying out of device selectors in other clients.

pipetune_hidden_nodes = {}

local function proxy_property(proxy, key)
  local properties = proxy["properties"]
  if properties and properties[key] ~= nil then
    return properties[key]
  end
  local global_properties = proxy["global-properties"]
  if global_properties then
    return global_properties[key]
  end
  return nil
end

local function is_internal_node(node)
  local internal = proxy_property(node, "node.pipetune.internal")
  local name = proxy_property(node, "node.name")
  return internal == true or internal == "true" or
      name == "control.endpoint.pipetune.playback" or
      name == "control.endpoint.pipetune.capture"
end

local function is_wireplumber(client)
  return proxy_property(client, "wireplumber.daemon") ~= nil
end

local function is_node_owner(client, owner_id)
  local client_id = tonumber(client["bound-id"])
  return client_id ~= nil and owner_id ~= nil and client_id == owner_id
end

local function hide_node_from_client(client, node_id, owner_id)
  if not is_wireplumber(client) and not is_node_owner(client, owner_id) then
    client:update_permissions { [node_id] = "-" }
  end
end

pipetune_nodes_om = ObjectManager {
  Interest { type = "node" },
}
pipetune_clients_om = ObjectManager {
  Interest { type = "client" },
}

pipetune_nodes_om:connect("object-added", function(om, node)
  if not is_internal_node(node) then
    return
  end

  local node_id = node["bound-id"]
  local owner_id = tonumber(proxy_property(node, "client.id"))
  pipetune_hidden_nodes[node_id] = { owner_id = owner_id }
  for client in pipetune_clients_om:iterate() do
    hide_node_from_client(client, node_id, owner_id)
  end
end)

pipetune_nodes_om:connect("object-removed", function(om, node)
  pipetune_hidden_nodes[node["bound-id"]] = nil
end)

pipetune_clients_om:connect("object-added", function(om, client)
  for node_id, hidden_node in pairs(pipetune_hidden_nodes) do
    hide_node_from_client(client, node_id, hidden_node.owner_id)
  end
end)

pipetune_nodes_om:activate()
pipetune_clients_om:activate()
)wpvis"};

constexpr auto kWirePlumber05NodeVisibilityConfiguration =
    std::string_view{R"wp05conf(# Managed by PipeTune.
wireplumber.components = [
  {
    name = "pipetune-node-visibility.lua"
    type = script/lua
    provides = policy.pipetune-node-visibility
  }
]

wireplumber.profiles = {
  main = {
    policy.pipetune-node-visibility = required
  }
}
)wp05conf"};

std::string_view wirePlumberNodeVisibilityPolicy() noexcept {
  return kWirePlumberNodeVisibilityPolicy;
}

std::string_view wirePlumber05NodeVisibilityConfiguration() noexcept {
  return kWirePlumber05NodeVisibilityConfiguration;
}

} // namespace pipetune
