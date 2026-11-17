-- REFramework runs this once per main Lua state, after native plugin initialization.
local key = "__LifetimeGuardBoneSystemStarted"
if rawget(_G, key) then return end
rawset(_G, key, true)

local start, reason = package.loadlib("reframework/plugins/DStorageFileLifetimeGuard.dll",
    "lifetimeguard_bonesystem_start")
if not start then
    log.error("[LifetimeGuard BoneSystem] " .. tostring(reason))
    return
end
local ok, err = pcall(start)
if not ok then log.error("[LifetimeGuard BoneSystem] " .. tostring(err)) end
