-- The installer provides only paths, the ownership marker and the release label.
return function(registry, profile)

    local function write_report(value)
        if json and type(json.dump_file) == "function" then pcall(json.dump_file, profile.report_path, value) end
    end

    local function scalar_snapshot(handle)
        local result = { version = profile.version }
        for name, value in pairs(handle.snapshot()) do
            local kind = type(value)
            if kind == "boolean" or kind == "number" or kind == "string" then result[name] = value end
        end
        result.status = result.installed and "ready" or (result.restored and "restored" or "unavailable")
        return result
    end

    local function install()
        local previous = rawget(_G, profile.key)
        if previous then
            local status = previous.snapshot()
            assert(status.installed == true, "existing lookup patch is not the current owner")
            write_report(status)
            return
        end
        assert(type(registry) == "table", "BoneSystem private registry unavailable")
        assert(type(sdk) == "table" and type(sdk.fix_bone) == "function", "BoneSystem is not initialized")

        -- Resolve once in the existing private state; no callbacks, timers or later scans.
        local seen, found, checked, entries = {}, nil, 0, 0
        local function visit(fn, depth)
            if seen[fn] or depth > 8 then return end
            local info = debug.getinfo(fn, "S")
            if info.what ~= "Lua" or not info.source:match("[\\/]BoneSystem%.lua$") then return end
            seen[fn] = true
            checked = checked + 1
            assert(checked <= 512, "closure search limit exceeded")
            for i = 1, 255 do
                local name, value = debug.getupvalue(fn, i)
                if not name then break end
                if type(value) == "function" then
                    if name == "load_data" then
                        assert(not found or found == value, "ambiguous load_data closure")
                        found = value
                    end
                    visit(value, depth + 1)
                end
            end
        end
        for index, value in next, registry do
            entries = entries + 1
            assert(entries <= 16384, "registry search limit exceeded")
            if type(index) == "number" and type(value) == "function" then visit(value, 0) end
        end
        assert(found, "original load_data closure not found")
        local module = assert(loadfile(profile.lookup_path))()
        local handle, reason = module.install({ load_data = found }, _G)
        assert(handle, reason)
        local state = {
            snapshot = function() return scalar_snapshot(handle) end,
            restore = handle.restore,
        }
        rawset(_G, profile.key, state)
        write_report(state.snapshot())
        if log and log.info then log.info("[LifetimeGuard BoneSystem] ready; unused directory lookup omitted") end
    end

    local ok, err = pcall(install)
    if not ok then
        write_report({ version = profile.version, status = "unavailable", reason = tostring(err) })
        -- Let the native caller report failure after the Lua stack has been restored.
        error(err, 0)
    end
end
