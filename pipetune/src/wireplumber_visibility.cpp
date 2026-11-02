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
pipetune_other_outputs = {}
pipetune_aggregate_owners = {}

-- PipeWire keeps client permissions when WirePlumber exits. Persist each
-- override before denying access, so a new policy instance can restore it
-- even if the aggregate disappeared while WirePlumber was not running.
local permission_state = State("pipetune-output-visibility")
local saved_permissions = permission_state:load()
local pending_restores = {}
local server_cookie = tostring(Core.get_info().cookie)
local nodes_installed = false
local clients_installed = false
local volume_restoring = false

-- Keep the user's configured preference separate from the effective fallback.
-- WirePlumber owns this lease so a crashed processing client cannot strand the
-- configured default on a logical sink that no longer exists.
local default_state = State("pipetune-default-output")
local saved_default = default_state:load()
local managed_defaults = {}
local public_sinks = {}
local default_metadata = nil
local default_restoring = false
local default_update_pending = false
local setting_default = false
local configured_key = "default.configured.audio.sink"

-- Restore a newly started daemon once. A surviving daemon owns its current
-- controls, including when this policy restarts before a delayed save finishes.
local master_state = State("pipetune-master-output")
local saved_master = master_state:load()
local master_save = nil
local pending_masters = {}

local function finite_gain(value)
  return type(value) == "number" and value >= 0 and value <= 3.4028234663852886e38
end

local function output_name(value)
  if value == nil then return nil end
  local json = Json.Raw(value)
  if json == nil or not json:is_object() then return nil end
  local ok, parsed = pcall(function() return json:parse() end)
  if ok and type(parsed) == "table" and type(parsed.name) == "string" then return parsed.name end
  return nil
end

local function output_volume(name)
  if name == nil then return nil end
  for node in pipetune_nodes_om:iterate() do
    if node.properties["node.name"] == name then
      for param in node:iterate_params("Props") do
        local props = param:parse().properties
        if props then
          local gain = props.volume or 1.0
          if type(props.channelVolumes) == "table" and #props.channelVolumes > 0 then
            local channel = 0.0
            for _, value in ipairs(props.channelVolumes) do
              if not finite_gain(value) then return nil end
              channel = math.max(channel, value)
            end
            gain = gain * channel
          end
          if finite_gain(gain) then return gain, props.mute == true end
        end
      end
    end
  end
  return nil
end

local function restore_pending_masters()
  if default_metadata == nil or default_metadata:find(0, "default.audio.sink") == nil then return end
  for node, restore in pairs(pending_masters) do
    restore()
    pending_masters[node] = nil
  end
end

local function store_master(name, gain, muted)
  local gain_key, mute_key = name .. ":gain", name .. ":mute"
  local gain_value, mute_value = string.format("%.17g", gain), tostring(muted)
  if saved_master[gain_key] == gain_value and saved_master[mute_key] == mute_value then return end
  saved_master[gain_key], saved_master[mute_key] = gain_value, mute_value
  if master_save then master_save:destroy() end
  master_save = Core.timeout_add(1000, function()
    master_save = nil
    local ok, error = master_state:save(saved_master)
    if not ok then Log.warning("cannot save PipeTune master output: " .. tostring(error)) end
    return false
  end)
end

