-- System mpv integration: silent video also inhibits idle screen-off.
-- Leases expire after a crash; pause/EOF releases them. Never block playback.
local mp = require 'mp'
local utils = require 'mp.utils'
local token, busy = nil, false
local function active()
    return not mp.get_property_native('idle-active', true)
        and not mp.get_property_native('pause', false)
        and not mp.get_property_native('eof-reached', false)
        and mp.get_property('path') ~= nil
end
local update
update = function()
    if busy then return end
    local playing = active()
    if not playing and not token then return end
    local request
    if playing then
        request = {method='inhibit', scopes={'screen'},
                   reason='mpv playback', seconds=45, token=token}
    else
        request = {method='release', token=token}
    end
    busy = true
    mp.command_native_async({name='subprocess', playback_only=false,
        args={'/usr/bin/env', '-u', 'LD_PRELOAD', '-u', 'LD_LIBRARY_PATH',
              '/usr/local/bin/santos-powerctl', utils.format_json(request)},
        capture_stdout=true, capture_stderr=true}, function(success, result)
        busy = false
        local reply = success and result and utils.parse_json(result.stdout or '')
        if reply and reply.ok then
            token = playing and reply.result.token or nil
        else
            token = nil -- daemon restart/expired lease: obtain a new lease next tick
        end
        if active() ~= playing then update() end
    end)
end
mp.add_periodic_timer(15, update)
for _, name in ipairs({'pause', 'idle-active', 'eof-reached'}) do
    mp.observe_property(name, 'bool', update)
end
mp.register_event('file-loaded', update)
mp.register_event('end-file', update)
-- mpv tears down asynchronous subprocesses on exit; release synchronously only
-- during shutdown (bounded to one second), after playback has already stopped.
mp.register_event('shutdown', function()
    if token then
        mp.command_native({name='subprocess', playback_only=false,
            args={'/usr/bin/env', '-u', 'LD_PRELOAD', '-u', 'LD_LIBRARY_PATH',
                  '/usr/local/bin/santos-powerctl', '--timeout', '1',
                  utils.format_json({method='release', token=token})},
            capture_stdout=true, capture_stderr=true})
    end
end)
