/* pipetune - Engine and User Interface for Applied EffeTune DSP on a Linux Desktop
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/pipetune/
 */
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
pipetune_audio_stream_owners = {}
pipetune_audio_stream_counts = {}

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

local function is_audio_stream(node)
  local media_class = proxy_property(node, "media.class")
  return media_class == "Stream/Output/Audio" or
      media_class == "Stream/Input/Audio"
end

local function is_wireplumber(client)
  return proxy_property(client, "wireplumber.daemon") ~= nil
end

local function is_pipetune(client)
  return proxy_property(client, "application.process.binary") == "pipetune"
end

local function client_id(client)
  return tonumber(client["bound-id"])
end

local function is_node_owner(client, owner_id)
  local id = client_id(client)
  return id ~= nil and owner_id ~= nil and id == owner_id
end

local function update_node_permissions(client, node_id, owner_id)
  if is_wireplumber(client) or is_pipetune(client) or
      is_node_owner(client, owner_id) then
    return
  end

  local id = client_id(client)
  if id ~= nil and pipetune_audio_stream_counts[id] ~= nil then
    -- WirePlumber 0.4 cannot express PipeWire's link-only permission.
    -- Temporarily restore access so this client's stream can link.
    client:update_permissions { [node_id] = "all" }
  else
    client:update_permissions { [node_id] = "-" }
  end
end

local function update_client_permissions(client)
  for node_id, hidden_node in pairs(pipetune_hidden_nodes) do
    update_node_permissions(client, node_id, hidden_node.owner_id)
  end
end

local function update_client_by_id(id)
  for client in pipetune_clients_om:iterate() do
    if client_id(client) == id then
      update_client_permissions(client)
      return
    end
  end
end

pipetune_nodes_om = ObjectManager {
  Interest { type = "node" },
}
pipetune_clients_om = ObjectManager {
  Interest { type = "client" },
}

pipetune_nodes_om:connect("object-added", function(om, node)
  if is_audio_stream(node) then
    local node_id = node["bound-id"]
    local owner_id = tonumber(proxy_property(node, "client.id"))
    if owner_id ~= nil then
      pipetune_audio_stream_owners[node_id] = owner_id
      pipetune_audio_stream_counts[owner_id] =
          (pipetune_audio_stream_counts[owner_id] or 0) + 1
      update_client_by_id(owner_id)
    end
    return
  end

  if not is_internal_node(node) then
    return
  end

  local node_id = node["bound-id"]
  local owner_id = tonumber(proxy_property(node, "client.id"))
  pipetune_hidden_nodes[node_id] = { owner_id = owner_id }
  for client in pipetune_clients_om:iterate() do
    update_node_permissions(client, node_id, owner_id)
  end
end)

pipetune_nodes_om:connect("object-removed", function(om, node)
  local node_id = node["bound-id"]
  pipetune_hidden_nodes[node_id] = nil

  local owner_id = pipetune_audio_stream_owners[node_id]
  if owner_id == nil then
    return
  end
  pipetune_audio_stream_owners[node_id] = nil
  local count = pipetune_audio_stream_counts[owner_id] or 0
  if count <= 1 then
    pipetune_audio_stream_counts[owner_id] = nil
    update_client_by_id(owner_id)
  else
    pipetune_audio_stream_counts[owner_id] = count - 1
  end
end)

pipetune_clients_om:connect("object-added", function(om, client)
  update_client_permissions(client)
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