local function monitor_master(node)
  local properties = node.properties
  local mode = properties["node.pipetune.master-state"]
  local name = properties["node.name"]
  if name == nil or (mode ~= "restore" and mode ~= "retained") then return end
  local restoring = mode == "restore"
  local gain = tonumber(saved_master[name .. ":gain"])
  local muted = saved_master[name .. ":mute"] == "true"
  local function changed(node, id)
    if id ~= "Props" then return end
    for param in node:iterate_params("Props") do
      local parsed = param:parse()
      if parsed.pod_type == "Object" and parsed.object_id == "Props" then
        local props = parsed.properties
        local channels = props.channelVolumes
        if props.volume == 1.0 and type(props.mute) == "boolean" and
            type(channels) == "table" and #channels == 2 and
            finite_gain(channels[1]) and channels[1] == channels[2] then
          -- Ignore the initial cached defaults until the requested controls
          -- have made a complete round trip through PipeTune's normalization.
          if restoring and (gain == nil or props.mute ~= muted or
              math.abs(channels[1] - gain) > math.max(1, gain) * 0.000001) then return end
          restoring = false
          store_master(name, channels[1], props.mute)
        end
      end
    end
  end
  node:connect("params-changed", changed)
  if restoring then
    pending_masters[node] = function()
      if not finite_gain(gain) then
        gain = tonumber(saved_default["inherited-gain"])
        muted = saved_default["inherited-mute"] == "true"
        if not finite_gain(gain) then
          gain, muted = output_volume(output_name(default_metadata:find(0, "default.audio.sink")))
        end
      end
      -- With no previous output there is no OS volume to inherit.
      if not finite_gain(gain) then gain, muted = 1.0, false end
      -- State values may parse as integers; Float arrays need Lua floats.
      gain = gain + 0.0
      node:set_param("Props", Pod.Object {
        "Spa:Pod:Object:Param:Props", "Props",
        volume = 1.0, mute = muted,
        channelVolumes = Pod.Array { "Spa:Float", gain, gain },
      })
    end
    Core.sync(function(error) if error == nil then restore_pending_masters() end end)
  else
    changed(node, "Props")
  end
end

local function save_default()
  local ok, error = default_state:save(saved_default)
  if not ok then Log.warning("cannot save PipeTune default output: " .. tostring(error)) end
  return ok
end

local function update_default_output()
  if not nodes_installed or default_metadata == nil or default_restoring then return end
  -- In 0.4 the metadata object appears before the default-node policy starts
  -- listening. Its first effective selection is the initialization barrier.
  if default_metadata:find(0, "default.audio.sink") == nil then return end
  local _, name = next(managed_defaults)
  local current, current_type = default_metadata:find(0, configured_key)
  if name ~= nil then
    local value = Json.Object { name = name }:get_data()
    if saved_default["aggregate"] == nil then
      local gain, muted = output_volume(output_name(default_metadata:find(0, "default.audio.sink")))
      saved_default["inherited-gain"] = gain and string.format("%.17g", gain) or nil
      saved_default["inherited-mute"] = muted and "true" or "false"
      saved_default["aggregate"] = value
      saved_default["previous-present"] = current ~= nil and "true" or "false"
      saved_default["previous-value"] = current
      saved_default["previous-type"] = current_type
      if not save_default() then
        saved_default = default_state:load()
        return
      end
    end
    local selected = saved_default["selected-value"] or value
    if output_name(current) ~= output_name(selected) then
      setting_default = true
      default_metadata:set(0, configured_key, "Spa:String:JSON", selected)
      setting_default = false
    end
  elseif saved_default["aggregate"] ~= nil then
    if next(public_sinks) ~= nil or volume_restoring then return end
    default_restoring = true
    if output_name(current) == output_name(saved_default["aggregate"]) or
        (saved_default["selected-managed"] == "true" and
         output_name(current) == output_name(saved_default["selected-value"])) then
      local previous = saved_default["previous-present"] == "true" and
          saved_default["previous-value"] or nil
      default_metadata:set(0, configured_key, saved_default["previous-type"], previous)
    end
    -- Keep recovery data until the server acknowledges the restoration.
    Core.sync(function(error)
      default_restoring = false
      if error == nil and next(managed_defaults) == nil then
        saved_default = {
          ["selected-value"] = saved_default["selected-value"],
          ["selected-managed"] = saved_default["selected-managed"],
        }
        save_default()
      end
    end)
  end
  restore_pending_masters()
end

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

