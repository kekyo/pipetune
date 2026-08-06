-- PipeTune transparent-filter compatibility policy for WirePlumber 0.4.

-- WirePlumber 0.4 scripts keep long-lived proxy objects in their global
-- environment. Root the policy objects likewise so Lua collection cannot
-- detach graph and metadata callbacks after this script finishes loading.
pipetune_policy_runtime = {}

local policy_metadata = nil
local default_metadata = nil
local rescan_pending = false
local managed_targets = {}
local nodes_by_id = {}
local pending_metadata_updates = {}

local function parse_bool(value)
  return value ~= nil and
      (tostring(value):lower() == "true" or tostring(value) == "1")
end

local function parse_json(value, expected_kind)
  if value == nil then
    return nil
  end
  local raw = Json.Raw(value)
  if raw == nil then
    return nil
  end
  if expected_kind == "object" and not raw:is_object() then
    return nil
  end
  if expected_kind == "array" and not raw:is_array() then
    return nil
  end
  return raw:parse()
end

local function metadata_type(value)
  return tonumber(value) ~= nil and "Spa:Id" or "Spa:String"
end

local function remember_metadata_update(subject, key, value)
  local subject_updates = pending_metadata_updates[subject]
  if subject_updates == nil then
    subject_updates = {}
    pending_metadata_updates[subject] = subject_updates
  end
  subject_updates[key] = {
    present = value ~= nil,
    value = value ~= nil and tostring(value) or "",
  }
end

local function consume_metadata_update(subject, key, value)
  local subject_updates = pending_metadata_updates[subject]
  local expected = subject_updates ~= nil and subject_updates[key] or nil
  if expected == nil then
    return false
  end
  subject_updates[key] = nil
  if next(subject_updates) == nil then
    pending_metadata_updates[subject] = nil
  end
  return expected.present == (value ~= nil) and
      (not expected.present or expected.value == tostring(value))
end

local function set_metadata(metadata, subject, key, value)
  remember_metadata_update(subject, key, value)
  if value == nil then
    metadata:set(subject, key, nil, nil)
  else
    metadata:set(subject, key, metadata_type(value), tostring(value))
  end
end

local function find_node_by_value(value, use_object_serial)
  if value == nil then
    return nil
  end
  for _, node in pairs(nodes_by_id) do
    local properties = node.properties
    if properties["node.name"] == tostring(value) or
        properties["object.path"] == tostring(value) or
        (use_object_serial and
         properties["object.serial"] == tostring(value)) or
        (not use_object_serial and
         tostring(node["bound-id"]) == tostring(value)) then
      return node
    end
  end
  return nil
end

local function default_sink_node()
  local default_nodes = Plugin.find("default-nodes-api")
  if default_nodes == nil then
    return nil
  end
  local id = default_nodes:call("get-default-node", "Audio/Sink")
  if id == nil or id == Id.INVALID then
    return nil
  end
  return nodes_by_id[tonumber(id)]
end

local function node_target_name(node)
  local properties = node.properties
  local target_object = properties["target.object"]
  if target_object ~= nil then
    local target = find_node_by_value(target_object, true)
    if target ~= nil then
      return target.properties["node.name"], true
    end
  end
  local node_target = properties["node.target"]
  if node_target ~= nil then
    local target = find_node_by_value(node_target, false)
    if target ~= nil then
      return target.properties["node.name"], true
    end
  end

  if default_metadata ~= nil then
    local subject = node["bound-id"]
    local metadata_target = default_metadata:find(subject, "target.object")
    if metadata_target ~= nil then
      local target = find_node_by_value(metadata_target, true)
      if target ~= nil then
        return target.properties["node.name"], true
      end
    end
    metadata_target = default_metadata:find(subject, "target.node")
    if metadata_target ~= nil then
      local target = find_node_by_value(metadata_target, false)
      if target ~= nil then
        return target.properties["node.name"], true
      end
    end
  end

  local target = default_sink_node()
  return target ~= nil and target.properties["node.name"] or nil, false
end

local function smart_filter_target(node)
  local properties = node.properties
  if properties["pipetune.target.node"] ~= nil then
    return properties["pipetune.target.node"]
  end
  local target = parse_json(properties["filter.smart.target"], "object")
  if target ~= nil then
    return target["node.name"]
  end
  return nil
end

local function smart_filter_array(node, key)
  local parsed = parse_json(node.properties[key], "array")
  return parsed or {}
