local Lookup = assert(loadfile("reframework/autorun/WMPOptimizationPatches/BoneSystem/Lookup.lua"))()
local Provider = { schema = "wmp.bonesystem.provider.v1" }

function Provider.open(env, api)
    local handle
    -- WMP's existing bridge dispatches here before the independent autorun may have run.
    local independent = env.loadfile("reframework/autorun/LifetimeGuard.lua") ~= nil
    local function has_external_owner()
        return independent or rawget(env, "__BoneSystemLookupFix") ~= nil
    end
    local status, message = "disabled", "not_owned_no_write"
    local instance = {}

    function instance:snapshot()
        if not handle and has_external_owner() then
            return {status = "external_owner", message = "independent_lifetimeguard_owns_lookup",
                active = false, runtime_source = "standalone", activity_unknown = true}
        end
        local observation = handle and handle.snapshot()
        if not observation then
            return {status = status, message = message, active = false, runtime_source = "compatibility_pack"}
        end
        return {
            status = observation.installed and "working" or "conflict",
            message = observation.ownership,
            active = observation.installed == true,
            runtime_source = "compatibility_pack",
            skipped_calls = observation.skipped_calls,
            other_cells_unchanged = observation.other_cells_unchanged,
            original_cell_connected = observation.original_cell_connected,
            restored = observation.restored,
        }
    end

    function instance:set_enabled(enabled)
        if enabled and not handle and has_external_owner() then
            return true, self:snapshot()
        end
        if enabled and not handle then
            local attached, reason = Lookup.install(api, env)
            if not attached then
                status, message = "incompatible", reason
                return false, self:snapshot()
            end
            handle = attached
        elseif not enabled and handle then
            local restored, reason = handle.restore()
            if not restored then
                local record = self:snapshot()
                record.status, record.message = "conflict", reason
                return false, record
            end
            handle = nil
        end
        status, message = "disabled", "not_owned_no_write"
        local record = self:snapshot()
        return record.status ~= "conflict", record
    end
    return instance
end

return Provider