local function audio_stream_direction(node)
  local media_class = proxy_property(node, "media.class")
  if media_class == "Stream/Output/Audio" then return "output" end
  if media_class == "Stream/Input/Audio" then return "input" end
  return nil
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
  local private_output = next(pipetune_aggregate_owners) ~= nil and hidden and hidden.output
  local counts = id ~= nil and pipetune_audio_stream_counts[id] or nil
  local direction = hidden and hidden.output and "output" or "input"
  if counts ~= nil and counts[direction] > 0 and not private_output then
    -- WirePlumber 0.4 cannot express PipeWire's link-only permission.
    -- Restore only the direction needed by this client's streams. An input
    -- peak meter in a desktop selector must not reveal internal outputs.
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

local function update_output_permissions(client)
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
  for node_id, output in pairs(pipetune_other_outputs) do
    local key = permission_key(client, output)
    if key ~= nil then
      local hide = (next(pipetune_aggregate_owners) ~= nil or volume_restoring) and
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
  update_output_permissions(client)
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

-- Own physical output gains for the lifetime of selected distribution streams.
-- Persist before changing hardware, and use Route.save=false so temporary unity
-- is never written into WirePlumber's ordinary single-output route state.
local device_volume_state = State("pipetune-device-volumes")
local saved_device_volumes = device_volume_state:load()
local volume_leases = {}
local volume_streams = {}
local volume_buses = {}
local volume_devices = {}
local volume_devices_installed = false
local volume_update_pending = false
local volume_update
local volume_owners = {}
local volume_owner_clients = {}
local volume_owner_modes = {}

local function queue_volume_update()
  if volume_update_pending then return end
  volume_update_pending = true
  Core.sync(function(error)
    volume_update_pending = false
    if error == nil then volume_update() end
  end)
end

local function read_volume(node)
  for pod in node:iterate_params("Props") do
    local props = pod:parse().properties
    if props and type(props.mute) == "boolean" and type(props.channelVolumes) == "table" and
        #props.channelVolumes > 0 and #props.channelVolumes <= 16 and finite_gain(props.volume or 1.0) then
      local channels = {}
      for _, gain in ipairs(props.channelVolumes) do
        if not finite_gain(gain) then return nil end
        table.insert(channels, gain + 0.0)
      end
      return { volume = (props.volume or 1.0) + 0.0, mute = props.mute, channels = channels }
    end
  end
  return nil
end

local function volume_equal(actual, expected)
  if actual == nil or expected == nil or actual.mute ~= expected.mute or
      #actual.channels ~= #expected.channels then return false end
  for index, value in ipairs(actual.channels) do
    local a, b = value * actual.volume, expected.channels[index] * expected.volume
    if math.abs(a - b) > math.max(1.0, math.abs(b)) * 0.00001 then return false end
  end
  return true
end

local function unity_volume(channels, muted)
  local values = {}
  for index = 1, channels do values[index] = 1.0 end
  return { volume = 1.0, mute = muted, channels = values }
end

local function volume_pod(value)
  local channels = { "Spa:Float" }
  for _, gain in ipairs(value.channels) do table.insert(channels, gain + 0.0) end
  return Pod.Object { "Spa:Pod:Object:Param:Props", "Props",
    volume = value.volume + 0.0, mute = value.mute, channelVolumes = Pod.Array(channels) }
end

local function volume_failure(lease, message)
  if lease.failure == nil then
    lease.failure = message
    Log.warning("PipeTune output volume: " .. message)
  end
  queue_volume_update()
end

local function watch_volume_request(lease, message)
  if lease.timer then lease.timer:destroy() end
  lease.timer = Core.timeout_add(3000, function()
    lease.timer = nil
    volume_failure(lease, message)
    return false
  end)
end

local function acknowledge_volume(lease)
  lease.pending = nil
  if lease.timer then lease.timer:destroy(); lease.timer = nil end
end

