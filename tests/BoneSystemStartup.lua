-- Exercise the real LifetimeGuard export using a pinned stock lookup closure.
local function check_probe(expected_log, expected_locks)
    local message, depth, locks = probe_bonesystem()
    assert(depth == 0 and locks == expected_locks, message)
    if expected_log then
        assert(message:find(expected_log, 1, true), message)
    else
        assert(message == "", message)
    end
end

if case_mode ~= "ready" and case_mode ~= "unmatched-lookup" then
    check_probe(case_mode == "unsupported" and "refused" or nil, 0)
    assert(rawget(_G, owner_key) == nil)
    return
end

local result, reads, globs, reports = {}, 0, 0, {}
sdk = {fix_bone = function() end}
log = {info = function() end, error = function(reason) error(reason) end}
fs = {glob = function() globs = globs + 1; return {} end}
json = {
    load_file = function(path)
        assert(path == "folder\\config.json")
        reads = reads + 1
        return result
    end,
    dump_file = function(path, payload) reports[#reports + 1] = {path, payload} end,
}
local fn = assert(loadfile("load_data.stock.luac", "b"))
local cells = {function() return "folder/config.json" end, _G,
    function(files, path) assert(files[1] == path); return 1 end, {"folder\\config.json"}}
for i, value in ipairs(cells) do
    assert(debug.setupvalue(fn, i, value))
end
local original = debug.upvalueid(fn, 2)
local function publish(candidate)
    debug.getregistry()[998877] = assert(load("local load_data = ...; return function() return load_data end",
        "@fixture/BoneSystem.lua"))(candidate)
end

if case_mode == "unmatched-lookup" then
    publish(function() return result end)
    check_probe("bootstrap failed", 1)
    assert(rawget(_G, owner_key) == nil and reports[1][2].status == "unavailable")
    assert(debug.upvalueid(fn, 2) == original)
    return
end

publish(fn)
check_probe("ready", 1)
local handle = assert(rawget(_G, owner_key))
local first = debug.upvalueid(fn, 2)
check_probe("ready", 1)
assert(rawget(_G, owner_key) == handle and debug.upvalueid(fn, 2) == first)
assert(fn({}) == result and reads == 1 and globs == 0)
local snapshot = handle.snapshot()
assert(snapshot.installed and snapshot.skipped_calls == 1 and snapshot.original_cell_preserved)
local expected = owner_key == "__BoneSystemLookupFix" and "LifetimeGuard/BoneSystem.json"
    or "WMPOptimizationPatches/BoneSystem.json"
assert(#reports == 2 and reports[1][1] == expected and reports[1][2].version == "1.0.0")
assert(handle.restore() and debug.upvalueid(fn, 2) == original)
assert(handle.snapshot().restored and handle.snapshot().original_cell_connected)
assert(fn({}) == result and globs == 1)
