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
pipetune_physical_outputs = {}
pipetune_aggregate_owners = {}
pipetune_public_inputs = {}

-- PipeWire keeps client permissions when WirePlumber exits. Persist each
-- override before denying access, so a new policy instance can restore it
-- even if the aggregate disappeared while WirePlumber was not running.
local permission_state = State("pipetune-output-visibility")
local saved_permissions = permission_state:load()
local pending_restores = {}
local server_cookie = tostring(Core.get_info().cookie)
local nodes_installed = false
local clients_installed = false

local function save_permissions()
  local ok, error = permission_state:save(saved_permissions)
  if not ok then
    Log.warning("cannot save PipeTune output permissions: " .. tostring(error))
  end
  return ok
end

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
  local hidden = pipetune_hidden_nodes[node_id]
  local private_output = next(pipetune_public_inputs) ~= nil and hidden and hidden.output
  if id ~= nil and pipetune_audio_stream_counts[id] ~= nil and not private_output then
    -- WirePlumber 0.4 cannot express PipeWire's link-only permission.
    -- Temporarily restore access so this client's stream can link.
    client:update_permissions { [node_id] = "all" }
  else
    client:update_permissions { [node_id] = "-" }
  end
end

local function permission_key(client, output)
  local serial = proxy_property(client, "object.serial")
  if serial == nil or output.serial == nil then return nil end
  -- Global IDs can be reused. Object serials are unique for this server's
  -- lifetime, and its cookie prevents replay after a PipeWire restart.
  return server_cookie .. ":" .. tostring(serial) .. ":" .. tostring(output.serial)
end

local function update_physical_permissions(client)
  if not nodes_installed or not clients_installed then return end
  local id = client_id(client)
  -- Desktop selectors and pipewire-pulse use unrestricted clients. Do not
  -- replace permissions managed by a portal or another restricted policy.
  if id == nil or proxy_property(client, "pipewire.access") ~= "unrestricted" then return end
  local owns_aggregate = false
  for _, owner_id in pairs(pipetune_aggregate_owners) do
    if id == owner_id then owns_aggregate = true end
  end
  local updates, added, hidden, restored = {}, {}, {}, {}
  for node_id, output in pairs(pipetune_physical_outputs) do
    local key = permission_key(client, output)
    if key ~= nil then
      local hide = next(pipetune_aggregate_owners) ~= nil and
          not is_wireplumber(client) and not is_pipetune(client) and
          not is_node_owner(client, output.owner_id) and not owns_aggregate
      if hide then
        updates[node_id] = "-"
        hidden[key] = true
        if saved_permissions[key] == nil then
          saved_permissions[key] = "all"
          added[key] = true
        end
      elseif saved_permissions[key] == "all" and pending_restores[key] == nil then
        updates[node_id] = "all"
        restored[key] = true
      end
    end
  end
  if next(added) ~= nil and not save_permissions() then
    for key in pairs(added) do saved_permissions[key] = nil end
    return
  end
  if next(updates) == nil then return end
  for key in pairs(hidden) do pending_restores[key] = nil end
  for key in pairs(restored) do pending_restores[key] = restored end
  client:update_permissions(updates)
  if next(restored) ~= nil then
    -- Keep recovery records until PipeWire has applied the restoration.
    -- A new denial invalidates this acknowledgement for that object pair.
    Core.sync(function(error)
      local changed = false
      for key in pairs(restored) do
        if pending_restores[key] == restored then
          pending_restores[key] = nil
          if error == nil then
            saved_permissions[key] = nil
            changed = true
          end
        end
      end
      if changed then save_permissions() end
    end)
  end
end

local function forget_permissions(client_serial, node_serial)
  local changed = false
  for key in pairs(saved_permissions) do
    local cookie, client, node = key:match("^(%d+):(%d+):(%d+)$")
    if cookie == server_cookie and
        (client == client_serial or node == node_serial) then
      saved_permissions[key] = nil
      pending_restores[key] = nil
      changed = true
    end
  end
  if changed then save_permissions() end
end

local function update_client_permissions(client)
  update_physical_permissions(client)
  for node_id, hidden_node in pairs(pipetune_hidden_nodes) do
    update_node_permissions(client, node_id, hidden_node.owner_id)
  end
end

local function update_all_client_permissions()
  for client in pipetune_clients_om:iterate() do
    update_client_permissions(client)
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
  local node_id = node["bound-id"]
  local owner_id = tonumber(proxy_property(node, "client.id"))
  local aggregate = proxy_property(node, "node.pipetune.aggregate")
  if aggregate == "true" or aggregate == true then
    pipetune_aggregate_owners[node_id] = owner_id or -1
    if proxy_property(node, "node.pipetune.public-input") == "true" then
      pipetune_public_inputs[node_id] = true
    end
    update_all_client_permissions()
    return
  end
  local virtual = proxy_property(node, "node.virtual")
  if proxy_property(node, "media.class") == "Audio/Sink" and
      virtual ~= "true" and virtual ~= true and not is_internal_node(node) then
    pipetune_physical_outputs[node_id] = {
      owner_id = owner_id,
      serial = proxy_property(node, "object.serial"),
    }
    update_all_client_permissions()
    return
  end
  if is_audio_stream(node) then
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

  pipetune_hidden_nodes[node_id] = {
    owner_id = owner_id,
    output = proxy_property(node, "media.class") == "Audio/Sink",
  }
  for client in pipetune_clients_om:iterate() do
    update_node_permissions(client, node_id, owner_id)
  end
end)

pipetune_nodes_om:connect("object-removed", function(om, node)
  local node_id = node["bound-id"]
  pipetune_hidden_nodes[node_id] = nil
  local output = pipetune_physical_outputs[node_id]
  if output ~= nil and output.serial ~= nil then
    forget_permissions(nil, tostring(output.serial))
  end
  pipetune_physical_outputs[node_id] = nil
  if pipetune_aggregate_owners[node_id] ~= nil then
    pipetune_aggregate_owners[node_id] = nil
    pipetune_public_inputs[node_id] = nil
    -- The aggregate's lifetime is the mode switch. This also runs when
    -- its client disappears without sending a shutdown request.
    update_all_client_permissions()
  end

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

pipetune_clients_om:connect("object-removed", function(om, client)
  local serial = proxy_property(client, "object.serial")
  if serial ~= nil then forget_permissions(tostring(serial), nil) end
end)

local function restore_initial_permissions()
  if not nodes_installed or not clients_installed then return end
  -- Wait for both complete inventories before deciding that an aggregate is
  -- absent, or that a saved object pair no longer exists.
  local live = {}
  for client in pipetune_clients_om:iterate() do
    for _, output in pairs(pipetune_physical_outputs) do
      local key = permission_key(client, output)
      if key ~= nil then live[key] = true end
    end
  end
  local changed = false
  for key in pairs(saved_permissions) do
    if live[key] == nil then
      saved_permissions[key] = nil
      changed = true
    end
  end
  if changed then save_permissions() end
  update_all_client_permissions()
end

pipetune_nodes_om:connect("installed", function()
  nodes_installed = true
  restore_initial_permissions()
end)
pipetune_clients_om:connect("installed", function()
  clients_installed = true
  restore_initial_permissions()
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