local function gate_volume_stream(stream, open)
  local expected = unity_volume(stream.channels, not open)
  if volume_equal(read_volume(stream.node), expected) then
    acknowledge_volume(stream)
    return true
  end
  if stream.failure ~= nil then return false end
  if stream.pending == open then return false end
  stream.pending = open
  -- 0.4's Lua binding returns no value; only readback acknowledges a write.
  if stream.node:set_param("Props", volume_pod(expected)) == false then
    volume_failure(stream, "cannot gate distribution stream " .. stream.name)
  else
    watch_volume_request(stream, "distribution stream did not confirm its gain: " .. stream.name)
  end
  return false
end

local function physical_target(node)
  local properties = node.properties
  local device = volume_devices[tonumber(properties["device.id"])]
  local profile_device = tonumber(properties["card.profile.device"])
  local route = nil
  if device and profile_device then
    for pod in device:iterate_params("Route") do
      local value = pod:parse().properties
      if value and value.direction == "Output" and value.device == profile_device then route = value; break end
    end
  end
  local route_name = route and route.name or ""
  if route and route_name == "" then
    for pod in device:iterate_params("EnumRoute") do
      local value = pod:parse().properties
      if value and value.direction == "Output" and value.index == route.index then route_name = value.name; break end
    end
    if route_name == "" then return nil end
  end
  local name = properties["node.name"]
  if name == nil then return nil end
  -- Names and route identities survive numeric ID reuse and reconnection.
  local key = Json.Array { name, device and device.properties["device.name"] or "",
    properties["node.device.profile.name"] or "", route_name,
    device and (device.properties["device.serial"] or device.properties["device.bus-path"]) or "" }:get_data()
  return { node = node, device = device, route = route, key = key, name = name }
end

local function set_physical_volume(target, value)
  if target.route then
    return target.device:set_param("Route", Pod.Object { "Spa:Pod:Object:Param:Route", "Route",
      index = target.route.index, device = target.route.device, save = false, props = volume_pod(value) })
  end
  return target.node:set_param("Props", volume_pod(value))
end

local function save_device_volumes()
  local ok, error = device_volume_state:save(saved_device_volumes)
  if not ok then Log.warning("cannot save PipeTune device volumes: " .. tostring(error)) end
  return ok
end

