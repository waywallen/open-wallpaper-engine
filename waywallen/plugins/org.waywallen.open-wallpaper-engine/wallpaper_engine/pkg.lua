-- Reading one member out of a Wallpaper Engine `.pkg`.
--
-- The container is a table of contents followed by the members stored as
-- they are; only textures carry compression of their own. Reading the
-- scene document is therefore two windowed reads — the header, then the
-- member's own range — which is what keeps a 200 MB package from being
-- pulled into memory for a single JSON object.

local M = {}

local HEADER_WINDOW = 64 * 1024
local HEADER_LIMIT  = 1024 * 1024
local MEMBER_LIMIT  = 1024 * 1024
local PATH_LIMIT    = 4096

local function read_i32(blob, pos)
    if pos < 1 or pos + 3 > #blob then return nil end
    return string.unpack("<i4", blob, pos)
end

local function read_sized_string(blob, pos, limit)
    local len, next_pos = read_i32(blob, pos)
    if not len or len < 0 or len > limit then return nil end
    if next_pos + len - 1 > #blob then return nil end
    return string.sub(blob, next_pos, next_pos + len - 1), next_pos + len
end

-- entry, header_size, truncated. `truncated` says the window ended inside
-- the table of contents, so a wider one may still find the member; a false
-- entry with truncated false means the package simply does not carry it.
local function find_member(blob, name)
    local stamp, pos = read_sized_string(blob, 1, 64)
    if not stamp then return nil, nil, true end
    if string.sub(stamp, 1, 4) ~= "PKGV" then return nil, nil, false end

    local count
    count, pos = read_i32(blob, pos)
    if not count or count < 0 then return nil, nil, true end

    local found
    for _ = 1, count do
        local path, offset, length
        path, pos = read_sized_string(blob, pos, PATH_LIMIT)
        if not path then return nil, nil, true end
        offset, pos = read_i32(blob, pos)
        if not offset then return nil, nil, true end
        length, pos = read_i32(blob, pos)
        if not length then return nil, nil, true end
        if not found and offset >= 0 and length > 0
            and string.lower(path):match("([^/\\]+)$") == name
        then
            found = { offset = offset, length = length }
        end
    end
    return found, pos - 1, false
end

-- The member's bytes, or nil when the package cannot be read: a missing
-- `ctx.fs.read_bytes` (an older daemon), a header wider than the budget,
-- or a member too large to be a document.
function M.read_member(ctx, path, name)
    local read_bytes = ctx.fs and ctx.fs.read_bytes
    if type(read_bytes) ~= "function" then return nil end

    local window = HEADER_WINDOW
    while window <= HEADER_LIMIT do
        local blob = read_bytes(path, 0, window)
        if type(blob) ~= "string" then return nil end
        local entry, header_size, truncated = find_member(blob, name)
        if entry then
            if entry.length > MEMBER_LIMIT then return nil end
            return read_bytes(path, header_size + entry.offset, entry.length)
        end
        -- A short read means the window already covered the whole file.
        if not truncated or #blob < window then return nil end
        window = window * 2
    end
    return nil
end

return M
