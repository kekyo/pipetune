-- Load PipeTune alongside WirePlumber 0.4's device-monitor components so the
-- setup handshake does not wait for every physical device to activate.

load_script("pipetune/policy-0.4.lua")