volume_update = function()
  if not nodes_installed or not clients_installed or not volume_devices_installed then return end
  local targets, desired, status, failures = {}, {}, {}, {}
  local active_buses = {}
  for _, bus in pairs(volume_buses) do active_buses[bus.owner] = true end
  for node in pipetune_nodes_om:iterate() do
    local name = node.properties["node.name"]
    if volume_owners[name] then
      volume_owner_clients[name] = tonumber(node.properties["client.id"])
      volume_owner_modes[name] = node.properties["node.pipetune.public-input"] == "true"
    end
    if node.properties["media.class"] == "Audio/Sink" and
        not is_internal_node(node) and node.properties["node.pipetune.public-sink"] ~= "true" then
      local target = physical_target(node)
      if target then targets[tostring(node.properties["object.serial"])] = target end
    end
  end
  for _, stream in pairs(volume_streams) do
    local target = targets[stream.serial]
    volume_owners[stream.owner] = true
    if target then
      local selection = desired[target.key] or { target = target, streams = {}, owner = stream.owner }
      table.insert(selection.streams, stream)
      desired[target.key] = selection
    else
      gate_volume_stream(stream, false)
      status[stream.owner] = "pending"
    end
    if stream.failure then failures[stream.owner] = stream.failure end
  end
  -- A replacement bus declares its selections before its streams are bound.
  -- Keep those leases across stream recreation, including idle output changes.
  for _, bus in pairs(volume_buses) do
    for serial in string.gmatch(bus.targets, "[^,]+") do
      local target = targets[serial]
      if target and not desired[target.key] then
        desired[target.key] = { target = target, streams = {}, owner = bus.owner }
        status[bus.owner] = "pending"
      end
    end
  end
  local restoring = false
  for _, target in pairs(targets) do
    local selection = desired[target.key]
    local serialized = saved_device_volumes[target.key]
    local current = read_volume(target.node)
    local lease = volume_leases[target.key]
    if selection or serialized then
      if not lease then
        local snapshot = nil
        if serialized then
          local ok, decoded = pcall(function() return Json.Raw(serialized):parse() end)
          if ok then snapshot = decoded end
        elseif current then
          snapshot = { owner = selection.owner, original = current }
          saved_device_volumes[target.key] = Json.Object { owner = snapshot.owner,
            original = Json.Object { volume = current.volume, mute = current.mute,
              channels = Json.Array(current.channels) } }:get_data()
          if not save_device_volumes() then saved_device_volumes[target.key] = nil; snapshot = nil end
        end
        if snapshot and snapshot.original and snapshot.owner then
          lease = { snapshot = snapshot }
          volume_leases[target.key] = lease
        else
          for _, stream in ipairs(selection and selection.streams or {}) do
            gate_volume_stream(stream, false)
            volume_failure(stream, "cannot snapshot output gain: " .. target.name)
          end
        end
      end
      if lease then
        local owner = lease.snapshot.owner
        volume_owners[owner] = true
        -- ObjectManager binds replacement nodes asynchronously. An empty
        -- stream inventory during that gap is not a deselection. The persistent
        -- daemon client, a new bus roster, or an explicit single-mode input
        -- distinguishes rebuilding from actual release, without a sleep timer.
        if not selection and not active_buses[owner] and volume_owner_modes[owner] ~= false then
          for client in pipetune_clients_om:iterate() do
            if client_id(client) == volume_owner_clients[owner] then
              selection = { streams = {}, owner = owner }
              restoring = true
              status[owner] = "pending"
              break
            end
          end
        end
        local expected = selection and unity_volume(#lease.snapshot.original.channels, false) or lease.snapshot.original
        if lease.selected ~= (selection ~= nil) then
          acknowledge_volume(lease)
          lease.failure = nil
          lease.selected = selection ~= nil
        end
        if volume_equal(current, expected) then
          acknowledge_volume(lease)
          if selection then
            for _, stream in ipairs(selection.streams) do
              if not gate_volume_stream(stream, true) then status[owner] = "pending" end
            end
          else
            saved_device_volumes[target.key] = nil
            if save_device_volumes() then volume_leases[target.key] = nil
            else
              saved_device_volumes[target.key] = serialized
              volume_failure(lease, "cannot acknowledge restored output: " .. target.name)
            end
          end
        else
          status[owner] = "pending"
          local gated = true
          for _, stream in ipairs(selection and selection.streams or {}) do
            if not gate_volume_stream(stream, false) then gated = false end
          end
          if gated and lease.pending == nil and lease.failure == nil then
            lease.pending = true
            if set_physical_volume(target, expected) == false then
              volume_failure(lease, "cannot set output gain: " .. target.name)
            else
              watch_volume_request(lease, "output did not confirm " .. (selection and "unity gain: " or "restored gain: ") .. target.name)
            end
          end
        end
        if lease.failure then failures[owner] = lease.failure end
        if not selection and volume_leases[target.key] ~= nil then restoring = true end
      end
    end
  end
  -- Keep desktop access and the default-output lease until restoration is
  -- acknowledged. Disconnected/inactive routes retain their recovery record.
  if volume_restoring ~= restoring then
    volume_restoring = restoring
    update_all_client_permissions()
    update_default_output()
  end
  if default_metadata then
    for owner in pairs(volume_owners) do
      local serial = nil
      for node in pipetune_nodes_om:iterate() do
        if node.properties["node.name"] == owner .. ".distribution" then
          serial = math.max(serial or 0, tonumber(node.properties["object.serial"]))
        end
      end
      local state = failures[owner] and "error:" .. failures[owner] or
        status[owner] or (serial and "ready:" .. tostring(serial) or "restored")
      default_metadata:set(0, "pipetune.volume." .. owner, "Spa:String", state)
    end
  end
end

-- Core only keeps a weak reference. Keep this manager alive after script
-- initialization so later cards are resolved through their hardware Routes.
volume_devices_om = ObjectManager { Interest { type = "device" } }
volume_devices_om:connect("object-added", function(om, device)
  volume_devices[device["bound-id"]] = device
  device:connect("params-changed", function(node, id)
    if id == "Route" or id == "Profile" then queue_volume_update() end
  end)
  queue_volume_update()
end)
volume_devices_om:connect("object-removed", function(om, device)
  volume_devices[device["bound-id"]] = nil
  queue_volume_update()
end)
volume_devices_om:connect("installed", function()
  volume_devices_installed = true
  queue_volume_update()
end)

pipetune_nodes_om = ObjectManager {
  Interest { type = "node" },
}
pipetune_clients_om = ObjectManager {
  Interest { type = "client" },
}

pipetune_nodes_om:connect("object-added", function(om, node)
  local node_id = node["bound-id"]
  local owner_id = tonumber(proxy_property(node, "client.id"))
  if proxy_property(node, "node.pipetune.target-serial") then
    volume_streams[node_id] = { node = node, serial = tostring(proxy_property(node, "node.pipetune.target-serial")),
      channels = tonumber(proxy_property(node, "node.pipetune.output-channels")),
      name = proxy_property(node, "node.pipetune.output-id"), owner = proxy_property(node, "node.pipetune.public-target") }
  end
  node:connect("params-changed", function(node, id) if id == "Props" then queue_volume_update() end end)
  queue_volume_update()
  if proxy_property(node, "node.pipetune.public-sink") == "true" then
    public_sinks[node_id] = proxy_property(node, "node.name")
  end
  local aggregate = proxy_property(node, "node.pipetune.aggregate")
  if aggregate == "true" or aggregate == true then
    local volume_owner = proxy_property(node, "node.pipetune.public-target")
    if volume_owner then
      volume_owners[volume_owner] = true
      volume_buses[node_id] = { owner = volume_owner, targets = proxy_property(node, "node.pipetune.volume-targets") or "" }
    end
    monitor_master(node)
    pipetune_aggregate_owners[node_id] = owner_id or -1
    if proxy_property(node, "node.pipetune.manage-default") == "true" then
      -- A private distribution node keeps the public target's lease alive
      -- while rate renegotiation replaces the public input stream.
      managed_defaults[node_id] = proxy_property(node, "node.pipetune.public-target") or
          proxy_property(node, "node.name")
      update_default_output()
    end
    update_all_client_permissions()
    if not is_internal_node(node) then return end
  end
  if proxy_property(node, "node.pipetune.public-sink") == "true" then return end
  if proxy_property(node, "media.class") == "Audio/Sink" and
      not is_internal_node(node) then
    pipetune_other_outputs[node_id] = {
      owner_id = owner_id,
      serial = proxy_property(node, "object.serial"),
    }
    update_all_client_permissions()
    return
  end
  local direction = audio_stream_direction(node)
  if direction ~= nil then
    if owner_id ~= nil then
      pipetune_audio_stream_owners[node_id] = { id = owner_id, direction = direction }
      local counts = pipetune_audio_stream_counts[owner_id] or { input = 0, output = 0 }
      counts[direction] = counts[direction] + 1
      pipetune_audio_stream_counts[owner_id] = counts
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
  pending_masters[node] = nil
  local node_id = node["bound-id"]
  if volume_streams[node_id] then acknowledge_volume(volume_streams[node_id]); volume_streams[node_id] = nil end
  volume_buses[node_id] = nil
  -- Removal precedes graph reconciliation; keep hidden outputs inaccessible
  -- until the saved gains of any still-live devices have been restored.
  if next(volume_leases) ~= nil then volume_restoring = true end
  queue_volume_update()
  if public_sinks[node_id] then
    public_sinks[node_id] = nil
    update_default_output()
  end
  pipetune_hidden_nodes[node_id] = nil
  local output = pipetune_other_outputs[node_id]
  if output ~= nil and output.serial ~= nil then
    forget_permissions(nil, tostring(output.serial))
  end
  pipetune_other_outputs[node_id] = nil
  if pipetune_aggregate_owners[node_id] ~= nil then
    pipetune_aggregate_owners[node_id] = nil
    managed_defaults[node_id] = nil
    update_default_output()
    -- The aggregate's lifetime is the mode switch. This also runs when
    -- its client disappears without sending a shutdown request.
    update_all_client_permissions()
  end

  local owner = pipetune_audio_stream_owners[node_id]
  if owner == nil then
    return
  end
  pipetune_audio_stream_owners[node_id] = nil
  local counts = pipetune_audio_stream_counts[owner.id]
  counts[owner.direction] = counts[owner.direction] - 1
  if counts.input == 0 and counts.output == 0 then
    pipetune_audio_stream_counts[owner.id] = nil
  end
  update_client_by_id(owner.id)
end)

pipetune_clients_om:connect("object-added", function(om, client)
  update_client_permissions(client)
end)

pipetune_clients_om:connect("object-removed", function(om, client)
  queue_volume_update()
  local serial = proxy_property(client, "object.serial")
  if serial ~= nil then forget_permissions(tostring(serial), nil) end
end)

local function restore_initial_permissions()
  if not nodes_installed or not clients_installed then return end
  -- Wait for both complete inventories before deciding that an aggregate is
  -- absent, or that a saved object pair no longer exists.
  local live = {}
  for client in pipetune_clients_om:iterate() do
    for _, output in pairs(pipetune_other_outputs) do
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
  queue_volume_update()
  update_default_output()
  restore_initial_permissions()
end)
pipetune_clients_om:connect("installed", function()
  clients_installed = true
  queue_volume_update()
  restore_initial_permissions()
end)

