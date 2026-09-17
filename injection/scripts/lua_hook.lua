local M = {}

function M.create(opts)
    opts = opts or {}
    local hooks = {}
    local knownCoroutines = setmetatable({}, { __mode = "k" })
    local knownCount = 0
    local installed = false
    local rawCreate = nil

    local function discover(co)
        if not co then
            return
        end
        if knownCoroutines[co] then
            return
        end
        knownCoroutines[co] = true
        knownCount = knownCount + 1
        if opts.onDiscover then
            opts.onDiscover(co)
        end
    end

    function hooks.install()
        if installed then
            return
        end
        installed = true

        if coroutine.running then
            discover(coroutine.running())
        end

        if coroutine.create and not rawCreate then
            rawCreate = coroutine.create
            coroutine.create = function(fn)
                local co = rawCreate(fn)
                discover(co)
                return co
            end
        end

    end

    function hooks.uninstall()
        if rawCreate then
            coroutine.create = rawCreate
            rawCreate = nil
        end
        installed = false
    end

    function hooks.discover(co)
        discover(co)
    end

    function hooks.discoverAll()
        if coroutine.running then
            discover(coroutine.running())
        end
        for co in pairs(knownCoroutines) do
            discover(co)
        end
    end

    function hooks.getKnownCount()
        return knownCount
    end

    function hooks.isInstalled()
        return installed
    end

    return hooks
end

return M
