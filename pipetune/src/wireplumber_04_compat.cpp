/* pipetune - Engine and User Interface for Applied EffeTune DSP on a Linux Desktop
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/pipetune/
 */
#include "wireplumber_04_compat.h"

#include <string_view>

namespace pipetune {

constexpr auto kWirePlumber04Policy = std::string_view{R"wp04lua(-- Managed by PipeTune.
-- WirePlumber 0.5 ignores policy.lua.d and uses PipeTune's smart-filter
-- properties directly. WirePlumber 0.4 uses these endpoints and compatibility
-- scripts to insert the same filter without changing the default audio device.

load_script("pipetune-node-visibility.lua")

default_policy.policy.roles = default_policy.policy.roles or {}
default_policy.policy.roles["PipeTune-Playback"] = {
  ["alias"] = {
    "Default", "Multimedia", "Movie", "Music", "Game", "Notification",
    "Communication", "Speech", "Production", "Accessibility", "Test",
  },
  ["priority"] = 25,
  ["action.default"] = "mix",
}
default_policy.policy.roles["PipeTune-Capture"] = {
  ["alias"] = {
    "Default", "Capture", "Communication", "Speech", "Production", "Test",
  },
  ["priority"] = 25,
  ["action.default"] = "mix",
  ["media.class"] = "Audio/Source",
}
default_policy.policy.roles["PipeTune-Filter-Output"] = {
  ["priority"] = 25,
  ["action.default"] = "mix",
}

default_policy.endpoints = default_policy.endpoints or {}
default_policy.endpoints["endpoint.pipetune.playback"] = {
  ["media.class"] = "Audio/Sink",
  ["role"] = "PipeTune-Playback",
  ["priority"] = 1000,
}
default_policy.endpoints["endpoint.pipetune.capture"] = {
  ["media.class"] = "Audio/Source",
  ["role"] = "PipeTune-Capture",
  ["priority"] = 1000,
}

local pipetune_enable_default_policy = default_policy.enable
default_policy.enable = function()
  pipetune_enable_default_policy()

  if components["policy-endpoint-client.lua"] then
    components["policy-endpoint-client.lua"] = nil
    load_script("pipetune-endpoint-client.lua", default_policy.policy)
  end
  if components["policy-endpoint-device.lua"] then
    components["policy-endpoint-device.lua"] = nil
    load_script("pipetune-endpoint-device.lua", default_policy.policy)
  end
end
)wp04lua"};
constexpr auto kWirePlumber04EndpointClientPolicy =
    std::string_view{R"wp04lua(-- Managed by PipeTune for WirePlumber 0.4 compatibility.
-- Derived from WirePlumber 0.4.17 policy-endpoint-client.lua.
-- WirePlumber
--
-- Copyright © 2021 Collabora Ltd.
--    @author Julian Bouzas <julian.bouzas@collabora.com>
--
-- SPDX-License-Identifier: MIT

-- Receive script arguments from config.lua
local config = ... or {}
config.roles = config.roles or {}

local self = {}
self.scanning = false
self.pending_rescan = false
self.syncing = false
self.pending_sync = false

function rescan ()
  for si in linkables_om:iterate() do
    handleLinkable (si)
  end
end

function scheduleRescan ()
  if self.scanning then
    self.pending_rescan = true
    return
  end

  self.scanning = true
  rescan ()
  self.scanning = false

  if self.pending_rescan then
    self.pending_rescan = false
    Core.sync(function ()
      scheduleRescan ()
    end)
  end
end

function scheduleSynchronizedRescan ()
  if self.syncing then
    self.pending_sync = true
    return
  end

  self.syncing = true
  Core.sync(function ()
    self.syncing = false
    scheduleRescan ()

    if self.pending_sync then
      self.pending_sync = false
      scheduleSynchronizedRescan ()
    end
  end)
end

function findRole(role, tmc)
  if role and not config.roles[role] then
    -- find the role with matching alias
    for r, p in pairs(config.roles) do
      -- default media class can be overridden in the role config data
      mc = p["media.class"] or "Audio/Sink"
      if (type(p.alias) == "table" and tmc == mc) then
        for i = 1, #(p.alias), 1 do
          if role == p.alias[i] then
            return r
          end
        end
      end
    end

    -- otherwise get the lowest priority role
    local lowest_priority_p = nil
    local lowest_priority_r = nil
    for r, p in pairs(config.roles) do
      mc = p["media.class"] or "Audio/Sink"
      if tmc == mc and (lowest_priority_p == nil or
          p.priority < lowest_priority_p.priority) then
        lowest_priority_p = p
        lowest_priority_r = r
      end
    end
    return lowest_priority_r
  end
  return role
end

function findTargetEndpoint (node, media_class, role)
  local target_class_assoc = {
    ["Stream/Input/Audio"] = "Audio/Source",
    ["Stream/Output/Audio"] = "Audio/Sink",
    ["Stream/Input/Video"] = "Video/Source",
  }
  local media_role = nil
  local highest_priority = -1
  local target = nil

  -- get target media class
  local target_media_class = target_class_assoc[media_class]
  if not target_media_class then
    return nil
  end

  -- find highest priority endpoint by role
  media_role = findRole(role, target_media_class)
  for si_target_ep in endpoints_om:iterate {
    Constraint { "role", "=", media_role, type = "pw-global" },
    Constraint { "media.class", "=", target_media_class, type = "pw-global" },
  } do
    local priority = tonumber(si_target_ep.properties["priority"])
    if priority > highest_priority then
      highest_priority = priority
      target = si_target_ep
    end
  end

  return target
end

function createLink (si, si_target_ep)
  local out_item = nil
  local in_item = nil
  local si_props = si.properties
  local target_ep_props = si_target_ep.properties

  if si_props["item.node.direction"] == "output" then
    -- playback
    out_item = si
    in_item = si_target_ep
  else
    -- capture
    out_item = si_target_ep
    in_item = si
  end

  Log.info (string.format("link %s <-> %s",
      tostring(si_props["node.name"]),
      tostring(target_ep_props["name"])))

  -- create and configure link
  local si_link = SessionItem ( "si-standard-link" )
  if not si_link:configure {
    ["out.item"] = out_item,
    ["in.item"] = in_item,
    ["out.item.port.context"] = "output",
    ["in.item.port.context"] = "input",
    ["is.policy.endpoint.client.link"] = true,
    ["media.role"] = target_ep_props["role"],
    ["target.media.class"] = target_ep_props["media.class"],
    ["item.plugged.usec"] = si_props["item.plugged.usec"],
  } then
    Log.warning (si_link, "failed to configure si-standard-link")
    return
  end

  -- register
  si_link:register()
end

function checkLinkable (si)
  -- only handle session items that has a node associated proxy
  local node = si:get_associated_proxy ("node")
  if not node or not node.properties then
    return false
  end

  -- only handle stream session items
  local media_class = node.properties["media.class"]
  if not media_class or not string.find (media_class, "Stream") then
    return false
  end

  -- Determine if we can handle item by this policy
  if endpoints_om:get_n_objects () == 0 then
    Log.debug (si, "item won't be handled by this policy")
    return false
  end

  return true
end

function handleLinkable (si)
  if not checkLinkable (si) then
    return
  end

  local node = si:get_associated_proxy ("node")
  local media_class = node.properties["media.class"] or ""
  local media_role = node.properties["media.role"] or "Default"
  Log.info (si, "handling item " .. tostring(node.properties["node.name"]) ..
      " with role " .. media_role)

  -- find proper target endpoint
  local si_target_ep = findTargetEndpoint (node, media_class, media_role)
  if not si_target_ep then
    Log.info (si, "... target endpoint not found")
    return
  end

  -- Check if item is linked to proper target, otherwise re-link
  for link in links_om:iterate() do
    local out_id = tonumber(link.properties["out.item.id"])
    local in_id = tonumber(link.properties["in.item.id"])
    if out_id == si.id or in_id == si.id then
      local is_out = out_id == si.id and true or false
      for peer_ep in endpoints_om:iterate() do
        if peer_ep.id == (is_out and in_id or out_id) then

          if peer_ep.id == si_target_ep.id then
            Log.info (si, "... already linked to proper target endpoint")
            return
          end

          -- remove old link if active, otherwise schedule rescan
          if ((link:get_active_features() & Feature.SessionItem.ACTIVE) ~= 0) then
            link:remove ()
            Log.info (si, "... moving to new target")
          else
            scheduleRescan ()
            Log.info (si, "... scheduled rescan")
            return
          end

        end
      end
    end
  end

  -- create new link
  createLink (si, si_target_ep)
end

function unhandleLinkable (si)
  if not checkLinkable (si) then
    return
  end

  local node = si:get_associated_proxy ("node")
  Log.info (si, "unhandling item " .. tostring(node.properties["node.name"]))

  -- remove any links associated with this item
  for silink in links_om:iterate() do
    local out_id = tonumber (silink.properties["out.item.id"])
    local in_id = tonumber (silink.properties["in.item.id"])
    if out_id == si.id or in_id == si.id then
      silink:remove ()
      Log.info (silink, "... link removed")
    end
  end
end

endpoints_om = ObjectManager { Interest { type = "SiEndpoint" }}
linkables_om = ObjectManager { Interest { type = "SiLinkable",
  -- only handle si-audio-adapter and si-node
  Constraint {
    "item.factory.name", "=", "si-audio-adapter", type = "pw-global" },
  Constraint {
    "active-features", "!", 0, type = "gobject" },
  Constraint {
    "node.link-group", "-" },
  }
}
links_om = ObjectManager { Interest { type = "SiLink",
  -- only handle links created by this policy
  Constraint { "is.policy.endpoint.client.link", "=", true, type = "pw-global" },
} }

linkables_om:connect("objects-changed", function (om)
  scheduleRescan ()
end)

endpoints_om:connect("objects-changed", function (om)
  scheduleSynchronizedRescan ()
end)

linkables_om:connect("object-removed", function (om, si)
  unhandleLinkable (si)
end)

endpoints_om:activate()
linkables_om:activate()
links_om:activate()
scheduleSynchronizedRescan ()
)wp04lua"};
constexpr auto kWirePlumber04EndpointDevicePolicy =
    std::string_view{R"wp04lua(-- Managed by PipeTune for WirePlumber 0.4 compatibility.
-- Derived from WirePlumber 0.4.17 policy-endpoint-device.lua.
-- WirePlumber
--
-- Copyright © 2021 Collabora Ltd.
--    @author Julian Bouzas <julian.bouzas@collabora.com>
--
-- SPDX-License-Identifier: MIT

-- Receive script arguments from config.lua
local config = ... or {}

-- ensure config.move and config.follow are not nil
config.move = config.move or false
config.follow = config.follow or false

local self = {}
self.scanning = false
self.pending_rescan = false

function rescan ()
  -- check endpoints and register new links
  for si_ep in endpoints_om:iterate() do
    handleLinkable(si_ep)
  end

  -- handle filters only if we have endpoints
  if endpoints_om:get_n_objects () > 0 then
    for filter in streams_om:iterate {
      Constraint { "node.link-group", "+" },
    } do
      handleFilter(filter)
    end
  end
end

function scheduleRescan ()
  if self.scanning then
    self.pending_rescan = true
    return
  end

  self.scanning = true
  rescan ()
  self.scanning = false

  if self.pending_rescan then
    self.pending_rescan = false
    Core.sync(function ()
      scheduleRescan ()
    end)
  end
end

function findFilterTarget (props)
  for si_target in linkables_om:iterate {
    -- exclude filter targets
    Constraint { "node.link-group", "+" },
  } do
    local si_props = si_target.properties
    local target_endpoint = si_props["target.endpoint"] or
        si_props["node.pipetune.target-endpoint"]
    if target_endpoint == props["name"] then
      return si_target
    end
  end
end

function findTargetByDefaultNode (target_media_class)
  local def_id = default_nodes:call("get-default-node", target_media_class)
  if def_id ~= Id.INVALID then
    for si_target in linkables_om:iterate {
      -- exclude filter targets
      Constraint { "node.link-group", "-" },
    } do
      local target_node = si_target:get_associated_proxy ("node")
      if target_node["bound-id"] == def_id then
        return si_target
      end
    end
  end
  return nil
end

function findTargetByFirstAvailable (target_media_class)
  for si_target in linkables_om:iterate {
    -- exclude filter targets
    Constraint { "node.link-group", "-" },
  } do
    local target_node = si_target:get_associated_proxy ("node")
    if target_node.properties["media.class"] == target_media_class then
      return si_target
    end
  end
  return nil
end

function findUndefinedTarget (si_ep)
  local media_class = si_ep.properties["media.class"]
  local target_class_assoc = {
    ["Audio/Source"] = "Audio/Source",
    ["Stream/Output/Audio"] = "Audio/Sink",
    ["Audio/Sink"] = "Audio/Sink",
    ["Video/Source"] = "Video/Source",
  }
  local target_media_class = target_class_assoc[media_class]
  if not target_media_class then
    return nil
  end

  local si_target = findFilterTarget (si_ep.properties)
  if not si_target then
    si_target = findTargetByDefaultNode (target_media_class)
  end
  if not si_target then
    si_target = findTargetByFirstAvailable (target_media_class)
  end
  return si_target
end

function createLink (si_ep, si_target, is_filter)
  local out_item = nil
  local in_item = nil
  local ep_props = si_ep.properties
  local target_props = si_target.properties

  if target_props["item.node.direction"] == "input" then
    -- playback
    out_item = si_ep
    in_item = si_target
  else
    -- capture
    in_item = si_ep
    out_item = si_target
  end

  local link_string = string.format("link %s <-> %s ",
    (is_filter and ep_props["node.name"] or ep_props["name"]),
    target_props["node.name"])

  -- create and configure link
  local si_link = SessionItem ( "si-standard-link" )

  Log.info(si_link, link_string)

  if not si_link:configure {
    ["out.item"] = out_item,
    ["in.item"] = in_item,
    ["out.item.port.context"] = "output",
    ["in.item.port.context"] = "input",
    ["is.policy.endpoint.device.link"] = true,
  } then
    Log.warning (si_link, "failed to configure si-standard-link")
    return
  end

  -- register
  si_link:register ()

  Log.info (si_link, " activating link " .. link_string)

  -- activate
  si_link:activate (Feature.SessionItem.ACTIVE, function (l, e)
    if e then
      Log.warning (l, "failed to activate link: " .. link_string .. tostring(e))
      l:remove ()
    else
      Log.info (l, "activated link " .. link_string)
    end
  end)
end

function handleFilter(filter)
  handleLinkable(filter)
end

function handleLinkable (si)
  local si_props = si.properties
  local is_filter = (si_props["node.link-group"] ~= nil)
  if is_filter then
    Log.info (si, "handling filter " .. si_props["node.name"])
  else
    Log.info (si, "handling endpoint " .. si_props["name"])
  end

  -- find proper target item
  local si_target = findUndefinedTarget (si)
  if not si_target then
    Log.info (si, "... target item not found")
    return
  end

  -- Check if item is linked to proper target, otherwise re-link
  for link in links_om:iterate() do
    local out_id = tonumber(link.properties["out.item.id"])
    local in_id = tonumber(link.properties["in.item.id"])
    if out_id == si.id or in_id == si.id then
      local is_out = out_id == si.id and true or false
      for peer in linkables_om:iterate() do
        if peer.id == (is_out and in_id or out_id) then

          if peer.id == si_target.id then
            Log.info (si, "... already linked to proper target")
            return
          end

          -- remove old link if active, otherwise schedule rescan
          if ((link:get_active_features() & Feature.SessionItem.ACTIVE) ~= 0) then
            link:remove ()
            Log.info (si, "... moving to new target")
          else
            scheduleRescan ()
            Log.info (si, "... scheduled rescan")
            return
          end

        end
      end
    end
  end

  -- create new link
  createLink (si, si_target, is_filter)
end

function unhandleLinkable (si)
  local si_props = si.properties

  Log.info (si, string.format("unhandling item: %s (%s)",
      tostring(si_props["node.name"]), tostring(si_props["node.id"])))

  -- remove any links associated with this item
  for silink in links_om:iterate() do
    local out_id = tonumber (silink.properties["out.item.id"])
    local in_id = tonumber (silink.properties["in.item.id"])
    if out_id == si.id or in_id == si.id then
      silink:remove ()
      Log.info (silink, "... link removed")
    end
  end
end

default_nodes = Plugin.find("default-nodes-api")
endpoints_om = ObjectManager { Interest { type = "SiEndpoint" }}
linkables_om = ObjectManager {
  Interest {
    type = "SiLinkable",
    -- only handle device si-audio-adapter items
    Constraint { "item.factory.name", "=", "si-audio-adapter", type = "pw-global" },
    Constraint { "item.node.type", "=", "device", type = "pw-global" },
    Constraint { "active-features", "!", 0, type = "gobject" },
  }
}
streams_om = ObjectManager {
  Interest {
    type = "SiLinkable",
    -- only handle stream si-audio-adapter items
    Constraint { "item.factory.name", "=", "si-audio-adapter", type = "pw-global" },
    Constraint { "active-features", "!", 0, type = "gobject" },
    Constraint { "media.class", "=", "Stream/Output/Audio" },
  }
}
links_om = ObjectManager {
  Interest {
    type = "SiLink",
    -- only handle links created by this policy
    Constraint { "is.policy.endpoint.device.link", "=", true, type = "pw-global" },
  }
}

-- listen for default node changes if config.follow is enabled
if config.follow then
  default_nodes:connect("changed", function (p)
    scheduleRescan ()
  end)
end

linkables_om:connect("objects-changed", function (om)
  scheduleRescan ()
end)

endpoints_om:connect("object-added", function (om)
  scheduleRescan ()
end)

linkables_om:connect("object-removed", function (om, si)
  unhandleLinkable (si)
end)

endpoints_om:activate()
linkables_om:activate()
links_om:activate()
streams_om:activate()
)wp04lua"};

std::string_view wirePlumber04CompatibilityPolicy() noexcept {
  return kWirePlumber04Policy;
}

std::string_view wirePlumber04EndpointClientPolicy() noexcept {
  return kWirePlumber04EndpointClientPolicy;
}

std::string_view wirePlumber04EndpointDevicePolicy() noexcept {
  return kWirePlumber04EndpointDevicePolicy;
}

} // namespace pipetune