local defaults_om = ObjectManager {
  Interest { type = "metadata", Constraint { "metadata.name", "=", "default" } },
}
defaults_om:connect("object-added", function(om, metadata)
  default_metadata = metadata
  queue_volume_update()
  metadata:connect("changed", function(m, subject, key, value_type, value)
    if subject ~= 0 then return end
    if key == configured_key and not default_restoring and not setting_default and
        (saved_default["aggregate"] ~= nil or saved_default["selected-value"] ~= nil) then
      -- An OS choice takes precedence over PipeTune's initial selection.
      -- Retain it across mode changes and daemon restarts. A choice made
      -- after all PipeTune outputs have closed supersedes the saved choice.
      local previous = default_state:load()
      if saved_default["aggregate"] == nil and next(public_sinks) == nil then
        saved_default["selected-value"] = nil
        saved_default["selected-managed"] = nil
      else
        saved_default["selected-value"] = value
        saved_default["selected-managed"] = nil
        for _, public_name in pairs(public_sinks) do
          if public_name == output_name(value) then saved_default["selected-managed"] = "true" end
        end
      end
      if not save_default() then saved_default = previous; return end
    end
    if (key == configured_key or key == "default.audio.sink") and not default_update_pending then
      -- ImplMetadata dispatches changes synchronously. Rewriting inside that
      -- signal can deliver the nested value before the original notification
      -- reaches other clients. Apply the decision after this dispatch ends.
      default_update_pending = true
      Core.sync(function(error)
        default_update_pending = false
        if error == nil then update_default_output() end
      end)
    end
  end)
  update_default_output()
end)
defaults_om:connect("object-removed", function(om, metadata)
  if default_metadata == metadata then default_metadata = nil end
end)

pipetune_nodes_om:activate()
pipetune_clients_om:activate()
defaults_om:activate()
volume_devices_om:activate()
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
