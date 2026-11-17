-- Offline experimental candidate. No automatic entry point or global fs writes.
local M = {}
local dbg, dump = debug, string.dump
local expected_hex = table.concat({
-- BEGIN GENERATED STOCK STRIPPED DUMP HEX
    "1b4c7561540019930d0a1a0a04080878560000000000000000000000287740048001ec01f8010006a389000000000100",
    "00c4000202c200000038010080340101003d017f00b800008008010000480102000b0101000e01020180010100030201",
    "008382010044010402800002000b0101040e0102058301030044010202890102000902030080020100c4010302c0017f",
    "00380200808b0101078e01030800020100c5010200c601000088010000c8010200c7010100890487737472696e670485",
    "6773756204822f04825c048366730485676c6f620493426f6e6553797374656d5c5c2e2a6a736f6e04856a736f6e048a",
    "6c6f61645f66696c6584011d00000000010f000103008080808080",
-- END GENERATED STOCK STRIPPED DUMP HEX
})
local pattern = "BoneSystem\\\\.*json"
local names = { "get_part_path", "_ENV", "find_table", "json_files" }
local active = setmetatable({}, { __mode = "k" })

local function cell(value)
    return function() return value end
end

local function capable()
    if type(dbg) ~= "table" or type(dump) ~= "function" then
        return false
    end
    for _, key in ipairs({ "getinfo", "getupvalue", "upvalueid", "upvaluejoin" }) do
        if type(dbg[key]) ~= "function" then return false end
    end
    return true
end

local function read_expected()
    if #expected_hex == 0 or #expected_hex % 2 ~= 0 or not expected_hex:match("^%x+$") then
        return nil, "embedded_expected_dump_invalid"
    end
    return (expected_hex:gsub("..", function(pair) return string.char(tonumber(pair, 16)) end))
end

local function inspect(fn, expected)
    if type(fn) ~= "function" then return nil, "load_data_not_function" end
    local info = dbg.getinfo(fn, "Su")
    if info.what ~= "Lua" or info.nparams ~= 1 or info.isvararg or info.nups ~= 4
        or info.linedefined ~= 236 or info.lastlinedefined ~= 248
        or not info.source:match("[\\/]BoneSystem%.lua$") then
        return nil, "unknown_function_metadata"
    end
    if dump(fn, true) ~= expected then return nil, "stripped_dump_mismatch" end
    local slots = {}
    for i = 1, 4 do
        local name, value = dbg.getupvalue(fn, i)
        if name ~= names[i] then return nil, "unknown_upvalue_name_" .. i end
        slots[i] = { value = value, id = dbg.upvalueid(fn, i) }
    end
    if dbg.getupvalue(fn, 5) ~= nil then return nil, "extra_upvalue" end
    if type(slots[1].value) ~= "function" or type(slots[3].value) ~= "function"
        or type(slots[4].value) ~= "table" then
        return nil, "unknown_upvalue_type"
    end
    return slots
end

local function same_other_cells(fn, slots)
    for _, i in ipairs({ 1, 3, 4 }) do
        if dbg.upvalueid(fn, i) ~= slots[i].id then return false end
    end
    return true
end

local function install_checked(api, env)
    if not capable() then return nil, "required_lua54_debug_unavailable" end
    if type(api) ~= "table" or type(env) ~= "table" then
        return nil, "api_and_env_must_be_tables"
    end
    local fn = rawget(api, "load_data")
    local expected, err = read_expected()
    if not expected then return nil, err end
    local slots
    slots, err = inspect(fn, expected)
    if not slots then return nil, err end
    local previous = active[fn]
    if previous then
        if previous.intact() and rawequal(previous.original(), env) then
            return previous.handle, "already_installed"
        end
        return nil, "active_install_conflict"
    end
    if not rawequal(slots[2].value, env) then return nil, "env_identity_mismatch" end
    if type(env.fs) ~= "table" or type(env.fs.glob) ~= "function"
        or type(env.string) ~= "table" or type(env.string.gsub) ~= "function"
        or type(env.json) ~= "table" or type(env.json.load_file) ~= "function" then
        return nil, "unknown_environment_structure"
    end

    -- Keep the original shared cell, not merely its current table value.
    local original = cell(nil)
    dbg.upvaluejoin(original, 1, fn, 2)
    local skipped_calls = 0
    local fs_proxy = setmetatable({
        glob = function(...)
            if select("#", ...) == 1 and rawequal((...), pattern) then
                skipped_calls = skipped_calls + 1
                return nil
            end
            return original().fs.glob(...)
        end,
    }, {
        __index = function(_, key) return original().fs[key] end,
        __newindex = function(_, key, value) original().fs[key] = value end,
    })
    local env_proxy = setmetatable({}, {
        __index = function(_, key)
            if key == "fs" then return fs_proxy end
            return original()[key]
        end,
        __newindex = function(_, key, value) original()[key] = value end,
    })
    local fork = cell(env_proxy)
    local fork_id = dbg.upvalueid(fork, 1)
    local restored = false
    local function intact()
        local name, value = dbg.getupvalue(fn, 2)
        return name == "_ENV" and rawequal(value, env_proxy)
            and dbg.upvalueid(fn, 2) == fork_id
            and dbg.upvalueid(original, 1) == slots[2].id
            and same_other_cells(fn, slots)
    end
    local handle = {}
    function handle.snapshot()
        local owns_fork = not restored and intact()
        local original_connected = dbg.upvalueid(fn, 2) == slots[2].id
        return {
            installed = owns_fork,
            restored = restored,
            skipped_calls = skipped_calls,
            ownership = owns_fork and "owned_fork"
                or (restored and original_connected and "restored_original_cell" or "conflict"),
            owns_fork = owns_fork,
            original_cell_preserved = dbg.upvalueid(original, 1) == slots[2].id,
            original_cell_connected = original_connected,
            other_cells_unchanged = same_other_cells(fn, slots),
            closure = fn,
        }
    end
    handle.status = handle.snapshot
    function handle.restore()
        if restored then
            if dbg.upvalueid(fn, 2) == slots[2].id then return true, "already_restored" end
            return false, "retired_handle_not_owner_no_write"
        end
        if not intact() then return false, "restore_conflict_no_write" end
        dbg.upvaluejoin(fn, 2, original, 1)
        if dbg.upvalueid(fn, 2) ~= slots[2].id then
            return false, "restore_readback_failed"
        end
        restored = true
        active[fn] = nil
        return true, "restored"
    end

    -- This is the sole mutation of any production closure during installation.
    dbg.upvaluejoin(fn, 2, fork, 1)
    if not intact() then
        dbg.upvaluejoin(fn, 2, original, 1)
        return nil, "install_readback_failed_rolled_back"
    end
    active[fn] = { original = original, intact = intact, handle = handle }
    return handle, "installed"
end

function M.install(api, env)
    local ok, handle, reason = pcall(install_checked, api, env)
    if not ok then return nil, "validation_failed: " .. tostring(handle) end
    return handle, reason
end

return M
