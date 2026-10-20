local project_util = import("wallpaper_engine.project")

local M = {}

-- Steam keeps its own record of the Workshop items it downloaded for an app in
-- `steamapps/workshop/appworkshop_<appid>.acf` (Valve KeyValues). It answers
-- "is this item subscribed" without asking the Community site, and it is the
-- only local signal that separates a real subscription from a directory the
-- user copied into `content/431960` by hand.
local ACF_REL = "/steamapps/workshop/appworkshop_" .. project_util.WE_APPID .. ".acf"

-- Ids Steam recorded a subscription for, or nil while no `.acf` could be read.
-- nil keeps every caller on the behaviour it had before this file existed.
local subscribed = nil

-- Bytes on disk per id, as Steam wrote them down when it unpacked the item.
-- A missing id means "the record does not say", never "empty".
local sizes = {}
local updates = {}

local ESCAPES = { n = "\n", t = "\t", r = "\r" }

-- Read one quoted token and return it with the position just after it. Steam
-- escapes with backslashes, so the token ends at the first unescaped quote.
local function read_quoted(text, start)
    local index = start + 1
    local chunks = {}
    while true do
        local stop = text:find('["\\]', index)
        if not stop then return nil end
        chunks[#chunks + 1] = text:sub(index, stop - 1)
        if text:sub(stop, stop) == '"' then
            return table.concat(chunks), stop + 1
        end
        local escaped = text:sub(stop + 1, stop + 1)
        if escaped == "" then return nil end
        chunks[#chunks + 1] = ESCAPES[escaped] or escaped
        index = stop + 2
    end
end

-- Minimal KeyValues reader: nested tables of strings, tolerant of comments and
-- unquoted tokens. Returns nil for anything it cannot make sense of, so a
-- truncated or foreign file never reports half of the subscriptions.
local function parse_keyvalues(text)
    local root = {}
    local stack = { root }
    local pending = nil
    local pos = 1
    while true do
        local at = text:find("%S", pos)
        if not at then break end
        local char = text:sub(at, at)
        local token = nil
        if char == "{" then
            local node = {}
            if pending ~= nil then
                stack[#stack][pending] = node
                pending = nil
            end
            stack[#stack + 1] = node
            pos = at + 1
        elseif char == "}" then
            if #stack == 1 then return nil end
            stack[#stack] = nil
            pos = at + 1
        elseif char == "/" and text:sub(at + 1, at + 1) == "/" then
            local eol = text:find("\n", at, true)
            if not eol then break end
            pos = eol + 1
        elseif char == '"' then
            token, pos = read_quoted(text, at)
            if token == nil then return nil end
        else
            local stop = text:find('[%s"{}]', at) or (#text + 1)
            token = text:sub(at, stop - 1)
            pos = stop
        end
        if token ~= nil then
            if pending == nil then
                pending = token
            else
                stack[#stack][pending] = token
                pending = nil
            end
        end
    end
    if #stack ~= 1 or pending ~= nil then return nil end
    return root
end

-- KeyValues keys are case-insensitive; Steam's casing is not part of the
-- format's contract, so look the field up both ways.
local function field(node, name)
    if type(node) ~= "table" then return nil end
    local direct = node[name]
    if direct ~= nil then return direct end
    local lowered = string.lower(name)
    for key, value in pairs(node) do
        if type(key) == "string" and string.lower(key) == lowered then return value end
    end
    return nil
end

-- Fold one Steam root's record into `found` and `found_sizes`. Returns whether
-- the file could be used at all: an unreadable or detail-less file must leave
-- the snapshot unset instead of declaring every installed item unsubscribed.
local function fold_root(ctx, steam_root, found, found_sizes, found_updates)
    local path = steam_root .. ACF_REL
    if not ctx.fs.exists(path) then return false end
    local text = ctx.fs.read(path)
    if not text or text == "" then
        ctx.log("wallpaper_engine: cannot read " .. path)
        return false
    end
    local ok, parsed = pcall(parse_keyvalues, text)
    if not ok or type(parsed) ~= "table" then
        ctx.log("wallpaper_engine: unreadable Steam Workshop state in " .. path)
        return false
    end
    local app = field(parsed, "AppWorkshop")
    -- `WorkshopItemsInstalled` is the download bookkeeping: for every item the
    -- client unpacked it holds the byte size of the unpacked directory, so the
    -- scan gets the size on disk without walking the tree. A directory copied
    -- into `content/431960` by hand is in no section and stays unmeasured.
    local downloaded = field(app, "WorkshopItemsInstalled")
    if type(downloaded) == "table" then
        for id, entry in pairs(downloaded) do
            local size = tonumber(field(entry, "size"))
            if size and size > 0 then
                found_sizes[tostring(id)] = math.floor(size)
            end
        end
    end
    local details = field(app, "WorkshopItemDetails")
    if type(details) ~= "table" then return false end
    if type(downloaded) == "table" then
        for id, installed in pairs(downloaded) do
            local entry = details[id]
            local manifest = field(installed, "manifest")
            local latest = field(entry, "latest_manifest") or field(entry, "manifest")
            if manifest and manifest ~= "" and manifest ~= "0"
                and latest and latest ~= "" and latest ~= "0" then
                found_updates[tostring(id)] = manifest ~= latest
            else
                local time = tonumber(field(installed, "timeupdated"))
                local latest_time = tonumber(field(entry, "latest_timeupdated") or field(entry, "timeupdated"))
                found_updates[tostring(id)] = time ~= nil and time > 0 and latest_time ~= nil and latest_time > time
            end
        end
    end
    local usable = false
    for id, entry in pairs(details) do
        if type(entry) == "table" then
            usable = true
            -- `subscribedby` is the SteamID that subscribed. A directory placed
            -- in `content/431960` by hand is in no record at all, and an item
            -- Steam only keeps for another reason carries no id here.
            local by = field(entry, "subscribedby")
            if by and by ~= "" and by ~= "0" then found[tostring(id)] = true end
        end
    end
    return usable
end

-- Re-read Steam's record for every registered Steam root. Called from the
-- source scan because that is the only callback whose context carries `ctx.fs`.
function M.refresh(ctx, roots)
    local found, found_sizes, found_updates, usable = {}, {}, {}, false
    for _, steam_root in ipairs(roots) do
        found_updates[steam_root] = {}
        if fold_root(ctx, steam_root, found, found_sizes, found_updates[steam_root]) then usable = true end
    end
    -- Sizes stand on their own: they describe the directories on this disk, so
    -- a record without usable details still gets to report the ones it listed.
    sizes = found_sizes
    updates = found_updates
    if not usable then
        subscribed = nil
        return
    end
    subscribed = found
    local count = 0
    for _ in pairs(found) do count = count + 1 end
    ctx.log("wallpaper_engine: " .. count .. " Workshop subscriptions recorded by Steam")
end

-- Drop the snapshot: it describes the account the Steam client is signed in as
-- and must not outlive a sign-out.
function M.forget()
    subscribed = nil
    updates = {}
end

-- "subscribed" for items Steam recorded on this machine, nil when the local
-- record cannot answer and the caller has to ask Steam Community instead.
function M.state(id)
    if subscribed and subscribed[tostring(id)] then return "subscribed" end
    return nil
end

-- Keep the snapshot in step with subscriptions made through waywallen, so an
-- item just unsubscribed here stops reporting the state Steam recorded before.
function M.mark(id, is_subscribed)
    if not subscribed then return end
    subscribed[tostring(id)] = is_subscribed or nil
end

-- Size on disk in bytes for an item Steam downloaded, nil for everything the
-- record does not cover. The daemon leaves a nil size to its own probing, and
-- the client hides the "Size" row while nothing knows the answer.
function M.size(id)
    return sizes[tostring(id)]
end

function M.update_available(id, steam_root)
    local root = updates[steam_root]
    return root ~= nil and root[tostring(id)] == true
end

-- The daemon turns a non-empty `external_id` into an "Unsubscribe" button, so
-- only items Steam recorded a subscription for may carry one. Without a
-- snapshot the id is passed through, which is what the scan did before.
function M.subscription_id(id)
    if subscribed and not subscribed[tostring(id)] then return nil end
    return id
end

return M
