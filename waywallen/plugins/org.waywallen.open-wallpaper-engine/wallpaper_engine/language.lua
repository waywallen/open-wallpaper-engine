-- Wallpaper Engine stores one copy of every language-dependent scene property
-- and gates each copy with a `condition` on the scene's language selector
-- (`language.value == 4`). waywallen neither evaluates that condition nor
-- applies the override before asking for the property list, so a multi-language
-- scene lists every variant at once: Vietnamese labels next to Chinese combo
-- options inside a Spanish UI.
--
-- The scene's own selector plus the locale the host UI is in are enough to pick
-- the variant. A condition this module cannot decide is left visible on
-- purpose: the list is rebuilt from the authored defaults, so hiding a property
-- that another condition gates on a live panel value would make it unreachable
-- once the user flips that value.

local M = {}

-- Primary language code to the names authors use in the selector. The label is
-- authored text rather than a stable key, so English and native spellings are
-- both listed; an unknown spelling keeps the authored default.
local LANGUAGE_NAMES = {
    ar = { "arabic", "العربية" },
    bg = { "bulgarian", "български" },
    cs = { "czech", "čeština" },
    da = { "danish", "dansk" },
    de = { "german", "deutsch" },
    el = { "greek", "ελληνικά" },
    en = { "english" },
    es = { "spanish", "español", "espanol" },
    fi = { "finnish", "suomi" },
    fr = { "french", "français", "francais" },
    he = { "hebrew", "עברית" },
    hu = { "hungarian", "magyar" },
    id = { "indonesian" },
    it = { "italian", "italiano" },
    ja = { "japanese", "日本語" },
    ko = { "korean", "한국어" },
    nb = { "norwegian", "norsk" },
    nl = { "dutch", "nederlands" },
    pl = { "polish", "polski" },
    pt = { "portuguese", "português", "portugues" },
    ro = { "romanian", "română" },
    ru = { "russian", "russia", "русский" },
    sk = { "slovak", "slovenčina" },
    sv = { "swedish", "svenska" },
    th = { "thai", "ไทย" },
    tr = { "turkish", "türkçe" },
    uk = { "ukrainian", "українська" },
    vi = { "vietnamese", "tiếng việt" },
    zh = { "chinese", "中文", "简体中文", "繁體中文" },
}

local LANGUAGE_CODES = {}
for code, names in pairs(LANGUAGE_NAMES) do
    for _, name in ipairs(names) do
        LANGUAGE_CODES[name] = code
    end
end

local function split(text, separator)
    local parts = {}
    local start = 1
    while true do
        local first, last = string.find(text, separator, start, true)
        if not first then
            parts[#parts + 1] = string.sub(text, start)
            return parts
        end
        parts[#parts + 1] = string.sub(text, start, first - 1)
        start = last + 1
    end
end

local function literal(text)
    local quoted = string.match(text, '^"(.*)"$') or string.match(text, "^'(.*)'$")
    if quoted then return quoted end
    if text == "true" then return true end
    if text == "false" then return false end
    return tonumber(text) or text
end

local function compare(left, operator, right)
    local left_number, right_number = tonumber(left), tonumber(right)
    if operator == "==" or operator == "~=" or operator == "!=" then
        local same
        if left_number ~= nil and right_number ~= nil then
            same = left_number == right_number
        else
            same = tostring(left) == tostring(right)
        end
        if operator == "==" then return same end
        return not same
    end
    if left_number == nil or right_number == nil then return nil end
    if operator == ">" then return left_number > right_number end
    if operator == "<" then return left_number < right_number end
    if operator == ">=" then return left_number >= right_number end
    if operator == "<=" then return left_number <= right_number end
    return nil
end

-- Decides one `property.value <operator> literal` clause, or nil when it is not
-- a clause this plugin can answer: an unknown property, an undecoded operator,
-- or a relational comparison against a non-number.
local function clause_holds(clause, values)
    local text = string.gsub(clause, "^%s+", "")
    text = string.gsub(text, "%s+$", "")
    local name, operator, right = string.match(text, "^(.-)%.value%s*([=~!<>]+)%s*(.-)$")
    if not name or name == "" or values[name] == nil then return nil end
    if operator ~= "==" and operator ~= "~=" and operator ~= "!=" and
        operator ~= ">" and operator ~= "<" and operator ~= ">=" and
        operator ~= "<=" then
        return nil
    end
    return compare(values[name], operator, literal(right))
end

-- True / false / nil ("cannot decide"). `values` only carries the properties
-- this module pins, so any clause about a live panel value stays undecidable by
-- construction.
local function condition_holds(expression, values)
    if type(expression) ~= "string" then return nil end
    local text = string.gsub(expression, "%s+", " ")
    if text == "" then return nil end
    if string.find(text, "(", 1, true) then return nil end
    local unknown = false
    for _, group in ipairs(split(text, "||")) do
        local holds, decided = true, true
        for _, clause in ipairs(split(group, "&&")) do
            local verdict = clause_holds(clause, values)
            if verdict == nil then
                decided = false
            elseif verdict == false then
                holds = false
            end
        end
        if decided and holds then return true end
        if not decided then unknown = true end
    end
    if unknown then return nil end
    return false
end

-- The primary language code the host UI is in. An explicit
-- `wallpaper_engine_locale` setting wins; otherwise the daemon's environment
-- decides, in POSIX precedence.
local function ui_language_code(ctx)
    local configured = ctx.config.get("wallpaper_engine_locale")
    if type(configured) == "string" and configured ~= "" then
        local code = string.match(string.lower(configured), "^(%a+)")
        if code then return code end
    end
    for _, name in ipairs({ "LC_ALL", "LC_MESSAGES", "LANG" }) do
        local value = ctx.env(name)
        if type(value) == "string" and value ~= "" then
            local code = string.match(value, "^(%a+)")
            if code then
                code = string.lower(code)
                if code ~= "c" and code ~= "posix" then return code end
            end
        end
    end
    return nil
end

-- The value of the scene's own language selector that matches the host UI, or
-- nil when the scene has no selector or offers no matching language; the
-- authored default then stays untouched.
function M.preferred_value(props, ctx)
    local selector = props["language"]
    if type(selector) ~= "table" or selector.type ~= "combo" or
        type(selector.options) ~= "table" then
        return nil
    end
    local code = ui_language_code(ctx)
    if not code then return nil end
    local fallback = nil
    for _, option in ipairs(selector.options) do
        if type(option) == "table" then
            local label = string.lower(tostring(option.label or ""))
            if LANGUAGE_CODES[label] == code then return option.value end
            local base = string.match(label, "^(.-)%s*%(")
            if fallback == nil and base and LANGUAGE_CODES[base] == code then
                fallback = option.value
            end
        end
    end
    return fallback
end

-- Filters the variants gated on the selected language and mirrors the value the
-- renderer will use into the kept entries, so the panel and the wallpaper
-- agree. Returns a new table; `props` is never modified.
function M.filter(props, ctx)
    local value = M.preferred_value(props, ctx)
    if value == nil then return props end
    local values = { language = value }
    local kept = {}
    for key, property in pairs(props) do
        if type(property) ~= "table" or
            condition_holds(property.condition, values) ~= false then
            if type(property) == "table" and values[key] ~= nil then
                local copy = {}
                for field, field_value in pairs(property) do
                    copy[field] = field_value
                end
                copy.value = values[key]
                property = copy
            end
            kept[key] = property
        end
    end
    return kept
end

return M
