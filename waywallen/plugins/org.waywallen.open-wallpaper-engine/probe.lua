local fs = require("waywallen.fs")
local json = require("waywallen.json")

local function scene_json(path)
    if path:lower():match("%.json$") then return fs.read(path) end
    if not path:lower():match("%.pkg$") then return nil end
    local entry = path:match("([^/]+)$"):sub(1, -5):lower() .. ".json"
    local total = fs.stat(path).size
    local cursor = 0
    local function integer()
        local value = fs.u32_le(path, cursor)
        cursor = cursor + 4
        if value > 2147483647 then error("Invalid PKG integer") end
        return value
    end
    local function text(limit)
        local length = integer()
        if length > limit then error("PKG string exceeds limit") end
        local value = fs.read_at(path, cursor, length)
        cursor = cursor + length
        return value
    end
    local version = text(64)
    if #version <= 4 or version:sub(1, 4) ~= "PKGV" then error("Invalid PKG version") end
    local count = integer()
    if count > 100000 then error("PKG file count exceeds limit") end
    local offset, length
    for _ = 1, count do
        local name = text(4096):gsub("\\", "/"):lower():gsub("^/+", "")
        local start, size = integer(), integer()
        if name == entry then offset, length = start, size end
    end
    if not offset then return nil end
    if not total or cursor > total or offset > total - cursor or length > total - cursor - offset then
        error("PKG scene range exceeds file")
    end
    if length > 8 * 1024 * 1024 then error("Scene JSON exceeds limit") end
    return fs.read_at(path, cursor + offset, length)
end

local function dimension(value)
    value = tonumber(value)
    if value and value >= 0.5 and value <= 2147483647 then return (value + 0.5) // 1 end
end

local function origin(value)
    if type(value) == "table" then return tonumber(value[1]), tonumber(value[2]) end
    if type(value) == "string" then
        local x, y = value:match("^%s*(%S+)%s+(%S+)")
        return tonumber(x), tonumber(y)
    end
end

return {
    probe = function(item)
        local source = scene_json(item.path)
        if not source then return nil end
        local scene = json.parse(source)
        local projection = type(scene.general) == "table" and scene.general.orthogonalprojection
        if type(projection) ~= "table" then return nil end
        if projection.auto == true then
            for _, object in ipairs(scene.objects or {}) do
                if object.image then
                    local x, y = origin(object.origin)
                    if x and y then
                        local width, height = dimension(x * 2), dimension(y * 2)
                        if width and height then return { width = width, height = height } end
                    end
                end
            end
            return nil
        end
        local width, height = dimension(projection.width), dimension(projection.height)
        if width and height then return { width = width, height = height } end
    end,
}