end

local function is_smart_filter_main(node)
  return node.properties["media.class"] == "Audio/Sink" and
      node.properties["node.link-group"] ~= nil and
      parse_bool(node.properties["filter.smart"])
end

local function is_pipetune_filter(node)
  return parse_bool(node.properties["pipetune.filter"])
end

local function filter_name(node)
  return node.properties["filter.smart.name"] or
      node.properties["node.link-group"]
end

local function filter_is_enabled(node)
  if is_pipetune_filter(node) then
    return policy_metadata ~= nil and
        parse_bool(policy_metadata:find(node["bound-id"], "filter.enabled"))
  end
  return not parse_bool(node.properties["filter.smart.disabled"])
end

local function playback_for_filter(main_node)
  local link_group = main_node.properties["node.link-group"]
  for _, node in pairs(nodes_by_id) do
    if node.properties["media.class"] == "Stream/Output/Audio" and
        node.properties["node.link-group"] == link_group then
      return node
    end
  end
  return nil
end

local function add_edge(edges, indegree, from_name, to_name)
  if from_name == nil or to_name == nil or from_name == to_name or
      edges[from_name] == nil or edges[to_name] == nil or
      edges[from_name][to_name] then
    return
  end
  edges[from_name][to_name] = true
  indegree[to_name] = indegree[to_name] + 1
end

local function sort_filters(filters)
  local by_name = {}
  local edges = {}
  local indegree = {}
  local pipetune_names = {}
  for _, node in ipairs(filters) do
    local name = filter_name(node)
    if name == nil or by_name[name] ~= nil then
      return nil
    end
    by_name[name] = node
    edges[name] = {}
    indegree[name] = 0
    if is_pipetune_filter(node) then
      table.insert(pipetune_names, name)
    end
  end
  for name, node in pairs(by_name) do
    for _, after_name in ipairs(smart_filter_array(node, "filter.smart.after")) do
      add_edge(edges, indegree, after_name, name)
    end
    for _, before_name in ipairs(smart_filter_array(node, "filter.smart.before")) do
      add_edge(edges, indegree, name, before_name)
    end
  end
  for name, _ in pairs(by_name) do
    for _, pipetune_name in ipairs(pipetune_names) do
      if name ~= pipetune_name then
        add_edge(edges, indegree, name, pipetune_name)
      end
    end
  end

  local ordered = {}
  while #ordered < #filters do
    local next_name = nil
    for name, count in pairs(indegree) do
      if count == 0 and
          (next_name == nil or name < next_name) then
        next_name = name
      end
    end
    if next_name == nil then
      return nil
    end
    table.insert(ordered, by_name[next_name])
    indegree[next_name] = -1
    for target_name, _ in pairs(edges[next_name]) do
      indegree[target_name] = indegree[target_name] - 1
    end
  end
  return ordered
end

local function publish_filter_state(node, state)
  if policy_metadata ~= nil and is_pipetune_filter(node) then
    policy_metadata:set(node["bound-id"], "filter.state", "Spa:String", state)
  end
end

local function restore_managed_target(id)
  local managed = managed_targets[id]
  if managed == nil or default_metadata == nil then
    return
  end
  set_metadata(default_metadata, id, "target.object", managed.original_target_object)
  managed_targets[id] = nil
end

local function target_stream(node, target_name, target_is_explicit)
  if default_metadata == nil or target_name == nil then
    return
  end
  local id = node["bound-id"]
  local managed = managed_targets[id]
  if managed == nil then
    managed = {
      node = node,
      original_target_object = default_metadata:find(id, "target.object"),
      physical_target = target_name,
      explicit = target_is_explicit,
    }
    managed_targets[id] = managed
  end
  set_metadata(default_metadata, id, "target.object", target_name)
end

local function stream_is_filterable(node)
  local properties = node.properties
  return properties["media.class"] == "Stream/Output/Audio" and
      properties["node.link-group"] == nil and
      not parse_bool(properties["item.node.encoded-only"]) and
      not parse_bool(properties["node.encoded-only"])
end

local function filters_for_target(target_name)
  local selected = {}
  local has_enabled_pipetune = false
  for _, node in pairs(nodes_by_id) do
    if is_smart_filter_main(node) and filter_is_enabled(node) then
      local target = smart_filter_target(node)
      if target == target_name or target == nil then
        table.insert(selected, node)
        has_enabled_pipetune = has_enabled_pipetune or
            (is_pipetune_filter(node) and target == target_name)
      end
    end
  end
  if not has_enabled_pipetune then
    return {}
  end
  return selected
