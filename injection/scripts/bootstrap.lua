local GLOBAL_KEY = "__COCOS_DEBUG_HEADLESS_PROFILER__"

local previous = rawget(_G, GLOBAL_KEY)
if previous and previous.cleanup then
    pcall(previous.cleanup)
end

--[[
  Script owns only compatibility discovery for coroutine.create.
  Start/stop, frame boundaries, and C++ hooks are driven entirely from C.
  Remote Lua from DebugView is drained here via Cocos Scheduler (safe Lua context).
]]
local hooks = hookModule.create({
    onDiscover = function(co)
        if _LUA_TOOL_profiler_hook_thread then
            pcall(_LUA_TOOL_profiler_hook_thread, co)
        end
    end,
})

hooks.install()

local state = {
    sched_id = nil,
    cleanup = function()
        if state.sched_id then
            pcall(function()
                local sch = cc.Director:getInstance():getScheduler()
                sch:unscheduleScriptEntry(state.sched_id)
            end)
            state.sched_id = nil
        end
        hooks.uninstall()
        rawset(_G, GLOBAL_KEY, nil)
    end,
}

-- Drain remote scripts from Scheduler so lua_pcall runs nested inside Lua,
-- never from drawScene / window-proc C++ hooks.
pcall(function()
    local sch = cc.Director:getInstance():getScheduler()
    state.sched_id = sch:scheduleScriptFunc(function()
        if _LUA_TOOL_drain_remote_lua then
            pcall(_LUA_TOOL_drain_remote_lua)
        end
    end, 0, false)
end)

rawset(_G, GLOBAL_KEY, state)
return "Cocos2d DebugView profiler ready"
