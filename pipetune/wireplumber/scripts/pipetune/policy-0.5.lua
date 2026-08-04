-- PipeTune Smart Filters integration for WirePlumber 0.5 and later.

Script.async_activation = true

local policy_metadata = nil
local filters_metadata = nil
local hidden_nodes = {}
local nodes_by_id = {}
local rescan_pending = false

local function parse_bool(value)
  return value ~= nil and
      (tostring(value):lower() == "true" or tostring(value) == "1")
end

local function parse_array(value)
  if value == nil then
    return {}
  end
  local raw = Json.Raw(value)
  if raw == nil or not raw:is_array() then
    return {}
  end
  return raw:parse()
end

local function json_quote(value)
  local escaped = tostring(value):gsub("\\", "\\\\")
      :gsub('"', '\\"'):gsub("\n", "\\n"):gsub("\r", "\\r")
  return '"' .. escaped .. '"'
end

local function json_array(values)
  local encoded = {}
  for _, value in ipairs(values) do
    table.insert(encoded, json_quote(value))
  end
  return "[ " .. table.concat(encoded, ", ") .. " ]"
end

local function is_pipetune_main(node)
  return node.properties["media.class"] == "Audio/Sink" and
      parse_bool(node.properties["pipetune.filter"])
end

local function is_smart_filter_main(node)
  return node.properties["media.class"] == "Audio/Sink" and
      node.properties["node.link-group"] ~= nil and
      parse_bool(node.properties["filter.smart"])
end

local function filter_name(node)
  return node.properties["filter.smart.name"] or
      node.properties["node.link-group"]
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

local function chain_has_cycle(filters, pipetune_name)
  local edges = {}
  local indegree = {}
  local count = 0
  for name, _ in pairs(filters) do
    edges[name] = {}
    indegree[name] = 0
    count = count + 1
  end
  for name, node in pairs(filters) do
    for _, other in ipairs(parse_array(node.properties["filter.smart.after"])) do
      add_edge(edges, indegree, other, name)
    end
    for _, other in ipairs(parse_array(node.properties["filter.smart.before"])) do
      add_edge(edges, indegree, name, other)
    end
    if name ~= pipetune_name then
      add_edge(edges, indegree, name, pipetune_name)
    end
  end
  local visited = 0
  while visited < count do
    local selected = nil
    for name, degree in pairs(indegree) do
      if degree == 0 then
        selected = name
        break
      end
    end
    if selected == nil then
      return true
    end
    indegree[selected] = -1
    visited = visited + 1
    for target, _ in pairs(edges[selected]) do
      indegree[target] = indegree[target] - 1
    end
  end
  return false
end

local function enabled_command(node)
  return policy_metadata ~= nil and
      parse_bool(policy_metadata:find(node["bound-id"], "filter.enabled"))
end

local function publish_state(node, state)
  if policy_metadata ~= nil then
    policy_metadata:set(node["bound-id"], "filter.state", "Spa:String", state)
  end
end

local function configure_pipetune_filter(node)
  if filters_metadata == nil then
    publish_state(node, "waiting")
    return
  end
  local target_name = node.properties["pipetune.target.node"]
  local pipetune_name = filter_name(node)
  local filters = {}
  local after = {}
  for _, candidate in pairs(nodes_by_id) do
    if is_smart_filter_main(candidate) then
      local name = filter_name(candidate)
      local candidate_target = candidate.properties["pipetune.target.node"]
      if name ~= nil and
          (candidate == node or candidate_target == nil or
           candidate_target == target_name) then
        filters[name] = candidate
        if candidate ~= node then
          table.insert(after, name)
        end
      end
    end
  end
  table.sort(after)
  local conflict = pipetune_name == nil or
      chain_has_cycle(filters, pipetune_name)
  local disabled = conflict or not enabled_command(node)
  filters_metadata:set(node["bound-id"], "filter.smart.after",
      "Spa:String:JSON", json_array(after))
  filters_metadata:set(node["bound-id"], "filter.smart.disabled",
      "Spa:String:JSON", disabled and "true" or "false")
  if conflict then
    publish_state(node, "conflict")
  elseif enabled_command(node) then
    publish_state(node, "active")
  else
    publish_state(node, "bypassed")
  end
end

local function rescan()
  rescan_pending = false
  for _, node in pairs(nodes_by_id) do
    if is_pipetune_main(node) then
      configure_pipetune_filter(node)
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

local function hide_node_from_client(client, node_id, owner_id)
  local properties = client.properties
  if properties["wireplumber.daemon"] or
      tostring(client["bound-id"]) == tostring(owner_id) then
    return
  end
  client:update_permissions { [node_id] = "l" }
end

local clients_om = ObjectManager { Interest { type = "client" } }
local nodes_om = ObjectManager { Interest { type = "node" } }
local filters_metadata_om = ObjectManager {
  Interest {
    type = "metadata",
    Constraint { "metadata.name", "=", "filters" },
  }
}

clients_om:connect("object-added", function(_, client)
  for node_id, owner_id in pairs(hidden_nodes) do
    hide_node_from_client(client, node_id, owner_id)
  end
end)

nodes_om:connect("object-added", function(_, node)
  local id = node["bound-id"]
  nodes_by_id[id] = node
  if is_pipetune_main(node) or
      parse_bool(node.properties["pipetune.filter.stream"]) then
    local owner_id = node.properties["client.id"]
    hidden_nodes[id] = owner_id
    for client in clients_om:iterate() do
      hide_node_from_client(client, id, owner_id)
    end
  end
  schedule_rescan()
end)

nodes_om:connect("object-removed", function(_, node)
  local id = node["bound-id"]
  nodes_by_id[id] = nil
  hidden_nodes[id] = nil
  schedule_rescan()
end)

filters_metadata_om:connect("object-added", function(_, metadata)
  filters_metadata = metadata
  schedule_rescan()
end)

clients_om:activate()
nodes_om:activate()
filters_metadata_om:activate()

policy_metadata = ImplMetadata("pipetune-policy")
policy_metadata:activate(Features.ALL, function(metadata, error)
  if error then
    Script:finish_activation_with_error(
        "failed to activate PipeTune policy metadata: " .. tostring(error))
    return
  end
  metadata:set(0, "protocol.version", "Spa:Int", "1")
  metadata:set(0, "policy.backend", "Spa:String", "wireplumber-0.5")
  metadata:set(0, "policy.state", "Spa:String", "ready")
  metadata:connect("changed", function(_, subject, key)
    if subject ~= 0 and key == "filter.enabled" then
      schedule_rescan()
    end
  end)
  Script:finish_activation()
  schedule_rescan()
end)