end

local function configure_target_chain(target_name)
  local filters = filters_for_target(target_name)
  if #filters == 0 then
    return nil
  end
  local ordered = sort_filters(filters)
  if ordered == nil then
    for _, node in ipairs(filters) do
      publish_filter_state(node, "conflict")
    end
    return nil
  end
  for index, main_node in ipairs(ordered) do
    local playback = playback_for_filter(main_node)
    if playback == nil then
      publish_filter_state(main_node, "waiting")
      return nil
    end
    local next_target = target_name
    if index < #ordered then
      next_target = ordered[index + 1].properties["node.name"]
    end
    local current_target, explicit = node_target_name(playback)
    target_stream(playback, next_target, explicit)
    publish_filter_state(main_node, "active")
  end
  return ordered[1].properties["node.name"]
end

local function rescan()
  rescan_pending = false
  local configured_targets = {}
  for _, node in pairs(nodes_by_id) do
    if is_pipetune_filter(node) then
      local target_name = smart_filter_target(node)
      if target_name ~= nil and configured_targets[target_name] == nil then
        configured_targets[target_name] = configure_target_chain(target_name)
      end
      if not filter_is_enabled(node) then
        publish_filter_state(node, "bypassed")
      end
    end
  end

  for id, managed in pairs(managed_targets) do
    if nodes_by_id[id] == nil or
        configured_targets[managed.physical_target] == nil then
      restore_managed_target(id)
    end
  end
  for _, node in pairs(nodes_by_id) do
    if stream_is_filterable(node) and managed_targets[node["bound-id"]] == nil then
      local target_name, explicit = node_target_name(node)
      local filter_target = configured_targets[target_name]
      if filter_target ~= nil then
        target_stream(node, filter_target, explicit)
        managed_targets[node["bound-id"]].physical_target = target_name
      end
    end
  end
end

local function schedule_rescan()
  if rescan_pending then
    return
  end
  rescan_pending = true
  Core.sync(function()
    rescan()
  end)
end

local nodes_om = ObjectManager { Interest { type = "node" } }
local default_metadata_om = ObjectManager {
  Interest {
    type = "metadata",
    Constraint { "metadata.name", "=", "default" },
  }
}
pipetune_policy_runtime.nodes = nodes_om
pipetune_policy_runtime.default_metadata = default_metadata_om

nodes_om:connect("object-added", function(_, node)
  local id = node["bound-id"]
  nodes_by_id[id] = node
  schedule_rescan()
end)

nodes_om:connect("object-removed", function(_, node)
  local id = node["bound-id"]
  nodes_by_id[id] = nil
  restore_managed_target(id)
  schedule_rescan()
end)

default_metadata_om:connect("object-added", function(_, metadata)
  default_metadata = metadata
  metadata:connect("changed", function(_, subject, key, _, value)
    if consume_metadata_update(subject, key, value) then
      return
    end
    local managed = managed_targets[subject]
    if managed ~= nil and (key == "target.object" or key == "target.node") then
      restore_managed_target(subject)
    end
    if key == "target.object" or key == "target.node" or
        (subject == 0 and key == "default.audio.sink") then
      schedule_rescan()
    end
  end)
  schedule_rescan()
end)

-- Stable WirePlumber 0.4 cannot express PipeWire's link-only permission in
-- its Lua API. Keeping smart filters readable preserves reliable linking;
-- filter.smart.targetable advertises their internal role to clients that
-- honor the newer smart-filter property.
nodes_om:activate()
default_metadata_om:activate()

policy_metadata = ImplMetadata("pipetune-policy")
pipetune_policy_runtime.policy_metadata = policy_metadata
policy_metadata:activate(Features.ALL, function(metadata, error)
  if error then
    Log.warning("failed to activate PipeTune policy metadata: " .. tostring(error))
    return
  end
  metadata:set(0, "protocol.version", "Spa:Int", "1")
  metadata:set(0, "policy.backend", "Spa:String", "wireplumber-0.4")
  metadata:set(0, "policy.state", "Spa:String", "ready")
  metadata:connect("changed", function(_, subject, key)
    if subject ~= 0 and key == "filter.enabled" then
      schedule_rescan()
    end
  end)
  schedule_rescan()
end)
